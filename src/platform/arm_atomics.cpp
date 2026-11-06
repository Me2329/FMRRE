// Atomic operations for ARMv5 (Android "armeabi"): the CPU has no ldrex/strex, so
// compilers turn atomics into __sync_* library calls. Like libgcc's linux-atomic.c,
// they are built on the kernel user helpers mapped at the top of every ARM Linux
// process (__kuser_cmpxchg at 0xffff0fc0, __kuser_memory_barrier at 0xffff0fa0).
// Compiled only for ARM < v6; every other architecture has native atomics.
#if defined(__arm__) && defined(__linux__) && defined(__ARM_ARCH) && __ARM_ARCH < 6

#include <sched.h>
#include <stdint.h>

#include <type_traits>

namespace {

// Returns 0 when *ptr was changed from oldv to newv. Includes the barriers SMP needs.
inline int kcas(int oldv, int newv, volatile int* ptr) {
    return (*(int (*)(int, int, volatile int*))0xffff0fc0)(oldv, newv, ptr);
}
inline void kbarrier() { (*(void (*)())0xffff0fa0)(); }

// 1/2/4-byte compare-and-swap on the aligned word that contains the value (little-endian).
template <typename T>
T cas(volatile void* p, T oldv, T newv, bool& swapped) {
    uintptr_t a = (uintptr_t)p;
    volatile int* w = (volatile int*)(a & ~uintptr_t(3));
    unsigned shift = sizeof(T) == 4 ? 0 : (a & 3) * 8;
    unsigned mask = sizeof(T) == 4 ? ~0u : ((1u << (8 * sizeof(T))) - 1) << shift;
    for (;;) {
        unsigned cur = (unsigned)*w;
        T curv = T((cur & mask) >> shift);
        if (curv != oldv) {
            kbarrier();
            swapped = false;
            return curv;
        }
        unsigned next = (cur & ~mask) | ((unsigned(newv) << shift) & mask);
        if (kcas((int)cur, (int)next, w) == 0) {
            swapped = true;
            return curv;
        }
    }
}

template <typename T, typename F>
T rmw(volatile void* p, F op, bool returnNew) {
    for (;;) {
        T old = *(volatile T*)p, nv = op(old);
        bool ok;
        cas<T>(p, old, nv, ok);
        if (ok) return returnNew ? nv : old;
    }
}

// 8-byte operations: a global lock built on the 4-byte helper (works on every kernel,
// unlike __kuser_cmpxchg64 which needs Linux 3.1+).
volatile int g_lock64 = 0;
struct Lock64 {
    Lock64() { while (kcas(0, 1, &g_lock64) != 0) sched_yield(); }
    ~Lock64() { kbarrier(); g_lock64 = 0; }
};

template <typename F>
uint64_t rmw64(volatile void* p, F op, bool returnNew) {
    Lock64 l;
    volatile uint64_t* q = (volatile uint64_t*)p;
    uint64_t old = *q, nv = op(old);
    *q = nv;
    return returnNew ? nv : old;
}

}  // namespace

// The names are compiler builtins, so they are defined through asm labels.
#define TW_SYNC_CAS(N, T)                                                                                    \
    extern "C" T tw_val_cas_##N(volatile void* p, T o, T n) __asm__("__sync_val_compare_and_swap_" #N);   \
    extern "C" T tw_val_cas_##N(volatile void* p, T o, T n) { bool s; return cas<T>(p, o, n, s); }           \
    extern "C" bool tw_bool_cas_##N(volatile void* p, T o, T n) __asm__("__sync_bool_compare_and_swap_" #N); \
    extern "C" bool tw_bool_cas_##N(volatile void* p, T o, T n) { bool s; cas<T>(p, o, n, s); return s; }    \
    extern "C" T tw_tas_##N(volatile void* p, T v) __asm__("__sync_lock_test_and_set_" #N);                 \
    extern "C" T tw_tas_##N(volatile void* p, T v) { return rmw<T>(p, [v](T) { return v; }, false); }       \
    extern "C" void tw_rel_##N(volatile void* p) __asm__("__sync_lock_release_" #N);                        \
    extern "C" void tw_rel_##N(volatile void* p) { kbarrier(); *(volatile T*)p = 0; }

