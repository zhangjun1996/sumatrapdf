/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef BaseUtil_h
#define BaseUtil_h

/* OS_DARWIN - Any Darwin-based OS, including Mac OS X and iPhone OS */
#ifdef __APPLE__
#define OS_DARWIN 1
#else
#define OS_DARWIN 0
#endif

/* OS_LINUX - Linux */
#ifdef __linux__
#define OS_LINUX 1
#else
#define OS_LINUX 0
#endif

#if defined(WIN32) || defined(_WIN32)
#define OS_WIN 1
#else
#define OS_WIN 0
#endif

/* OS_UNIX - Any Unix-like system */
#if OS_DARWIN || OS_LINUX || defined(unix) || defined(__unix) || defined(__unix__)
#define OS_UNIX 1
#endif

#if defined(_MSC_VER)
#define COMPILER_MSVC 1
#else
#define COMPILER_MSVC 0
#endif

#if defined(__GNUC__)
#define COMPILER_GCC 1
#else
#define COMPILER_GCC 0
#endif

#if defined(__clang__)
#define COMPILER_CLANG 1
#else
#define COMPILER_CLAGN 0
#endif

#if defined(__MINGW32__)
#define COMPILER_MINGW 1
#else
#define COMPILER_MINGW 0
#endif

#if OS_WIN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#endif

#include "BuildConfig.h"

#if OS_WIN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <windowsx.h>
#include <winsafer.h>
#include <wininet.h>

// nasty but necessary
#if defined(min) || defined(max)
#error "min or max defined"
#endif
#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))
#include <gdiplus.h>
#undef NOMINMAX
#undef min
#undef max

#include <io.h>

#ifdef DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
// TODO: this breaks placement new but without this we
// don't get leaked memory allocation source
#define DEBUG_NEW new (_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

#endif

// Most common C includes
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <float.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>
#include <errno.h>

#include <fcntl.h>

#define _USE_MATH_DEFINES
#include <math.h>

// most common c++ includes
#include <cstdint>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <array>
#include <vector>
#include <limits>
//#include <iostream>
//#include <locale>

typedef uint8_t u8;

// TODO: don't use INT_MAX and UINT_MAX
#ifndef INT_MAX
#define INT_MAX std::numeric_limits<int>::max()
#endif

#ifndef UINT_MAX
#define UINT_MAX std::numeric_limits<unsigned int>::max()
#endif

#if COMPILER_MSVC
#define NO_INLINE __declspec(noinline)
#else
// assuming gcc or similar
#define NO_INLINE __attribute__((noinline))
#endif

#define NoOp() ((void)0)
#define dimof(array) (sizeof(DimofSizeHelper(array)))
template <typename T, size_t N>
char (&DimofSizeHelper(T (&array)[N]))[N];

// like dimof minus 1 to account for terminating 0
#define static_strlen(array) (sizeof(DimofSizeHelper(array)) - 1)

// UNUSED is for marking unreferenced function arguments/variables
// UNREFERENCED_PARAMETER is in windows SDK but too long. We use it if available,
// otherwise we define our own version.
// UNUSED might already be defined by mupdf\fits\system.h
#if !defined(UNUSED)
#if defined(UNREFERENCED_PARAMETER)
#define UNUSED UNREFERENCED_PARAMETER
#else
#define UNUSED(P) ((void)P)
#endif
#endif

#if COMPILER_MSVC
// https://msdn.microsoft.com/en-us/library/4dt9kyhy.aspx
// enable msvc equivalent of -Wundef gcc option, warns when doing "#if FOO" and FOO is not defined
// can't be turned on globally because windows headers have those
#pragma warning(default : 4668)
#endif

// TODO: is there a better way?
#if COMPILER_MSVC
#define IS_UNUSED
#else
#define IS_UNUSED __attribute__((unused))
#endif

#if COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 6011) // silence /analyze: de-referencing a nullptr pointer
#endif
// Note: it's inlined to make it easier on crash reports analyzer (if wasn't inlined
// CrashMe() would show up as the cause of several different crash sites)
//
// Note: I tried doing this via RaiseException(0x40000015, EXCEPTION_NONCONTINUABLE, 0, 0);
// but it seemed to confuse callstack walking
inline void CrashMe() {
    char* p = nullptr;
    *p = 0;
}
#if COMPILER_MSVC
#pragma warning(pop)
#endif

