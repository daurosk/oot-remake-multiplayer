// SPDX-License-Identifier: GPL-3.0-or-later
// A texture's parts in another colour (hookshot/src/coop_look_paint.h).
//
// The golden blocks are random (synthetic, nothing from the game) and were
// run through the puppet pack tool's rewrite (tools/puppet_pack/tunic_extra.py,
// its is_green swapped for these tones): the C++ must give the same bytes.
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "coop_look_paint.h"

using namespace CoopLookPaint;

namespace {

std::vector<std::uint8_t> Hex(const std::string& h) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    }
    return out;
}

struct Golden {
    Format format;
    std::uint32_t tones;
    std::uint32_t colour;
    const char* in;
    const char* out;
};

const Golden kGolden[] = {
    {Format::astc, kBlue | kPurple, 0xC83020,
        "2b902f8911e81818f8c99d5d5d9831958f0be21124179c3dd9f73817ce6e118d1b037494dd1c01cc6adfc974d1729a11"
        "fe2008d737e8f517d69ed6f181bd1a4529825e79455971b21ae105aab2d6a3100b08880f0f422dbb36fb94b3cb6d424f"
        "a5a969d270831572af40db4852eb74b71118a88c1f772047597125e27e970d230a81713531eccc70a5b38c29f942241c"
        "1ca0339d4a9150609d67072687aa68a2a8d0395d92fd6179ab96721fc3ce871d9e89fc155ae2e424b3f7281a55a1eabf"
        "c2ec6454bddbeda2424219395286fc9c8e25065bf51323bb363e6b0726a956fd5240594204b79231241e49b12964ea9a"
        "ddeb7af44ad079f905c7585d9b259e14dd555b32adf6755613d05134b52a8f7b923409c0a1980633915a607abe3996e3"
        "3188bef87a81c9b8feab55656fb091764e49c166e65c4cbeed4961f28f44bd157e288821b2407a05c16ca5032ba9a2cc"
        "89220e6fb3808efc798b2adff4c4de3115e97ed30d5cb8dffcde0636d2982a0731bb6542eeb35a8b0668446bb4cd8dd5"
        "b5106c807db3792156ae2588dbbfde2611b80101557daac6466f96cca77459261fea1e2c70353d2436d53679376b7d9e"
        "e5907353b044adae5480b284b7aedd8798a0ce9c39232ef906ee6b81c0d62b9bbf031d83a3160c33add156b1b4a1f546"
        "225501e21e1e2323df03b1f71f89bd7765e83638a08e26bf824fd7f3f49a18188629f46504f68ae25a1fbd7ca5f1ac29"
        "96521c32751b4b5b52e48a511e079a178573220af6f9e8e73f606cc7e9267a2e91f43b4562927ac19b8d2a2153ecdcd1"
        "82d9186c7b967ed9696a52e9b69b0cadaea06aa29141ac160e8837cd5587b34263802a7442b3f4028ca6d256a1d48afe"
        "427a6ec260ffb33499df3a4aff8818728938803029e6a483d0e989795adae43ed5c044ef9c3b72721b41351b22ae87a8"
        "1ec0a4d1b5586da6a130a36ea10011a8f549a164ae6466f8b52bb681a2deca4e2368ec0d2fde44d67fca3b216639a603"
        "0991dbde7c798581b3b9a8ab10de8ef88ec72c48c48a4999a6aa013e9cd653b15e883e4f980ec6376115495b1154d62e",
        "2b902f8911e81818f8c99d5d5d9831958f0be29117219c3dd9f72811ce6e118d1b037494dd1c01cc6adfc974d1729a11"
        "fe2008d737e8f517d69ed6f181bd1a4529825e79455971b21ae105aab2d6a3100b08880f0f422dbb36fb94b3cb6d424f"
        "a5a969d270831572af40db4852eb74b71118a88c1f772047597125e27e970d230a8171bf316ecc4aa4b38c29f942241c"
        "1ca0339d4a9150609d67072687aa68a2a8d0395d92fd6179ab96721fc3ce871d9e89fc155ae2e424b3f7281a55a1eabf"
        "c2ec6454bddbeda2424219395286fc9c8e25065bf51323bb363e6b0726a956fd5240b52010b69231241e49b12964ea9a"
        "ddeb7a3408d3f9200cc7585d9b259e14dd555bd09792ba6411725334b52a8f7b923409c0a1980633915a607abe3996e3"
        "3188bef87a81c9b8feab55656fb091764e49c166e65c4cbeed4961f28f44bd157e288821b2407a05c16ca5032ba9a2cc"
        "89220e6fb3808efc798b2adff4c4de3115e97ef34348b8dffcde0622d2982a0731bb6542eeb35a8b0668446bb4cd8dd5"
        "b5106c807db3792156ae2588dbbfde2611b80101557daac6466f96cca77459261fea1e2c70353d2436d53679376b7d9e"
        "e5907353b044adae5480b284b7aedd8798a0ce9c39232ef906ee6b81c0d62b9bbf031d83a3160c33add156b1b4a1f546"
        "225501e21e1e2323df03b1f71f89bd7765e83638a24e46bf824fd7f3f49a18188629f4659f208ae25a1fbd7ca5f1ac29"
        "96521c32751b4b5b52e48a511e079a178573220af6f9e8e73f606cc7e9267a2e91f43b4562927ac19b8d2a2153ecdcd1"
        "82d918ac32977ed9257852e9b49b0cadaea06aa29141ac160e8837cd5587b34263802a7442b3f4028ca6d256a1d48afe"
        "427a6ec260ffb33499df3a4aff8818728938803029e6a483d0e989795adae43ed5c0ce734e3a72721b41351b22ae87a8"
        "1ec0a4d1b5586da6a130a36ea10011a8f549a1e4ac6466f8b52bb681a2deca4e2368ec0d2fde444270d21c136639a603"
        "0991dbde7c798581b3b9a8ab10de8ef88ec79a180c8a4999a6aa013e9cd653b15e883e4f980ec6376115495b1154d62e"},
    {Format::astc, kGrey, 0x2050FF,
        "6d25cf734c49a1dd273e4d8fab5f5bdb8d1099ec05e8fdc7c1d734777648ab732922f17fa60e2d01ef4a96a9dfd2c115"
        "82b62300df0ba836540ab03541a252ed14a4b2120e38f59001f523a1dd3fd0321e9a09cc8673a07c48507c2d8ba09cae"
        "d571b8da4ee21d9a6a574fa4d9a3b3935d68af19f593c70a44e042fdecc177250acccd2ed41e93a0916975270c54914f"
        "2f758ee80555773af2775bd1b51c469c0f30d7e1ac2e867be937819abf6e18141903e8095bc02983a04b72f663635341"
        "6e88c941218dba6b202cdfd2bd8ca2e71104b298cb24b27d3e941e4b74952385f2a1328f11464ca7d6e036da4db1a99d"
        "04cbed23fc160297a009fb08c93870eca5cc115a9dc3bd15a4b965afa1c62879b2712a95e3bc2f0398b3de9bbeea7e12"
        "73c07215f155c9843dc60dc0d752b883dd0fc6b2fd06e61c52d04b7ff38caac7744155ce4d692fc18a660aea30544633"
        "2583469596c5b9eb394805519884bb35a1f8cd36059cb5d466e8fa6937acee61091330a0331a1d85be9d6e7e71d811ac"
        "1ec7b0cee303625e76ad5df590bcf51379e8d3b6fbc34209626bd92d810ff939c10283b26db762f237703a85bf431b80"
        "2e714c9d17dc2effc2c596c1aed4c0286d2b1be14ab2ad801af0f8cbe678c7d109a014dbfc820104f0c13759e2473581"
        "32b208d7a1f489ad46a77ab08589ae704e99ae386ff77d70a069d38663e00b8f4e69cb4f973adaab876f9761ffc26cf4"
        "8d69af924dd5657f9f9672330dc53c69cdfb3089ad7e411db204d080ff7843011e081890f8dd361fb5d3b47d2166dd17"
        "4e41e9892ac17c753819ed69e26eb8eb2ed5d2d07b5989ef96cd8ae61e4f86eb078adeec42f59d42ac6224219043bdb3"
        "98482440a6f0bab7835b9e3205d38cb30424c5872f8df4e0918dd06f6cb6f305999572a33160e537b51c9587be015737"
        "df6d6e425b584d634d08d61808df4e6f2fd58b04442e9718356a703334f8e491124e7481752e79a2aa6816c2aa69ef57"
        "1ec86230b42a87cd4eb40f12da2c697ba14818d79a797428206484fda9e078489dd048eda5a745accb116218bc616c94",
        "6d25cf734c49a1dd273e4d8fab5f5bdb8d1099ec05e8fdc7c1d734777648ab732922f17fa60e2d01ef4a96a9dfd2c115"
        "82b62300df0ba836540ab03541a252ed14a4b2120e38f59001f523a1dd3fd0321e9a09cc8673a07c48507c2d8ba09cae"
        "d571b8da4ee21d9a6a574fa4d9a3b3935d68af19f593c70a44e042fdecc177250acccd2ed41e93a0916975270c54914f"
        "2f758ee80555773af2775bd1b51c469c0f30d7e1ac2e867be937819abf6e18141903e8095bc02983a04b72f663635341"
        "6e88c941218d45cb212cdfd2bd8ca2e71104b298cb24b27d3e941e4b74952385f2a1328f11464ca7d6e036da4db1a99d"
        "04cbed23fc160297a009fb08c93870eca5cc115a9dc3bd15a4b965afa1c62879b2712a95e3bc2f0398b3de9bbeea7e12"
        "73c07215f155c9843dc60dc0d752b883dd0fc6b2fd06e61c52d04b7ff38caac7744155ce4d692fc18a660aea30544633"
        "2583469596c5b9eb394805519884bb35a1f8cd36059cb5d466e8fa6937acee61091330a0331a1d85be9d6e7e71d811ac"
        "1ec7b0cee303625e76ad5df590bcf51379e8d376eec34209626bd92d810ff939c102bd3dffcf52e937703a85bf431b80"
        "2e714c9d17dc2effc2c596c1aed4c0286d2b1be14ab2ad801af0f8cbe678c7d109a014dbfc820104f0c13759e2473581"
        "32b208d7a1f489ad46a77ab08589ae704e99ae386ff77d70a069d38663e00b8f4e69cb4f973adaab876f9761ffc26cf4"
        "8d69af924dd5657f9f9672330dc53c69cdfb3089ad7e411db204d080ff7843011e081890f8dd361fb5d3b47d2166dd17"
        "4e41e9892ac17c753819ed69e26eb8eb2ed5d2d07b5989ef96cd8ae61e4f86eb078adeec42f59d42ac6224219043bdb3"
        "98482440a6f0bab7835b9e3205d38cb30424c5872f8df4e0918dd06f6cb6f305999572a33160e537b51c9587be015737"
        "df6d6e425b584d634d08d61808df4e6f2fd58b04442e9718356a703334f8e491124e7481752e79a2aa6816c2aa69ef57"
        "1ec86230b42a87cd4eb40f12da2c697ba14818d79a797428206484fda9e078489dd048eda5a745accb116218bc616c94"},
    {Format::astc, kRed | kBrown | kGold, 0x30C060,
        "a2e81478007152dea8651f3fce59796199735b0320db2ebba29a7b376c967c681d288c47b1829c2e219c95ea9dcc11de"
        "17e9cdb03baa663226c270f7968e10f74db7dd00961392984a198dadab432ab5b9688056e6d8e6102767594f6b4ef85a"
        "3fd1d750864c4d2fdf20317bf545f878186829585af0328332bad35ac8b688e4310c17ccb330075e57ed14dc01bbbd4b"
        "4e8f8e8b6c1e921742bffc4ff8749baf1eab0414d65776a04b371c26564e9cb83e4da3d688e00135a2276d21208236fb"
        "09d961a15ec380e562febac91e44f069c2244c1b17d8372b40023f6fd8c57c5f8debe547d0cbd1d5e1dd557c56c3cd43"
        "3df3f60200fa6b795b98078c905112c6e680b2de9fd7f1ddddd936b0600c25733fc9a484c3d15024d5d482870cb4eefa"
        "3171f8ba097ca86a86d6fc2bdd52e59da6b109a32bfe4c452d156db858f5fafe210c803187bf5e8c71da2009bd895d6a"
        "8e80e33c91d8cfb7fcf436e7162ebf5fad84751deed258b00babd4d4a81e188c4589129d7d66274d8a3b4618af8e974b"
        "66a88ad10d9d102199434a6ba722c4681ea77b9d7baa178a66f20424d3055053230c24a1b44862e0e60a2be4c8997590"
        "3b983760191ce06d25d4826e82ba07fa44c9f2f88e0d53b5104c396159b74662ad008798d2230d985cde8342fae4b3bd"
        "c18a21fbe967014da1e00d7fd0024640eeeb331fd19df9a8b2da269f1404f9f58b4a6103acb7f47a2728568865d4708f"
        "73a216a549210ddd4828969a9feb72e1cd519388ec4641b57e110c56f505139d8389fc4b07e734bba465a587904bd006"
        "cec5de76a52e8a59a1136d5c6feb87ae4dd07f6dfad84a988e425d93a27063b0056d5c7bec4bd3dba32c66e9c7e858d7"
        "aff54e49ec2ffda5a6ddddc72e76f2788f0922e969769a4f5ef386245fe45a2a0515687fe36711f9282eab5490bfb689"
        "8fa2d3cf5b0a6e0c80ed83117eeb8dbbf1a98a069dde42558f38fcb8c1f9e86db1d0e04a7999adb4dde701eb0cd41e70"
        "ce90e2e0b819ac9fb6a3632252528d02242364437ff72c10b5973a8de665523c2e82d215bbe8be9273fbfeaa4dc53309",
        "a2e81478007152dea8651f3fce59796199735b0320db2ebba29a7b376c967c681d288c47b1829c2e219c95ea9dcc11de"
        "17e9cdb03baa663226c270f7968e10f74db7dd00961392984a198dadab432ab5b9688056e6d8e6102767594f6b4ef85a"
        "3fd1d750864c4d2fdf20317bf545f878186829585af0328332bad35ac8b688e4310c17ccb330075e57ed14dc01bbbd4b"
        "4e8f8e8b6c1e921742bffc4ff8749baf1eab0414d65776a04b371c26564e9cb83e4da3b6e0450235a2276d21208236fb"
        "09d961a15ec380e562febac91e44f069c2244c1b17d8372b40023f6fd8c57c5f8debe547d0cbd1d521da596f54c3cd43"
        "3df3f60200fa6b795b98078c905112c6e680b2de9fd7f1ddddd936b0600c25733fc9a484c3d15024d5d482870cb4eefa"
        "3171f8ba097ca86a86d6fc2bdd52e59da6b10983aefe4c452d156db858f5fafe210c803187bf5a0c25f42009bd895d6a"
        "8e80e33c91d8cfb7fcf436e7162ebf5fad848f8e18191313ab0ad5d4a81e188c4589129d7d66274d8a3b4618af8e974b"
        "66a88ad1159d102199434a6ba722c4681e872b34ccf060722a320424d3055053230c24a1b44862e0e60a2be4c8997590"
        "3b983760191ce06d25d4826e82ba07fa44c9f2f88e0d53b5104c396159b74662ad008798d2230d985cde8342fae4b3bd"
        "c18a21fbe967014da1e00d7fd0024640eeeb331fd19df9a8b2da269f1404f9f58b4a6103acb7f47a2728568865d4708f"
        "73a216a549210ddd4828969a9feb72e1cd519388ec4641b57e110c56f505139d8389fc4b07e734bba465a587904bd006"
        "cec5de76a52e8a59a1136d5c6feb87ae4dd07f6dfa188b97ceb15c93a27063b0056d5c7bec4bd3dba32c66e9c7e858d7"
        "aff54e49ecd3fda5a6ddddc72e76f2788f0922e969769a4f5ef386245fe45a2a0515687fe36711c12b2eab5490bfb689"
        "8f82297aa54ad71880ed83117eeb8dbbf1a98a069dde42558f38fcb8c1f9e86db1d0e04a7999adb4dde701eb0cd41e70"
        "ce90e2e0b819ac9fb6a3632252528d02242364437ff72c10b5973a8de665523c2e82d215bbe8be9273fbfeaa4dc53309"},
    {Format::bc1, kBlue | kPurple, 0xE0C020,
        "fd3feb3c9250b7974a9b528b69636321a461b55ef55b7beafd809a9a9e925b79a6342fa0fdb8b294d97fc6103d92089b"
        "25fa5e039f52a8e8a95f64d6589c1f7844066542ec38008d331dfd3b272416317694e2fe860397b774306378b5437d8a"
        "755622d6d7a0b48cb048f279829caa652af899a3e1f16bdc45cc8e26863d5f3b7f3a86a244b9d026843738deb48d03ed"
        "3034ef8590e4d26399aec2bd13a8e003589fe1ab3edaf8c6515d6410246fce2892601bc222871ef56a4d5297d238f40a"
        "9fe01f4dd917b3c7ce62f0074384d5d2603193dd4c79f944961506791781419820d804b84f628feb38e0f9e0ee503a63"
        "8930d1b67080c1c95e1fcdebdb9a4b6d",
        "fd3feb3c9250b797a4f64a9b3c363674a461b55ef55b7bea84cda4f69e925b79a6342fa0fdb8b294d97fa0413d92089b"
        "25fa64ee9f52a8e8a95f64d6589c1f7844066542ec38008d331d25ff27241631e2fee6ff925783b36378217bb0173c9a"
        "755622d6d7a0b48c217b03bd829caa6585ff2af8b4a43e8945cc8e26863d5f3b86a2c4dd11b8c562843738deb48d03ed"
        "3034ef8590e4d26399aec2bd13a8e003589fe1ab3edaf8c6515d2029246fce28c28b25ff22871ef56a4d5297d238f40a"
        "e6ffa5ff8c42e69243c5f0074384d5d2603193dd4c79f944961506791781419820d804b84f628feb38e0f9e0ee503a63"
        "2152d1b67080c1c95e1fcdebdb9a4b6d"},
};

}  // namespace

