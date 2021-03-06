/* Copyright (C) 2009-2016 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied, modified
   or distributed except as expressly authorized under the terms of that
   license. Refer to licensing information at http://www.artifex.com
   or contact Artifex Software, Inc.,  7 Mt. Lassen Drive - Suite A-134,
   San Rafael, CA  94903, U.S.A., +1(415)492-9861, for further information.
*/

/* Inspired by Fortify by Simon P Bullen. */

/* Set the following if you're only looking for leaks, not memory overwrites
 * to speed the operation */
/* #define MEMENTO_LEAKONLY */

/* Set the following to keep extra details about the history of blocks */
#define MEMENTO_DETAILS

/* Don't keep blocks around if they'd mean losing more than a quarter of
 * the freelist. */
#define MEMENTO_FREELIST_MAX_SINGLE_BLOCK (MEMENTO_FREELIST_MAX/4)

#define COMPILING_MEMENTO_C

/* We have some GS specific tweaks; more for the GS build environment than
 * anything else. */
#define MEMENTO_GS_HACKS

#ifdef MEMENTO_GS_HACKS
/* For GS we include malloc_.h. Anyone else would just include memento.h */
#include "malloc_.h"
#ifdef __MACH__
#include <string.h>
#else
#ifndef memset
void *memset(void *,int,size_t);
#endif
#endif
int atexit(void (*)(void));
#else
#include "memento.h"
#include <stdio.h>
#include <stdlib.h>
#endif
#ifndef _MSC_VER
#include <stdint.h>
#include <limits.h>
#endif

#ifdef MEMENTO

#ifndef MEMENTO_CPP_EXTRAS_ONLY

#ifdef MEMENTO_ANDROID
#include <android/log.h>

static int
android_fprintf(FILE *file, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR,"memento", fmt, args);
    va_end(args);
}

#define fprintf android_fprintf
#define MEMENTO_STACKTRACE_METHOD 0
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>

static int
windows_fprintf(FILE *file, const char *fmt, ...)
{
    va_list args;
    char text[4096];
    int ret;

    va_start(args, fmt);
    ret = vfprintf(file, fmt, args);
    va_end(args);

    va_start(args, fmt);
    vsnprintf(text, 4096, fmt, args);
    OutputDebugStringA(text);
    va_end(args);

    return ret;
}

#define fprintf windows_fprintf
#endif

#ifndef MEMENTO_STACKTRACE_METHOD
#ifdef __GNUC__
#define MEMENTO_STACKTRACE_METHOD 1
#endif
#ifdef _WIN32
#define MEMENTO_STACKTRACE_METHOD 2
#endif
#endif

#if defined(__linux__)
#define MEMENTO_HAS_FORK
#elif defined(__APPLE__) && defined(__MACH__)
#define MEMENTO_HAS_FORK
#endif

/* Define the underlying allocators, just in case */
void *MEMENTO_UNDERLYING_MALLOC(size_t);
void MEMENTO_UNDERLYING_FREE(void *);
void *MEMENTO_UNDERLYING_REALLOC(void *,size_t);
void *MEMENTO_UNDERLYING_CALLOC(size_t,size_t);

/* And some other standard functions we use. We don't include the header
 * files, just in case they pull in unexpected others. */
int atoi(const char *);
char *getenv(const char *);

/* How far to search for pointers in each block when calculating nestings */
/* mupdf needs at least 34000ish (sizeof(fz_shade))/ */
#define MEMENTO_PTRSEARCH 65536

#ifndef MEMENTO_MAXPATTERN
#define MEMENTO_MAXPATTERN 0
#endif

#ifdef MEMENTO_GS_HACKS
#include "valgrind.h"
#else
#ifdef HAVE_VALGRIND
#include "valgrind/memcheck.h"
#else
#define VALGRIND_MAKE_MEM_NOACCESS(p,s)  do { } while (0==1)
#define VALGRIND_MAKE_MEM_UNDEFINED(p,s)  do { } while (0==1)
#define VALGRIND_MAKE_MEM_DEFINED(p,s)  do { } while (0==1)
#endif
#endif

enum {
    Memento_PreSize  = 16,
    Memento_PostSize = 16
};

/* Some compile time checks */
typedef struct
{
    char MEMENTO_PRESIZE_MUST_BE_A_MULTIPLE_OF_4[Memento_PreSize & 3 ? -1 : 1];
    char MEMENTO_POSTSIZE_MUST_BE_A_MULTIPLE_OF_4[Memento_PostSize & 3 ? -1 : 1];
    char MEMENTO_POSTSIZE_MUST_BE_AT_LEAST_4[Memento_PostSize >= 4 ? 1 : -1];
    char MEMENTO_PRESIZE_MUST_BE_AT_LEAST_4[Memento_PreSize >= 4 ? 1 : -1];
} MEMENTO_SANITY_CHECK_STRUCT;

#define MEMENTO_UINT32 unsigned int
#define MEMENTO_UINT16 unsigned short

#define MEMENTO_PREFILL_UINT32  ((MEMENTO_UINT32)(MEMENTO_PREFILL  | (MEMENTO_PREFILL <<8) | (MEMENTO_PREFILL <<16) |(MEMENTO_PREFILL <<24)))
#define MEMENTO_POSTFILL_UINT16 ((MEMENTO_UINT16)(MEMENTO_POSTFILL | (MEMENTO_POSTFILL<<8)))
#define MEMENTO_POSTFILL_UINT32 ((MEMENTO_UINT32)(MEMENTO_POSTFILL | (MEMENTO_POSTFILL<<8) | (MEMENTO_POSTFILL<<16) |(MEMENTO_POSTFILL<<24)))
#define MEMENTO_FREEFILL_UINT16 ((MEMENTO_UINT16)(MEMENTO_FREEFILL | (MEMENTO_FREEFILL<<8)))
#define MEMENTO_FREEFILL_UINT32 ((MEMENTO_UINT32)(MEMENTO_FREEFILL | (MEMENTO_FREEFILL<<8) | (MEMENTO_FREEFILL<<16) |(MEMENTO_FREEFILL<<24)))

enum {
    Memento_Flag_OldBlock = 1,
    Memento_Flag_HasParent = 2,
    Memento_Flag_BreakOnFree = 4,
    Memento_Flag_BreakOnRealloc = 8,
    Memento_Flag_Freed = 16
};

enum {
    Memento_EventType_malloc = 0,
    Memento_EventType_calloc = 1,
    Memento_EventType_realloc = 2,
    Memento_EventType_free = 3,
    Memento_EventType_new = 4,
    Memento_EventType_delete = 5,
    Memento_EventType_newArray = 6,
    Memento_EventType_deleteArray = 7
};

static const char *eventType[] =
{
    "malloc",
    "calloc",
    "realloc",
    "free",
    "new",
    "delete",
    "new[]",
    "delete[]"
};

/* When we list leaked blocks at the end of execution, we search for pointers
 * between blocks in order to be able to give a nice nested view.
 * Unfortunately, if you have are running your own allocator (such as
 * ghostscripts chunk allocator) you can often find that the header of the
 * block always contains pointers to next or previous blocks. This tends to
 * mean the nesting displayed is "uninteresting" at best :)
 *
 * As a hack to get around this, we have a define MEMENTO_SKIP_SEARCH that
 * indicates how many bytes to skip over at the start of the chunk.
 * This may cause us to miss true nestings, but such is life...
 */
#ifndef MEMENTO_SEARCH_SKIP
#ifdef MEMENTO_GS_HACKS
#define MEMENTO_SEARCH_SKIP (2*sizeof(void *))
#else
#define MEMENTO_SEARCH_SKIP 0
#endif
#endif

#define MEMENTO_CHILD_MAGIC   ((Memento_BlkHeader *)('M' | ('3' << 8) | ('m' << 16) | ('3' << 24)))
#define MEMENTO_SIBLING_MAGIC ((Memento_BlkHeader *)('n' | ('t' << 8) | ('0' << 16) | ('!' << 24)))

#ifdef MEMENTO_DETAILS
typedef struct Memento_BlkDetails Memento_BlkDetails;

struct Memento_BlkDetails
{
    Memento_BlkDetails *next;
    char                type;
    char                count;
    int                 sequence;
    void               *stack[1];
};
#endif /* MEMENTO_DETAILS */

typedef struct Memento_BlkHeader Memento_BlkHeader;

struct Memento_BlkHeader
{
    size_t               rawsize;
    int                  sequence;
    int                  lastCheckedOK;
    int                  flags;
    Memento_BlkHeader   *next;
    Memento_BlkHeader   *prev; /* Reused as 'parent' when printing nested list */

    const char          *label;

    /* Entries for nesting display calculations. Set to magic
     * values at all other time.  */
    Memento_BlkHeader   *child;
    Memento_BlkHeader   *sibling;

#ifdef MEMENTO_DETAILS
    Memento_BlkDetails  *details;
    Memento_BlkDetails **details_tail;
#endif

