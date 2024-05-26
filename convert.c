#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>

#include "mrlib.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

#include <time.h>
double get_time_in_micros(void) {
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	return (((double)spec.tv_sec) * 1000000) + (((double)spec.tv_nsec) / 1000);
}

typedef struct SampleNode {
    const char *funcName;
    int funcNameLength;
    int id;
    int childCount;
    int *childs;
} SampleNode;

typedef struct NodeParents {
    int *array, length;
} NodeParents;

typedef struct EvalStackEntry {
    int sampleNodeId;
    int duration;
    int startTime;
} EvalStackEntry;

typedef struct EmitedEvalStackEntries {
    EvalStackEntry entries[8192];
    int count;
    struct EmitedEvalStackEntries *next;
} EmitedEvalStackEntries;

typedef struct EmitedEvalStackList {
    EmitedEvalStackEntries *head;
    EmitedEvalStackEntries *tail;
} EmitedEvalStackList;

#define STACK_SIZE (32 * 1024)

typedef struct EvalStack {
    EvalStackEntry stack[STACK_SIZE];
    int length;

    int currentTime;
    EmitedEvalStackList emitted;
} EvalStack;

static void
readJsonNumberArray(json_t *jArray, int *output) {
    json_t *values = json_values(jArray);

    for(size_t i = 0; i < jArray->len; i++) {
        json_t *value = values + i;
        assert(value->type == JSON_NUMBER);
        output[i] = value->number;
    }
}

static SampleNode
parseSampleNode(Arena *arena, json_t *node) {
    SampleNode result = { 0 };

    json_t *values = json_values(node);
    char **keys = json_keys(node);
    for(size_t i = 0; i < node->len; i++) {
        char *name = keys[i];
        json_t *value = values + i;

        if(strcmp(name, "id") == 0) {
            assert(value->type == JSON_NUMBER);
            result.id = value->number;
        } else if(strcmp(name, "children") == 0) {
            assert(value->type == JSON_ARRAY);
            result.childCount = value->len;
            if(result.childCount > 0) {
                result.childs = arrayPush(arena, int, result.childCount);
                readJsonNumberArray(value, result.childs);
            }
        } else if(strcmp(name, "callFrame") == 0) {
            assert(value->type == JSON_OBJECT);
            json_t *obj = values + i;

            json_t *callFrameValues = json_values(value);
            char **callFrameKeys = json_keys(value);
            for(size_t j = 0; j < value->len; j++) {
                char *name = callFrameKeys[j];
                if(strcmp(name, "functionName") == 0) {
                    result.funcName = callFrameValues[j].string;
                    result.funcNameLength = callFrameValues[j].len;
                    // result.funcName = ((struct json_string_s *)J->value->payload)->string;
                    if(strlen(result.funcName) == 0) {
                        result.funcName = "(anonymous)";
                        result.funcNameLength = strlen(result.funcName);
                    }
                    break;
                }
            }
        }
    }

    assert(result.id > 0);
    assert(result.funcName);

    return result;
}

static void
parseSampleNodes(Arena *arena, json_t *jArray, SampleNode *output) {
    json_t *values = json_values(jArray);

    for(size_t i = 0; i < jArray->len; i++) {
        json_t *value = values + i;
        assert(value->type == JSON_OBJECT);
        SampleNode out = parseSampleNode(arena, value);
        output[out.id - 1] = out;
    }
}

static void
emitEntry(EvalStack *stack, EvalStackEntry *entry) {
    EmitedEvalStackList *list = &stack->emitted;
    EmitedEvalStackEntries *node = NULL;

    if(list->tail && list->tail->count < ARRAY_LENGTH(list->tail->entries)) {
        node = list->tail;
    } else {
        node = calloc(1, sizeof(*node));

        if(!list->head) {
            list->head = node;
            list->tail = node;
        } else {
            list->tail->next = node;
            list->tail = node;
        }
    }

    node->entries[node->count++] = *entry;
}

