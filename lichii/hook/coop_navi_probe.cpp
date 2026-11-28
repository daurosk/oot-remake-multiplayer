// SPDX-License-Identifier: GPL-3.0-or-later
// Row 316 probes: our Navi and the fairies, read only (coop_navi_probe.h).
#include "coop_navi_probe.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>

namespace CoopNaviProbe {

namespace {

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "\"0x%llx\"", static_cast<unsigned long long>(v));
    return b;
}

std::string Num(float v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", static_cast<double>(v));
    return b;
}

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20 || u >= 0x7f) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", u);
            out += b;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// The command name a slot points to: a C string, read up to 63 bytes.
std::string ReadName(CoopPuppet::Guest& guest, std::uint64_t text) {
    if (text == 0) return {};
    char name[64] = {};
    if (!guest.Read(text, name, sizeof name - 1)) return {};
    return std::string(name, strnlen(name, sizeof name - 1));
}

std::string ActorJson(const Actor& a) {
    std::string out = "{\"actor\":" + Hex(a.actor) + ",\"number\":" + std::to_string(a.number) +
                      ",\"read\":" + (a.read ? "true" : "false");
    if (!a.read) return out + "}";
    out += ",\"state\":" + std::to_string(a.state) + ",\"present\":" +
           (a.state == CoopPuppet::addr::kStatePresent ? "true" : "false");
    out += ",\"pos\":[" + Num(a.position[0]) + "," + Num(a.position[1]) + "," + Num(a.position[2]) + "]";
    out += ",\"rot\":[";
    for (std::size_t i = 0; i < a.rotation.size(); ++i) out += (i ? "," : "") + Num(a.rotation[i]);
    out += "],\"slot_count\":" + std::to_string(a.slot_count) + ",\"slots\":[";
    for (std::size_t i = 0; i < a.slots.size(); ++i) {
        out += std::string(i ? "," : "") + "{\"name\":" + Quote(a.slots[i].name) + ",\"frame\":" + Num(a.slots[i].frame) +
               "}";
    }
    return out + "]}";
}

std::atomic<bool> g_wanted{false};
std::mutex g_mutex;
std::optional<Sample> g_last;

}  // namespace

Actor ReadActor(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint8_t number) {
    namespace pa = CoopPuppet::addr;
    Actor out;
    out.actor = actor;
    out.number = number;
    if (actor == 0) return out;
    if (!guest.Read(actor + pa::kActorState, &out.state, sizeof out.state) ||
        !guest.Read(actor + pa::kActorPosition, out.position.data(), sizeof out.position) ||
        !guest.Read(actor + pa::kActorRotation, out.rotation.data(), sizeof out.rotation)) {
        return out;
    }
    out.read = true;
    std::uint64_t at = actor;
    for (const std::uint64_t step : pa::kControllerPath) {
        std::uint64_t next = 0;
        if (!guest.Read(at + step, &next, sizeof next) || next == 0) return out;
        at = next;
    }
    std::int32_t count = 0;
    std::uint64_t array = 0;
    if (!guest.Read(at + pa::kSlotCount, &count, sizeof count) || !guest.Read(at + pa::kSlots, &array, sizeof array)) {
        return out;
    }
    out.slot_count = count;
    for (std::int32_t i = 0; i < count && i < kMostSlots && array != 0; ++i) {
        std::uint64_t slot = 0, text = 0;
        Slot s;
        if (guest.Read(array + kSlotStride * static_cast<std::uint64_t>(i), &slot, sizeof slot) && slot != 0) {
            if (guest.Read(slot + pa::kSlotName, &text, sizeof text)) s.name = ReadName(guest, text);
            guest.Read(slot + pa::kSlotFrame, &s.frame, sizeof s.frame);
        }
        out.slots.push_back(std::move(s));
    }
    return out;
}

std::string Json(const Sample& s) {
    std::string out = "{\"ms\":" + std::to_string(s.ms) + ",\"frames\":" + std::to_string(s.frames) +
                      ",\"navi\":" + (s.navi.actor != 0 ? ActorJson(s.navi) : std::string("null")) + ",\"fairies\":[";
    for (std::size_t i = 0; i < s.fairies.size(); ++i) out += (i ? "," : "") + ActorJson(s.fairies[i]);
    return out + "]}";
}

void Store(Sample s) {
    std::lock_guard lock(g_mutex);
    g_last = std::move(s);
}

std::string StateJson() {
    g_wanted.store(true, std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    return g_last ? Json(*g_last) : std::string("null");
}

bool Wanted() { return g_wanted.load(std::memory_order_relaxed); }

}  // namespace CoopNaviProbe