    char                 preblk[Memento_PreSize];
};

/* In future this could (should) be a smarter data structure, like, say,
 * splay trees. For now, we use a list.
 */
typedef struct Memento_Blocks
{
    Memento_BlkHeader *head;
    Memento_BlkHeader *tail;
} Memento_Blocks;

/* And our global structure */
static struct {
    int            inited;
    Memento_Blocks used;
    Memento_Blocks free;
    size_t         freeListSize;
    int            sequence;
    int            paranoia;
    int            paranoidAt;
    int            countdown;
    int            lastChecked;
    int            breakAt;
    int            failAt;
    int            failing;
    int            nextFailAt;
    int            squeezeAt;
    int            squeezing;
    int            segv;
    int            pattern;
    int            nextPattern;
    int            patternBit;
    size_t         maxMemory;
    size_t         alloc;
    size_t         peakAlloc;
    size_t         totalAlloc;
    size_t         numMallocs;
    size_t         numFrees;
    size_t         numReallocs;
} globals;

#define MEMENTO_EXTRASIZE (sizeof(Memento_BlkHeader) + Memento_PostSize)

/* Round up size S to the next multiple of N (where N is a power of 2) */
#define MEMENTO_ROUNDUP(S,N) ((S + N-1)&~(N-1))

#define MEMBLK_SIZE(s) MEMENTO_ROUNDUP(s + MEMENTO_EXTRASIZE, MEMENTO_MAXALIGN)

#define MEMBLK_FROMBLK(B)   (&((Memento_BlkHeader*)(void *)(B))[-1])
#define MEMBLK_TOBLK(B)     ((void*)(&((Memento_BlkHeader*)(void*)(B))[1]))
#define MEMBLK_POSTPTR(B) \
          (&((unsigned char *)(void *)(B))[(B)->rawsize + sizeof(Memento_BlkHeader)])

enum
{
    SkipStackBackTraceLevels = 4
};

#if defined(MEMENTO_STACKTRACE_METHOD) && MEMENTO_STACKTRACE_METHOD == 1
extern size_t backtrace(void **, int);
extern void backtrace_symbols_fd(void **, size_t, int);
extern char **backtrace_symbols(void **, size_t);

#define MEMENTO_BACKTRACE_MAX 256

/* Libbacktrace gubbins */
#ifdef HAVE_LIBDL
#include <dlfcn.h>
#endif

typedef void (*backtrace_error_callback) (void *data, const char *msg, int errnum);

typedef struct backtrace_state *(*backtrace_create_state_type)(
    const char *filename, int threaded,
    backtrace_error_callback error_callback, void *data);

typedef int (*backtrace_full_callback) (void *data, uintptr_t pc,
                                        const char *filename, int lineno,
                                        const char *function);

typedef int (*backtrace_pcinfo_type)(struct backtrace_state *state,
                                     uintptr_t pc,
                                     backtrace_full_callback callback,
                                     backtrace_error_callback error_callback,
                                     void *data);

typedef void (*backtrace_syminfo_callback) (void *data, uintptr_t pc,
                                            const char *symname,
                                            uintptr_t symval,
                                            uintptr_t symsize);

typedef int (*backtrace_syminfo_type)(struct backtrace_state *state,
                                      uintptr_t addr,
                                      backtrace_syminfo_callback callback,
                                      backtrace_error_callback error_callback,
                                      void *data);

static backtrace_syminfo_type backtrace_syminfo;
static backtrace_create_state_type backtrace_create_state;
static backtrace_pcinfo_type backtrace_pcinfo;
static struct backtrace_state *my_backtrace_state;
static void *libbt;
static void (*print_stack_value)(void *address);
static char backtrace_exe[4096];
static void *current_addr;

#ifdef HAVE_LIBDL
static void error2_cb(void *data, const char *msg, int errnum)
{
}

static void syminfo_cb(void *data, uintptr_t pc, const char *symname, uintptr_t symval, uintptr_t symsize)
{
    if (sizeof(void *) == 4)
        fprintf(stderr, "    0x%08lx %s\n", pc, symname?symname:"?");
    else
        fprintf(stderr, "    0x%016lx %s\n", pc, symname?symname:"?");
}

static void error_cb(void *data, const char *msg, int errnum)
{
    backtrace_syminfo(my_backtrace_state,
                     (uintptr_t)current_addr,
                     syminfo_cb,
                     error2_cb,
                     NULL);
}

static int full_cb(void *data, uintptr_t pc, const char *fname, int line, const char *fn)
{
    if (sizeof(void *) == 4)
        fprintf(stderr, "    0x%08lx %s(%s:%d)\n", pc, fn?fn:"?", fname?fname:"?", line);
    else
        fprintf(stderr, "    0x%016lx %s(%s:%d)\n", pc, fn?fn:"?", fname?fname:"?", line);
    return 0;
}

static void print_stack_libbt(void *addr)
{
    current_addr = addr;
    backtrace_pcinfo(my_backtrace_state,
                     (uintptr_t)addr,
                     full_cb,
                     error_cb,
                     NULL);
}

static void print_stack_libbt_failed(void *addr)
{
    char **strings = backtrace_symbols(&addr, 1);

    if (strings == NULL || strings[0] == NULL)
    {
        if (sizeof(void *) == 4)
            fprintf(stderr, "    [0x%08lx]\n", (uintptr_t)addr);
        else
            fprintf(stderr, "    [0x%016lx]\n", (uintptr_t)addr);
    }
    else
    {
        fprintf(stderr, "    %s\n", strings[0]);
    }
    (free)(strings);
}

static int init_libbt(void)
{
    libbt = dlopen("libbacktrace.so", RTLD_LAZY);
    if (libbt == NULL)
        libbt = dlopen("/opt/lib/libbacktrace.so", RTLD_LAZY);
    if (libbt == NULL)
        libbt = dlopen("/lib/libbacktrace.so", RTLD_LAZY);
    if (libbt == NULL)
        libbt = dlopen("/usr/lib/libbacktrace.so", RTLD_LAZY);
    if (libbt == NULL)
        libbt = dlopen("/usr/local/lib/libbacktrace.so", RTLD_LAZY);
    if (libbt == NULL)
        goto fail;

    backtrace_create_state = dlsym(libbt, "backtrace_create_state");
    backtrace_syminfo      = dlsym(libbt, "backtrace_syminfo");
    backtrace_pcinfo       = dlsym(libbt, "backtrace_pcinfo");

    if (backtrace_create_state == NULL ||
        backtrace_syminfo == NULL ||
        backtrace_pcinfo == NULL)
    {
        goto fail;
    }

    my_backtrace_state = backtrace_create_state(backtrace_exe,
                                                1 /*BACKTRACE_SUPPORTS_THREADS*/,
                                                error_cb,
                                                NULL);
    if (my_backtrace_state == NULL)
        goto fail;

    print_stack_value = print_stack_libbt;

    return 1;

 fail:
    libbt = NULL;
    backtrace_create_state = NULL;
    backtrace_syminfo = NULL;
    print_stack_value = print_stack_libbt_failed;
    return 0;
}
#endif

static void print_stack_default(void *addr)
{
    char **strings = backtrace_symbols(&addr, 1);

    if (strings == NULL || strings[0] == NULL)
    {
        fprintf(stderr, "    [0x%p]\n", addr);
    }
#ifdef HAVE_LIBDL
    else if (strchr(strings[0], ':') == NULL)
    {
        /* Probably a "path [address]" format string */
        char *s = strchr(strings[0], ' ');

	if (s != strings[0])
	{
            memcpy(backtrace_exe, strings[0], s - strings[0]);
            backtrace_exe[s-strings[0]] = 0;
	    if (init_libbt())
                print_stack_value(addr);
	}
    }
#endif
    else
    {
        fprintf(stderr, "    %s\n", strings[0]);
    }
    free(strings);
}

static void Memento_initStacktracer(void)
{
    print_stack_value = print_stack_default;
}

static int Memento_getStacktrace(void **stack, int *skip)
{
    size_t num;

    num = backtrace(&stack[0], MEMENTO_BACKTRACE_MAX);

    *skip = SkipStackBackTraceLevels;
    if (num <= SkipStackBackTraceLevels)
        return 0;
    return (int)(num-SkipStackBackTraceLevels);
}

static void Memento_showStacktrace(void **stack, int numberOfFrames)
{
    int i;

    for (i = 0; i < numberOfFrames; i++)
    {
        print_stack_value(stack[i]);
    }
}
#elif defined(MEMENTO_STACKTRACE_METHOD) && MEMENTO_STACKTRACE_METHOD == 2
#include <Windows.h>

/* We use DbgHelp.dll rather than DbgHelp.lib. This avoids us needing
 * extra link time complications, and enables us to fall back gracefully
 * if the DLL cannot be found.
 *
 * To achieve this we have our own potted versions of the required types
 * inline here.
 */
#ifdef _WIN64
typedef DWORD64 DWORD_NATIVESIZED;
#else
typedef DWORD DWORD_NATIVESIZED;
#endif

