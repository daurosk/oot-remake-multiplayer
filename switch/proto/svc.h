// Kernel calls the prototype makes itself (no libnx, no SDK headers).
// Numbers and register use as in the Horizon kernel ABI (switchbrew.org/wiki/SVC).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace svc {

struct MemoryInfo {
    uint64_t addr;
    uint64_t size;
    uint32_t state;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
};

inline constexpr uint32_t kCurrentProcess = 0xFFFF8001;  // pseudo handle
inline constexpr uint32_t kPermR = 1;
inline constexpr uint32_t kPermRW = 3;
inline constexpr uint32_t kPermRX = 5;

// 0x06 QueryMemory(MemoryInfo* out, u32* page_info, u64 addr)
inline uint32_t QueryMemory(MemoryInfo* out, uint64_t addr) {
    register uint64_t x0 __asm__("x0") = reinterpret_cast<uint64_t>(out);
    register uint64_t x1 __asm__("x1");
    register uint64_t x2 __asm__("x2") = addr;
    __asm__ volatile("svc 0x06" : "+r"(x0), "=r"(x1), "+r"(x2) : : "memory");
    return static_cast<uint32_t>(x0);
}

// 0x27 OutputDebugString(const char* s, u64 len): the emulator logs it.
inline uint32_t OutputDebugString(const char* s, size_t len) {
    register uint64_t x0 __asm__("x0") = reinterpret_cast<uint64_t>(s);
    register uint64_t x1 __asm__("x1") = len;
    __asm__ volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory");
    return static_cast<uint32_t>(x0);
}

// 0x73 SetProcessMemoryPermission(handle, addr, size, perm)
inline uint32_t SetProcessMemoryPermission(uint32_t handle, uint64_t addr, uint64_t size,
                                           uint32_t perm) {
    register uint64_t x0 __asm__("x0") = handle;
    register uint64_t x1 __asm__("x1") = addr;
    register uint64_t x2 __asm__("x2") = size;
    register uint64_t x3 __asm__("x3") = perm;
    __asm__ volatile("svc 0x73" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "memory");
    return static_cast<uint32_t>(x0);
}

}  // namespace svc
