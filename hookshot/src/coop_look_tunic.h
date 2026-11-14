// SPDX-License-Identifier: GPL-3.0-or-later
// Your own Link's tunic in another colour while the game runs (docs/puppet.md,
// "Changing your colour in the game").
//
// The tunic's colour is in textures (reports puppet-colors-2: no material
// parameter shows). A loaded texture's texels lie in the game's memory as
// they are in its file, and Hookshot decodes a texture again when those bytes
// are written (its renderer marks the texture stale: gpu/render/rx_texture.cpp
// textures_written). So the colour changes by writing another colour's texels
// over them: same format, same size, nothing else moves.
//
// The puppet pack has those texels per colour (tools/puppet_pack/look_live.py:
// coop_look/<colour>.bin, green being the game's own). A texture is found by
// its texels, and taken only inside its own file's image: the file's header
// (BNTX) in front of them, naming the texture. The puppets' copies of the same
// texels have another name and are left alone.
//
// Nothing here runs on the game's thread: Wardrobe::Step is the worker's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace CoopLookTunic {

struct Texture {
    std::string name;            // in the game: "Angin_Tile_A_Alb"
    std::uint64_t offset = 0;    // of the texels in the texture's file
    std::vector<std::uint8_t> texels;
};
// One colour's file: every tunic texture, in the pack's order.
using Texels = std::vector<Texture>;

// The first bytes of a texture's texels, compared before anything else.
inline constexpr std::size_t kHead = 64;
// Texels start at a multiple of this in memory (a block is 8 or 16 bytes).
inline constexpr std::uint64_t kAlign = 16;
// A texture file's header is at most this long (the game's are 4096 bytes).
inline constexpr std::uint64_t kMaxOffset = 0x10000;

// A coop_look file. Nothing, and why, for one that is cut short, of another
// version, or with a texture this code cannot place (no room for a header,
// fewer than kHead bytes).
std::optional<Texels> Parse(const std::vector<std::uint8_t>& file, std::string* why = nullptr);
// The file of these texels (Parse reads it back).
std::vector<std::uint8_t> Pack(const Texels& texels);
// Two colours of one pack: the same textures, each as long and at the same
// offset. Only then may one's texels be written over the other's.
bool SameLayout(const Texels& a, const Texels& b);

// The game's memory.
class Memory {
public:
    virtual ~Memory() = default;
    // False when any byte is not there (nothing read or written).
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) const = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
};

// The end of the memory mapped from `begin` on, to the nearest `step`, at
// most `limit` (the game's heap is one block: Hookshot's host/hos_mem.c).
// `begin` itself when nothing is.
std::uint64_t MappedEnd(const Memory& memory, std::uint64_t begin, std::uint64_t limit, std::uint64_t step);

// A texture's texels in memory, and the colour they are in.
struct Place {
    std::size_t texture = 0;
    std::uint64_t address = 0;
    std::shared_ptr<const Texels> colour;
};

struct Search {
    std::vector<Place> places;
    std::size_t copies = 0;   // the same texels outside the texture's own file (a puppet's copy): left alone
};
// Every texture of `colours` in [begin, end): its texels whole, in its own
// file's image. Memory that cannot be read is passed over.
Search Find(const Memory& memory, std::uint64_t begin, std::uint64_t end,
            const std::vector<std::shared_ptr<const Texels>>& colours);
// The place still has its file's header and its colour's texels, whole (a
// few megabytes compared for the whole tunic: nothing next to a frame).
bool Holds(const Memory& memory, const Place& place);
// Writes `colour`'s texels at the place, once its bytes are checked whole to
// be the colour it has. False, and nothing written, when they are not.
bool Put(Memory& memory, Place& place, const std::shared_ptr<const Texels>& colour);

// Keeps your Link's tunic in the colour asked for.
class Wardrobe {
public:
    struct Host {
        Memory* memory = nullptr;
        // The memory to search now: begin and end.
        std::function<std::pair<std::uint64_t, std::uint64_t>()> range;
        // A colour's file from the puppet pack ("blue"); nothing when the
        // pack has none.
        std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)> file;
        std::function<void(const std::string&)> log;
        // The game's own look's file name ("green" for the tunic) and what the
        // log calls the textures ("tunic").
        std::string base = "green";
        std::string what = "tunic";
    };
    // After a search that found the tunic: the next one (textures loaded
    // since, as after a change of age). After one that found nothing: sooner.
    static constexpr std::uint64_t kSearchAgainMs = 60'000;
    static constexpr std::uint64_t kSearchSoonMs = 10'000;

    explicit Wardrobe(Host host) : host_(std::move(host)) {}
    // The colour to wear from now on: "green" (the game's own) or a colour of
    // the pack. Any thread.
    void Wear(std::string colour);
    // One round of the work, about once a second: `now_ms` on any steady
    // clock. One thread.
    void Step(std::uint64_t now_ms);
    // Search the memory again at the next Step, not a minute later (the pause
    // screen closed: the equipment may have changed). Any thread.
    void Kick();
    // Whether Step has anything to do (a colour other than the game's was
    // asked for at some time). Any thread.
    bool Busy() const;

    const std::vector<Place>& places() const { return places_; }   // Step's thread

private:
    std::shared_ptr<const Texels> Load(const std::string& colour);

    Host host_;
    mutable std::mutex mutex_;
    std::string wanted_;                      // guarded
    bool busy_ = false;                       // guarded
    bool kicked_ = false;                     // guarded
    // Step's thread only:
    std::string asked_;                       // the wanted colour of the last round
    std::shared_ptr<const Texels> green_, target_;
    std::string target_name_;
    std::string refused_;                     // a colour the pack does not have (said once)
    std::vector<Place> places_;
    bool search_now_ = false;
    std::uint64_t next_search_ms_ = 0;
    bool said_nothing_found_ = false;
};

}  // namespace CoopLookTunic