#define MEMENTO_BACKTRACE_MAX 64

typedef USHORT (__stdcall *My_CaptureStackBackTraceType)(__in ULONG, __in ULONG, __out PVOID*, __out_opt PULONG);

typedef struct MY_IMAGEHLP_LINE {
    DWORD    SizeOfStruct;
    PVOID    Key;
    DWORD    LineNumber;
    PCHAR    FileName;
    DWORD_NATIVESIZED    Address;
} MY_IMAGEHLP_LINE, *MY_PIMAGEHLP_LINE;

typedef BOOL (__stdcall *My_SymGetLineFromAddrType)(HANDLE hProcess, DWORD_NATIVESIZED dwAddr, PDWORD pdwDisplacement, MY_PIMAGEHLP_LINE Line);

typedef struct MY_SYMBOL_INFO {
    ULONG       SizeOfStruct;
    ULONG       TypeIndex;        // Type Index of symbol
    ULONG64     Reserved[2];
    ULONG       info;
    ULONG       Size;
    ULONG64     ModBase;          // Base Address of module comtaining this symbol
    ULONG       Flags;
    ULONG64     Value;            // Value of symbol, ValuePresent should be 1
    ULONG64     Address;          // Address of symbol including base address of module
    ULONG       Register;         // register holding value or pointer to value
    ULONG       Scope;            // scope of the symbol
    ULONG       Tag;              // pdb classification
    ULONG       NameLen;          // Actual length of name
    ULONG       MaxNameLen;
    CHAR        Name[1];          // Name of symbol
} MY_SYMBOL_INFO, *MY_PSYMBOL_INFO;

typedef BOOL (__stdcall *My_SymFromAddrType)(HANDLE hProcess, DWORD64 Address, PDWORD64 Displacement, MY_PSYMBOL_INFO Symbol);
typedef BOOL (__stdcall *My_SymInitializeType)(HANDLE hProcess, PSTR UserSearchPath, BOOL fInvadeProcess);

static My_CaptureStackBackTraceType Memento_CaptureStackBackTrace;
static My_SymGetLineFromAddrType Memento_SymGetLineFromAddr;
static My_SymFromAddrType Memento_SymFromAddr;
static My_SymInitializeType Memento_SymInitialize;
static HANDLE Memento_process;

static void Memento_initStacktracer(void)
{
    HMODULE mod = LoadLibrary("kernel32.dll");

    if (mod == NULL)
        return;
    Memento_CaptureStackBackTrace = (My_CaptureStackBackTraceType)(GetProcAddress(mod, "RtlCaptureStackBackTrace"));
    if (Memento_CaptureStackBackTrace == NULL)
        return;
    mod = LoadLibrary("Dbghelp.dll");
    if (mod == NULL) {
        Memento_CaptureStackBackTrace = NULL;
        return;
    }
    Memento_SymGetLineFromAddr =
            (My_SymGetLineFromAddrType)(GetProcAddress(mod,
#ifdef _WIN64
                                                       "SymGetLineFromAddr64"
#else
                                                       "SymGetLineFromAddr"
#endif
                                        ));
    if (Memento_SymGetLineFromAddr == NULL) {
        Memento_CaptureStackBackTrace = NULL;
        return;
    }
    Memento_SymFromAddr = (My_SymFromAddrType)(GetProcAddress(mod, "SymFromAddr"));
    if (Memento_SymFromAddr == NULL) {
        Memento_CaptureStackBackTrace = NULL;
        return;
    }
    Memento_SymInitialize = (My_SymInitializeType)(GetProcAddress(mod, "SymInitialize"));
    if (Memento_SymInitialize == NULL) {
        Memento_CaptureStackBackTrace = NULL;
        return;
    }
    Memento_process = GetCurrentProcess();
    Memento_SymInitialize(Memento_process, NULL, TRUE);
}

static int Memento_getStacktrace(void **stack, int *skip)
{
    if (Memento_CaptureStackBackTrace == NULL)
        return 0;

    *skip = 0;
    /* Limit us to 63 levels due to windows bug */
    return Memento_CaptureStackBackTrace(SkipStackBackTraceLevels, 63-SkipStackBackTraceLevels, stack, NULL);
}

static void Memento_showStacktrace(void **stack, int numberOfFrames)
{
    MY_IMAGEHLP_LINE line;
    int i;
    char symbol_buffer[sizeof(MY_SYMBOL_INFO) + 1024 + 1];
    MY_SYMBOL_INFO *symbol = (MY_SYMBOL_INFO *)symbol_buffer;

    symbol->MaxNameLen = 1024;
    symbol->SizeOfStruct = sizeof(MY_SYMBOL_INFO);
    line.SizeOfStruct = sizeof(MY_IMAGEHLP_LINE);
    for (i = 0; i < numberOfFrames; i++)
    {
        DWORD64 dwDisplacement64;
        DWORD dwDisplacement;
        Memento_SymFromAddr(Memento_process, (DWORD64)(stack[i]), &dwDisplacement64, symbol);
        Memento_SymGetLineFromAddr(Memento_process, (DWORD_NATIVESIZED)(stack[i]), &dwDisplacement, &line);
        fprintf(stderr, "    %s in %s:%d\n", symbol->Name, line.FileName, line.LineNumber);
    }
}
#else
static void Memento_initStacktracer(void)
{
}

static int Memento_getStacktrace(void **stack, int *skip)
{
    *skip = 0;
    return 0;
}

static void Memento_showStacktrace(void **stack, int numberOfFrames)
{
}
#endif /* MEMENTO_STACKTRACE_METHOD */

#ifdef MEMENTO_DETAILS
static void Memento_storeDetails(Memento_BlkHeader *head, int type)
{
    void *stack[MEMENTO_BACKTRACE_MAX];
    Memento_BlkDetails *details;
    int count;
    int skip;

#ifdef MEMENTO_STACKTRACE_METHOD
    count = Memento_getStacktrace(stack, &skip);
#else
    skip = 0;
    count = 0;
#endif

    details = MEMENTO_UNDERLYING_MALLOC(sizeof(*details) + (count-1) * sizeof(void *));
    if (details == NULL)
        return;

    if (count)
        memcpy(&details->stack, &stack[skip], count * sizeof(void *));

    details->type = type;
    details->count = count;
    details->sequence = globals.sequence;
    details->next = NULL;
    *head->details_tail = details;
    head->details_tail = &details->next;
}
#endif

void Memento_breakpoint(void)
{
    /* A handy externally visible function for breakpointing */
#if 0 /* Enable this to force automatic breakpointing */
#ifdef DEBUG
#ifdef _MSC_VER
    __asm int 3;
#endif
#endif
#endif
}

static void Memento_addBlockHead(Memento_Blocks    *blks,
                                 Memento_BlkHeader *b,
                                 int                type)
{
    if (blks->tail == NULL)
        blks->tail = b;
    b->next    = blks->head;
    b->prev    = NULL;
    if (blks->head)
    {
        VALGRIND_MAKE_MEM_DEFINED(&blks->head->prev, sizeof(blks->head->prev));
        blks->head->prev = b;
        VALGRIND_MAKE_MEM_NOACCESS(&blks->head->prev, sizeof(blks->head->prev));
    }
    blks->head = b;
#ifndef MEMENTO_LEAKONLY
    memset(b->preblk, MEMENTO_PREFILL, Memento_PreSize);
    memset(MEMBLK_POSTPTR(b), MEMENTO_POSTFILL, Memento_PostSize);
#endif
    VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(b), Memento_PostSize);
    if (type == 0) { /* malloc */
        VALGRIND_MAKE_MEM_UNDEFINED(MEMBLK_TOBLK(b), b->rawsize);
    } else if (type == 1) { /* free */
        VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_TOBLK(b), b->rawsize);
    }
    VALGRIND_MAKE_MEM_NOACCESS(b, sizeof(Memento_BlkHeader));
}

static void Memento_addBlockTail(Memento_Blocks    *blks,
                                 Memento_BlkHeader *b,
                                 int                type)
{
    VALGRIND_MAKE_MEM_DEFINED(&blks->tail, sizeof(Memento_BlkHeader *));
    if (blks->head == NULL)
        blks->head = b;
    b->prev = blks->tail;
    b->next = NULL;
    if (blks->tail) {
        VALGRIND_MAKE_MEM_DEFINED(&blks->tail->next, sizeof(blks->tail->next));
        blks->tail->next = b;
        VALGRIND_MAKE_MEM_NOACCESS(&blks->tail->next, sizeof(blks->tail->next));
    }
    blks->tail = b;
#ifndef MEMENTO_LEAKONLY
    memset(b->preblk, MEMENTO_PREFILL, Memento_PreSize);
    memset(MEMBLK_POSTPTR(b), MEMENTO_POSTFILL, Memento_PostSize);
#endif
    VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(b), Memento_PostSize);
    if (type == 0) { /* malloc */
        VALGRIND_MAKE_MEM_UNDEFINED(MEMBLK_TOBLK(b), b->rawsize);
    } else if (type == 1) { /* free */
        VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_TOBLK(b), b->rawsize);
    }
    VALGRIND_MAKE_MEM_NOACCESS(b, sizeof(Memento_BlkHeader));
    VALGRIND_MAKE_MEM_NOACCESS(&blks->tail, sizeof(Memento_BlkHeader *));
}

