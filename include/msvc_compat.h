#pragma once

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>
#include <string.h>

#ifndef __thread
#define __thread __declspec(thread)
#endif

/* The runtime uses these GCC/Clang builtins in portable hot-path code. */
#ifndef __builtin_expect
#define __builtin_expect(expression, expected) (expression)
#endif

#ifndef __builtin_clz
static __inline unsigned int psr_builtin_clz(unsigned int value) {
    unsigned long index;
    _BitScanReverse(&index, value);
    return 31u - (unsigned int)index;
}
#define __builtin_clz(value) psr_builtin_clz((unsigned int)(value))
#endif

#ifndef __builtin_strcmp
#define __builtin_strcmp strcmp
#endif

#ifndef __ATOMIC_RELAXED
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_SEQ_CST 5
#endif

/* MSVC has Interlocked intrinsics rather than GCC's __atomic builtins. */
#ifndef __atomic_fetch_add
#define __atomic_fetch_add(pointer, value, order) \
    (sizeof(*(pointer)) == 8 \
        ? (unsigned __int64)_InterlockedExchangeAdd64( \
            (volatile long long*)(pointer), (long long)(value)) \
        : (unsigned __int64)_InterlockedExchangeAdd( \
            (volatile long*)(pointer), (long)(value)))
#endif

#ifndef __atomic_add_fetch
#define __atomic_add_fetch(pointer, value, order) \
    (__atomic_fetch_add((pointer), (value), (order)) + (value))
#endif

#ifndef __atomic_load_n
#define __atomic_load_n(pointer, order) \
    (sizeof(*(pointer)) == 8 \
        ? (unsigned __int64)_InterlockedCompareExchange64( \
            (volatile long long*)(pointer), 0, 0) \
        : (unsigned __int64)_InterlockedCompareExchange( \
            (volatile long*)(pointer), 0, 0))
#endif

#ifndef __atomic_store_n
#define __atomic_store_n(pointer, value, order) do { \
    if (sizeof(*(pointer)) == 8) { \
        _InterlockedExchange64((volatile long long*)(pointer), (long long)(value)); \
    } else { \
        _InterlockedExchange((volatile long*)(pointer), (long)(value)); \
    } \
} while (0)
#endif

#ifndef __atomic_thread_fence
#define __atomic_thread_fence(order) _ReadWriteBarrier()
#endif

#endif
