// Tests for the table of game builds (lichii/hook/coop_game_build.h): the
// lookup, the refusal text, and that the table agrees with the constants the
// readers use today, so a reader can move onto the table without a change.
#include <doctest/doctest.h>

#include <string>

#include "../../coop/host/src/addresses.hpp"
#include "coop_fairy.h"
#include "coop_game_build.h"
#include "coop_hold.h"
#include "coop_puppet.h"
#include "coop_svc.h"
#include "coop_world.h"

using CoopGameBuild::Find;

namespace {

// Where a `bl` at `at` goes.
std::uint64_t BlTarget(std::uint64_t at, std::uint32_t word) {
    std::int64_t imm = word & 0x3ffffff;
    if (imm & 0x2000000) {
        imm -= 0x4000000;
    }
    return at + static_cast<std::uint64_t>(imm * 4);
}

} // namespace

TEST_CASE("game build: the known build is found by either id, in either case") {
    const auto* b = Find("e0f3ccfaf50065ae");
    REQUIRE(b != nullptr);
    CHECK(b->version == "1.1.1");
    CHECK(Find("E0F3CCFAF50065AE") == b);
    CHECK(Find("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348") == b);
    CHECK(Find("e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348") == b);
    CHECK(&CoopGameBuild::kPrimary == b);
}

TEST_CASE("game build: other builds and broken ids are not found") {
    CHECK(Find("") == nullptr);
    CHECK(Find("e0f3ccfaf500") == nullptr);
    CHECK(Find("0000000000000000") == nullptr);
    CHECK(Find("e0f3ccfaf50065af") == nullptr);
    CHECK(Find("e0f3ccfaf50065ae00") == nullptr);  // neither length
    CHECK(Find("E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3349") == nullptr);
}

TEST_CASE("game build: the refusal names the running build and the one needed") {
    const std::string panel = CoopGameBuild::PanelRefusal("0123456789abcdef");
    CHECK(panel.find("0123456789abcdef") != std::string::npos);
    CHECK(panel.find("1.1.1") != std::string::npos);
    CHECK(panel.size() <= 80);  // one line of the panel
    const std::string log = CoopGameBuild::LogRefusal("0123456789abcdef");
    CHECK(log.find("0123456789abcdef") != std::string::npos);
    CHECK(log.find("1.1.1 (build e0f3ccfaf50065ae)") != std::string::npos);
    CHECK(CoopGameBuild::Supported() == "1.1.1 (build e0f3ccfaf50065ae)");
}

TEST_CASE("game build: the table matches the full build id the loader patch wants") {
    // coop_svc.cpp's kMainBuild is file-local; it is this string.
    CHECK(CoopGameBuild::kPrimary.main_id == "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348");
    CHECK(CoopGameBuild::kPrimary.main_id.substr(0, 16) == "E0F3CCFAF50065AE");
}

TEST_CASE("game build: the table agrees with the readers' constants") {
    const auto& m = CoopGameBuild::kPrimary.main;
    namespace p = CoopPuppet::addr;
    namespace h = coop::host::addr;
    CHECK(m.actor_manager_ptr == p::kActorManagerPtr);
    CHECK(m.actor_manager_ptr == CoopFairy::addr::kManagerPtr);
    CHECK(m.game_ui_module_ptr == h::kGameUiModulePtr);
    CHECK(m.scene_module_ptr == h::kSceneModulePtr);
    CHECK(m.banc_module_ptr == h::kBancModulePtr);
    CHECK(m.event_manager_ptr == h::kEventManagerPtr);
    CHECK(m.foliage_module_ptr == CoopWorld::kFoliageModule);
    CHECK(m.actor_vtable == p::kActorVtable);
    CHECK(m.got_create_default == p::kGotCreateDefault);
    CHECK(m.create_actor == p::kCreate);
    CHECK(m.malloc == p::kMalloc);
    CHECK(m.request_delete == p::kRequestDelete);
    CHECK(m.take_created == p::kTakeCreated);
    CHECK(m.drop_create == p::kDropCreate);
    CHECK(m.play_as == p::kPlayAs);
    CHECK(m.set_as_frame == p::kSetAsFrame);
    CHECK(m.sync_as_slot == p::kSyncAsSlot);
    CHECK(m.equip == p::kEquip);
    CHECK(m.equip_change_state == p::kEquipChangeState);
    CHECK(m.set_matrix == p::kSetMatrix);
    CHECK(m.cut_foliage == CoopWorld::kCutFoliage);
    // The loader patches.
    CHECK(m.svc_hook_rare == CoopSvc::kHooks[0].offset);
    CHECK(m.svc_hook_frame == CoopSvc::kHooks[1].offset);
    CHECK(m.svc_cave == CoopSvc::kCaveOffset);
    CHECK(m.cave_end == CoopSvc::kCaveEnd);
    CHECK(m.hold_hook == CoopHold::kHookOffset);
    CHECK(m.hold_cave == CoopHold::kCaveOffset);
    CHECK(m.hold_table == CoopHold::kTableOffset);
    // Both hooked calls go to frame_step in the unpatched game.
    for (const auto& hook : CoopSvc::kHooks) {
        CHECK(BlTarget(hook.offset, hook.original) == m.frame_step);
    }
    // The caves sit in main's zero padding, before rodata.
    CHECK(m.svc_cave < m.hold_cave);
    CHECK(m.hold_cave < m.hold_table);
    CHECK(m.hold_table < m.cave_end);
    // game_flags_ptr, world_mgr_ptr and set_time are file-local constants
    // (coop_flags.cpp, coop/host/src/game_flags.cpp, coop_time.cpp): checked
    // by hand against docs/flags.md and docs/time.md, not here.
}