typedef struct BlkCheckData {
    int found;
    int preCorrupt;
    int postCorrupt;
    int freeCorrupt;
    int index;
} BlkCheckData;

static int Memento_Internal_checkAllocedBlock(Memento_BlkHeader *b, void *arg)
{
#ifndef MEMENTO_LEAKONLY
    int             i;
    MEMENTO_UINT32 *ip;
    unsigned char  *p;
    BlkCheckData   *data = (BlkCheckData *)arg;

    ip = (MEMENTO_UINT32 *)(void *)(b->preblk);
    i = Memento_PreSize>>2;
    do {
        if (*ip++ != MEMENTO_PREFILL_UINT32)
            goto pre_corrupt;
    } while (--i);
    if (0) {
pre_corrupt:
        data->preCorrupt = 1;
    }
    /* Postfill may not be aligned, so have to be slower */
    p = MEMBLK_POSTPTR(b);
    i = Memento_PostSize-4;
    if ((intptr_t)p & 1)
    {
        if (*p++ != MEMENTO_POSTFILL)
            goto post_corrupt;
        i--;
    }
    if ((intptr_t)p & 2)
    {
        if (*(MEMENTO_UINT16 *)p != MEMENTO_POSTFILL_UINT16)
            goto post_corrupt;
        p += 2;
        i -= 2;
    }
    do {
        if (*(MEMENTO_UINT32 *)p != MEMENTO_POSTFILL_UINT32)
            goto post_corrupt;
        p += 4;
        i -= 4;
    } while (i >= 0);
    if (i & 2)
    {
        if (*(MEMENTO_UINT16 *)p != MEMENTO_POSTFILL_UINT16)
            goto post_corrupt;
        p += 2;
    }
    if (i & 1)
    {
        if (*p != MEMENTO_POSTFILL)
            goto post_corrupt;
    }
    if (0) {
post_corrupt:
        data->postCorrupt = 1;
    }
    if ((data->freeCorrupt | data->preCorrupt | data->postCorrupt) == 0) {
        b->lastCheckedOK = globals.sequence;
    }
    data->found |= 1;
#endif
    return 0;
}

static int Memento_Internal_checkFreedBlock(Memento_BlkHeader *b, void *arg)
{
#ifndef MEMENTO_LEAKONLY
    int            i;
    unsigned char *p;
    BlkCheckData  *data = (BlkCheckData *)arg;

    p = MEMBLK_TOBLK(b); /* p will always be aligned */
    i = b->rawsize - 4;
    /* Attempt to speed this up by checking an (aligned) int at a time */
    while (i >= 0) {
        if (*(MEMENTO_UINT32 *)p != MEMENTO_FREEFILL_UINT32)
            goto mismatch;
        p += 4;
        i -= 4;
    }
    i += 4;
    if (i & 2) {
        if (*(MEMENTO_UINT16 *)p != MEMENTO_FREEFILL_UINT16)
            goto mismatch;
        p += 2;
        i -= 2;
    }
mismatch:
    while (i) {
        if (*p++ != (unsigned char)MEMENTO_FREEFILL)
            break;
        i--;
    }
    if (i) {
        data->freeCorrupt = 1;
        data->index       = b->rawsize-i;
    }
    return Memento_Internal_checkAllocedBlock(b, arg);
#else
    return 0;
#endif
}

static void Memento_removeBlock(Memento_Blocks    *blks,
                                Memento_BlkHeader *b)
{
    VALGRIND_MAKE_MEM_DEFINED(b, sizeof(*b));
    if (b->next) {
        VALGRIND_MAKE_MEM_DEFINED(&b->next->prev, sizeof(b->next->prev));
        b->next->prev = b->prev;
        VALGRIND_MAKE_MEM_NOACCESS(&b->next->prev, sizeof(b->next->prev));
    }
    if (b->prev) {
        VALGRIND_MAKE_MEM_DEFINED(&b->prev->next, sizeof(b->prev->next));
        b->prev->next = b->next;
        VALGRIND_MAKE_MEM_NOACCESS(&b->prev->next, sizeof(b->prev->next));
    }
    if (blks->tail == b)
        blks->tail = b->prev;
    if (blks->head == b)
        blks->head = b->next;
}

static void free_block(Memento_BlkHeader *head)
{
#ifdef MEMENTO_DETAILS
    Memento_BlkDetails *details = head->details;

    while (details)
    {
        Memento_BlkDetails *next = details->next;
        MEMENTO_UNDERLYING_FREE(details);
        details = next;
    }
#endif
    MEMENTO_UNDERLYING_FREE(head);
}

static int Memento_Internal_makeSpace(size_t space)
{
    /* If too big, it can never go on the freelist */
    if (space > MEMENTO_FREELIST_MAX_SINGLE_BLOCK)
        return 0;
    /* Pretend we added it on. */
    globals.freeListSize += space;
    /* Ditch blocks until it fits within our limit */
    while (globals.freeListSize > MEMENTO_FREELIST_MAX) {
        Memento_BlkHeader *head = globals.free.head;
        VALGRIND_MAKE_MEM_DEFINED(head, sizeof(*head));
        globals.free.head = head->next;
        globals.freeListSize -= MEMBLK_SIZE(head->rawsize);
        free_block(head);
    }
    /* Make sure we haven't just completely emptied the free list */
    /* (This should never happen, but belt and braces... */
    if (globals.free.head == NULL)
        globals.free.tail = NULL;
    return 1;
}

static int Memento_appBlocks(Memento_Blocks *blks,
                             int             (*app)(Memento_BlkHeader *,
                                                    void *),
                             void           *arg)
{
    Memento_BlkHeader *head = blks->head;
    Memento_BlkHeader *next;
    int                result;
    while (head) {
        VALGRIND_MAKE_MEM_DEFINED(head, sizeof(Memento_BlkHeader));
        VALGRIND_MAKE_MEM_DEFINED(MEMBLK_TOBLK(head),
                                  head->rawsize + Memento_PostSize);
        result = app(head, arg);
        next = head->next;
        VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(head), Memento_PostSize);
        VALGRIND_MAKE_MEM_NOACCESS(head, sizeof(Memento_BlkHeader));
        if (result)
            return result;
        head = next;
    }
    return 0;
}

/* Distrustful - check the block is a real one */
static int Memento_appBlockUser(Memento_Blocks    *blks,
                                int                (*app)(Memento_BlkHeader *,
                                                          void *),
                                void              *arg,
                                Memento_BlkHeader *b)
{
    Memento_BlkHeader *head = blks->head;
    Memento_BlkHeader *next;
    int                result;
    while (head && head != b) {
        VALGRIND_MAKE_MEM_DEFINED(head, sizeof(Memento_BlkHeader));
        next = head->next;
       VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(head), Memento_PostSize);
        head = next;
    }
    if (head == b) {
        VALGRIND_MAKE_MEM_DEFINED(head, sizeof(Memento_BlkHeader));
        VALGRIND_MAKE_MEM_DEFINED(MEMBLK_TOBLK(head),
                                  head->rawsize + Memento_PostSize);
        result = app(head, arg);
        VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(head), Memento_PostSize);
        VALGRIND_MAKE_MEM_NOACCESS(head, sizeof(Memento_BlkHeader));
        return result;
    }
    return 0;
}

static int Memento_appBlock(Memento_Blocks    *blks,
                            int                (*app)(Memento_BlkHeader *,
                                                      void *),
                            void              *arg,
                            Memento_BlkHeader *b)
{
    int result;
    VALGRIND_MAKE_MEM_DEFINED(b, sizeof(Memento_BlkHeader));
    VALGRIND_MAKE_MEM_DEFINED(MEMBLK_TOBLK(b),
                              b->rawsize + Memento_PostSize);
    result = app(b, arg);
    VALGRIND_MAKE_MEM_NOACCESS(MEMBLK_POSTPTR(b), Memento_PostSize);
    VALGRIND_MAKE_MEM_NOACCESS(b, sizeof(Memento_BlkHeader));
    return result;
}

static void showBlock(Memento_BlkHeader *b, int space)
{
    fprintf(stderr, "0x%p:(size=%d,num=%d)",
            MEMBLK_TOBLK(b), (int)b->rawsize, b->sequence);
    if (b->label)
        fprintf(stderr, "%c(%s)", space, b->label);
}

static void blockDisplay(Memento_BlkHeader *b, int n)
{
    n++;
    while (n > 40)
    {
            fprintf(stderr, "*");
            n -= 40;
    }
    while(n > 0)
    {
        int i = n;
        if (i > 32)
            i = 32;
        n -= i;
        fprintf(stderr, "%s", &"                                "[32-i]);
    }
    showBlock(b, '\t');
    fprintf(stderr, "\n");
}

