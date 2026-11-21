/* Shim de libkern/OSAtomic.h (Mac) usando builtins do clang. */
#pragma once
#include <stdint.h>
static inline int32_t OSAtomicIncrement32(volatile int32_t *p) { return __sync_add_and_fetch(p, 1); }
static inline int32_t OSAtomicDecrement32(volatile int32_t *p) { return __sync_sub_and_fetch(p, 1); }
static inline int32_t OSAtomicAdd32(int32_t v, volatile int32_t *p) { return __sync_add_and_fetch(p, v); }
static inline int64_t OSAtomicIncrement64(volatile int64_t *p) { return __sync_add_and_fetch(p, 1); }
static inline int64_t OSAtomicDecrement64(volatile int64_t *p) { return __sync_sub_and_fetch(p, 1); }
static inline int64_t OSAtomicAdd64(int64_t v, volatile int64_t *p) { return __sync_add_and_fetch(p, v); }
static inline int OSAtomicCompareAndSwap32(int32_t o, int32_t n, volatile int32_t *p) { return __sync_bool_compare_and_swap(p, o, n); }
static inline int OSAtomicCompareAndSwap64(int64_t o, int64_t n, volatile int64_t *p) { return __sync_bool_compare_and_swap(p, o, n); }
static inline int OSAtomicCompareAndSwapPtr(void *o, void *n, void *volatile *p) { return __sync_bool_compare_and_swap(p, o, n); }
static inline int32_t OSAtomicOr32(uint32_t v, volatile uint32_t *p) { return __sync_or_and_fetch(p, v); }
static inline int32_t OSAtomicAnd32(uint32_t v, volatile uint32_t *p) { return __sync_and_and_fetch(p, v); }
