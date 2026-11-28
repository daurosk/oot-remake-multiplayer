// Tests for the row 316 Navi probe (lichii/hook/coop_navi_probe.cpp): every AS slot read, the JSON the bridge shows.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_navi_probe.h"

namespace {

class ProbeGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t at, std::size_t size) { regions_[at].assign(size, 0); }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* p = Find(address, size);
        if (p == nullptr) return false;
        std::memcpy(out, p, size);
        return true;
    }
    bool Write(std::uint64_t, const void*, std::size_t) override {
        ++writes;
        return false;
    }
    template <typename T>
    void Put(std::uint64_t at, T value) {
        auto* p = Find(at, sizeof(T));
        REQUIRE(p != nullptr);
        std::memcpy(p, &value, sizeof(T));
    }
    void PutText(std::uint64_t at, const char* text) {
        auto* p = Find(at, std::strlen(text) + 1);
        REQUIRE(p != nullptr);
        std::memcpy(p, text, std::strlen(text) + 1);
    }
    int writes = 0;

private:
    unsigned char* Find(std::uint64_t address, std::size_t size) {
        auto it = regions_.upper_bound(address);
        if (it == regions_.begin()) return nullptr;
        --it;
        if (address + size > it->first + it->second.size()) return nullptr;
        return it->second.data() + (address - it->first);
    }
    std::map<std::uint64_t, std::vector<unsigned char>> regions_;
};

namespace pa = CoopPuppet::addr;
constexpr std::uint64_t kActor = 0x10000, kA = 0x20000, kB = 0x21000, kC = 0x22000, kCtrl = 0x23000;
constexpr std::uint64_t kArray = 0x24000, kSlot0 = 0x25000, kSlot1 = 0x26000, kText0 = 0x27000, kText1 = 0x27100;

// An actor with the AS chain (CoopPuppet::addr::kControllerPath) and three slots, the third empty.
void MakeNavi(ProbeGuest& g) {
    for (const std::uint64_t at : {kActor, kA, kB, kC, kCtrl, kArray, kSlot0, kSlot1}) g.Map(at, 0x400);
    g.Map(kText0, 0x200);
    g.Put<std::uint32_t>(kActor + pa::kActorState, pa::kStatePresent);
    const float pos[3] = {1.5f, 2.0f, -3.25f};
    for (int i = 0; i < 3; ++i) g.Put<float>(kActor + pa::kActorPosition + 4 * i, pos[i]);
    for (int i = 0; i < 9; ++i) g.Put<float>(kActor + pa::kActorRotation + 4 * i, i % 4 == 0 ? 1.0f : 0.0f);
    g.Put<std::uint64_t>(kActor + pa::kControllerPath[0], kA);
    g.Put<std::uint64_t>(kA + pa::kControllerPath[1], kB);
    g.Put<std::uint64_t>(kB + pa::kControllerPath[2], kC);
    g.Put<std::uint64_t>(kC + pa::kControllerPath[3], kCtrl);
    g.Put<std::int32_t>(kCtrl + pa::kSlotCount, 3);
    g.Put<std::uint64_t>(kCtrl + pa::kSlots, kArray);
    g.Put<std::uint64_t>(kArray, kSlot0);
    g.Put<std::uint64_t>(kArray + CoopNaviProbe::kSlotStride, kSlot1);
    g.Put<std::uint64_t>(kSlot0 + pa::kSlotName, kText0);
    g.Put<float>(kSlot0 + pa::kSlotFrame, 12.5f);
    g.Put<std::uint64_t>(kSlot1 + pa::kSlotName, kText1);
    g.Put<float>(kSlot1 + pa::kSlotFrame, 3.0f);
    g.PutText(kText0, "Wait");
    g.PutText(kText1, "Status_Red");
}

}  // namespace

TEST_CASE("navi probe: every AS slot by the 0x48 stride, an empty slot kept in place, nothing written") {
    ProbeGuest g;
    MakeNavi(g);
    const auto a = CoopNaviProbe::ReadActor(g, kActor);
    REQUIRE(a.read);
    CHECK(a.state == pa::kStatePresent);
    CHECK(a.position[2] == doctest::Approx(-3.25f));
    CHECK(a.slot_count == 3);
    REQUIRE(a.slots.size() == 3);
    CHECK(a.slots[0].name == "Wait");
    CHECK(a.slots[0].frame == doctest::Approx(12.5f));
    CHECK(a.slots[1].name == "Status_Red");
    CHECK(a.slots[2].name.empty());
    CHECK(g.writes == 0);
}

TEST_CASE("navi probe: no controller -> the actor's own fields, slot_count -1, no actor -> navi null") {
    ProbeGuest g;
    MakeNavi(g);
    g.Put<std::uint64_t>(kActor + pa::kControllerPath[0], 0);
    const auto a = CoopNaviProbe::ReadActor(g, kActor);
    CHECK(a.read);
    CHECK(a.slot_count == -1);
    CHECK(a.slots.empty());

    CoopNaviProbe::Sample s;
    s.fairies.push_back(CoopNaviProbe::ReadActor(g, kActor, 2));
    const std::string json = CoopNaviProbe::Json(s);
    CHECK(json.find("\"navi\":null") != std::string::npos);
    CHECK(json.find("\"number\":2") != std::string::npos);
    CHECK(json.find("\"present\":true") != std::string::npos);
}

TEST_CASE("navi probe: the bridge's StateJson asks once (Wanted), then shows the stored sample") {
    ProbeGuest g;
    MakeNavi(g);
    CHECK_FALSE(CoopNaviProbe::Wanted());
    CHECK(CoopNaviProbe::StateJson() == "null");
    CHECK(CoopNaviProbe::Wanted());
    CoopNaviProbe::Sample s;
    s.frames = 7;
    s.navi = CoopNaviProbe::ReadActor(g, kActor);
    CoopNaviProbe::Store(s);
    const std::string json = CoopNaviProbe::StateJson();
    CHECK(json.find("\"frames\":7") != std::string::npos);
    CHECK(json.find("{\"name\":\"Status_Red\",\"frame\":3}") != std::string::npos);
    CHECK(json.find("\"pos\":[1.5,2,-3.25]") != std::string::npos);
}