// CrashIf() is like assert() except it crashes in debug and pre-release builds.
// The idea is that assert() indicates "can't possibly happen" situation and if
// it does happen, we would like to fix the underlying cause.
// In practice in our testing we rarely get notified when an assert() is triggered
// and they are disabled in builds running on user's computers.
// Now that we have crash reporting, we can get notified about such cases if we
// use CrashIf() instead of assert(), which we should be doing from now on.
//
// Enabling it in pre-release builds but not in release builds is trade-off between
// shipping small executables (each CrashIf() adds few bytes of code) and having
// more testing on user's machines and not only in our personal testing.
// To crash uncoditionally use CrashAlwaysIf(). It should only be used in
// rare cases where we really want to know a given condition happens. Before
// each release we should audit the uses of CrashAlawysIf()
//
// Just as with assert(), the condition is not guaranteed to be executed
// in some builds, so it shouldn't contain the actual logic of the code

inline void CrashIfFunc(bool cond) {
#if defined(SVN_PRE_RELEASE_VER) || defined(DEBUG)
    if (cond) {
        CrashMe();
    }
#else
    UNUSED(cond);
#endif
}

// Sometimes we want to assert only in debug build (not in pre-release)
inline void CrashIfDebugOnlyFunc(bool cond) {
#if defined(DEBUG)
    if (cond) {
        CrashMe();
    }
#else
    UNUSED(cond);
#endif
}

#if COMPILER_MSVC
#define while_0_nowarn __pragma(warning(push)) __pragma(warning(disable : 4127)) while (0) __pragma(warning(pop))
#else
#define while_0_nowarn while (0)
#endif

// __analysis_assume is defined by msvc for prefast analysis
#if !defined(__analysis_assume)
#define __analysis_assume(x)
#endif

#define CrashIfDebugOnly(cond)      \
    do {                            \
        __analysis_assume(!(cond)); \
        CrashIfDebugOnlyFunc(cond); \
    }                               \
    while_0_nowarn

#define CrashAlwaysIf(cond)         \
    do {                            \
        __analysis_assume(!(cond)); \
        if (cond) {                 \
            CrashMe();              \
        }                           \
    }                               \
    while_0_nowarn

#define CrashIf(cond)               \
    do {                            \
        __analysis_assume(!(cond)); \
        CrashIfFunc(cond);          \
    }                               \
    while_0_nowarn

// AssertCrash is like assert() but crashes like CrashIf()
// It's meant to make converting assert() easier (converting to
// CrashIf() requires inverting the condition, which can introduce bugs)
#define AssertCrash(cond)        \
    do {                         \
        __analysis_assume(cond); \
        CrashIfFunc(!(cond));    \
    }                            \
    while_0_nowarn

#if !OS_WIN
void ZeroMemory(void* p, size_t len);
#endif

template <typename T>
inline T* AllocArray(size_t n) {
    return (T*)calloc(n, sizeof(T));
}

template <typename T>
inline T* AllocStruct() {
    return (T*)calloc(1, sizeof(T));
}

template <typename T>
inline void ZeroStruct(T* s) {
    ZeroMemory((void*)s, sizeof(T));
}

template <typename T>
inline void ZeroArray(T& a) {
    size_t size = sizeof(a);
    ZeroMemory((void*)&a, size);
}

template <typename T>
inline T limitValue(T val, T min, T max) {
    CrashIf(min > max);
    if (val < min)
        return min;
    if (val > max)
        return max;
    return val;
}

// return true if adding n to val overflows. Only valid for n > 0
template <typename T>
inline bool addOverflows(T val, T n) {
    CrashIf(n <= 0);
    T res = val + n;
    return val > res;
}

void* memdup(const void* data, size_t len);
bool memeq(const void* s1, const void* s2, size_t len);

size_t RoundToPowerOf2(size_t size);
uint32_t MurmurHash2(const void* key, size_t len);

size_t RoundUp(size_t n, size_t rounding);
int RoundUp(int n, int rounding);

template <typename T>
void ListInsert(T** root, T* el) {
    el->next = *root;
    *root = el;
}

