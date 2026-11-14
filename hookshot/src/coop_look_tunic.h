// SPDX-License-Identifier: GPL-3.0-or-later
// Your own Link's tunic and gear in other colours while the game runs
// (docs/puppet.md, "Changing your colour in the game" and "The Link
// customizer").
//
// The colours are in textures (reports puppet-colors-2: no material
// parameter shows). Nothing of the game is written: Longshot's texture
// overrides (patch 0020, hos_coop_texture.h) have its renderer draw a texture
// the game made with other texels, while the game's memory keeps its own.
// Same format, same size, nothing else moves.
//
// The puppet pack has those texels per colour (tools/puppet_pack/look_live.py:
// coop_look/<colour>.bin, green being the game's own). A texture is one the
// game made (Longshot lists them) whose storage holds a colour's texels, and
// only inside its own file's image: the file's header (BNTX) in front of them,
// naming the texture. The puppets' copies of the same texels have another name
// and are left alone.
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
// How much larger than its texels a texture's storage may be (its layout
// rounded up): only storages up to this much larger are compared.
inline constexpr std::uint64_t kStorageSlack = 0x10000;
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

// The game's textures and Longshot's overrides of them (hos_coop_texture.h).
struct Live {
    std::uint64_t address = 0;   // its storage: where its texels start
    std::uint64_t size = 0;
    std::uint64_t serial = 0;    // new each time a texture is made
};
class Textures {
public:
    virtual ~Textures() = default;
    // The textures the game has whose storage is size_min to size_max bytes;
    // *generation changes whenever one is made or finalized.
    virtual std::vector<Live> List(std::uint64_t size_min, std::uint64_t size_max, std::uint64_t* generation) const = 0;
    // The game's memory, read only. False when any byte is not there.
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) const = 0;
    // The renderer draws these bytes in place of [address, address + size);
    // `bytes` null: as the game's memory holds them again.
    virtual bool Override(std::uint64_t address, const void* bytes, std::size_t size) = 0;
};

// A texture of your Link's, the colour the game's memory holds there and the
// colour it is drawn in.
struct Place {
    std::size_t texture = 0;
    std::uint64_t address = 0;
    std::uint64_t serial = 0;
    std::shared_ptr<const Texels> held;    // in the game's memory
    std::shared_ptr<const Texels> drawn;   // on screen: held, or an override's
    bool overridden = false;
};

struct Search {
    std::vector<Place> places;
    std::size_t copies = 0;   // the same texels outside the texture's own file (a puppet's copy): left alone
};
// Every texture of `colours` the game has made: its storage holds the
// colour's texels whole, in its own file's image.
Search Find(const Textures& textures, const std::vector<std::shared_ptr<const Texels>>& colours);
// Draws the place in `colour`: an override, or none when the game's memory
// holds that colour already. False when Longshot refused it (nothing changed).
bool Draw(Textures& textures, Place& place, const std::shared_ptr<const Texels>& colour);

// Keeps your Link's tunic (or gear) in the colour asked for.
class Wardrobe {
public:
    struct Host {
        Textures* textures = nullptr;
        // A colour's file from the puppet pack ("blue"); nothing when the
        // pack has none.
        std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)> file;
        std::function<void(const std::string&)> log;
        // The game's own look's file name ("green" for the tunic) and what the
        // log calls the textures ("tunic").
        std::string base = "green";
        std::string what = "tunic";
    };

    explicit Wardrobe(Host host) : host_(std::move(host)) {}
    // The colour to wear from now on: "green" (the game's own) or a colour of
    // the pack. Any thread.
    void Wear(std::string colour);
    // One round of the work, about once a second: `now_ms` on any steady
    // clock. One thread.
    void Step(std::uint64_t now_ms);
    // Look at the game's textures again at the next Step (the pause screen
    // closed: the equipment may have changed). Any thread.
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
    bool look_now_ = true;
    bool looked_ = false;
    std::uint64_t generation_ = 0;
    bool said_nothing_found_ = false;
};

}  // namespace CoopLookTunic