static int Memento_listBlock(Memento_BlkHeader *b,
                             void              *arg)
{
    int *counts = (int *)arg;
    blockDisplay(b, 0);
    counts[0]++;
    counts[1]+= b->rawsize;
    return 0;
}

static void doNestedDisplay(Memento_BlkHeader *b,
                            int depth)
{
    /* Try and avoid recursion if we can help it */
    do {
        blockDisplay(b, depth);
        if (b->sibling) {
            if (b->child)
                doNestedDisplay(b->child, depth+1);
            b = b->sibling;
        } else {
            b = b->child;
            depth++;
        }
    } while (b);
}

static int ptrcmp(const void *a_, const void *b_)
{
    const char **a = (const char **)a_;
    const char **b = (const char **)b_;
    return (int)(*a-*b);
}

static
int Memento_listBlocksNested(void)
{
    int count, size, i;
    Memento_BlkHeader *b, *prev;
    void **blocks, *minptr, *maxptr;
    intptr_t mask;

    /* Count the blocks */
    count = 0;
    size = 0;
    for (b = globals.used.head; b; b = b->next) {
        VALGRIND_MAKE_MEM_DEFINED(b, sizeof(*b));
        size += b->rawsize;
        count++;
    }

    /* Make our block list */
    blocks = MEMENTO_UNDERLYING_MALLOC(sizeof(void *) * count);
    if (blocks == NULL)
        return 1;

    /* Populate our block list */
    b = globals.used.head;
    minptr = maxptr = MEMBLK_TOBLK(b);
    mask = (long)minptr;
    for (i = 0; b; b = b->next, i++) {
        void *p = MEMBLK_TOBLK(b);
        mask &= (long)p;
        if (p < minptr)
            minptr = p;
        if (p > maxptr)
            maxptr = p;
        blocks[i] = p;
        b->flags &= ~Memento_Flag_HasParent;
        b->child   = NULL;
        b->sibling = NULL;
        b->prev    = NULL; /* parent */
    }
    qsort(blocks, count, sizeof(void *), ptrcmp);

    /* Now, calculate tree */
    for (b = globals.used.head; b; b = b->next) {
        char *p = MEMBLK_TOBLK(b);
        int end = (b->rawsize < MEMENTO_PTRSEARCH ? b->rawsize : MEMENTO_PTRSEARCH);
        for (i = MEMENTO_SEARCH_SKIP; i < end; i += sizeof(void *)) {
            void *q = *(void **)(&p[i]);
            void **r;

            /* Do trivial checks on pointer */
            if ((mask & (intptr_t)q) != mask || q < minptr || q > maxptr)
                continue;

            /* Search for pointer */
            r = bsearch(&q, blocks, count, sizeof(void *), ptrcmp);
            if (r) {
                /* Found child */
                Memento_BlkHeader *child = MEMBLK_FROMBLK(*r);
                Memento_BlkHeader *parent;

                /* We're assuming tree structure, not graph - ignore second
                 * and subsequent pointers. */
                if (child->prev != NULL) /* parent */
                    continue;
                if (child->flags & Memento_Flag_HasParent)
                    continue;

                /* Not interested in pointers to ourself! */
                if (child == b)
                        continue;

                /* We're also assuming acyclicness here. If this is one of
                 * our parents, ignore it. */
                parent = b->prev; /* parent */
                while (parent != NULL && parent != child)
                    parent = parent->prev; /* parent */
                if (parent == child)
                    continue;

                child->sibling = b->child;
                b->child = child;
                child->prev = b; /* parent */
                child->flags |= Memento_Flag_HasParent;
            }
        }
    }

    /* Now display with nesting */
    for (b = globals.used.head; b; b = b->next) {
        if ((b->flags & Memento_Flag_HasParent) == 0)
            doNestedDisplay(b, 0);
    }
    fprintf(stderr, " Total number of blocks = %d\n", count);
    fprintf(stderr, " Total size of blocks = %d\n", size);

    MEMENTO_UNDERLYING_FREE(blocks);

    /* Now put the blocks back for valgrind, and restore the prev
     * and magic values. */
    prev = NULL;
    for (b = globals.used.head; b;) {
      Memento_BlkHeader *next = b->next;
      b->prev = prev;
      b->child = MEMENTO_CHILD_MAGIC;
      b->sibling = MEMENTO_SIBLING_MAGIC;
      prev = b;
      VALGRIND_MAKE_MEM_NOACCESS(b, sizeof(*b));
      b = next;
    }

    return 0;
}

void Memento_listBlocks(void)
{
    fprintf(stderr, "Allocated blocks:\n");
    if (Memento_listBlocksNested())
    {
        int counts[2];
        counts[0] = 0;
        counts[1] = 0;
        Memento_appBlocks(&globals.used, Memento_listBlock, &counts[0]);
        fprintf(stderr, " Total number of blocks = %d\n", counts[0]);
        fprintf(stderr, " Total size of blocks = %d\n", counts[1]);
    }
}

static int Memento_listNewBlock(Memento_BlkHeader *b,
                                void              *arg)
{
    if (b->flags & Memento_Flag_OldBlock)
        return 0;
    b->flags |= Memento_Flag_OldBlock;
    return Memento_listBlock(b, arg);
}

void Memento_listNewBlocks(void)
{
    int counts[2];
    counts[0] = 0;
    counts[1] = 0;
    fprintf(stderr, "Blocks allocated and still extant since last list:\n");
    Memento_appBlocks(&globals.used, Memento_listNewBlock, &counts[0]);
    fprintf(stderr, "  Total number of blocks = %d\n", counts[0]);
    fprintf(stderr, "  Total size of blocks = %d\n", counts[1]);
}

static void Memento_endStats(void)
{
    fprintf(stderr, "Total memory malloced = %u bytes\n", (unsigned int)globals.totalAlloc);
    fprintf(stderr, "Peak memory malloced = %u bytes\n", (unsigned int)globals.peakAlloc);
    fprintf(stderr, "%u mallocs, %u frees, %u reallocs\n", (unsigned int)globals.numMallocs,
            (unsigned int)globals.numFrees, (unsigned int)globals.numReallocs);
    fprintf(stderr, "Average allocation size %u bytes\n", (unsigned int)
            (globals.numMallocs != 0 ? globals.totalAlloc/globals.numMallocs: 0));
}

void Memento_stats(void)
{
    fprintf(stderr, "Current memory malloced = %u bytes\n", (unsigned int)globals.alloc);
    Memento_endStats();
}

#ifdef MEMENTO_DETAILS
static int showInfo(Memento_BlkHeader *b, void *arg)
{
    Memento_BlkDetails *details;

    fprintf(stderr, "0x%p:(size=%d,num=%d)",
            MEMBLK_TOBLK(b), (int)b->rawsize, b->sequence);
    if (b->label)
        fprintf(stderr, " (%s)", b->label);
    fprintf(stderr, "\nEvents:\n");

    details = b->details;
    while (details)
    {
        fprintf(stderr, "  Event %d (%s)\n", details->sequence, eventType[(int)details->type]);
        Memento_showStacktrace(details->stack, details->count);
        details = details->next;
    }
    return 0;
}
#endif

void Memento_listBlockInfo(void)
{
#ifdef MEMENTO_DETAILS
    fprintf(stderr, "Details of allocated blocks:\n");
    Memento_appBlocks(&globals.used, showInfo, NULL);
#endif
}

static void Memento_fin(void)
{
    Memento_checkAllMemory();
    Memento_endStats();
    if (globals.used.head != NULL) {
        Memento_listBlocks();
#ifdef MEMENTO_DETAILS
        fprintf(stderr, "\n");
        Memento_listBlockInfo();
#endif
        Memento_breakpoint();
    }
    if (globals.segv) {
        fprintf(stderr, "Memory dumped on SEGV while squeezing @ %d\n", globals.failAt);
    } else if (globals.squeezing) {
        if (globals.pattern == 0)
            fprintf(stderr, "Memory squeezing @ %d complete\n", globals.squeezeAt);
        else
            fprintf(stderr, "Memory squeezing @ %d (%d) complete\n", globals.squeezeAt, globals.pattern);
    }
    if (globals.failing)
    {
        fprintf(stderr, "MEMENTO_FAILAT=%d\n", globals.failAt);
        fprintf(stderr, "MEMENTO_PATTERN=%d\n", globals.pattern);
    }
    if (globals.nextFailAt != 0)
    {
        fprintf(stderr, "MEMENTO_NEXTFAILAT=%d\n", globals.nextFailAt);
        fprintf(stderr, "MEMENTO_NEXTPATTERN=%d\n", globals.nextPattern);
    }
}

static void Memento_inited(void)
{
    /* A good place for a breakpoint */
}