template <typename T>
bool ListRemove(T** root, T* el) {
    T** currPtr = root;
    T* curr;
    for (;;) {
        curr = *currPtr;
        if (!curr)
            return false;
        if (curr == el)
            break;
        currPtr = &(curr->next);
    }
    *currPtr = el->next;
    return true;
}

// Base class for allocators that can be provided to Vec class
// (and potentially others). Needed because e.g. in crash handler
// we want to use Vec but not use standard malloc()/free() functions
class Allocator {
  public:
    Allocator() {}
    virtual ~Allocator(){};
    virtual void* Alloc(size_t size) = 0;
    virtual void* Realloc(void* mem, size_t size) = 0;
    virtual void Free(void* mem) = 0;

    // helper functions that fallback to malloc()/free() if allocator is nullptr
    // helps write clients where allocator is optional
    static void* Alloc(Allocator* a, size_t size);

    template <typename T>
    static T* Alloc(Allocator* a, size_t n = 1) {
        size_t size = n * sizeof(T);
        return (T*)AllocZero(a, size);
    }

    static void* AllocZero(Allocator* a, size_t size);
    static void Free(Allocator* a, void* p);
    static void* Realloc(Allocator* a, void* mem, size_t size);
    static void* MemDup(Allocator* a, const void* mem, size_t size, size_t padding = 0);
    static char* StrDup(Allocator* a, const char* str);
    static std::string AllocString(Allocator* a, std::string str);

#if OS_WIN
    static WCHAR* StrDup(Allocator* a, const WCHAR* str);
#endif
};

// PoolAllocator is for the cases where we need to allocate pieces of memory
// that are meant to be freed together. It simplifies the callers (only need
// to track this object and not all allocated pieces). Allocation and freeing
// is faster. The downside is that free() is a no-op i.e. it can't free memory
// for re-use.
//
// Note: we could be a bit more clever here by allocating data in 4K chunks
// via VirtualAlloc() etc. instead of malloc(), which would lower the overhead
class PoolAllocator : public Allocator {
    // we'll allocate block of the minBlockSize unless
    // asked for a block of bigger size
    size_t minBlockSize = 4096;
    size_t allocRounding = 8;

    struct MemBlockNode {
        struct MemBlockNode* next;
        size_t size;
        size_t free;

        size_t Used() { return size - free; }
        char* DataStart() { return (char*)this + sizeof(MemBlockNode); }
        // data follows here
    };

    MemBlockNode* currBlock = nullptr;
    MemBlockNode* firstBlock = nullptr;

  public:
    explicit PoolAllocator() {}

    void SetMinBlockSize(size_t newMinBlockSize);
    void SetAllocRounding(size_t newRounding);
    void FreeAll();
    virtual ~PoolAllocator() override;
    void AllocBlock(size_t minSize);

    // Allocator methods
    virtual void* Realloc(void* mem, size_t size) override;

    virtual void Free(void*) override {
        // does nothing, we can't free individual pieces of memory
    }

    virtual void* Alloc(size_t size) override;

    void* FindNthPieceOfSize(size_t size, size_t n) const;

    // only valid for structs, could alloc objects with
    // placement new()
    template <typename T>
    T* AllocStruct() {
        return (T*)Alloc(sizeof(T));
    }

    // Iterator for easily traversing allocated memory as array
    // of values of type T. The caller has to enforce the fact
    // that the values stored are indeed values of T
    // cf. http://www.cprogramming.com/c++11/c++11-ranged-for-loop.html
    template <typename T>
    class Iter {
        MemBlockNode* block;
        size_t blockPos;

      public:
        Iter(MemBlockNode* block) : block(block), blockPos(0) {
            CrashIf(block && (block->Used() % sizeof(T)) != 0);
            CrashIf(block && block->Used() == 0);
        }

        bool operator!=(const Iter& other) const { return block != other.block || blockPos != other.blockPos; }
        T& operator*() const { return *(T*)(block->DataStart() + blockPos); }
        Iter& operator++() {
            blockPos += sizeof(T);
            if (block->Used() == blockPos) {
                block = block->next;
                blockPos = 0;
                CrashIf(block && block->Used() == 0);
            }
            return *this;
        }
    };

    template <typename T>
    Iter<T> begin() {
        return Iter<T>(firstBlock);
    }
    template <typename T>
    Iter<T> end() {
        return Iter<T>(nullptr);
    }
};