TEST_CASE("look paint: tones of colours") {
    CHECK(ToneOf({0, 0, 0}) == kBlack);
    CHECK(ToneOf({160, 156, 149}) == kGrey);        // a blade
    CHECK(ToneOf({39, 45, 90}) == kBlue);           // the Hylian Shield's face
    CHECK(ToneOf({123, 48, 38}) == kRed);           // its bird
    CHECK(ToneOf({96, 81, 66}) == kBrown);          // wood, leather
    CHECK(ToneOf({213, 180, 113}) == kGold);        // the gold gauntlets
    CHECK(ToneOf({62, 81, 42}) == kGreen);          // the tunic
    CHECK(ToneOf({78, 78, 137}) == kBlue);          // the Master Sword's hilt
}

TEST_CASE("look paint: formats as a BNTX names them") {
    CHECK(FormatOf(0x1a06) == Format::bc1);
    CHECK(FormatOf(0x1c01) == Format::bc3);
    CHECK(FormatOf(0x2d06) == Format::astc);
    CHECK(FormatOf(0x3306) == Format::astc);       // 8x6
    CHECK(FormatOf(0x3a06) == Format::astc);
    CHECK_FALSE(FormatOf(0x0b01).has_value());     // RGBA8: not rewritten
    CHECK(BlockBytes(Format::bc1) == 8);
    CHECK(BlockBytes(Format::astc) == 16);
}

