/* Atomic long long for state one thread writes and another reads, so the order holds and
 * TSan sees it. Loads acquire, stores release. */
#ifndef RANT_TEST_ATOMIC_H
#define RANT_TEST_ATOMIC_H

#if defined(_MSC_VER)
#include <intrin.h>
static inline long long test_atomic_load(const long long *p){
    return _InterlockedOr64((volatile long long *)p, 0);
}
static inline void test_atomic_store(long long *p, long long v){ _InterlockedExchange64(p, v); }
static inline long long test_atomic_add(long long *p, long long v){
    return _InterlockedExchangeAdd64(p, v) + v;
}
/* stores v when *p still holds old, 1 when it did */
static inline int test_atomic_cas(long long *p, long long old, long long v){
    return _InterlockedCompareExchange64(p, v, old) == old;
}
#else
static inline long long test_atomic_load(const long long *p){
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void test_atomic_store(long long *p, long long v){
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
static inline long long test_atomic_add(long long *p, long long v){
    return __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
}
static inline int test_atomic_cas(long long *p, long long old, long long v){
    return __atomic_compare_exchange_n(p, &old, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

#endif /* RANT_TEST_ATOMIC_H */