static void Memento_init(void)
{
    char *env;
    memset(&globals, 0, sizeof(globals));
    globals.inited    = 1;
    globals.used.head = NULL;
    globals.used.tail = NULL;
    globals.free.head = NULL;
    globals.free.tail = NULL;
    globals.sequence  = 0;
    globals.countdown = 1024;

    env = getenv("MEMENTO_FAILAT");
    globals.failAt = (env ? atoi(env) : 0);

    env = getenv("MEMENTO_PARANOIA");
    globals.paranoia = (env ? atoi(env) : 0);
    if (globals.paranoia == 0)
        globals.paranoia = -1024;

    env = getenv("MEMENTO_PARANOIDAT");
    globals.paranoidAt = (env ? atoi(env) : 0);

    env = getenv("MEMENTO_SQUEEZEAT");
    globals.squeezeAt = (env ? atoi(env) : 0);

    env = getenv("MEMENTO_PATTERN");
    globals.pattern = (env ? atoi(env) : 0);

    env = getenv("MEMENTO_MAXMEMORY");
    globals.maxMemory = (env ? atoi(env) : 0);

    atexit(Memento_fin);

    Memento_initStacktracer();

    Memento_inited();
}

typedef struct findBlkData {
    void              *addr;
    Memento_BlkHeader *blk;
    int                flags;
} findBlkData;

static int Memento_containsAddr(Memento_BlkHeader *b,
                                void *arg)
{
    findBlkData *data = (findBlkData *)arg;
    char *blkend = &((char *)MEMBLK_TOBLK(b))[b->rawsize];
    if ((MEMBLK_TOBLK(b) <= data->addr) &&
        ((void *)blkend > data->addr)) {
        data->blk = b;
        data->flags = 1;
        return 1;
    }
    if (((void *)b <= data->addr) &&
        (MEMBLK_TOBLK(b) > data->addr)) {
        data->blk = b;
        data->flags = 2;
        return 1;
    }
    if (((void *)blkend <= data->addr) &&
        ((void *)(blkend + Memento_PostSize) > data->addr)) {
        data->blk = b;
        data->flags = 3;
        return 1;
    }
    return 0;
}

void Memento_info(void *addr)
{
#ifdef MEMENTO_DETAILS
    findBlkData data;

    data.addr  = addr;
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.used, Memento_containsAddr, &data);
    if (data.blk != NULL)
        showInfo(data.blk, NULL);
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.free, Memento_containsAddr, &data);
    if (data.blk != NULL)
        showInfo(data.blk, NULL);
#else
    printf("Memento not compiled with details support\n");
#endif
}

#ifdef MEMENTO_HAS_FORK
#include <unistd.h>
#include <sys/wait.h>
#ifdef MEMENTO_STACKTRACE_METHOD
#if MEMENTO_STACKTRACE_METHOD == 1
#include <signal.h>
#endif
#endif

/* FIXME: Find some portable way of getting this */
/* MacOSX has 10240, Ubuntu seems to have 256 */
#define OPEN_MAX 10240

/* stashed_map[j] = i means that filedescriptor i-1 was duplicated to j */
int stashed_map[OPEN_MAX];

static void Memento_signal(void)
{
    fprintf(stderr, "SEGV after Memory squeezing @ %d\n", globals.squeezeAt);

#ifdef MEMENTO_STACKTRACE_METHOD
#if MEMENTO_STACKTRACE_METHOD == 1
    {
      void *array[100];
      size_t size;

      size = backtrace(array, 100);
      fprintf(stderr, "------------------------------------------------------------------------\n");
      fprintf(stderr, "Backtrace:\n");
      backtrace_symbols_fd(array, size, 2);
      fprintf(stderr, "------------------------------------------------------------------------\n");
    }
#endif
#endif

    exit(1);
}

static int squeeze(void)
{
    pid_t pid;
    int i, status;

    if (globals.patternBit < 0)
        return 1;
    if (globals.squeezing && globals.patternBit >= MEMENTO_MAXPATTERN)
        return 1;

    if (globals.patternBit == 0)
        globals.squeezeAt = globals.sequence;

    if (!globals.squeezing) {
        fprintf(stderr, "Memory squeezing @ %d\n", globals.squeezeAt);
    } else
        fprintf(stderr, "Memory squeezing @ %d (%x,%x)\n", globals.squeezeAt, globals.pattern, globals.patternBit);

    /* When we fork below, the child is going to snaffle all our file pointers
     * and potentially corrupt them. Let's make copies of all of them before
     * we fork, so we can restore them when we restart. */
    for (i = 0; i < OPEN_MAX; i++) {
        if (stashed_map[i] == 0) {
            int j = dup(i);
            stashed_map[j] = i+1;
        }
    }

    pid = fork();
    if (pid == 0) {
        /* Child */
        signal(SIGSEGV, Memento_signal);
        /* In the child, we always fail the next allocation. */
        if (globals.patternBit == 0) {
            globals.patternBit = 1;
        } else
            globals.patternBit <<= 1;
        globals.squeezing = 1;
        return 1;
    }

    /* In the parent if we hit another allocation, pass it (and record the
     * fact we passed it in the pattern. */
    globals.pattern |= globals.patternBit;
    globals.patternBit <<= 1;

    /* Wait for pid to finish */
    waitpid(pid, &status, 0);

    if (status != 0) {
        fprintf(stderr, "Child status=%d\n", status);
    }

    /* Put the files back */
    for (i = 0; i < OPEN_MAX; i++) {
        if (stashed_map[i] != 0) {
            dup2(i, stashed_map[i]-1);
            close(i);
            stashed_map[i] = 0;
        }
    }

    return 0;
}
#else
#include <signal.h>

static void Memento_signal(void)
{
    globals.segv = 1;
    /* If we just return from this function the SEGV will be unhandled, and
     * we'll launch into whatever JIT debugging system the OS provides. At
     * least fprintf(stderr, something useful first. If MEMENTO_NOJIT is set, then
     * just exit to avoid the JIT (and get the usual atexit handling). */
    if (getenv("MEMENTO_NOJIT"))
        exit(1);
    else
        Memento_fin();
}

int squeeze(void)
{
    fprintf(stderr, "Memento memory squeezing disabled as no fork!\n");
    return 0;
}
#endif

static void Memento_startFailing(void)
{
    if (!globals.failing) {
        fprintf(stderr, "Starting to fail...\n");
        fflush(stderr);
        globals.failing = 1;
        globals.failAt = globals.sequence;
        globals.nextFailAt = globals.sequence+1;
        globals.pattern = 0;
        globals.patternBit = 0;
        signal(SIGSEGV, Memento_signal);
        signal(SIGABRT, Memento_signal);
        Memento_breakpoint();
    }
}

static void Memento_event(void)
{
    globals.sequence++;
    if ((globals.sequence >= globals.paranoidAt) && (globals.paranoidAt != 0)) {
        globals.paranoia = 1;
        globals.countdown = 1;
    }
    if (--globals.countdown == 0) {
        Memento_checkAllMemory();
        if (globals.paranoia > 0)
            globals.countdown = globals.paranoia;
        else
        {
            globals.countdown = -globals.paranoia;
            if (globals.paranoia > INT_MIN/2)
                globals.paranoia *= 2;
        }
    }

    if (globals.sequence == globals.breakAt) {
        fprintf(stderr, "Breaking at event %d\n", globals.breakAt);
        Memento_breakpoint();
    }
}

int Memento_breakAt(int event)
{
    globals.breakAt = event;
    return event;
}

void *Memento_label(void *ptr, const char *label)
{
    Memento_BlkHeader *block;

    if (ptr == NULL)
        return NULL;
    block = MEMBLK_FROMBLK(ptr);
    VALGRIND_MAKE_MEM_DEFINED(&block->label, sizeof(block->label));
    block->label = label;
    VALGRIND_MAKE_MEM_UNDEFINED(&block->label, sizeof(block->label));
    return ptr;
}

int Memento_failThisEvent(void)
{
    int failThisOne;

    if (!globals.inited)
        Memento_init();

    Memento_event();

    if ((globals.sequence >= globals.failAt) && (globals.failAt != 0))
        Memento_startFailing();
    if ((globals.sequence >= globals.squeezeAt) && (globals.squeezeAt != 0)) {
        return squeeze();
    }

    if (!globals.failing)
        return 0;
    failThisOne = ((globals.patternBit & globals.pattern) == 0);
    /* If we are failing, and we've reached the end of the pattern and we've
     * still got bits available in the pattern word, and we haven't already
     * set a nextPattern, then extend the pattern. */
    if (globals.failing &&
        ((~(globals.patternBit-1) & globals.pattern) == 0) &&
        (globals.patternBit != 0) &&
        globals.nextPattern == 0)
    {
        /* We'll fail this one, and set the 'next' one to pass it. */
        globals.nextFailAt = globals.failAt;
        globals.nextPattern = globals.pattern | globals.patternBit;
    }
    globals.patternBit = (globals.patternBit ? globals.patternBit << 1 : 1);

    return failThisOne;
}

