#ifndef MRLIB_H
#define MRLIB_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <math.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef float f32;
typedef double f64;

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define CLAMP(a,min,max) (MIN(MAX((a), min), max))
#define BETWEEN(x,min,max) ((x) >= (min) && (x) <= (max))
#define ARRAY_LENGTH(a) (sizeof((a))/sizeof((a)[0]))

#define KILOBYTE (1024)
#define MEGABYTE (1024 * KILOBYTE)
#define GIGABYTE (1024 * MEGABYTE)

#define U8_MAX  (0xFF)
#define U8_MIN  (0x00)
#define S8_MAX  (0x7F)
#define S8_MIN  (-0x80)

#define U16_MAX (0xFFFF)
#define U16_MIN (0x0000)
#define S16_MAX (0x7FFF)
#define S16_MIN (-0x8000)

#define U32_MAX (0xFFFFFFFF)
#define U32_MIN (0x00000000)
#define S32_MAX (0x7FFFFFFF)
#define S32_MIN (-0x80000000)

#define U64_MAX (0xFFFFFFFFFFFFFFFF)
#define U64_MIN (0x0000000000000000)
#define S64_MAX (0x7FFFFFFFFFFFFFFF)
#define S64_MIN (-0x8000000000000000)

#define F32_MAX (*(float*)&(uint32_t){0x7F800000})
#define F32_MIN (*(float*)&(uint32_t){0xFF800000})

#define F64_MAX (*(double*)&(uint64_t){0x7FF0000000000000})
#define F64_MIN (*(double*)&(uint64_t){0xFFF0000000000000})

#define SLL_STACK_PUSH_(H,N) N->next=H,H=N
#define SLL_STACK_POP_(H) H=H=H->next
#define SLL_QUEUE_PUSH_MULTIPLE_(F,L,FF,LL) if(LL){if(F){L->next=FF;}else{F=FF;}L=LL;L->next=0;}
#define SLL_QUEUE_PUSH_(F,L,N) SLL_QUEUE_PUSH_MULTIPLE_(F,L,N,N)
#define SLL_QUEUE_POP_(F,L) if (F==L) { F=L=0; } else { F=F->next; }

#define SLL_STACK_PUSH(H,N) (SLL_STACK_PUSH_((H),(N)))
#define SLL_STACK_POP(H) (SLL_STACK_POP_((H)))
#define SLL_QUEUE_PUSH_MULTIPLE(F,L,FF,LL) STMNT( SLL_QUEUE_PUSH_MULTIPLE_((F),(L),(FF),(LL)) )
#define SLL_QUEUE_PUSH(F,L,N) STMNT( SLL_QUEUE_PUSH_((F),(L),(N)) )
#define SLL_QUEUE_POP(F,L) STMNT( SLL_QUEUE_POP_((F),(L)) )

#ifdef _WIN32
#include <windows.h>
#include <sys/stat.h>

typedef HANDLE FileHandle;
typedef struct __stat64 stat64_t;
typedef DWORD ThreadReturnType;
typedef ThreadReturnType (*ThreadFunction)(void *);
typedef HANDLE ThreadHandle;
typedef CRITICAL_SECTION Mutex;