static void
evalStackTrace(EvalStack *stack, SampleNode *nodes, NodeParents *parents, int nodeId, int timeDelta) {
    if(nodeId == 0) { return; }

    static int unwoundStack[STACK_SIZE] = { 0 };
    int stackDepth = 0;
    if(strcmp(nodes[nodeId].funcName, "(garbage collector)") == 0) {
        unwoundStack[0] = nodeId;
        for(int i = 0; i < stack->length; i++) {
            unwoundStack[i+1] = stack->stack[stack->length - i - 1].sampleNodeId;
        }
        stackDepth = stack->length + 1;
    } else {
        for(stackDepth = 0; nodeId != 0; stackDepth++, nodeId = parents->array[nodeId]) {
            unwoundStack[stackDepth] = nodeId;
        }
    }

    for(int i = stackDepth; i < stack->length; i++) {
        emitEntry(stack, stack->stack + i);
        memset(stack->stack + i, 0, sizeof(stack->stack[0]));
    }

    stack->length = stackDepth;

    for(int i = 0; i < stackDepth; i++) {
        nodeId = unwoundStack[stackDepth - i - 1];
        assert(i < ARRAY_LENGTH(stack->stack));
        EvalStackEntry *entry = stack->stack + i;

        if(entry->sampleNodeId != nodeId) {
            if(entry->sampleNodeId > 0) {
                emitEntry(stack, entry);
            }

            entry->sampleNodeId = nodeId;
            entry->duration = 0;
            entry->startTime = stack->currentTime;
        }

        entry->duration += timeDelta;
    }
}

typedef struct CPUProfile {
    float startTime;
    int sampleCount;
    int *samples;
    int deltaCount;
    int *deltas;
    int sampleNodeCount;
    SampleNode *sampleNodes;
    NodeParents parents;
} CPUProfile;

static CPUProfile
parseCPUProfileJSON(Arena *arena, String jsonString) {
    CPUProfile cpuprofile = { 
        .startTime = -1.0f, .sampleCount = 0,
        .samples = NULL, .deltaCount = 0,
        .deltas = NULL, .sampleNodeCount = 0,
        .sampleNodes = NULL, .parents = { 0 }
    };

    json_t *json = 0x0;
    unsigned int tokens_capacity = 1 + jsonString.size / 2;
    json_token_t *tokens = arrayPush(arena, json_token_t, tokens_capacity);
    unsigned int size_req;
    int tokens_len = json_tokenize((char *)jsonString.data, jsonString.size, tokens, tokens_capacity, &size_req);
    if (tokens_len > 0) {
        json = arenaPush(arena, size_req);
        json_parse_tokens((char *)jsonString.data, tokens, tokens_len, json);
    }

    assert(json->type == JSON_OBJECT);
    json_t *values = json_values(json);
    char **keys = json_keys(json);
    for(size_t i = 0; i < json->len; i++) {
        char *name = keys[i];
        json_t *value = values + i;

        if(strcmp(name, "startTime") == 0) {
            assert(value->type == JSON_NUMBER);
            cpuprofile.startTime = value->number;
        } else if(strcmp(name, "samples") == 0) {
            assert(value->type == JSON_ARRAY);
            cpuprofile.sampleCount = value->len;
            cpuprofile.samples = arrayPush(arena, int, cpuprofile.sampleCount);

            readJsonNumberArray(value, cpuprofile.samples);
            for(int i = 0 ; i < cpuprofile.sampleCount; i++) {
                cpuprofile.samples[i] -= 1;
            }
        } else if(strcmp(name, "timeDeltas") == 0) {
            assert(value->type == JSON_ARRAY);
            cpuprofile.deltaCount = value->len;
            cpuprofile.deltas = arrayPush(arena, int, cpuprofile.sampleCount);

            readJsonNumberArray(value, cpuprofile.deltas);
        } else if(strcmp(name, "nodes") == 0) {
            assert(value->type == JSON_ARRAY);
            cpuprofile.sampleNodeCount = value->len;
            cpuprofile.sampleNodes = arrayPush(arena, SampleNode, cpuprofile.sampleNodeCount);

            parseSampleNodes(arena, value, cpuprofile.sampleNodes);
        }
    }

    assert(cpuprofile.deltaCount == cpuprofile.sampleCount);

    cpuprofile.parents.length = cpuprofile.sampleNodeCount;
    cpuprofile.parents.array = (int *)calloc(1, sizeof(cpuprofile.parents.array[0]) * cpuprofile.parents.length);

    for(int i = 0; i < cpuprofile.parents.length; i++) {
        SampleNode *node = cpuprofile.sampleNodes + i;
        for(int j = 0; j < node->childCount; j++) {
            assert(cpuprofile.parents.array[node->childs[j] - 1] == 0);
            cpuprofile.parents.array[node->childs[j] - 1] = i;
        }
    }

    return cpuprofile;
}

static EvalStack
unpackStack(CPUProfile *profile) {
    EvalStack stack = { 0 };

    for(int i = 0; i < profile->sampleCount; i++) {
        evalStackTrace(&stack, profile->sampleNodes, &profile->parents, profile->samples[i], profile->deltas[i]);
        stack.currentTime += profile->deltas[i];
    }

    return stack;
}

