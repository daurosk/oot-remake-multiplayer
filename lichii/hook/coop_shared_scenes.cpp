// SPDX-License-Identifier: GPL-3.0-or-later
// Shared story cutscenes: who joins another player's cutscene (coop_shared_scenes.h).
#include "coop_shared_scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace CoopSharedScenes {

namespace {

// Event/EventFlow file names (romfs, build e0f3ccfaf50065ae), sorted, as the
// header says which are left out.
constexpr std::string_view kNames[] = {

    "Dm_DeathMtCrater_0000", "Dm_DeathMtCrater_0010", "Dm_DeathMtTrail_0000", "Dm_DeathMtTrail_0005",
    "Dm_DeathMtTrail_0006", "Dm_DeathMtTrail_0007", "Dm_DeathMtTrail_0010", "Dm_DeathMtTrail_0020",
    "Dm_DeathMtTrail_0025", "Dm_DeathMtTrail_0027", "Dm_DeathMtTrail_0030", "Dm_DeathMtTrail_0035",
    "Dm_DeathMtTrail_0040", "Dm_DeathMtTrail_0050", "Dm_Deku_0000", "Dm_Deku_0003",
    "Dm_Deku_0010", "Dm_DesertColossus_0000", "Dm_DesertColossus_0020", "Dm_DesertColossus_0025",
    "Dm_DesertColossus_0030", "Dm_DesertColossus_0040", "Dm_DodongoCave_0000", "Dm_DodongoCave_0010",
    "Dm_DodongoCave_0020", "Dm_DodongoCave_0030", "Dm_FairyFountain_Courage", "Dm_FairyFountain_Din",
    "Dm_FairyFountain_Frore", "Dm_FairyFountain_Nayru", "Dm_FairyFountain_Power", "Dm_FairyFountain_Wisdom",
    "Dm_FinalBattle_0000", "Dm_FinalBattle_0002", "Dm_FinalBattle_0005", "Dm_FinalBattle_0010",
    "Dm_FinalBattle_0030", "Dm_FireTemple_0000", "Dm_FireTemple_0010", "Dm_FireTemple_0020",
    "Dm_FireTemple_0025", "Dm_FireTemple_0026", "Dm_FireTemple_0029", "Dm_FireTemple_0030",
    "Dm_ForestTemple_0000", "Dm_ForestTemple_0005", "Dm_ForestTemple_0007", "Dm_ForestTemple_0010",
    "Dm_ForestTemple_0011", "Dm_ForestTemple_0012", "Dm_ForestTemple_0013", "Dm_ForestTemple_0014",
    "Dm_ForestTemple_0015", "Dm_ForestTemple_0016", "Dm_ForestTemple_0017", "Dm_ForestTemple_0020",
    "Dm_ForestTemple_0021", "Dm_ForestTemple_0022", "Dm_ForestTemple_0031", "Dm_ForestTemple_0032",
    "Dm_GanonCastle_0000", "Dm_GerudoValley_0000", "Dm_GerudoValley_0010", "Dm_GerudoValley_0020",
    "Dm_Gerudo_0000", "Dm_Gerudo_0010", "Dm_Gerudo_0020", "Dm_Gerudo_0025",
    "Dm_Gerudo_0030", "Dm_Goron_0000", "Dm_Goron_0010", "Dm_Goron_0020",
    "Dm_Goron_0025", "Dm_Goron_0028", "Dm_Goron_0030", "Dm_Goron_0040",
    "Dm_Goron_0045", "Dm_Graveyard_0000", "Dm_Graveyard_0010", "Dm_Graveyard_0030",
    "Dm_HrlCastle_0000", "Dm_HrlCastle_0005", "Dm_HrlCastle_0020", "Dm_HrlCastle_0030",
    "Dm_HrlCastle_0035", "Dm_HrlCastle_0045", "Dm_HrlCastle_0050", "Dm_HrlCastle_0060",
    "Dm_HrlCastle_0070", "Dm_HrlField_0000", "Dm_HrlField_0005", "Dm_HrlField_0010",
    "Dm_HrlField_0020", "Dm_HrlField_0030", "Dm_HrlField_0040", "Dm_HrlField_0050",
    "Dm_HrlField_0060", "Dm_HrlField_0070", "Dm_HrlField_0071", "Dm_HrlField_0072",
    "Dm_HrlField_0073", "Dm_HrlField_0080", "Dm_InsideGanonCastle_0000", "Dm_InsideGanonCastle_0003",
    "Dm_InsideGanonCastle_0005", "Dm_InsideGanonCastle_0006", "Dm_InsideGanonCastle_0007", "Dm_InsideGanonCastle_0010",
    "Dm_InsideGanonCastle_0015", "Dm_InsideGanonCastle_0020", "Dm_InsideGanonCastle_0025", "Dm_InsideGanonCastle_0030",
    "Dm_InsideGanonCastle_0032", "Dm_InsideGanonCastle_0035", "Dm_InsideGanonCastle_0040", "Dm_InsideGanonCastle_0050",
    "Dm_InsideGanonCastle_0055", "Dm_InsideGanonCastle_0060", "Dm_InsideGanonCastle_0070", "Dm_InsideGanonCastle_0075",
    "Dm_InsideGanonCastle_0080", "Dm_InsideGanonCastle_0085", "Dm_InsideGanonCastle_0090", "Dm_InsideGanonCastle_0100",
    "Dm_InsideGanonCastle_0110", "Dm_InsideGanonCastle_0150", "Dm_InsideGanonCastle_0160", "Dm_InsideGanonCastle_0170",
    "Dm_InsideGanonCastle_0180", "Dm_InsideGanonCastle_0190", "Dm_InsideJabu_Jabu_0000", "Dm_InsideJabu_Jabu_0010",
    "Dm_InsideJabu_Jabu_0020", "Dm_InsideJabu_Jabu_0030", "Dm_InsideJabu_Jabu_0050", "Dm_InsideJabu_Jabu_0060",
    "Dm_Kakariko_0000", "Dm_Kakariko_0010", "Dm_Kakariko_0025", "Dm_Kakariko_0030",
    "Dm_Kakariko_0040", "Dm_Kakariko_0050", "Dm_Kakariko_0060", "Dm_Kokiri_0030",
    "Dm_Kokiri_0045", "Dm_Kokiri_0050", "Dm_Kokiri_0060", "Dm_Kokiri_0063",
    "Dm_Kokiri_0070", "Dm_Kokiri_0075", "Dm_Kokiri_0080", "Dm_Kokiri_0090",
    "Dm_Kokiri_0095", "Dm_Kokiri_0130", "Dm_Kokiri_0140", "Dm_Kokiri_0145",
    "Dm_Kokiri_0150", "Dm_Kokiri_0155", "Dm_Kokiri_0158", "Dm_Kokiri_0160",
    "Dm_Kokiri_0162", "Dm_Kokiri_0170", "Dm_Kokiri_0175", "Dm_LakeHrl_0000",
    "Dm_LakeHrl_0003", "Dm_LakeHrl_0005", "Dm_LakeHrl_0010", "Dm_LakeHrl_0011",
    "Dm_LakeHrl_0020", "Dm_LakeHrl_0030", "Dm_LakeHrl_0035", "Dm_LonLon_0010",
    "Dm_LonLon_0020", "Dm_LonLon_0030", "Dm_LonLon_0040", "Dm_LostWood_0000",
    "Dm_LostWood_0010", "Dm_LostWood_0015", "Dm_LostWood_0021", "Dm_LostWood_0025",
    "Dm_LostWood_0030", "Dm_LostWood_0040", "Dm_SacredForest_0000", "Dm_ShadowTemple_0000",
    "Dm_ShadowTemple_0010", "Dm_ShadowTemple_0015", "Dm_ShadowTemple_0020", "Dm_SpiritTemple_0000",
    "Dm_SpiritTemple_0010", "Dm_SpiritTemple_0020", "Dm_SpiritTemple_0030", "Dm_SpiritTemple_0040",
    "Dm_WaterTemple_0000", "Dm_WaterTemple_0010", "Dm_WaterTemple_0015", "Dm_WaterTemple_0020",
    "Dm_WaterTemple_0021", "Dm_WaterTemple_0022", "Dm_WaterTemple_0040", "Dm_ZoraFountain_0000",
    "Dm_ZoraFountain_0010", "Dm_ZoraFountain_0020", "Dm_ZoraFountain_0030", "Dm_ZoraFountain_0040",
    "Dm_ZoraRiver_0000", "Dm_ZoraRiver_0005", "Dm_ZoraRiver_0010", "Dm_Zora_0000",
    "Dm_Zora_0010", "Dm_Zora_0020", "Get_StoneOfFire", "Get_StoneOfForest",
    "Get_StoneOfWater", "Meet_DekuTree", "Meet_GreatFairy",
};

} // namespace