static void *do_malloc(size_t s, int eventType)
{
    Memento_BlkHeader *memblk;
    size_t             smem = MEMBLK_SIZE(s);

    if (Memento_failThisEvent())
        return NULL;

    if (s == 0)
        return NULL;

    globals.numMallocs++;

    if (globals.maxMemory != 0 && globals.alloc + s > globals.maxMemory)
        return NULL;

    memblk = MEMENTO_UNDERLYING_MALLOC(smem);
    if (memblk == NULL)
        return NULL;

    globals.alloc      += s;
    globals.totalAlloc += s;
    if (globals.peakAlloc < globals.alloc)
        globals.peakAlloc = globals.alloc;
#ifndef MEMENTO_LEAKONLY
    memset(MEMBLK_TOBLK(memblk), MEMENTO_ALLOCFILL, s);
#endif
    memblk->rawsize       = s;
    memblk->sequence      = globals.sequence;
    memblk->lastCheckedOK = memblk->sequence;
    memblk->flags         = 0;
    memblk->label         = 0;
    memblk->child         = MEMENTO_CHILD_MAGIC;
    memblk->sibling       = MEMENTO_SIBLING_MAGIC;
#ifdef MEMENTO_DETAILS
    memblk->details       = NULL;
    memblk->details_tail  = &memblk->details;
    Memento_storeDetails(memblk, Memento_EventType_malloc);
#endif /* MEMENTO_DETAILS */
    Memento_addBlockHead(&globals.used, memblk, 0);
    return MEMBLK_TOBLK(memblk);
}

void *Memento_malloc(size_t s)
{
    return do_malloc(s, Memento_EventType_malloc);
}

void *Memento_calloc(size_t n, size_t s)
{
    void *block = do_malloc(n*s, Memento_EventType_calloc);

    if (block)
        memset(block, 0, n*s);
    return block;
}

/* Treat blocks from the user with suspicion, and check them the slow
 * but safe way. */
static int checkBlockUser(Memento_BlkHeader *memblk, const char *action)
{
#ifndef MEMENTO_LEAKONLY
    BlkCheckData data;

    memset(&data, 0, sizeof(data));
    Memento_appBlockUser(&globals.used, Memento_Internal_checkAllocedBlock,
                         &data, memblk);
    if (!data.found) {
        /* Failure! */
        fprintf(stderr, "Attempt to %s block ", action);
        showBlock(memblk, 32);
        fprintf(stderr, "\n");
        Memento_breakpoint();
        return 1;
    } else if (data.preCorrupt || data.postCorrupt) {
        fprintf(stderr, "Block ");
        showBlock(memblk, ' ');
        fprintf(stderr, " found to be corrupted on %s!\n", action);
        if (data.preCorrupt) {
            fprintf(stderr, "Preguard corrupted\n");
        }
        if (data.postCorrupt) {
            fprintf(stderr, "Postguard corrupted\n");
        }
        fprintf(stderr, "Block last checked OK at allocation %d. Now %d.\n",
                memblk->lastCheckedOK, globals.sequence);
        Memento_breakpoint();
        return 1;
    }
#endif
    return 0;
}

static int checkBlock(Memento_BlkHeader *memblk, const char *action)
{
#ifndef MEMENTO_LEAKONLY
    BlkCheckData data;
#endif

    if (memblk->child != MEMENTO_CHILD_MAGIC ||
        memblk->sibling != MEMENTO_SIBLING_MAGIC)
    {
        /* Failure! */
        fprintf(stderr, "Attempt to %s invalid block ", action);
        showBlock(memblk, 32);
        fprintf(stderr, "\n");
        Memento_breakpoint();
        return 1;
    }

#ifndef MEMENTO_LEAKONLY
    memset(&data, 0, sizeof(data));
    Memento_appBlock(&globals.used, Memento_Internal_checkAllocedBlock,
                     &data, memblk);
    if (!data.found) {
        /* Failure! */
        fprintf(stderr, "Attempt to %s block ", action);
        showBlock(memblk, 32);
        fprintf(stderr, "\n");
        Memento_breakpoint();
        return 1;
    } else if (data.preCorrupt || data.postCorrupt) {
        fprintf(stderr, "Block ");
        showBlock(memblk, ' ');
        fprintf(stderr, " found to be corrupted on %s!\n", action);
        if (data.preCorrupt) {
            fprintf(stderr, "Preguard corrupted\n");
        }
        if (data.postCorrupt) {
            fprintf(stderr, "Postguard corrupted\n");
        }
        fprintf(stderr, "Block last checked OK at allocation %d. Now %d.\n",
                memblk->lastCheckedOK, globals.sequence);
        Memento_breakpoint();
        return 1;
    }
#endif
    return 0;
}

static void do_free(void *blk, int eventType)
{
    Memento_BlkHeader *memblk;

    if (!globals.inited)
        Memento_init();

    Memento_event();

    if (blk == NULL)
        return;

    memblk = MEMBLK_FROMBLK(blk);
    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (checkBlock(memblk, "free"))
        return;

#ifdef MEMENTO_DETAILS
    Memento_storeDetails(memblk, Memento_EventType_free);
#endif

    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (memblk->flags & Memento_Flag_BreakOnFree)
        Memento_breakpoint();

    globals.alloc -= memblk->rawsize;
    globals.numFrees++;

    Memento_removeBlock(&globals.used, memblk);

    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (Memento_Internal_makeSpace(MEMBLK_SIZE(memblk->rawsize))) {
        VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
        VALGRIND_MAKE_MEM_DEFINED(MEMBLK_TOBLK(memblk),
                                  memblk->rawsize + Memento_PostSize);
#ifndef MEMENTO_LEAKONLY
        memset(MEMBLK_TOBLK(memblk), MEMENTO_FREEFILL, memblk->rawsize);
#endif
        memblk->flags |= Memento_Flag_Freed;
        Memento_addBlockTail(&globals.free, memblk, 1);
    } else {
        free_block(memblk);
    }
}

void Memento_free(void *blk)
{
    do_free(blk, Memento_EventType_free);
}

static void *do_realloc(void *blk, size_t newsize, int type)
{
    Memento_BlkHeader *memblk, *newmemblk;
    size_t             newsizemem;
    int                flags;

    if (Memento_failThisEvent())
        return NULL;

    memblk     = MEMBLK_FROMBLK(blk);
    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (checkBlock(memblk, "realloc"))
        return NULL;

#ifdef MEMENTO_DETAILS
    Memento_storeDetails(memblk, type);
#endif

    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (memblk->flags & Memento_Flag_BreakOnRealloc)
        Memento_breakpoint();

    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    if (globals.maxMemory != 0 && globals.alloc - memblk->rawsize + newsize > globals.maxMemory)
        return NULL;

    newsizemem = MEMBLK_SIZE(newsize);
    Memento_removeBlock(&globals.used, memblk);
    VALGRIND_MAKE_MEM_DEFINED(memblk, sizeof(*memblk));
    flags = memblk->flags;
    newmemblk  = MEMENTO_UNDERLYING_REALLOC(memblk, newsizemem);
    if (newmemblk == NULL)
    {
        Memento_addBlockHead(&globals.used, memblk, 2);
        return NULL;
    }
    globals.numReallocs++;
    globals.totalAlloc += newsize;
    globals.alloc      -= newmemblk->rawsize;
    globals.alloc      += newsize;
    if (globals.peakAlloc < globals.alloc)
        globals.peakAlloc = globals.alloc;
    newmemblk->flags = flags;
    if (newmemblk->rawsize < newsize) {
        char *newbytes = ((char *)MEMBLK_TOBLK(newmemblk))+newmemblk->rawsize;
        VALGRIND_MAKE_MEM_DEFINED(newbytes, newsize - newmemblk->rawsize);
#ifndef MEMENTO_LEAKONLY
        memset(newbytes, MEMENTO_ALLOCFILL, newsize - newmemblk->rawsize);
#endif
        VALGRIND_MAKE_MEM_UNDEFINED(newbytes, newsize - newmemblk->rawsize);
    }
    newmemblk->rawsize = newsize;
#ifndef MEMENTO_LEAKONLY
    VALGRIND_MAKE_MEM_DEFINED(newmemblk->preblk, Memento_PreSize);
    memset(newmemblk->preblk, MEMENTO_PREFILL, Memento_PreSize);
    VALGRIND_MAKE_MEM_UNDEFINED(newmemblk->preblk, Memento_PreSize);
    VALGRIND_MAKE_MEM_DEFINED(MEMBLK_POSTPTR(newmemblk), Memento_PostSize);
    memset(MEMBLK_POSTPTR(newmemblk), MEMENTO_POSTFILL, Memento_PostSize);
    VALGRIND_MAKE_MEM_UNDEFINED(MEMBLK_POSTPTR(newmemblk), Memento_PostSize);
#endif
    Memento_addBlockHead(&globals.used, newmemblk, 2);
    return MEMBLK_TOBLK(newmemblk);
}