static FileHandle
openFile(const char *filepath) {
    HANDLE f = CreateFileA(filepath, GENERIC_READ | GENERIC_WRITE,
							FILE_SHARE_READ | FILE_SHARE_WRITE,
							0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    return f;
}

static void
readFile(FileHandle handle, u8 *out, u64 length) {
	ReadFile(handle, out, length, NULL, NULL);
}

static void
writeFile(FileHandle handle, u8 *data, u64 length) {
    DWORD bytesWritten = 0;

    WriteFile(handle, data, length, &bytesWritten, NULL);
    assert(bytesWritten == length);
}

static void
closeFile(FileHandle handle) {
	CloseHandle(handle);
}

static stat64_t
fileStat(const char *path) {
	struct __stat64 out;
    _stat64(path, &out);
    return out;
}

static ThreadHandle
createThread(ThreadFunction fn, void *user_pointer) {
    return CreateThread(0x0, 0, fn, user_pointer, 0, 0x0);
}

static u64
getCurrentThreadId() {
    return GetCurrentThreadId();
}

static void
joinAndDestroyThread(ThreadHandle handle) {
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}

static Mutex
createMutex() {
    Mutex mutex;
    InitializeCriticalSection(&mutex);
    return mutex;
}

static void
destroyMutex(Mutex *mutex) {
    DeleteCriticalSection(mutex);
}

static void
lockMutex(Mutex *mutex) {
    EnterCriticalSection(mutex);
}

static void
unlockMutex(Mutex *mutex) {
    LeaveCriticalSection(mutex);
}

static u32
getProcessorCount() {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    return system_info.dwNumberOfProcessors;
}

#elif __linux__
#include <sys/stat.h>
#include <pthread.h>
typedef void * ThreadReturnType;
typedef ThreadReturnType (*ThreadFunction)(void *);
typedef pthread_t ThreadHandle;
typedef pthread_mutex_t Mutex;

typedef struct stat stat64_t;

static stat64_t
fileStat(const char *path) {
    struct stat out;
    stat(path, &out);
    return out;
}

static ThreadHandle
createThread(ThreadFunction fn, void *user_pointer) {
    ThreadHandle handle;
    pthread_create(&handle, NULL, fn, user_pointer);
    return handle;
}

static void
joinAndDestroyThread(ThreadHandle handle) {
    pthread_join(handle, NULL);
}

static Mutex
createMutex() {
    Mutex mutex;
    pthread_mutex_init(&mutex, NULL);
    return mutex;
}

static void
destroyMutex(Mutex *mutex) {
    pthread_mutex_destroy(mutex);
}

static void
lockMutex(Mutex *mutex) {
    pthread_mutex_lock(mutex);
}

static void
unlockMutex(Mutex *mutex) {
    pthread_mutex_unlock(mutex);
}

static u64
getCurrentThreadId() {
    return (u64)pthread_self();
}

#ifdef __linux__
#include <sys/sysinfo.h>
static u32
getProcessorCount() {
    return get_nprocs();
}
#elif __APPLE__
#include <sys/sysctl.h>
static u32
getProcessorCount() {
    int mib[] = { CTL_HW, HW_NCPU };

    int numCPU;
    size_t len = sizeof(numCPU);
    sysctl(mib, 2, &numCPU, &len, NULL, 0);

    return numCPU;
}
#endif

#elif __APPLE__
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

typedef int FileHandle;
typedef struct stat stat64_t;

static stat64_t
fileStat(const char *path) {
    struct stat out;
    stat(path, &out);
    return out;
}

static FileHandle
openFile(const char *filepath) {
    return open(filepath, O_RDWR | O_CREAT, 0644);
}

static void
readFile(FileHandle handle, u8 *out, u64 length) {
    read(handle, out, length);
}

static void
writeFile(FileHandle handle, u8 *data, u64 length) {
    write(handle, data, length);
}

static void
closeFile(FileHandle handle) {
    close(handle);
}

#endif

static u64
byteSwap64(u64 x) {
    return ((x >> 56) & 0xFF) |
           ((x >> 40) & 0xFF) << 8  |
           ((x >> 24) & 0xFF) << 16 |
           ((x >> 8)  & 0xFF) << 24 |
           ((x      ) & 0xFF) << 32 |
           ((x << 8)  & 0xFF) << 40 |
           ((x << 16) & 0xFF) << 48 |
           ((x << 24) & 0xFF) << 56;
}

static u32
byteSwap32(u32 x) {
    return ((x >> 24) & 0xFF) |
           ((x >> 16) & 0xFF) << 8  |
           ((x >> 8)  & 0xFF) << 16 |
           ((x      ) & 0xFF) << 24;
}

static u16
byteSwap16(u16 x) {
    return ((x >> 8) & 0xFF) | ((x & 0xFF) << 8);
}

static u64
readCPUTimer()
{
#if defined(__x86_64__) || defined(_M_AMD64)
    return __rdtsc();
#else
    u64 tsc = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
    return tsc;
#endif
}

static u64
readCPUFrequency()
{
#if defined(__x86_64__) || defined(_M_AMD64)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid(0x15, eax, ebx, ecx, edx);
    if (eax != 0 && ebx != 0) {
        // freq in Hz = (EBX / EAX) * 1000
        return ((unsigned long long)ebx / (unsigned long long) eax) * 1000ULL;
    } else {
        // Fallback: CPUID(0x16) often gives base frequency in MHz
        __cpuid(0x16, eax, ebx, ecx, edx);
        if (eax) {
            return (unsigned long long)eax * 1000000ULL;
        }
    }
    return 0;
#else
    u64 tsc = 0;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(tsc));
    return tsc;
#endif
}