std::span<const std::string_view> Names() {
    return kNames;
}

bool IsShared(std::string_view name) {
    return std::binary_search(std::begin(kNames), std::end(kNames), name);
}

void Joiner::Visit(const std::string& scene) {
    if (scene.empty() || scene == scene_) {
        return;  // a load keeps what waits; its scene decides next frame
    }
    scene_ = scene;
    done_.clear();
    joined_.clear();
    std::erase_if(pending_, [&](const Pending& p) { return p.scene != scene; });
}

void Joiner::Started(std::uint8_t player, const std::string& name, const std::string& scene,
                     std::optional<Vec3> starter, std::uint64_t now_us) {
    for (const Pending& p : pending_) {
        if (p.name == name && p.scene == scene) {
            return;  // two players started it: one is enough
        }
    }
    pending_.push_back(Pending{player, name, scene, starter, now_us});
}

bool Joiner::Ours(const std::string& name, const std::string& scene) {
    Visit(scene);
    std::erase_if(pending_, [&](const Pending& p) { return p.name == name; });
    if (joined_.contains(name) || done_.contains(name)) {
        return false;  // we only joined it, or sent it already
    }
    done_.insert(name);
    return true;
}

std::string Joiner::Why(const Pending& p, const Here& here, bool played) const {
    if (!here.scene.empty() && p.scene != here.scene) {
        return "we are in another scene";
    }
    if (done_.contains(p.name)) {
        return "it played here already this visit";
    }
    if (played) {
        return "this save has it played already";
    }
    if (!p.starter) {
        return "where they are is not known";
    }
    if (here.link) {
        const float dx = (*here.link)[0] - (*p.starter)[0];
        const float dy = (*here.link)[1] - (*p.starter)[1];
        const float dz = (*here.link)[2] - (*p.starter)[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > kRoomM) {
            char text[64];
            std::snprintf(text, sizeof text, "too far: %.0f m from them", d);
            return text;
        }
    }
    if (!here.can_start || !here.link || here.scene.empty()) {
        return here.now_us >= p.at_us + kJoinUs ? "our game was busy for 3 s" : "wait";
    }
    return "";
}

std::vector<std::string> Joiner::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

bool OnFromEnv(std::string_view text) {
    return text != "0";
}

} // namespace CoopSharedScenes