void *Memento_realloc(void *blk, size_t newsize)
{
    if (blk == NULL)
        return do_malloc(newsize, Memento_EventType_realloc);
    if (newsize == 0) {
        do_free(blk, Memento_EventType_realloc);
        return NULL;
    }

    return do_realloc(blk, newsize, Memento_EventType_realloc);
}

int Memento_checkBlock(void *blk)
{
    Memento_BlkHeader *memblk;

    if (blk == NULL)
        return 0;
    memblk = MEMBLK_FROMBLK(blk);
    return checkBlockUser(memblk, "check");
}

static int Memento_Internal_checkAllAlloced(Memento_BlkHeader *memblk, void *arg)
{
    BlkCheckData *data = (BlkCheckData *)arg;

    Memento_Internal_checkAllocedBlock(memblk, data);
    if (data->preCorrupt || data->postCorrupt) {
        if ((data->found & 2) == 0) {
            fprintf(stderr, "Allocated blocks:\n");
            data->found |= 2;
        }
        fprintf(stderr, "  Block ");
        showBlock(memblk, ' ');
        if (data->preCorrupt) {
            fprintf(stderr, " Preguard ");
        }
        if (data->postCorrupt) {
            fprintf(stderr, "%s Postguard ",
                    (data->preCorrupt ? "&" : ""));
        }
        fprintf(stderr, "corrupted.\n    "
                "Block last checked OK at allocation %d. Now %d.\n",
                memblk->lastCheckedOK, globals.sequence);
        data->preCorrupt  = 0;
        data->postCorrupt = 0;
        data->freeCorrupt = 0;
    }
    else
        memblk->lastCheckedOK = globals.sequence;
    return 0;
}

static int Memento_Internal_checkAllFreed(Memento_BlkHeader *memblk, void *arg)
{
    BlkCheckData *data = (BlkCheckData *)arg;

    Memento_Internal_checkFreedBlock(memblk, data);
    if (data->preCorrupt || data->postCorrupt || data->freeCorrupt) {
        if ((data->found & 4) == 0) {
            fprintf(stderr, "Freed blocks:\n");
            data->found |= 4;
        }
        fprintf(stderr, "  ");
        showBlock(memblk, ' ');
        if (data->freeCorrupt) {
            fprintf(stderr, " index %d (address 0x%p) onwards", data->index,
                    &((char *)MEMBLK_TOBLK(memblk))[data->index]);
            if (data->preCorrupt) {
                fprintf(stderr, "+ preguard");
            }
            if (data->postCorrupt) {
                fprintf(stderr, "+ postguard");
            }
        } else {
            if (data->preCorrupt) {
                fprintf(stderr, " preguard");
            }
            if (data->postCorrupt) {
                fprintf(stderr, "%s Postguard",
                        (data->preCorrupt ? "+" : ""));
            }
        }
        fprintf(stderr, " corrupted.\n"
                "    Block last checked OK at allocation %d. Now %d.\n",
                memblk->lastCheckedOK, globals.sequence);
        data->preCorrupt  = 0;
        data->postCorrupt = 0;
        data->freeCorrupt = 0;
    }
    else
        memblk->lastCheckedOK = globals.sequence;
    return 0;
}

int Memento_checkAllMemory(void)
{
#ifndef MEMENTO_LEAKONLY
    BlkCheckData data;

    memset(&data, 0, sizeof(data));
    Memento_appBlocks(&globals.used, Memento_Internal_checkAllAlloced, &data);
    Memento_appBlocks(&globals.free, Memento_Internal_checkAllFreed, &data);
    if (data.found & 6) {
        Memento_breakpoint();
        return 1;
    }
#endif
    return 0;
}

int Memento_setParanoia(int i)
{
    globals.paranoia = i;
    if (globals.paranoia > 0)
        globals.countdown = globals.paranoia;
    else
        globals.countdown = -globals.paranoia;
    return i;
}

int Memento_paranoidAt(int i)
{
    globals.paranoidAt = i;
    return i;
}

int Memento_getBlockNum(void *b)
{
    Memento_BlkHeader *memblk;
    if (b == NULL)
        return 0;
    memblk = MEMBLK_FROMBLK(b);
    return (memblk->sequence);
}

int Memento_check(void)
{
    int result;

    fprintf(stderr, "Checking memory\n");
    result = Memento_checkAllMemory();
    fprintf(stderr, "Memory checked!\n");
    return result;
}

int Memento_find(void *a)
{
    findBlkData data;

    data.addr  = a;
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.used, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Address 0x%p is in %sallocated block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, "\n");
        return data.blk->sequence;
    }
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.free, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Address 0x%p is in %sfreed block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, "\n");
        return data.blk->sequence;
    }
    return 0;
}

void Memento_breakOnFree(void *a)
{
    findBlkData data;

    data.addr  = a;
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.used, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Will stop when address 0x%p (in %sallocated block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, ") is freed\n");
        data.blk->flags |= Memento_Flag_BreakOnFree;
        return;
    }
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.free, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Can't stop on free; address 0x%p is in %sfreed block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "Can't stop on free; address 0x%p is not in a known block.\n", a);
}

void Memento_breakOnRealloc(void *a)
{
    findBlkData data;

    data.addr  = a;
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.used, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Will stop when address 0x%p (in %sallocated block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, ") is freed (or realloced)\n");
        data.blk->flags |= Memento_Flag_BreakOnFree | Memento_Flag_BreakOnRealloc;
        return;
    }
    data.blk   = NULL;
    data.flags = 0;
    Memento_appBlocks(&globals.free, Memento_containsAddr, &data);
    if (data.blk != NULL) {
        fprintf(stderr, "Can't stop on free/realloc; address 0x%p is in %sfreed block ",
                data.addr,
                (data.flags == 1 ? "" : (data.flags == 2 ?
                                         "preguard of " : "postguard of ")));
        showBlock(data.blk, ' ');
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "Can't stop on free/realloc; address 0x%p is not in a known block.\n", a);
}

int Memento_failAt(int i)
{
    globals.failAt = i;
    if ((globals.sequence > globals.failAt) &&
        (globals.failing != 0))
        Memento_startFailing();
    return i;
}

size_t Memento_setMax(size_t max)
{
    globals.maxMemory = max;
    return max;
}
#endif /* MEMENTO_CPP_EXTRAS_ONLY */

#ifdef __cplusplus
/* Dumb overrides for the new and delete operators */

void *operator new(size_t size)
{
    if (size == 0)
        size = 1;
    return do_malloc(size, Memento_EventType_new);
}

void  operator delete(void *pointer)
{
    return do_free(pointer, Memento_EventType_delete);
}

/* Some C++ systems (apparently) don't provide new[] or delete[]
 * operators. Provide a way to cope with this */
#ifndef MEMENTO_CPP_NO_ARRAY_CONSTRUCTORS
void *operator new[](size_t size)
{
    if (size == 0)
        size = 1;
    return do_malloc(size, Memento_EventType_newArray);
}

void  operator delete[](void *pointer)
{
    return do_free(pointer, Memento_EventType_deleteArray);
}
#endif /* MEMENTO_CPP_NO_ARRAY_CONSTRUCTORS */
#endif /* __cplusplus */

#else

/* Just in case anyone has left some debugging code in... */
void (Memento_breakpoint)(void)
{
}

int (Memento_checkBlock)(void *b)
{
    return 0;
}

int (Memento_checkAllMemory)(void)
{
    return 0;
}

int (Memento_check)(void)
{
    return 0;
}

int (Memento_setParanoia)(int i)
{
    return 0;
}

int (Memento_paranoidAt)(int i)
{
    return 0;
}

int (Memento_breakAt)(int i)
{
    return 0;
}

int  (Memento_getBlockNum)(void *i)
{
    return 0;
}

int (Memento_find)(void *a)
{
    return 0;
}

int (Memento_failAt)(int i)
{
    return 0;
}

void (Memento_breakOnFree)(void *a)
{
}

void (Memento_breakOnRealloc)(void *a)
{
}

#undef Memento_malloc
#undef Memento_free
#undef Memento_realloc
#undef Memento_calloc

void *Memento_malloc(size_t size)
{
    return MEMENTO_UNDERLYING_MALLOC(size);
}

void Memento_free(void *b)
{
    MEMENTO_UNDERLYING_FREE(b);
}

void *Memento_realloc(void *b, size_t s)
{
    return MEMENTO_UNDERLYING_REALLOC(b, s);
}

void *Memento_calloc(size_t n, size_t s)
{
    return MEMENTO_UNDERLYING_CALLOC(n, s);
}

void (Memento_listBlocks)(void)
{
}

void (Memento_listNewBlocks)(void)
{
}

size_t (Memento_setMax)(size_t max)
{
    return 0;
}

void (Memento_stats)(void)
{
}

void *(Memento_label)(void *ptr, const char *label)
{
    return ptr;
}

void (Memento_info)(void *addr)
{
}

void (Memento_listBlockInfo)(void)
{
}

#endif