static u64
cyclesToNanoSeconds(u64 cycles, u64 frequency) {
    return (cycles * 1e9) / frequency;
}

typedef struct MemoryCursor {
    u8* basePointer;
    u8* cursorPointer;
    u8 *commitedTo;
    size_t size;
} MemoryCursor;

typedef struct MemoryCursorNode {
    struct MemoryCursorNode *next;
    MemoryCursor cursor;
} MemoryCursorNode;

typedef struct Arena {
    MemoryCursorNode *cursorNode;
    size_t chunkSize;
    size_t alignment;
} Arena;

static void
cursorCommitChunk(MemoryCursor *cursor, u8 *pointer, u64 size) {
    if(pointer + size > cursor->commitedTo) {
        u64 trailingSize = cursor->size - (size_t)(pointer - cursor->basePointer);
        size = MIN(MAX(256 * MEGABYTE, size), trailingSize);
#if _WIN32
        VirtualAlloc(pointer, size, MEM_COMMIT, PAGE_READWRITE);
#else
        u64 pageSize = 16 * KILOBYTE;
        void *alignedPointer = (void *)((size_t)pointer & ~(pageSize - 1));
        size_t additionalSize = (size_t)pointer - (size_t)alignedPointer;
        mprotect(alignedPointer, additionalSize + size, PROT_READ | PROT_WRITE);
#endif
        cursor->commitedTo = pointer + size;
    }
}

static u64
roundUp(u64 v, u64 multiple) {
    return ((v + multiple - 1) / multiple) * multiple;
}

static MemoryCursorNode *
arenaAddNode(Arena *arena, size_t size) {
    MemoryCursorNode *result = 0x0;

    assert(arena);
    size += sizeof(MemoryCursorNode);
    size = roundUp(size, arena->chunkSize);
    
#ifdef _WIN32
    void *memory = (u8 *)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
    VirtualAlloc(memory, sizeof(MemoryCursorNode), MEM_COMMIT, PAGE_READWRITE);
#else
    void *memory = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(memory, sizeof(MemoryCursorNode), PROT_READ | PROT_WRITE);
#endif

    assert(memory);
    
    result = (MemoryCursorNode *)memory;
    
    result->cursor.basePointer = (u8 *)memory + sizeof(MemoryCursorNode);
    result->cursor.cursorPointer = result->cursor.basePointer;
    result->cursor.commitedTo = result->cursor.basePointer;
    result->cursor.size = size - sizeof(MemoryCursorNode);
    SLL_STACK_PUSH(arena->cursorNode, result);
    
    return result;
}

static void
cursorClear(MemoryCursor *cursor, u8 clearTo) {
    memset(cursor->basePointer, clearTo, cursor->size);
    cursor->cursorPointer = cursor->basePointer;
}