TEST_CASE("look paint: a shade keeps the brightness against the part's average") {
    const Rgb target{200, 48, 32};
    const double base = Luma({40, 46, 90});
    CHECK(Shade({40, 46, 90}, target, base) == target);   // the average becomes the colour
    const Rgb dark = Shade({20, 23, 45}, target, base);
    CHECK(dark[0] < target[0]);
    CHECK(Luma(dark) < Luma(target));
}

TEST_CASE("look paint: the same bytes as the pack tool's rewrite") {
    for (const Golden& g : kGolden) {
        CAPTURE(g.colour);
        const auto in = Hex(g.in);
        const auto want = Hex(g.out);
        std::vector<std::uint8_t> out(in.size());
        const std::size_t changed = Recolour(g.format, in, out, {Rule{g.tones, g.colour}});
        CHECK(changed > 0);
        CHECK((out == want));
        // in place too
        auto same = in;
        Recolour(g.format, same, same, {Rule{g.tones, g.colour}});
        CHECK((same == want));
    }
}

TEST_CASE("look paint: parts of one texture, each its own colour; the rest stays") {
    // BC1 blocks: a blue one, a grey one, a red one, a brown one (c0 > c1: 4-colour mode).
    auto block = [](std::uint16_t c0, std::uint16_t c1) {
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c0 >> 8),
                                         static_cast<std::uint8_t>(c1), static_cast<std::uint8_t>(c1 >> 8),
                                         0x1b, 0x1b, 0x1b, 0x1b};
    };
    auto rgb565 = [](int r, int g, int b) {
        return static_cast<std::uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
    };
    std::vector<std::uint8_t> tex;
    for (const auto& b : {block(rgb565(60, 70, 160), rgb565(30, 35, 80)), block(rgb565(180, 176, 170), rgb565(90, 88, 85)),
                          block(rgb565(150, 50, 40), rgb565(80, 28, 20)), block(rgb565(110, 90, 70), rgb565(60, 50, 40))}) {
        tex.insert(tex.end(), b.begin(), b.end());
    }
    std::vector<std::uint8_t> out(tex.size());
    const std::size_t changed = Recolour(Format::bc1, tex, out, {{kGrey, 0xE0C030}, {kBlue, 0x20A040}});
    CHECK(changed == 2);
    auto c0 = [&](std::size_t i) { return static_cast<std::uint16_t>(out[i * 8] | out[i * 8 + 1] << 8); };
    const auto ends = Ends(Format::bc1, std::span<const std::uint8_t>(out.data(), 8));
    CHECK(ToneOf(ends[0]) == kGreen);              // the blue part is green now
    CHECK(ToneOf(Ends(Format::bc1, std::span<const std::uint8_t>(out.data() + 8, 8))[0]) == kGold);   // the grey one gold
    CHECK(std::equal(out.begin() + 16, out.end(), tex.begin() + 16));   // red and brown untouched
    CHECK(c0(0) > static_cast<std::uint16_t>(out[2] | out[3] << 8));   // still 4-colour mode
    // A rule for a tone the texture has not: nothing changes.
    CHECK(Recolour(Format::bc1, tex, out, {{kPink, 0xFF00FF}}) == 0);
    CHECK((out == tex));
}
