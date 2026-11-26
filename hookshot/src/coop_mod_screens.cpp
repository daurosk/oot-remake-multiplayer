// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_mod_screens.h"

#include "coop_mod_api.h"

namespace CoopModScreens {

namespace {

// Addresses below 1 MB (a null pointer's page) read as 0, as patch 0028's peek.
template <class T>
T Peek(const Reader& read, std::uint64_t a) {
    T v{};
    if (a < 0x100000 || !read(a, &v, sizeof v)) {
        return T{};
    }
    return v;
}

}  // namespace

std::uint32_t KindOf(std::uint64_t vtable_offset) {
    switch (vtable_offset) {
    case kTitleVtable: return COOP_MOD_SCREEN_TITLE;
    case kLoadListVtable: return COOP_MOD_SCREEN_LOAD_LIST;
    case kLoadQuestionVtable: return COOP_MOD_SCREEN_LOAD_QUESTION;
    default: return COOP_MOD_SCREEN_OTHER;
    }
}

std::uint64_t FocusedButton(const Reader& read, std::uint64_t screen) {
    if (Peek<std::uint64_t>(read, Peek<std::uint64_t>(read, screen + kScreenFocus) + kFocusManager) == 0) {
        return 0;
    }
    const std::uint64_t head = screen + kListHead;
    const auto off = static_cast<std::int64_t>(Peek<std::int32_t>(read, screen + kListEntryOffset));
    std::uint64_t node = Peek<std::uint64_t>(read, screen + kListFirst), found = 0;
    for (int i = 0; i < kMaxButtons && node != 0 && node != head; ++i) {
        const std::uint64_t button = Peek<std::uint64_t>(read, node - static_cast<std::uint64_t>(off) + kEntryButton);
        const std::uint8_t state = button != 0 ? Peek<std::uint8_t>(read, button + kButtonState) : 0;
        if (state != 0 && (state & 0xfe) != 4) {
            if (found != 0) {
                return 0;
            }
            found = button;
        }
        node = Peek<std::uint64_t>(read, node + 8);
    }
    return found;
}

std::string ButtonName(const Reader& read, std::uint64_t button) {
    if (button == 0) {
        return {};
    }
    const std::uint64_t at = Peek<std::uint64_t>(read, button + kNameText);
    if (at < 0x100000) {
        return {};
    }
    std::string name;
    for (std::size_t i = 0; i < kNameMax; ++i) {   // (byte by byte: the text may end just before unmapped memory)
        char c = 0;
        if (!read(at + i, &c, 1)) {
            return {};
        }
        if (c == 0) {
            return name;
        }
        if (c < 0x20 || c > 0x7e) {
            return {};
        }
        name += c;
    }
    return name;   // (cut at kNameMax)
}

bool SoundName(const char* name) {
    if (name == nullptr || *name == 0) {
        return false;
    }
    std::size_t n = 0;
    for (; name[n] != 0; ++n) {
        const char c = name[n];
        if (n >= kSoundNameMax || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

std::uint8_t FocusState(const Reader& read, std::uint64_t button) {
    return button != 0 ? Peek<std::uint8_t>(read, button + kButtonState) : 0;
}

bool Choosable(const Reader& read, std::uint64_t button) {
    std::uint32_t flags = 0;
    return button != 0 && read(button + kButtonFlags, &flags, sizeof flags) && (flags & kChoosable) == kChoosable;
}

std::vector<std::uint64_t> Buttons(const Reader& read, std::uint64_t screen) {
    std::vector<std::uint64_t> out;
    if (screen == 0) {
        return out;
    }
    const std::uint64_t head = screen + kListHead;
    const auto off = static_cast<std::int64_t>(Peek<std::int32_t>(read, screen + kListEntryOffset));
    std::uint64_t node = Peek<std::uint64_t>(read, screen + kListFirst);
    for (int i = 0; i < kMaxButtons && node != 0 && node != head; ++i) {
        if (const std::uint64_t button = Peek<std::uint64_t>(read, node - static_cast<std::uint64_t>(off) + kEntryButton);
            button != 0) {
            out.push_back(button);
        }
        node = Peek<std::uint64_t>(read, node + 8);
    }
    return out;
}

std::optional<Box> HitBox(const Reader& read, std::uint64_t button) {
    const std::uint64_t box = Peek<std::uint64_t>(read, button + kHitBox);
    float m[12] = {}, r[4] = {};
    if (button == 0 || box < 0x100000 || !read(box + kBoxMatrix, m, sizeof m) || !read(box + kBoxRect, r, sizeof r)) {
        return std::nullopt;
    }
    const float sx = m[0], sy = m[4], tx = m[9], ty = m[10];
    if (!(sx > 1e-3f) || !(sy > 1e-3f) || !(r[2] > 0) || !(r[3] > 0)) {
        return std::nullopt;   // (not shown: its page's matrix is all 0)
    }
    return Box{tx + sx * r[0], ty + sy * r[1], tx + sx * (r[0] + r[2]), ty + sy * (r[1] + r[3])};
}

}  // namespace CoopModScreens