static char *
getOutputPath(Arena *arena, char *inputPath) {
    int inputPathLength = strlen(inputPath);

    for(int i = inputPathLength - 1; i >= 0; i--) {
        if(inputPath[i] == '.') {
            inputPathLength = i;
            break;
        }
    }

    const char *ext = "_spall.json";
    char *outputPath = arrayPush(arena, char, inputPathLength + strlen(ext) + 1);
    strncpy(outputPath, inputPath, inputPathLength);
    strncpy(outputPath + inputPathLength, ext, strlen(ext) + 1);

    return outputPath;
}

static int
writeNumber(char *output, int value) {
    int written = 0;
    if (value < 0) {
        output[written++] = '-';
        value = -value;
    }

    // Calculate the length of the number
    int temp = value;
    int len = 0;
    do {
        len++;
        temp /= 10;
    } while (temp > 0);

    // Write the number to the output buffer
    for (int i = len - 1; i >= 0; i--) {
        output[written + i] = (value % 10) + '0';
        value /= 10;
    }
    written += len;

    return written;
}

static String
writeOutput(Arena *arena, EvalStack *stack, CPUProfile cpuprofile) {
    int entries = 0;
    int funcNameLengths = 0;
    for(EmitedEvalStackEntries *node = stack->emitted.head; node; node = node->next) {
        entries += node->count;
        for(int i = 0; i < node->count; i++) {
            EvalStackEntry *e = node->entries + i;
            funcNameLengths += cpuprofile.sampleNodes[e->sampleNodeId].funcNameLength;
        }
    }

    int OVERHEAD_SIZE = 512;
    int LINE_MAX_SIZE = 96;
    int outputSize = entries * LINE_MAX_SIZE + OVERHEAD_SIZE + funcNameLengths;

    char *output = arrayPush(arena, char, outputSize);
    char *outputPtr = output;
    memcpy(outputPtr, "{ \"traceEvents\": [\n", 19);
    outputPtr += 19;

    for(EmitedEvalStackEntries *node = stack->emitted.head; node; node = node->next) {
        for(int i = 0; i < node->count; i++) {
            EvalStackEntry *e = node->entries + i;
            // This is for speeeeed
            memcpy(outputPtr, "{\"dur\":", 7);
            outputPtr += 7;
            outputPtr += writeNumber(outputPtr, e->duration);
            memcpy(outputPtr, ",\"name\":\"", 9);
            outputPtr += 9;
            memcpy(outputPtr, cpuprofile.sampleNodes[e->sampleNodeId].funcName, cpuprofile.sampleNodes[e->sampleNodeId].funcNameLength);
            outputPtr += cpuprofile.sampleNodes[e->sampleNodeId].funcNameLength;
            memcpy(outputPtr, "\",\"ph\":\"X\",\"tid\":1,\"ts\":", 24);
            outputPtr += 24;
            outputPtr += writeNumber(outputPtr, e->startTime);
            memcpy(outputPtr, "},\n", 3);
            outputPtr += 3;
        }
    }

    memcpy(outputPtr - 2, "\n]}\n", 4);
    outputPtr += 4;

    return (String) { (u8 *)output, outputPtr - output };
}

static String
convertToPerfetto(Arena *arena, String input) {
    CPUProfile cpuprofile = parseCPUProfileJSON(arena, input);
    EvalStack stack = unpackStack(&cpuprofile);

    return writeOutput(arena, &stack, cpuprofile);
}

#ifdef EMSCRIPTEN
String result = { 0 };

String *convertStringToPerfetto(const char *string, int len) {
    Arena arena = arenaCreate(64 * MEGABYTE, 4096, 32);
    String input = { (u8 *)string, len };
    result = convertToPerfetto(&arena, input);
    arenaDestroy(&arena);
    return &result;
}
#endif

#ifndef EMSCRIPTEN
static void
convertFile(Arena *arena, char *path) {
    String input = readFileIntoString(arena, path);
    String output = convertToPerfetto(arena, input);

    char *outputPath = getOutputPath(arena, path);
    FILE *f = fopen(outputPath, "w");
    if(!f) {
        fprintf(stderr, "Failed to open file [%s] for writing\n", outputPath);
        return;
    }

    fprintf(f, "%s", output.data);
    fclose(f);

    printf("Converted %s to %s\n", path, outputPath);
}

int main(int argCount, char **args) {
    Arena arena = arenaCreate(64 * MEGABYTE, 4096, 32);
    if(argCount < 2) {
        fprintf(stderr, "Usage: convert file.cpuprofile\n");
        return 1;
    }

    for(int i = 1; i < argCount; i++) {
        u64 pos = arenaPos(&arena);
        convertFile(&arena, args[i]);
        arenaPopTo(&arena, pos);
    }

    return 0;
}
#endif
