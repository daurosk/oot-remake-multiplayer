// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_gear_params.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CoopGearParams {

using CoopCharColours::Game;

bool WearsPart(std::string_view actor_name, const ParamPart& part) {
    const std::string_view prefix = part.actor;
    if (actor_name.substr(0, prefix.size()) != prefix) return false;
    const std::string_view rest = actor_name.substr(prefix.size());
    // (the game's item, or a co-op copy: _Look<k> for a colour slot, _Blade<n> for a player number)
    return rest.empty() || rest.substr(0, 5) == "_Look" || rest.substr(0, 6) == "_Blade";
}

std::array<float, 3> Linear(std::uint32_t rgb) {
    std::array<float, 3> out{};
    for (int k = 0; k < 3; k++) {
        const float c = static_cast<float>((rgb >> (16 - 8 * k)) & 0xFF) / 255.0f;
        out[static_cast<std::size_t>(k)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return out;
}

namespace {

// The reads and calls of one frame (CoopCharColours' dresser does the same for the hair and eyes).
class Calls {
public:
    Calls(const Game& game, std::uint64_t cell, std::size_t budget) : g_(game), cell_(cell), budget_(budget) {}

    std::uint64_t R64(std::uint64_t a) const {
        std::uint64_t v = 0;
        return a != 0 && g_.read(a, &v, sizeof v) ? v : std::uint64_t{0};
    }
    template <typename T>
    T Rv(std::uint64_t a) const {
        T v{};
        if (a == 0 || !g_.read(a, &v, sizeof v)) return T{};
        return v;
    }
    std::string Name(std::uint64_t actor) const {
        char text[64] = {};
        const std::uint64_t at = R64(actor + 0x260);   // the actor's name (as the glue's name_of reads it)
        if (at == 0 || !g_.read(at, text, sizeof text - 1)) return {};
        return std::string(text);
    }
    bool Stopped() const { return failed || calls >= budget_; }

    std::optional<std::uint64_t> Call(std::uint64_t f, std::array<std::uint64_t, 8> x) {
        if (Stopped() || f == 0) return std::nullopt;
        calls++;
        auto v = g_.call(f, x);
        if (!v) failed = true;
        return v;
    }
    std::uint64_t Virtual(std::uint64_t object, std::uint64_t offset) const { return R64(R64(object) + offset); }

    // The unit's by-name call (+0xd0 a material, +0xe0 a parameter of `mat`) with the name in the cell. -1: none.
    std::int16_t ByName(std::uint64_t unit, std::uint64_t offset, std::uint64_t name, std::optional<std::int16_t> mat) {
        if (!g_.write(cell_ + CoopCharColours::kCellName, &name, sizeof name)) {
            failed = true;
            return -1;
        }
        std::array<std::uint64_t, 8> x{unit};
        if (mat) {
            x[1] = static_cast<std::uint64_t>(static_cast<std::int64_t>(*mat));
            x[2] = cell_ + CoopCharColours::kCellName;
        } else {
            x[1] = cell_ + CoopCharColours::kCellName;
        }
        const auto v = Call(Virtual(unit, offset), x);
        return v ? static_cast<std::int16_t>(*v & 0xFFFF) : static_cast<std::int16_t>(-1);
    }

    std::uint64_t MaterialObject(std::uint64_t unit, std::int64_t index) const {
        if (index < 0) return 0;
        const std::uint64_t ptrs = R64(unit + 0x110);
        if (ptrs != 0) return R64(ptrs + 8 * static_cast<std::uint64_t>(index));
        const std::uint64_t base = R64(unit + 0x108);
        return base != 0 ? base + 0x80 * static_cast<std::uint64_t>(index) : 0;
    }

    // The parameter's four floats: `rgb` (linear) in the first three, the file's own values otherwise.
    void SetFloats(std::uint64_t unit, std::int16_t mat, std::int16_t param, std::optional<std::array<float, 3>> rgb) {
        const std::uint64_t m = R64(MaterialObject(unit, mat));
        const std::uint64_t info = R64(R64(R64(m + 0x10)) + 0x20);
        const std::uint64_t values = R64(m + 0x58);
        if (m == 0 || info == 0 || values == 0) {
            failed = true;   // (the material is not as read: no set, tried again later)
            return;
        }
        const auto offset = Rv<std::uint16_t>(info + 0x18 * static_cast<std::uint64_t>(param) + 0x10);
        std::array<float, 4> f{};
        if (!g_.read(values + offset, f.data(), sizeof f)) {
            failed = true;
            return;
        }
        if (rgb) std::copy(rgb->begin(), rgb->end(), f.begin());
        if (!g_.write(cell_ + CoopCharColours::kCellFloats, f.data(), sizeof f)) {
            failed = true;
            return;
        }
        Call(Virtual(unit, CoopCharColours::kUnitSetFloats),
             {unit, static_cast<std::uint64_t>(static_cast<std::int64_t>(mat)), static_cast<std::uint64_t>(param),
              cell_ + CoopCharColours::kCellFloats});
    }

    std::size_t calls = 0;
    bool failed = false;

private:
    const Game& g_;
    std::uint64_t cell_;
    std::size_t budget_;
};

// The model units of an item actor: its model component (component table +8, as the player's), the model's list.
std::vector<std::uint64_t> Units(const Calls& c, std::uint64_t piece) {
    std::vector<std::uint64_t> out;
    const std::uint64_t component = c.R64(c.R64(piece + CoopCharColours::kActorComponents) + 8);
    const std::uint64_t model = c.R64(component + 0x18);
    const auto count = c.Rv<std::int32_t>(model + 0x20);
    const std::uint64_t list = c.R64(model + 0x28);
    if (component == 0 || model == 0 || list == 0 || count <= 0 || count > 64) return out;
    for (std::int32_t i = 0; i < count; i++) {
        const std::uint64_t p = c.R64(list + 8 * static_cast<std::uint64_t>(i));
        out.push_back(p != 0 ? c.R64(p) : 0);
    }
    return out;
}

// The equipment pieces an actor wears (its EquipmentUser's handles, as CoopCharColours::SetGearShown reads them):
// a handle whose id is its piece's, the piece not being made or deleted (state 7..11).
std::vector<std::uint64_t> Pieces(const Calls& c, std::uint64_t actor) {
    std::vector<std::uint64_t> out;
    const std::uint64_t equipment =
        c.R64(c.R64(actor + CoopCharColours::kActorComponents) + CoopCharColours::kActorEquipment);
    if (equipment == 0) return out;
    for (std::uint32_t k = 0; k < CoopCharColours::kGearCount; k++) {
        const std::uint64_t handle = equipment + CoopCharColours::kGearFirst + CoopCharColours::kGearStride * k;
        const auto id = c.Rv<std::uint32_t>(handle + 0x10);
        if (id == 0 || id == 0xFFFFFFFFu) continue;
        const std::uint64_t link = c.R64(handle + 0x8);
        if (link == 0 || c.Rv<std::uint32_t>(link + 0x48) != id) continue;
        const std::uint64_t piece = c.R64(link + 0x40);
        if (piece == 0 || c.Rv<std::uint32_t>(piece + 0x2c) - 7u < 5u) continue;
        if (std::find(out.begin(), out.end(), piece) == out.end()) out.push_back(piece);
    }
    return out;
}

}  // namespace

std::size_t Dresser::Frame(const Game& game, std::uint64_t cell, const std::vector<Target>& targets,
                           std::uint64_t now_ms, const std::function<void(const std::string&)>& say) {
    if (cell == 0 || !game.read || !game.write || !game.call || now_ms < retry_ms_) return 0;
    if (cell != cell_) {
        cell_ = cell;
        names_written_ = false;
    }
    if (!names_written_) {
        std::array<char, 2 * kNameBytes * kPartCount> names{};
        for (std::size_t i = 0; i < kPartCount; i++) {
            const auto put = [&names](std::size_t at, const char* s) {
                std::memcpy(&names[at], s, std::min<std::size_t>(std::strlen(s), kNameBytes - 1));
            };
            put(2 * kNameBytes * i, kParts[i].material);
            put(2 * kNameBytes * i + kNameBytes, kParts[i].param);
        }
        if (!game.write(cell + kCellPartNames, names.data(), names.size())) return 0;
        names_written_ = true;
    }
    for (auto& p : pieces_) p.seen = false;
    Calls c(game, cell, kCallsPerFrame);
    for (const Target& t : targets) {
        if (t.actor == 0) continue;
        for (const std::uint64_t piece : Pieces(c, t.actor)) {
            const std::string name = c.Name(piece);
            for (std::size_t i = 0; i < kPartCount; i++) {
                if (!WearsPart(name, kParts[i])) continue;
                auto it = std::find_if(pieces_.begin(), pieces_.end(), [&](const Piece& p) {
                    return p.actor == t.actor && p.piece == piece && p.part == i;
                });
                if (it == pieces_.end()) {
                    Piece fresh;
                    fresh.actor = t.actor, fresh.piece = piece, fresh.part = i;
                    pieces_.push_back(fresh);
                    it = pieces_.end() - 1;
                }
                Piece& p = *it;
                p.seen = true;
                const auto& colour = t.colours[i];
                if (!colour && !p.ever) continue;   // the game's own, never changed: nothing to do, no call
                if (c.Stopped()) continue;
                // Its model: looked up once, and anew when its units are others (the piece's model made again).
                const auto units = Units(c, piece);
                if (p.looked_up && units != p.units) {
                    p.looked_up = false;
                    p.put.reset();
                }
                if (!p.looked_up) {
                    if (units.empty()) continue;   // the model is not there yet: next frame
                    const std::uint64_t material_name = cell + kCellPartNames + 2 * kNameBytes * i;
                    p.unit = 0, p.material = -1, p.param = -1;
                    for (const std::uint64_t unit : units) {
                        if (unit == 0 || c.R64(unit) == 0 || c.Stopped()) continue;
                        const auto count = c.Call(c.Virtual(unit, CoopCharColours::kUnitMaterialCount), {unit});
                        if (!count) break;
                        const std::int16_t mat =
                            c.ByName(unit, CoopCharColours::kUnitMaterialByName, material_name, std::nullopt);
                        if (mat < 0 || static_cast<std::uint64_t>(mat) >= (*count & 0xFFFFFFFFu)) continue;
                        const std::int16_t param =
                            c.ByName(unit, CoopCharColours::kUnitParamByName, material_name + kNameBytes, mat);
                        if (param < 0) continue;
                        p.unit = unit, p.material = mat, p.param = param;
                        break;
                    }
                    if (c.Stopped()) continue;   // (a failed call or the budget: the look-up again next time)
                    p.looked_up = true;
                    p.units = units;
                    if (say && !p.said) {
                        p.said = true;
                        say(p.unit != 0 ? "gear params: " + name + " wears " + kParts[i].material + "." +
                                              kParts[i].param + " (material " + std::to_string(p.material) +
                                              ", parameter " + std::to_string(p.param) + ")"
                                        : "gear params: " + name + " has no " + kParts[i].material + "." +
                                              kParts[i].param + " (" + std::to_string(units.size()) +
                                              " model unit(s)): its colour is left alone");
                    }
                }
                if (p.unit == 0) continue;
                const bool changed = colour != p.put;
                const bool due = changed || (colour && now_ms - p.at_ms >= kAgainMs);
                if (!due) continue;
                c.SetFloats(p.unit, p.material, p.param, colour ? std::optional(Linear(*colour)) : std::nullopt);
                if (c.failed) break;
                p.put = colour;
                p.ever = p.ever || colour.has_value();
                p.at_ms = now_ms;
                if (!colour) p.ever = false;   // (the file's own is back: nothing more until a colour again)
            }
            if (c.failed) break;
        }
        if (c.failed) break;
    }
    if (c.failed) {
        // Not again before kAgainMs (a call that cannot be made fails every time), said once.
        retry_ms_ = now_ms + kAgainMs;
        if (say && !said_failed_) {
            say("gear params: a model call failed; tried again every " + std::to_string(kAgainMs) + " ms");
        }
        said_failed_ = true;
        return c.calls;   // (the frame stopped early: the pieces not reached keep their state)
    }
    // A piece no target wears any more (unequipped, deleted) is forgotten.
    pieces_.erase(std::remove_if(pieces_.begin(), pieces_.end(), [](const Piece& p) { return !p.seen; }),
                  pieces_.end());
    return c.calls;
}

}  // namespace CoopGearParams
