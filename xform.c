#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>

#include "mrlib.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

typedef enum OutputType {
    OutputType_None,
    OutputType_GTrace,
    OutputType_Spall,
    OutputType_Count,
} OutputType;

typedef struct SampleNode {
    String funcName;
    String path;
    String location;
    s32 id;
    s32 childCount;
    s32 *childs;
} SampleNode;

typedef struct NodeParents {
    s32 *array, length;
} NodeParents;

typedef NodeParents NodeDepths;

typedef struct EvalStackEntry {
    s32 sampleNodeId;
    s32 duration;
    s32 startTime;
} EvalStackEntry;

typedef struct EmitedEvalStackEntries {
    EvalStackEntry entries[8192];
    u32 count;
    struct EmitedEvalStackEntries *next;
} EmitedEvalStackEntries;

typedef struct EmitedEvalStackList {
    EmitedEvalStackEntries *head;
    EmitedEvalStackEntries *tail;
} EmitedEvalStackList;

#define STACK_SIZE (32 * 1024)

typedef struct EvalStack {
    EvalStackEntry stack[STACK_SIZE];
    s32 length;

    s32 currentTime;
    EmitedEvalStackList emitted;
} EvalStack;

static void
readJsonNumberArray(json_t *jArray, s32 *output) {
    json_t *values = json_values(jArray);

    s32 isArray = 1;
    for(u64 i = 0; i < jArray->len; i++) {
        json_t *value = values + i;
        isArray &= value->type == JSON_NUMBER;
        output[i] = value->number;
    }
    assert(isArray);
}

static SampleNode
parseSampleNode(Arena *arena, json_t *node) {
    SampleNode result = { 0 };

    json_t *values = json_values(node);
    char **keys = json_keys(node);
    for(u64 i = 0; i < node->len; i++) {
        char *name = keys[i];
        json_t *value = values + i;

        if(strcmp(name, "id") == 0) {
            assert(value->type == JSON_NUMBER);
            result.id = value->number;
        } else if(strcmp(name, "children") == 0) {
            assert(value->type == JSON_ARRAY);
            result.childCount = value->len;
            if(result.childCount > 0) {
                result.childs = arrayPush(arena, s32, result.childCount);
                readJsonNumberArray(value, result.childs);
            }
        } else if(strcmp(name, "callFrame") == 0) {
            assert(value->type == JSON_OBJECT);

            json_t *callFrameValues = json_values(value);
            char **callFrameKeys = json_keys(value);
            s32 lineNumber = -1;
            s32 columnNumber = -1;
            for(u64 j = 0; j < value->len; j++) {
                char *name = callFrameKeys[j];
                if(strcmp(name, "functionName") == 0) {
                    result.funcName = (String){ .data = (u8 *)callFrameValues[j].string, .size = callFrameValues[j].len };
                    if(result.funcName.size == 0) {
                        result.funcName = LIT_TO_STR("(anonymous)");
                    }
                } else if(strcmp(name, "url") == 0) {
                    result.path = (String){ .data = (u8 *)callFrameValues[j].string, .size = callFrameValues[j].len };

                    String filePrefix = LIT_TO_STR("file://");
                    if(stringStartsWith(result.path, filePrefix)) {
                        result.path.data += filePrefix.size;
                        result.path.size -= filePrefix.size;
                    }
                } else if(strcmp(name, "lineNumber") == 0) {
                    lineNumber = callFrameValues[j].number;
                } else if(strcmp(name, "columnNumber") == 0) {
                    columnNumber = callFrameValues[j].number;
                }
            }

            if(lineNumber != -1 && columnNumber != -1) {
                result.location = pushStringf(arena, "Location: %d:%d", lineNumber, columnNumber);
            }
        }
    }

    assert(result.id > 0);
    assert(result.funcName.size > 0);

    return result;
}