#define TW_SYNC_OP(N, T, NAME, EXPR)                                                                           \
    extern "C" T tw_fa_##NAME##_##N(volatile void* p, T v) __asm__("__sync_fetch_and_" #NAME "_" #N);        \
    extern "C" T tw_fa_##NAME##_##N(volatile void* p, T v) { return rmw<T>(p, [v](T o) { return T(EXPR); }, false); } \
    extern "C" T tw_af_##NAME##_##N(volatile void* p, T v) __asm__("__sync_" #NAME "_and_fetch_" #N);        \
    extern "C" T tw_af_##NAME##_##N(volatile void* p, T v) { return rmw<T>(p, [v](T o) { return T(EXPR); }, true); }

#define TW_SIGNED(x) ((std::make_signed_t<decltype(x)>)(x))
#define TW_SYNC_ALL(N, T)                                         \
    TW_SYNC_CAS(N, T)                                             \
    TW_SYNC_OP(N, T, add, o + v)                                  \
    TW_SYNC_OP(N, T, sub, o - v)                                  \
    TW_SYNC_OP(N, T, or, o | v)                                   \
    TW_SYNC_OP(N, T, and, o & v)                                  \
    TW_SYNC_OP(N, T, xor, o ^ v)                                  \
    TW_SYNC_OP(N, T, nand, ~(o & v))                              \
    TW_SYNC_OP(N, T, umin, o < v ? o : v)                         \
    TW_SYNC_OP(N, T, umax, o > v ? o : v)                         \
    TW_SYNC_OP(N, T, min, TW_SIGNED(o) < TW_SIGNED(v) ? o : v)    \
    TW_SYNC_OP(N, T, max, TW_SIGNED(o) > TW_SIGNED(v) ? o : v)

TW_SYNC_ALL(1, uint8_t)
TW_SYNC_ALL(2, uint16_t)
TW_SYNC_ALL(4, uint32_t)

// 8 bytes
extern "C" uint64_t tw_val_cas_8(volatile void* p, uint64_t o, uint64_t n) __asm__("__sync_val_compare_and_swap_8");
extern "C" uint64_t tw_val_cas_8(volatile void* p, uint64_t o, uint64_t n) {
    Lock64 l;
    volatile uint64_t* q = (volatile uint64_t*)p;
    uint64_t cur = *q;
    if (cur == o) *q = n;
    return cur;
}
extern "C" bool tw_bool_cas_8(volatile void* p, uint64_t o, uint64_t n) __asm__("__sync_bool_compare_and_swap_8");
extern "C" bool tw_bool_cas_8(volatile void* p, uint64_t o, uint64_t n) { return tw_val_cas_8(p, o, n) == o; }
extern "C" uint64_t tw_tas_8(volatile void* p, uint64_t v) __asm__("__sync_lock_test_and_set_8");
extern "C" uint64_t tw_tas_8(volatile void* p, uint64_t v) { return rmw64(p, [v](uint64_t) { return v; }, false); }
#define TW_SYNC_OP8(NAME, EXPR)                                                                                        \
    extern "C" uint64_t tw_fa_##NAME##_8(volatile void* p, uint64_t v) __asm__("__sync_fetch_and_" #NAME "_8");      \
    extern "C" uint64_t tw_fa_##NAME##_8(volatile void* p, uint64_t v) { return rmw64(p, [v](uint64_t o) { return EXPR; }, false); } \
    extern "C" uint64_t tw_af_##NAME##_8(volatile void* p, uint64_t v) __asm__("__sync_" #NAME "_and_fetch_8");      \
    extern "C" uint64_t tw_af_##NAME##_8(volatile void* p, uint64_t v) { return rmw64(p, [v](uint64_t o) { return EXPR; }, true); }
TW_SYNC_OP8(add, o + v)
TW_SYNC_OP8(sub, o - v)
TW_SYNC_OP8(or, o | v)
TW_SYNC_OP8(and, o & v)
TW_SYNC_OP8(xor, o ^ v)
TW_SYNC_OP8(nand, ~(o & v))
TW_SYNC_OP8(umin, o < v ? o : v)
TW_SYNC_OP8(umax, o > v ? o : v)
TW_SYNC_OP8(min, TW_SIGNED(o) < TW_SIGNED(v) ? o : v)
TW_SYNC_OP8(max, TW_SIGNED(o) > TW_SIGNED(v) ? o : v)

extern "C" void tw_sync_synchronize() __asm__("__sync_synchronize");
extern "C" void tw_sync_synchronize() { kbarrier(); }

#endif