static void
cursorDestroy(MemoryCursor *cursor) {
    if(cursor && cursor->basePointer) {
        void *pointer = (u8 *)cursor->basePointer - sizeof(MemoryCursorNode);

#ifdef _WIN32
        VirtualFree(pointer, 0, MEM_RELEASE);
#else
        munmap(pointer, cursor->size + sizeof(MemoryCursorNode));
#endif
    }
}

static size_t
cursorFreeBytes(MemoryCursor *cursor) {
    size_t result = cursor->size - (size_t)(cursor->cursorPointer - cursor->basePointer);
    return result;
}

static size_t
cursorTakenBytes(MemoryCursor *cursor) {
    size_t result = (size_t)(cursor->cursorPointer - cursor->basePointer);
    return result;
}

static void
arenaDestroy(Arena *arena) {
    if(arena) {
        MemoryCursorNode *toDestroy = 0x0;
        for(MemoryCursorNode *cursorNode = arena->cursorNode;
            cursorNode != 0x0;
            cursorNode = cursorNode->next) {
            if(toDestroy) {
                cursorDestroy(&toDestroy->cursor);
            }
            
            toDestroy = cursorNode;
        }
        
        if(toDestroy) {
            cursorDestroy(&toDestroy->cursor);
        }
        
        arena->cursorNode = 0x0;
    }
}

static Arena
arenaCreate(size_t size, size_t chunkSize, size_t alignment) {
    Arena result = { 0 };

    result.chunkSize = chunkSize;
    result.alignment = alignment;

    if(size > 0) {
    	arenaAddNode(&result, size);
    }

    return result;
}

static u64
arenaPos(Arena *arena) {
    u64 result = 0;

    if(arena) {
        MemoryCursorNode *toCount = 0x0;
        for(MemoryCursorNode *cursorNode = arena->cursorNode;
            cursorNode != 0x0;
            cursorNode = cursorNode->next) {
            if(toCount) {
                result += cursorTakenBytes(&toCount->cursor);
            }
            
            toCount = cursorNode;
        }
        
        if(toCount) {
            result += cursorTakenBytes(&toCount->cursor);
        }
    }

    return result;
}

static void *
arenaPush(Arena *arena, size_t size) {
    void *result = 0x0;
    
    if(size) {
        assert(arena);

        MemoryCursorNode *cursorNode = arena->cursorNode;
        if(!cursorNode) {
            cursorNode = arenaAddNode(arena, size);
        }
        
        MemoryCursor *cursor = &cursorNode->cursor;
        size_t bytesLeft = cursorFreeBytes(cursor);
        // Calculates how many bytes we need to add to be aligned on the 16 bytes.
        size_t alignmentMask = arena->alignment - 1; 
        size_t paddingNeeded = (arena->alignment - ((size_t)cursor->cursorPointer & alignmentMask)) & alignmentMask;
        
        if(size + paddingNeeded > bytesLeft) {
            cursorNode = arenaAddNode(arena, size + paddingNeeded);
            cursor = &cursorNode->cursor;

            // Since cursorPointer is new, we need to recalculate it
            paddingNeeded = (arena->alignment - ((size_t)cursor->cursorPointer & alignmentMask)) & alignmentMask;
        }

        cursorCommitChunk(cursor, cursor->cursorPointer, paddingNeeded + size);
        cursor->cursorPointer += paddingNeeded;
        result = cursor->cursorPointer;
        cursor->cursorPointer += size;
    }
    
    return result;
}

static void *
arenaPushZero(Arena *arena, size_t size) {
    void *memory = arenaPush(arena, size);
    memset(memory, 0, size);
    return memory;
}

static void
arenaPop(Arena *arena, size_t size) {
    assert(arena);

    if(size) {
        MemoryCursorNode *cursorNode = arena->cursorNode;
        if(!cursorNode) {
            return;
        }

        while(size > 0) {
            MemoryCursor *cursor = &cursorNode->cursor;

            u64 takenBytes = cursorTakenBytes(cursor);
            u64 toSubtract = MIN(size, takenBytes);
            u64 newOffset = takenBytes - toSubtract;
            size -= toSubtract;
            cursor->cursorPointer = cursor->basePointer + newOffset;
            cursorNode = cursorNode->next;
        }
    }
}