static void
parseSampleNodes(Arena *arena, json_t *jArray, SampleNode *output) {
    json_t *values = json_values(jArray);

    for(u64 i = 0; i < jArray->len; i++) {
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
evalStackTrace(EvalStack *stack, SampleNode *nodes, NodeParents *parents, s32 nodeId, s32 timeDelta) {
    if(nodeId == 0) { return; }

    static u32 unwoundStack[STACK_SIZE] = { 0 };
    u32 stackDepth = 0;
    if(stringMatch(nodes[nodeId].funcName, LIT_TO_STR("(garbage collector)"))) {
        unwoundStack[0] = nodeId;
        for(s32 i = 0; i < stack->length; i++) {
            unwoundStack[i+1] = stack->stack[stack->length - i - 1].sampleNodeId;
        }
        stackDepth = stack->length + 1;
    } else {
        for(stackDepth = 0; nodeId != 0; stackDepth++, nodeId = parents->array[nodeId]) {
            unwoundStack[stackDepth] = nodeId;
        }
    }

    for(s32 i = stackDepth; i < stack->length; i++) {
        emitEntry(stack, stack->stack + i);
        memset(stack->stack + i, 0, sizeof(stack->stack[0]));
    }

    stack->length = stackDepth;

    for(u32 i = 0; i < stackDepth; i++) {
        nodeId = unwoundStack[stackDepth - i - 1];
        assert((u32)i < ARRAY_LENGTH(stack->stack));
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
    s32 sampleCount;
    s32 *samples;
    s32 deltaCount;
    s32 *deltas;
    s32 sampleNodeCount;
    SampleNode *sampleNodes;
    NodeParents parents;
    NodeDepths depths;
} CPUProfile;

static void
computeDepth(NodeDepths *depths, SampleNode *nodes, u32 nodeId, u32 depth) {
    SampleNode *n = nodes + nodeId;
    for(s32 j = 0; j < n->childCount; j++) {
        u32 childId = n->childs[j] - 1;
        assert(depths->array[childId] == 0);
        depths->array[childId] = depth + 1;
        computeDepth(depths, nodes, childId, depth + 1);
    }
}

static CPUProfile
parseCPUProfileJSON(Arena *arena, String jsonString) {
    CPUProfile cpuprofile = { 
        .startTime = -1.0f, .sampleCount = 0,
        .samples = NULL, .deltaCount = 0,
        .deltas = NULL, .sampleNodeCount = 0,
        .sampleNodes = NULL, .parents = { 0 }
    };

    json_t *json = 0x0;
    u32 tokens_capacity = 1 + jsonString.size / 2;
    json_token_t *tokens = arrayPush(arena, json_token_t, tokens_capacity);
    u32 size_req;
    s32 tokens_len = json_tokenize((char *)jsonString.data, jsonString.size, tokens, tokens_capacity, &size_req);
    if (tokens_len > 0) {
        json = arenaPush(arena, size_req);
        json_parse_tokens((char *)jsonString.data, tokens, tokens_len, json);

        assert(json->type == JSON_OBJECT);
        json_t *values = json_values(json);
        char **keys = json_keys(json);
        for(u64 i = 0; i < json->len; i++) {
            char *name = keys[i];
            json_t *value = values + i;

            if(strcmp(name, "startTime") == 0) {
                assert(value->type == JSON_NUMBER);
                cpuprofile.startTime = value->number;
            } else if(strcmp(name, "samples") == 0) {
                assert(value->type == JSON_ARRAY);
                cpuprofile.sampleCount = value->len;
                cpuprofile.samples = arrayPush(arena, s32, cpuprofile.sampleCount);

                readJsonNumberArray(value, cpuprofile.samples);
                for(s32 i = 0 ; i < cpuprofile.sampleCount; i++) {
                    cpuprofile.samples[i] -= 1;
                }
            } else if(strcmp(name, "timeDeltas") == 0) {
                assert(value->type == JSON_ARRAY);
                cpuprofile.deltaCount = value->len;
                cpuprofile.deltas = arrayPush(arena, s32, cpuprofile.sampleCount);

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
        cpuprofile.parents.array = (s32 *)calloc(1, sizeof(cpuprofile.parents.array[0]) * cpuprofile.parents.length);

        for(s32 i = 0; i < cpuprofile.parents.length; i++) {
            SampleNode *node = cpuprofile.sampleNodes + i;
            for(s32 j = 0; j < node->childCount; j++) {
                assert(cpuprofile.parents.array[node->childs[j] - 1] == 0);
                cpuprofile.parents.array[node->childs[j] - 1] = i;
            }
        }

        cpuprofile.depths.length = cpuprofile.sampleNodeCount;
        cpuprofile.depths.array = (s32 *)calloc(1, sizeof(cpuprofile.depths.array[0]) * cpuprofile.depths.length);
        computeDepth(&cpuprofile.depths, cpuprofile.sampleNodes, 0, 0);
    }

    return cpuprofile;
}

static EvalStack
unpackStack(CPUProfile *profile) {
    EvalStack stack = { 0 };

    for(s32 i = 0; i < profile->sampleCount; i++) {
        evalStackTrace(&stack, profile->sampleNodes, &profile->parents, profile->samples[i], profile->deltas[i]);
        stack.currentTime += profile->deltas[i];
    }

    return stack;
}

static char *
getOutputPath(OutputType outputType, Arena *arena, char *inputPath) {
    s32 inputPathLength = strlen(inputPath);

    for(s32 i = inputPathLength - 1; i >= 0; i--) {
        if(inputPath[i] == '.') {
            inputPathLength = i;
            break;
        }
    }

    const char *ext;
    switch(outputType) {
        case OutputType_GTrace: {
            ext = "_gtrace.json";
            break;
        }
        case OutputType_Spall: {
            ext = ".spall";
            break;
        }
        default: {
            assert(false && "Unexpected outputType");
        }
    }
    char *outputPath = arrayPush(arena, char, inputPathLength + strlen(ext) + 1);
    strncpy(outputPath, inputPath, inputPathLength);
    strncpy(outputPath + inputPathLength, ext, strlen(ext) + 1);

    return outputPath;
}

static s32
writeNumber(char *output, s32 value) {
    s32 written = 0;
    if (value < 0) {
        output[written++] = '-';
        value = -value;
    }

    // Calculate the length of the number
    s32 temp = value;
    s32 len = 0;
    do {
        len++;
        temp /= 10;
    } while (temp > 0);

    // Write the number to the output buffer
    for (s32 i = len - 1; i >= 0; i--) {
        output[written + i] = (value % 10) + '0';
        value /= 10;
    }
    written += len;

    return written;
}

static String
writeGTraceOutput(Arena *arena, EvalStack *stack, CPUProfile cpuprofile) {
    s32 entries = 0;
    s32 funcNameLengths = 0;
    for(EmitedEvalStackEntries *node = stack->emitted.head; node; node = node->next) {
        entries += node->count;
        for(u32 i = 0; i < node->count; i++) {
            EvalStackEntry *e = node->entries + i;
            funcNameLengths += cpuprofile.sampleNodes[e->sampleNodeId].funcName.size;
        }
    }

    s32 OVERHEAD_SIZE = 512;
    s32 LINE_MAX_SIZE = 96;
    s32 outputSize = entries * LINE_MAX_SIZE + OVERHEAD_SIZE + funcNameLengths;

    char *output = arrayPush(arena, char, outputSize);
    char *outputPtr = output;
    memcpy(outputPtr, "{ \"traceEvents\": [\n", 19);
    outputPtr += 19;

    for(EmitedEvalStackEntries *node = stack->emitted.head; node; node = node->next) {
        for(u32 i = 0; i < node->count; i++) {
            EvalStackEntry *e = node->entries + i;
            // This is for speeeeed
            memcpy(outputPtr, "{\"dur\":", 7);
            outputPtr += 7;
            outputPtr += writeNumber(outputPtr, e->duration);
            memcpy(outputPtr, ",\"name\":\"", 9);
            outputPtr += 9;
            memcpy(outputPtr, cpuprofile.sampleNodes[e->sampleNodeId].funcName.data, cpuprofile.sampleNodes[e->sampleNodeId].funcName.size);
            outputPtr += cpuprofile.sampleNodes[e->sampleNodeId].funcName.size;
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

static u64
writeU8(void *ptr, u8 v) {
    ((u8 *)ptr)[0] = v;
    return sizeof(u8);
}

static u64
writeU32(void *ptr, u32 v) {
    ((u32 *)ptr)[0] = v;
    return sizeof(u32);
}

static u64
writeU64(void *ptr, u64 v) {
    ((u64 *)ptr)[0] = v;
    return sizeof(u64);
}

static u64
writeF64(void *ptr, f64 v) {
    ((f64 *)ptr)[0] = v;
    return sizeof(f64);
}

static u64
writeSpallBeginMarker(u8 *output, f64 timestamp, String fnName, String path, String location) {
    u8 *base = output;
    output += writeU8(output, 3);
    output += writeU8(output, 0);

    output += writeU32(output, 1);
    output += writeU32(output, 1);
    output += writeF64(output, timestamp);

    String blockName;
    if(path.size > 0) {
        static char buffer[256];
        blockName.data = (u8 *)buffer;
        blockName.size = snprintf(buffer, sizeof(buffer), "%.*s: %.*s", STRFMT(fnName), STRFMT(path));
    } else {
        blockName = fnName;
    }

    assert(blockName.size <= 255);

    output += writeU8(output, blockName.size);
    output += writeU8(output, location.size);

    memcpy(output, blockName.data, blockName.size);
    output += blockName.size;

    memcpy(output, location.data, location.size);
    output += location.size;

    return output - base;
}

static u64
writeSpallEndMarker(u8 *output, f64 timestamp) {
    u8 *base = output;
    output += writeU8(output, 4);
    output += writeU32(output, 1);
    output += writeU32(output, 1);
    output += writeF64(output, timestamp);
    return output - base;
}

static String
writeSpallOutput(Arena *arena, CPUProfile *profile) {
    s32 outputSize = 10 * MEGABYTE;
    u8 *output = arrayPush(arena, u8, outputSize);
    u8 *outputPtr = output;

    outputPtr += writeU64(outputPtr, 0x0BADF00D);
    outputPtr += writeU64(outputPtr, 1);
    outputPtr += writeF64(outputPtr, 1);
    outputPtr += writeU64(outputPtr, 0);

    s32 currentTime = 0;

    NodeParents *parents = &profile->parents;
    SampleNode *nodes = profile->sampleNodes;

    s32 previousNode = 0;
    bool wasGC = false;
    String emptyString = LIT_TO_STR("");
    for(s32 i = 0; i < profile->sampleCount; i++) {
        s32 nodeId = profile->samples[i]; 

        bool isNodeGC = stringMatch(nodes[nodeId].funcName, LIT_TO_STR("(garbage collector)"));
        if(isNodeGC) {
            if(!wasGC) {
                wasGC = true;
                outputPtr += writeSpallBeginMarker(outputPtr, currentTime, nodes[nodeId].funcName, emptyString, emptyString);
            }
        } else {
            if(wasGC) {
                wasGC = false;
                outputPtr += writeSpallEndMarker(outputPtr, currentTime);
            }

            // Find the common node
            s32 previousNodeDepth = profile->depths.array[previousNode];
            s32 currentNodeDepth = profile->depths.array[nodeId];
            s32 shallowest = MIN(previousNodeDepth, currentNodeDepth);

            s32 previousNodeWalking = previousNode;
            s32 currentNodeWalking = nodeId;
            for(s64 i = 0; i < previousNodeDepth - shallowest; i++) {
                previousNodeWalking = parents->array[previousNodeWalking];
            }
            for(s64 i = 0; i < currentNodeDepth - shallowest; i++) {
                currentNodeWalking = parents->array[currentNodeWalking];
            }

            while(previousNodeWalking != currentNodeWalking) {
                previousNodeWalking = parents->array[previousNodeWalking];
                currentNodeWalking = parents->array[currentNodeWalking];
            }

            s32 commonNode = currentNodeWalking;
            s32 walkingNode = previousNode;
            while(walkingNode != commonNode) {
                outputPtr += writeSpallEndMarker(outputPtr, currentTime);
                walkingNode = parents->array[walkingNode];
            }

            static s32 push[STACK_SIZE] = { 0 };
            s32 pushedCount = 0;
            walkingNode = nodeId;
            while(walkingNode != commonNode) {
                push[pushedCount++] = walkingNode;
                walkingNode = parents->array[walkingNode];
            }

            for(s64 i = pushedCount - 1; i >= 0; i--) {
                SampleNode *n = &nodes[push[i]];
                outputPtr += writeSpallBeginMarker(outputPtr, currentTime, n->funcName, n->path, n->location);
            }

            previousNode = nodeId;
        }

        currentTime += profile->deltas[i];
    }

    return (String) { (u8 *)output, outputPtr - output };
}

static String
convertToGTrace(Arena *arena, String input) {
    CPUProfile cpuprofile = parseCPUProfileJSON(arena, input);
    EvalStack stack = unpackStack(&cpuprofile);

    return writeGTraceOutput(arena, &stack, cpuprofile);
}

static String
convertToSpall(Arena *arena, String input) {
    CPUProfile cpuprofile = parseCPUProfileJSON(arena, input);
    return writeSpallOutput(arena, &cpuprofile);
}

#ifdef EMSCRIPTEN
String result = { 0 };

String *convertStringToGTrace(const char *string, s32 len) {
    Arena arena = arenaCreate(64 * MEGABYTE, 4096, 32);
    String input = { (u8 *)string, len };
    result = convertToGTrace(&arena, input);
    arenaDestroy(&arena);
    return &result;
}
#endif

#ifndef EMSCRIPTEN

bool deleteInputFiles = false;
#include <dirent.h>

static void
convertFile(OutputType outputType, Arena *arena, char *path) {
    u64 elapsed = -readCPUTimer();
    String input = readFileIntoString(arena, path);
    String output;
    switch(outputType) {
        case OutputType_GTrace: {
            output = convertToGTrace(arena, input);
            break;
        }
        case OutputType_Spall: {
            output = convertToSpall(arena, input);
            break;
        }
        default: {
            assert(false && "Unexpected outputType");
        }
    }

    char *outputPath = getOutputPath(outputType, arena, path);
    FILE *f = fopen(outputPath, "wb");
    if(!f) {
        fprintf(stderr, "Failed to open file [%s] for writing\n", outputPath);
        return;
    }

    fwrite(output.data, 1, output.size, f);
    fclose(f);

    elapsed += readCPUTimer();
    u64 elapsedNs = cyclesToNanoSeconds(elapsed, readCPUFrequency());
    String duration = pushStringNanoSeconds(arena, elapsedNs);
    printf("[%8.*s] Converted ➔ %s\n", STRFMT(duration), outputPath);

    if(deleteInputFiles) {
        int failed = remove(path);
        if(failed) {
            printf("Failed to delete input file [%s]\n", path);
        }
    }
}

static void
convertDirectory(OutputType outputType, Arena *arena, char *path) {
    DIR *dir = opendir(path);
    if(dir == NULL) {
        fprintf(stderr, "Failed to open directory = [%s]\n", path);
        exit(EXIT_FAILURE);
    }

    String extension = LIT_TO_STR(".cpuprofile");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        String fileName = { .data = (u8 *)entry->d_name, .size = strlen(entry->d_name) };
        if(stringEndsWith(fileName, extension)) {
            u64 pos = arenaPos(arena);
            convertFile(outputType, arena, entry->d_name);
            arenaPopTo(arena, pos);
        }
    }

    closedir(dir);
}

s32 main(s32 argCount, char **args) {
    OutputType outputType = OutputType_Spall;
    Arena arena = arenaCreate(64 * MEGABYTE, 4096, 32);
    if(argCount < 2) {
        fprintf(stderr, "Usage: xform file.cpuprofile\n");
        return 1;
    }

    for(s32 i = 1; i < argCount; i++) {
        String arg = { .data = (u8 *)args[i], .size = strlen(args[i]) };
        if(stringMatch(arg, LIT_TO_STR("-d"))) {
            deleteInputFiles = true;
        } else {
            stat64_t stat = fileStat(args[i]);
            if(stat.st_mode & S_IFDIR) {
                convertDirectory(outputType, &arena, args[i]);
            } else if(stat.st_mode & S_IFREG) {
                u64 pos = arenaPos(&arena);
                convertFile(outputType, &arena, args[i]);
                arenaPopTo(&arena, pos);
            }
        }
    }

    return 0;
}
#endif