// A helper for allocating an array of elements of type T
// either on stack (if they fit within StackBufInBytes)
// or in memory. Allocating on stack is a perf optimization
// note: not the best name
template <typename T, int StackBufInBytes>
class FixedArray {
    T stackBuf[StackBufInBytes / sizeof(T)];
    T* memBuf;

  public:
    explicit FixedArray(size_t elCount) {
        memBuf = nullptr;
        size_t stackEls = StackBufInBytes / sizeof(T);
        if (elCount > stackEls)
            memBuf = (T*)malloc(elCount * sizeof(T));
    }

    ~FixedArray() { free(memBuf); }

    T* Get() {
        if (memBuf)
            return memBuf;
        return &(stackBuf[0]);
    }
};

// OwnedData is for returning data. It combines pointer to data and size.
// It owns the data i.e. frees it in destructor.
// It cannot be copied, only moved, so that it's clear that ownership of
// data is being passed.
class OwnedData {
  public:
    char* data = nullptr;
    size_t size = 0;

    OwnedData() {}
    // takes ownership of data
    OwnedData(const char* data, size_t size = 0);
    ~OwnedData();

    OwnedData(const OwnedData& other) = delete;
    OwnedData& operator=(const OwnedData& other) = delete;

    OwnedData& operator=(OwnedData&& other);
    OwnedData(OwnedData&& other);

    bool IsEmpty();
    void Clear();
    void TakeOwnership(const char* s, size_t size = 0);
    char* StealData();
    char* Get() const;
    std::string AsView() const;

    // creates a copy of s
    static OwnedData MakeFromStr(const char* s, size_t size = 0);
};

// MaybeOwnedData is for returning data that might be owned by this class.
// It combines pointer to data and size.
// It owns the data i.e. frees it in destructor.
// It cannot be copied, only moved, so that it's clear that ownership of
// data is being passed.
class MaybeOwnedData {
  public:
    char* data = nullptr;
    size_t size = 0;
    bool isOwned = false;

    MaybeOwnedData(){};
    MaybeOwnedData(char* data, size_t size, bool isOwned);
    ~MaybeOwnedData();

    MaybeOwnedData(const MaybeOwnedData& other) = delete;
    MaybeOwnedData& operator=(const MaybeOwnedData& other) = delete;

    MaybeOwnedData& operator=(MaybeOwnedData&& other);
    MaybeOwnedData(MaybeOwnedData&& other);

    void Set(char* s, size_t len, bool isOwned);
    void freeIfOwned();
    OwnedData StealData();
};

// from https://pastebin.com/3YvWQa5c
#define CONCAT_INTERNAL(x, y) x##y
#define CONCAT(x, y) CONCAT_INTERNAL(x, y)

template <typename T>
struct ExitScope {
    T lambda;
    ExitScope(T lambda) : lambda(lambda) {}
    ~ExitScope() { lambda(); }
    ExitScope(const ExitScope&);

  private:
    ExitScope& operator=(const ExitScope&);
};

class ExitScopeHelp {
  public:
    template <typename T>
    ExitScope<T> operator+(T t) {
        return t;
    }
};

#define defer const auto& CONCAT(defer__, __LINE__) = ExitScopeHelp() + [&]()

/* How to use:
defer { free(tools_filename); };
defer { fclose(f); };
defer { instance->Release(); };
*/

#include "GeomUtil.h"
#include "StrUtil.h"
#include "Scoped.h"
#include "Vec.h"

#if OS_WIN
/* In debug mode, VS 2010 instrumentations complains about GetRValue() etc.
This adds equivalent functions that don't have this problem and ugly
substitutions to make sure we don't use Get*Value() in the future */
BYTE GetRValueSafe(COLORREF rgb);
BYTE GetGValueSafe(COLORREF rgb);
BYTE GetBValueSafe(COLORREF rgb);

#undef GetRValue
#define GetRValue UseGetRValueSafeInstead
#undef GetGValue
#define GetGValue UseGetGValueSafeInstead
#undef GetBValue
#define GetBValue UseGetBValueSafeInstead
#endif

#ifdef lstrcpy
#undef lstrcpy
#define lstrcpy dont_use_lstrcpy
#endif

#endif