static void
arenaPopTo(Arena *arena, size_t pos) {
    size_t currentPos = arenaPos(arena);
    if(currentPos > pos) {
        arenaPop(arena, currentPos - pos);
    }
}

static void
arenaClear(Arena *arena) {
    if(arena) {
        MemoryCursorNode *toClear = 0x0;
        for(MemoryCursorNode *cursorNode = arena->cursorNode;
            cursorNode != 0x0;
            cursorNode = cursorNode->next) {
            if(toClear) {
                cursorClear(&toClear->cursor, 0);
            }
            
            toClear = cursorNode;
        }
        
        if(toClear) {
            cursorClear(&toClear->cursor, 0);
        }
        
        arena->cursorNode = 0x0;
    }
}

#define arrayPush(a,T,c) ((T *)arenaPush((a), sizeof(T)*(c)))
#define arrayPushZero(a,T,c) ((T *)arenaPushZero((a), sizeof(T)*(c)))
#define structPush(a, T) ((T *)arenaPush((a), sizeof(T)))
#define bytesPush(a, c) (arenaPush((a), (c)))

#define STRFMT(s) (int)(s).size, (s).data
#define LIT_TO_STR(a) ((String){ .data = (u8 *)a, .size = (sizeof(a) - 1) })

typedef struct String
{
    u8 *data;
    u64 size;     
} String;

typedef struct SplitIterator
{
    char delim;
    u32 strLength;
    char *string;
    char *head;
} SplitIterator;

static String
stringPush(Arena *arena, u64 size) {
    String string = { 0 };
    string.data = arrayPush(arena, u8, size);
    string.size = size;
    return string;
}

static String
pushStringfv(Arena *arena, const char *format, va_list args) {
    va_list args2;
    va_copy(args2, args);
    u64 size = vsnprintf(0, 0, format, args);
    String result = stringPush(arena, size + 1);
    vsnprintf((char*)result.data, (size_t)result.size, format, args2);
    result.size -= 1;
    result.data[result.size] = 0;
    return result;
}

static String
pushStringf(Arena *arena, const char *format, ...) {
    va_list args;
    va_start(args, format);
    String result = pushStringfv(arena, format, args);
    va_end(args);
    return result;
}

static String
pushStringSI(Arena *arena, u64 bytes) {
    if(bytes == 0) {
        return pushStringf(arena, "0 ");
    }

    static const char *orderPrefix[] = {
        " ",
        " K",
        " M",
        " G",
        " T",
        " P",
    };

    u64 order = (u64)(log2(bytes) / log2(1000));
    double newValue = (double)bytes / pow(1000, order);

    return pushStringf(arena, "%0.2f%s", newValue, orderPrefix[order]);
}

static String
pushStringNanoSeconds(Arena *arena, u64 ns) {
    if(ns == 0) {
        return pushStringf(arena, "0ns");
    }

    static const char *orderPrefix[] = {
        "ns",
        "us",
        "ms",
        "s",
    };

    u64 order = (u64)(log2(ns) / log2(1000));
    double newValue = (double)ns / pow(1000, order);

    return pushStringf(arena, "%0.2f%s", newValue, orderPrefix[order]);
}

static s32
stringCompare(String a, String b){
    s32 result = 0;
    if(a.size != b.size) {
        return a.size < b.size ? -1 : 1;
    }

    for (u64 i = 0; i < a.size || i < b.size; i += 1){
        u8 ca = (i < a.size)?a.data[i]:0;
        u8 cb = (i < b.size)?b.data[i]:0;
        s32 dif = ((ca) - (cb));
        if (dif != 0){
            result = (dif > 0)?1:-1;
            break;
        }
    }

    return result;
}

