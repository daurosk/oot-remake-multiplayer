// SPDX-License-Identifier: GPL-3.0-or-later
#include "guest_memory.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace CoopNx2navi {
namespace {

// A note header: name size 4, descriptor size 20, type 3 (NT_GNU_BUILD_ID),
// then the name "GNU\0". The 20 bytes after it are the build id.
constexpr std::array<std::uint8_t, 16> kNote = {0x04, 0, 0, 0, 0x14, 0, 0, 0,
                                                0x03, 0, 0, 0, 'G',  'N', 'U', 0};
constexpr std::size_t kIdBytes = 20;

// User addresses only: below the first 64 KiB nothing is ever mapped, and
// above 2^47 is the kernel's.
constexpr std::uint64_t kLowest = 0x10000;
constexpr std::uint64_t kHighest = 0x7fffffffffffull;

bool InRange(std::uint64_t address, std::size_t size) {
    return address >= kLowest && address <= kHighest && size <= kHighest - address;
}

std::string Hex(const std::uint8_t* bytes, std::size_t count) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 15]);
    }
    return out;
}

}  // namespace

std::vector<std::string> FindBuildIds(std::span<const std::uint8_t> bytes) {
    std::vector<std::string> found;
    auto at = bytes.begin();
    while (true) {
        at = std::search(at, bytes.end(), kNote.begin(), kNote.end());
        if (at == bytes.end()) {
            break;
        }
        const auto id = at + static_cast<std::ptrdiff_t>(kNote.size());
        if (static_cast<std::size_t>(bytes.end() - id) >= kIdBytes) {
            found.push_back(Hex(&*id, kIdBytes));
        }
        ++at;
    }
    return found;
}

#ifdef _WIN32

namespace {

// Committed and open to reading. Asked first, because a kernel read of a
// guard page uses the guard up, and a thread's stack cannot grow without it.
bool Readable(std::uint64_t address, std::size_t size) {
    std::uint64_t at = address;
    const std::uint64_t end = address + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        const DWORD protect = info.Protect & 0xff;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 ||
            protect == PAGE_NOACCESS || protect == 0) {
            return false;
        }
        at = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

}  // namespace

bool SafeRead(std::uint64_t address, void* out, std::size_t size) {
    if (size == 0) {
        return true;
    }
    if (!InRange(address, size) || !Readable(address, size)) {
        return false;
    }
    // The page can still go away between the question and the read; the
    // kernel's copy then fails instead of faulting.
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size,
                             &done) != 0 &&
           done == size;
}

namespace {

// Committed, and plainly readable and writable: not code, not read-only data,
// not a guard page.
bool Writable(std::uint64_t address, std::size_t size) {
    if (!InRange(address, size)) {
        return false;
    }
    std::uint64_t at = address;
    const std::uint64_t end = address + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE) {
            return false;
        }
        at = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

// How many bytes are committed and readable from `base` on, at most `cap`.
std::uint64_t CommittedFrom(std::uint64_t base, std::uint64_t cap) {
    std::uint64_t at = base;
    while (at - base < cap) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info)) {
            break;
        }
        const DWORD protect = info.Protect & 0xff;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS ||
            protect == 0) {
            break;
        }
        at = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return std::min(at - base, cap);
}

}  // namespace

MainCheck CheckMain(std::uint64_t base, const std::string& expected_id) {
    MainCheck check;
    // main of the build co-op knows is about 80 MiB; a quarter of a GiB is
    // room for any build, and keeps the search short when modules follow it.
    constexpr std::uint64_t kCap = 256ull << 20;
    check.size = InRange(base, 8) ? CommittedFrom(base, kCap) : 0;
    if (check.size < 0x1000) {
        check.state = MainCheck::State::not_mapped;
        return check;
    }
    std::uint32_t header = 0;
    char magic[4] = {};
    if (!SafeRead(base + 4, &header, sizeof(header)) || header == 0 || header > check.size - 4 ||
        !SafeRead(base + header, magic, sizeof(magic)) || std::memcmp(magic, "MOD0", 4) != 0) {
        check.state = MainCheck::State::no_header;
        return check;
    }
    check.state = MainCheck::State::no_build_id;
    // In pieces, each starting a little before the last one ended, so a note
    // across two pieces is still found whole.
    constexpr std::size_t kPiece = 1 << 20;
    constexpr std::size_t kOverlap = kNote.size() + kIdBytes;
    std::vector<std::uint8_t> piece(kPiece + kOverlap);
    for (std::uint64_t at = 0; at < check.size; at += kPiece) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(piece.size(), check.size - at));
        if (!SafeRead(base + at, piece.data(), want)) {
            continue;  // freed or protected since the query: skip that piece
        }
        for (const std::string& id : FindBuildIds({piece.data(), want})) {
            const std::size_t n = std::min(id.size(), expected_id.size());
            if (n >= 16 && id.compare(0, n, expected_id, 0, n) == 0) {
                check.state = MainCheck::State::ok;
                check.build_id = id;
                return check;
            }
            if (check.build_id.empty()) {
                check.state = MainCheck::State::other_build;
                check.build_id = id;
            }
        }
    }
    return check;
}

const Memory::Slot* Memory::Page(std::uint64_t page) {
    Slot& slot = slots_[(page / kPage) % kSlots];
    if (slot.generation != generation_ || slot.page != page) {
        slot.page = page;
        slot.generation = generation_;
        slot.readable = SafeRead(page, slot.bytes.data(), kPage);
    }
    return slot.readable ? &slot : nullptr;
}

bool Memory::read(std::uint64_t address, std::span<std::uint8_t> out) {
    ++reads_;
    bool ok = false;
    if (InRange(address, out.size())) {
        const std::uint64_t page = address & ~static_cast<std::uint64_t>(kPage - 1);
        if (address + out.size() <= page + kPage) {
            if (const Slot* slot = Page(page)) {
                std::memcpy(out.data(), slot->bytes.data() + (address - page), out.size());
                ok = true;
            }
        } else {
            ok = SafeRead(address, out.data(), out.size());
        }
    }
    if (!ok) {
        ++failed_;
    }
    return ok;
}

bool Memory::write(std::uint64_t address, std::span<const std::uint8_t> data) {
    Fresh();
    if (data.empty()) {
        return true;
    }
    if (!Writable(address, data.size())) {
        return false;
    }
    SIZE_T done = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(address), data.data(),
                              data.size(), &done) != 0 &&
           done == data.size();
}

std::optional<std::uint32_t> Memory::compare_exchange_u32(std::uint64_t address,
                                                          std::uint32_t expected,
                                                          std::uint32_t desired) {
    Fresh();
    if ((address & 3) != 0 || !Writable(address, sizeof(std::uint32_t))) {
        return std::nullopt;
    }
    // The page was read-write a moment ago. The game does not give back the
    // memory its flag manager lives in while it runs, which is the only place
    // coop/host uses this.
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address),
                                   static_cast<LONG>(desired), static_cast<LONG>(expected)));
}

#endif  // _WIN32

}  // namespace CoopNx2navi
