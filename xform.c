#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>

#include "mrlib.h"

#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

#if defined(__GNUC__) || defined(__clang__)
#define ALWAYS_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE inline
#endif

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

typedef enum WriterType {
    WriterType_File,
} WriterType;

typedef struct FileWriter {
    FileHandle f;
    u8 buffer[128 * KILOBYTE];
    u32 position;
} FileWriter;

typedef struct Writer {
    WriterType type;
    union {
        FileWriter fw;
    } impl;
} Writer;

static Writer
writerFromFile(int f) {
    return (Writer) {
        .type = WriterType_File,
        .impl.fw = { .f = f },
    };
}

ALWAYS_INLINE static u64
fileWriterWriteAll(FileWriter *fw, void *data, u64 size) {
    u64 left = ARRAY_LENGTH(fw->buffer) - fw->position;
    if(left > size) {
        memcpy(&fw->buffer[fw->position], data, size);
        fw->position += size;
    } else {
        if(fw->position > 0) {
            writeFile(fw->f, fw->buffer, fw->position);
            fw->position = 0;
        }

        writeFile(fw->f, data, size);
    }
    return size;
}

ALWAYS_INLINE static u64
writerWriteAll(Writer *w, void *data, u64 size) {
    switch (w->type) {
        case WriterType_File:
            return fileWriterWriteAll(&w->impl.fw, data, size);
        default:
            return -1; /* Unknown implementation */
    }
}

ALWAYS_INLINE static u64
writerWriteString(Writer *w, String string) {
    return writerWriteAll(w, string.data, string.size);
}

static u64
writerWriteU8(Writer *w, u8 v) {
    return writerWriteAll(w, &v, sizeof(v));
}

static u64
writerWriteU32(Writer *w, u32 v) {
    return writerWriteAll(w, &v, sizeof(v));
}

static u64
writerWriteU64(Writer *w, u64 v) {
    return writerWriteAll(w, &v, sizeof(v));
}

static u64
writerWriteF64(Writer *w, f64 v) {
    return writerWriteAll(w, &v, sizeof(v));
}

static s32
writeNumber(Writer *w, s32 value) {
    u8 written = 0;
    u8 buffer[16] = { 0 };

    if (value < 0) {
        buffer[written++] = '-';
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
        buffer[written + i] = (value % 10) + '0';
        value /= 10;
    }
    written += len;

    return writerWriteAll(w, buffer, written);
}

static void
writeGTraceOutput(EvalStack *stack, CPUProfile cpuprofile, Writer *w) {
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

    writerWriteString(w, LIT_TO_STR("{ \"traceEvents\": [\n"));

    bool firstEntry = true;
    for(EmitedEvalStackEntries *node = stack->emitted.head; node; node = node->next) {
        for(u32 i = 0; i < node->count; i++) {
            EvalStackEntry *e = node->entries + i;

            if(firstEntry) {
                writerWriteString(w, LIT_TO_STR("{\"dur\":"));
            } else {
                writerWriteString(w, LIT_TO_STR(",\n{\"dur\":"));
            }
            writeNumber(w, e->duration);
            writerWriteString(w, LIT_TO_STR(",\"name\":\""));
            writerWriteString(w, cpuprofile.sampleNodes[e->sampleNodeId].funcName);
            writerWriteString(w, LIT_TO_STR("\",\"ph\":\"X\",\"tid\":1,\"ts\":"));
            writeNumber(w, e->startTime);
            writerWriteString(w, LIT_TO_STR("}"));
        }
    }

    writerWriteString(w, LIT_TO_STR("\n]}\n"));
}

static void
writeSpallBeginMarker(Writer *w, f64 timestamp, String fnName, String path, String location) {
    writerWriteU8(w, 3);
    writerWriteU8(w, 0);

    writerWriteU32(w, 1);
    writerWriteU32(w, 1);
    writerWriteF64(w, timestamp);

    String blockName;
    if(path.size > 0) {
        static char buffer[255];
        blockName.data = (u8 *)buffer;
        blockName.size = snprintf(buffer, sizeof(buffer), "%.*s: %.*s", STRFMT(fnName), STRFMT(path));
    } else {
        blockName = fnName;
        blockName.size = MIN(255, blockName.size);
    }

    assert(blockName.size <= 255);
    writerWriteU8(w, blockName.size);
    writerWriteU8(w, location.size);

    writerWriteAll(w, blockName.data, blockName.size);
    writerWriteAll(w, location.data, location.size);
}

static void
writeSpallEndMarker(Writer *w, f64 timestamp) {
    writerWriteU8(w, 4);
    writerWriteU32(w, 1);
    writerWriteU32(w, 1);
    writerWriteF64(w, timestamp);
}

static void
writeSpallOutput(CPUProfile *profile, Writer *w) {
    writerWriteU64(w, 0x0BADF00D);
    writerWriteU64(w, 1);
    writerWriteF64(w, 1);
    writerWriteU64(w, 0);

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
                writeSpallBeginMarker(w, currentTime, nodes[nodeId].funcName, emptyString, emptyString);
            }
        } else {
            if(wasGC) {
                wasGC = false;
                writeSpallEndMarker(w, currentTime);
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
                writeSpallEndMarker(w, currentTime);
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
                writeSpallBeginMarker(w, currentTime, n->funcName, n->path, n->location);
            }

            previousNode = nodeId;
        }

        currentTime += profile->deltas[i];
    }
}

static void
convertToGTrace(Arena *arena, String input, Writer *w) {
    CPUProfile cpuprofile = parseCPUProfileJSON(arena, input);
    EvalStack stack = unpackStack(&cpuprofile);

    writeGTraceOutput(&stack, cpuprofile, w);
}

static void
convertToSpall(Arena *arena, String input, Writer *w) {
    CPUProfile cpuprofile = parseCPUProfileJSON(arena, input);
    writeSpallOutput(&cpuprofile, w);
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

    char *outputPath = getOutputPath(outputType, arena, path);
    FileHandle f = openFile(outputPath);
    if(f == -1) {
        fprintf(stderr, "Failed to open file [%s] for writing\n", outputPath);
        return;
    }

    Writer writer = writerFromFile(f);
    String input = readFileIntoString(arena, path);
    switch(outputType) {
        case OutputType_GTrace: {
            convertToGTrace(arena, input, &writer);
            break;
        }
        case OutputType_Spall: {
            convertToSpall(arena, input, &writer);
            break;
        }
        default: {
            assert(false && "Unexpected outputType");
        }
    }

    close(f);

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
        } else if(stringMatch(arg, LIT_TO_STR("-gtrace"))){
            outputType = OutputType_GTrace;
        } else if(stringMatch(arg, LIT_TO_STR("-spall"))){
            outputType = OutputType_Spall;
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
