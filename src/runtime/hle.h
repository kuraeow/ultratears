// Registration of host implementations for guest symbols (from LASRecomp runtime/hle.h, MIT).
#pragma once
#include "rt.h"

// A host implementation of a guest symbol: either a function on the guest registers (HLE_FUNC), or an ordinary C
// function with integer/pointer arguments (HLE_NATIVE), called with x0..x7 and its result put in x0.
typedef struct { const char *name; HostFn fn; void *native; char ret; } HleImplEntry;
// HLE_NATIVE result kinds: 'v' void, 'b' bool/u8, 'w' 32-bit (int, unsigned, Result), 'x' 64-bit or pointer.
// Win64 passes the first four integer arguments in registers and the rest on the stack, so calling any
// integer/pointer-only function as fn(u64 x 8) is correct; float/double arguments or results need HLE_FUNC.
void hle_native_thunk(Cpu *c);

// HLE_FUNC("mangled_symbol", c_function) binds a guest import to a host function.
#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
#if defined(__linux__)
#define HLE_SECTION "hle_impls"  // ELF: the linker defines __start_ / __stop_ for C-identifier section names
#else
#define HLE_SECTION ".hle$m"  // PE/COFF: between the .hle$a / .hle$z markers in dispatch.c
#endif
#define HLE_FUNC(sym, fn)                                                                   \
    static void fn(Cpu *c);                                                                 \
    __attribute__((used, section(HLE_SECTION))) static const HleImplEntry                   \
        HLE_CAT(hle_entry_, __LINE__) = {sym, fn, 0, 0};                                    \
    static void fn(Cpu *c)

#define HLE_NATIVE(sym, cfn, kind)                                                          \
    __attribute__((used, section(HLE_SECTION))) static const HleImplEntry                   \
        HLE_CAT(hle_entry_, __LINE__) = {sym, hle_native_thunk, (void *)(cfn), kind};
// The same for a call that waits for the disk or the OS (nn::fs): on the Switch it is an IPC request that blocks its
// thread, so the thread gives its virtual core up for the call (host/threads.c runtime_core_away) and the game's
// other threads run meanwhile. The function must not hold a host lock when it returns. (The kind is stored in capitals.)
#define HLE_NATIVE_BLOCKING(sym, cfn, kind)                                                 \
    __attribute__((used, section(HLE_SECTION))) static const HleImplEntry                   \
        HLE_CAT(hle_entry_, __LINE__) = {sym, hle_native_thunk, (void *)(cfn), (char)((kind) - 32)};
