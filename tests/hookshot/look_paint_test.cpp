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

// The tunic (CoopLookParts::TunicPainted): random blocks (synthetic) run
// through the pack tool's own tunic rewrites: the green parts of the other
// textures (tunic_extra.py _astc_recolor, _bc_recolor, its real is_green,
// against a base given) and the whole tile (tunic.py recolor_bntx).
struct TunicGolden {
    Format format;
    Takes takes;
    std::uint32_t colour;
    double base;
    const char* in;
    const char* out;
};

const TunicGolden kTunicGolden[] = {
    {Format::astc, Takes::green, 0xD02020, 0.0746,
        "a1924cc59f4690198471e70901b74561b808d131cc03e8cacf4c4b62e5653b64c5f13a2b3ce47840781131ddd0003798"
        "21cb9281bc23aaa0bb25ffde19522cfb85aa33dd16817a737219c6b5f20e393742ca8461e8371e5db730d46eeaa627d4"
        "0fc4067798e9d1beb46b1ab702ef02d42e72b978b193b9df498198dc80f5077ef1f0bd428197f9026563422d7d9a8636"
        "d588a320c2a9e5e0702e7c3879e772b0250039414dcfd3a577d181611a2993f30de9b4c5b4630f989a118f25038b7101"
        "0db5332b8685450d01a57954f2e501646df561cc9074bb8df3eed9a137f2ccf5cbc0ee992ed685664c5bac3027bef519"
        "9e8988252877104ff2ec4c79e641ac4c",
        "a1924cc59f76881a8471e70901b74561b808d1319c03e8cacf4c4b62e5653b64c5f13aab14e478f8350d31ddd0003798"
        "21cb92e1df36aaa0bb25ffde19522cfb85aa33dd169162737219c6b5f20e393742ca842191321e55b730d46eeaa627d4"
        "0fc4067798e9d1beb46b1ab702ef02d42e72b978b193b9df498198dc80f5077ef1f0bd428197f9026563422d7d9a8636"
        "d588a320c2a9e5e0702e7c3879e772b02500ff9fccc6ccd277d181611a2993f30de9b4c5146071188e1b405816cb6500"
        "0db5332bfea5840a0125890515fe01646df5612ca028bb8df3ee815a31f2ccf5cbc0de8c2ed685664c5bac3027bef519"
        "9e89882588f8c1a3e0e01c7be641ac4c"},
    {Format::bc1, Takes::green, 0x3050E0, 0.0746,
        "b13cad3971121b1eb68acb33009079dc55d04a5b0b0cc495834f0143b7f4100f996d677690e1522b102b9b5760a0e8fd"
        "d8d781675f4e014279e80c2a216c9dd4e53981da96c9a07d78be2f5a947cfa19afd0349d66c0fdb5c84a8d323305a56b"
        "050e684222854cf887c10a770639114f6667496e94eac09b96ce0b153aeb46b4ad1dcd312de9a48ba1499c8a27135f45"
        "290e7d9bcc76805287080052c8382b97c962e3398b58ee399c7335b6807d33d9036c476c5de3183ba25958361f136960",
        "7f5cad3971121b1eb68a9f43009079dc55d05f430b0cc495df8edf3ab7f4100f996dff7d90e1522b102b1f9760a0e8fd"
        "bf9fdf8e5f4e014279e80c2a216c9dd4f52181da96c9a07d9f862f5a947cfa19afd0349d66c0fdb5de328d323305a56b"
        "7a327f6d66901df987c19f860639114fdf75df8e81ead58bbf8e9f5c3aeb46b4cd313f6d6ce8a19bd4219c8a27135f45"
        "9f757d9bcc7680528708382ac8382b97c962f5218b58ee399c733f7e807d33d9ff4b3f545de3183bff7da2594a463c35"},
    {Format::bc3, Takes::all, 0x7F3FBF, 0,   // (0: the texture's own average, as tunic.py)
        "25873f7b8dc78542cb43b0110d343d5ce1fc6cbe1b4ee99a2c6d406fa97207f719c5ec3eac07e63b8f236876c91d9d6b"
        "778c196e977af2b99ab62476f15a0c571f8adbe253ce2d94718c71a2901d04117247e8d509ff4ea4904ea049435f31af"
        "aa1c8e32dea0928eae6b339d68c66d89bbadedc2f0a1048e6963af8c2137088dede8de0acb52f42de43bcca9c2f873f2"
        "b7059c4790f7736e1395307cd695efbc53b9c6a208b6eb4d294247725096e80bac6193616c683cc4f29cf6b551fda97e"
        "cbb5cc2218a0b206339dc55f0234cb8cd2d39f4645076d22e6310b2c61a719f1e50ebd5abd1922d05054694581c7af3d"
        "26437c10ddfbfac1053aa33fbd4bfbea00000000000000000000000000000000",
        "25873f7b8dc785427261aa300d343d5ce1fc6cbe1b4ee99a1882ffb2a97207f719c5ec3eac07e63b51599da2c91d9d6b"
        "778c196e977af2b9ffb29c9af15a0c571f8adbe253ce2d94f7797259901d04117247e8d509ff4ea4bea2a930435f31af"
        "aa1c8e32dea0928e92615a8a68c66d89bbadedc2f0a1048e715917822137088dede8de0acb52f42d72615051c2f873f2"
        "b7059c4790f7736e398ad571d695efbc53b9c6a208b6eb4dcb380d495096e80bac6193616c683cc4398abea251fda97e"
        "cbb5cc2218a0b2065a8a1fbb0234cb8cd2d39f4645076d22a930926161a719f1e50ebd5abd1922d0d571388281c7af3d"
        "26437c10ddfbfac1ca301fbbbd4bfbea00000000000000000000000000000000"},
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

TEST_CASE("look paint: parts of one texture, each its own colour, the rest stays") {
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

TEST_CASE("look paint: the tunic's green is the pack tool's is_green") {
    // (tools/puppet_pack/tunic_extra.py is_green gave these)
    struct Sample {
        Rgb c;
        bool green;
    };
    const Sample samples[] = {
        {{62, 81, 42}, true},     {{200, 220, 190}, true},  {{190, 205, 120}, true}, {{150, 160, 150}, false},
        {{96, 81, 66}, false},    {{120, 110, 60}, true},   {{40, 200, 220}, false}, {{2, 5, 1}, false},
        {{5, 6, 5}, false},       {{90, 100, 95}, true},    {{255, 255, 255}, false}, {{30, 160, 60}, true},
        {{160, 150, 40}, true},   {{130, 145, 128}, true},  {{20, 40, 200}, false},
    };
    for (const Sample& s : samples) {
        CAPTURE(int(s.c[0]));
        CAPTURE(int(s.c[1]));
        CAPTURE(int(s.c[2]));
        CHECK(IsGreen(s.c) == s.green);
    }
    // The pale greens of the adult's lower tunic: no green tone by ToneOf, green by the pack tool.
    CHECK(ToneOf({200, 220, 190}) == kGrey);
    CHECK(ToneOf({160, 150, 40}) == kGold);
}

TEST_CASE("look paint: the tunic's textures, the same bytes as the pack tool's") {
    for (const TunicGolden& g : kTunicGolden) {
        CAPTURE(g.colour);
        const auto in = Hex(g.in);
        const auto want = Hex(g.out);
        const double base = g.base > 0 ? g.base : AverageLuma(g.format, in);
        std::vector<std::uint8_t> out(in.size());
        CHECK(Recolour(g.format, in, out, {Rule{0, g.colour, g.takes, base}}) > 0);
        CHECK((out == want));
    }
}

TEST_CASE("look paint: a base given shades every texture against it, as the tunic's tile") {
    // BC3 blocks whose ends are all one colour: a "tile" and a "dark tile" some 0.64 as bright.
    auto bc3 = [](int r, int g, int b) {
        const auto c = static_cast<std::uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 |
                                                  ((b * 31 + 127) / 255));
        std::vector<std::uint8_t> out;
        for (int i = 0; i < 8; ++i) {
            const std::uint8_t block[16] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0, static_cast<std::uint8_t>(c),
                                            static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c),
                                            static_cast<std::uint8_t>(c >> 8), 0, 0, 0, 0};
            out.insert(out.end(), block, block + 16);
        }
        return out;
    };
    const auto tile = bc3(76, 140, 60), dark = bc3(60, 114, 48);
    const double base = AverageLuma(Format::bc3, tile);
    CHECK(AverageLuma(Format::bc3, std::vector<std::uint8_t>(16, 0)) == 0.0);   // zero blocks are not counted
    const double ratio = AverageLuma(Format::bc3, dark) / base;
    CHECK(ratio == doctest::Approx(0.64).epsilon(0.05));
    for (const std::uint32_t colour : {0xD02020u, 0x3070FFu, 0xE8E8E8u}) {
        std::vector<std::uint8_t> t(tile.size()), d(dark.size());
        Recolour(Format::bc3, tile, t, {Rule{0, colour, Takes::all, base}});
        Recolour(Format::bc3, dark, d, {Rule{0, colour, Takes::all, base}});
        // The tile becomes the colour (its average), the dark one stays as much darker.
        const Rgb want{static_cast<std::uint8_t>(colour >> 16), static_cast<std::uint8_t>(colour >> 8),
                       static_cast<std::uint8_t>(colour)};
        CHECK(AverageLuma(Format::bc3, t) == doctest::Approx(Luma(want)).epsilon(0.05));
        CHECK(AverageLuma(Format::bc3, d) / AverageLuma(Format::bc3, t) == doctest::Approx(ratio).epsilon(0.06));
        // Without the base each texture is shaded against its own average: the dark one as bright as the tile.
        Recolour(Format::bc3, dark, d, {Rule{0, colour, Takes::all}});
        CHECK(AverageLuma(Format::bc3, d) == doctest::Approx(AverageLuma(Format::bc3, t)).epsilon(0.05));
    }
}