static SplitIterator
stringSplit(String string, char delim) {
    SplitIterator it = { 0 };
    
    it.string = (char *)string.data;
    it.strLength = string.size;
    it.head = (char *)string.data;
    it.delim = delim;
    
    return it;
}

static bool
stringMatch(String a, String b) {
    if(a.size == b.size) {
        for(u32 k = 0; k < a.size; k++) {
            if(a.data[k] != b.data[k]) {
                return false;
            }
        }
        return true;
    } else {
        return false;
    }
}

static bool
stringStartsWith(String a, String b) {
    assert(b.size > 0);
    if(b.size > a.size) {
        return false;
    }

    for(u32 i = 0; i < b.size; i++) {
        if(a.data[i] != b.data[i]) {
            return false;
        }
    }

    return true;
}

static bool
stringEndsWith(String a, String b) {
    assert(b.size > 0);
    assert(a.size > 0);
    if(b.size > a.size) {
        return false;
    }

    u64 offset = a.size - b.size;
    for(u32 i = 0; i < b.size; i++) {
        if(a.data[offset + i] != b.data[i]) {
            return false;
        }
    }

    return true;
}


static String
stringNextInSplit(SplitIterator *it) {
    String result = { 0 };
    
    u64 readLength = (u64)(it->head - it->string);
    
    if(readLength < it->strLength) {
        char *head = it->head;
        result.data = (u8 *)head;
        u64 toRead = it->strLength - readLength;
        for(u64 i = 0; (i < toRead) && (head[0] != it->delim); i++) {
            head++;
            result.size += 1;
        }
        
        if(head[0] == it->delim) {
            head++;
        }
        it->head = head;
    } else {
        result.data = NULL;
    }
    
    return result;
}

static String
stringConsumeIteratorIntoString(SplitIterator it)
{
    String result = { 0 };

    u64 readLength = (u64)(it.head - it.string);
    u64 toRead = it.strLength - readLength;

    result.data = (u8 *)it.head;
    result.size = toRead;
    
    return result;
}

static String
subStringUntilDelimiter(String string, u32 startOffset, u32 endOffset, char delimiter)
{
    String result = { 0 };
    result.data = string.data + startOffset;
    result.size = endOffset - startOffset;

    while(((result.size-1) + startOffset) < string.size && result.data[result.size-1] != '\n') {
        result.size += 1;
    }

    return result;
}

static String
readFileIntoString(Arena *arena, const char *filepath)
{
    String result = { 0 };
    FileHandle file = openFile(filepath);
    stat64_t stat = fileStat(filepath);
    result.size = stat.st_size;
    result.data = arrayPush(arena, u8, result.size);
    readFile(file, result.data, result.size);
    closeFile(file);
    return result;
}

static bool
isWhitespace(char c) {
    return c == 0x20 || (c >= 0x09 && c <= 0x0d);
}

static String
stringTrim(String str) {
    while(str.size > 0 && isWhitespace(str.data[0])) {
        str.data++;
        str.size--;
    }

    while(str.size > 0 && isWhitespace(str.data[str.size - 1])) {
        str.size--;
    }

    return str;
}

static u32
xorshift32(u32 value) {
    u32 x = value;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return x;
}

static u64
xorshift64(u64 value) {
	uint64_t x = value;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return x;
}

static u64
randomInRange(u64 atLeast, u64 to) {
    static u64 state = 0;
    if(state == 0) {
        state = readCPUTimer();
    }

    u64 range = to - atLeast;

    state = xorshift64(state);
    u64 x = state;
    __uint128_t m = (__uint128_t)(x) * (__uint128_t)(range);
    u64 l = (u64)m;

    if(l < range) {
        u64 t = -range;
        if(t >= range) {
            t -= range;
            if(t >= range) {
                 t %= range;
            }
        }
        while(l < t) {
            state = xorshift64(state);
            x = state;
            m = x * range;
            l = (u64)m;

        }
    }

    return atLeast + (u64)(m >> 64);
}

#endif
