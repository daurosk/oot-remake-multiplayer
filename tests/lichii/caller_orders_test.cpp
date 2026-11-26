// Every caller of game functions has its own order (lichii/hook/coop_caller_orders.h, room-objects): the numbers are
// unique, and every AddCaller in the hook takes its order from that file, so a clash fails here, not at run time.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "coop_caller_orders.h"

namespace {

std::filesystem::path Root() { return std::filesystem::path(COOP_TEST_SOURCE_DIR) / ".."; }

std::string Text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<std::filesystem::path> HookSources() {
    std::vector<std::filesystem::path> out;
    for (const char* dir : {"lichii/hook", "hookshot/src"}) {
        const auto d = Root() / dir;
        if (!std::filesystem::exists(d)) continue;
        for (const auto& e : std::filesystem::directory_iterator(d)) {
            const auto ext = e.path().extension().string();
            if (ext == ".cpp" || ext == ".h" || ext == ".hpp") out.push_back(e.path());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("caller orders: unique, and every order the header names is in its list") {
    CHECK(CoopCallerOrders::Unique());
    const std::string header = Text(Root() / "lichii/hook/coop_caller_orders.h");
    const std::regex def(R"(inline constexpr int (k\w+) = (\d+);)");
    std::map<std::string, int> named;
    std::set<int> values;
    for (auto it = std::sregex_iterator(header.begin(), header.end(), def); it != std::sregex_iterator(); ++it) {
        const int v = std::stoi((*it)[2].str());
        CHECK_MESSAGE(values.insert(v).second, "order " << v << " used twice in coop_caller_orders.h");
        named[(*it)[1].str()] = v;
    }
    REQUIRE(named.size() == CoopCallerOrders::kAll.size());  // a constant left out of kAll is not checked
}

TEST_CASE("caller orders: every AddCaller in the hook takes its order from coop_caller_orders.h") {
    const auto sources = HookSources();
    REQUIRE(sources.size() > 10);
    // Names defined from the header anywhere (kCallerOrder = CoopCallerOrders::kDrops, ...).
    std::set<std::string> from_header;
    const std::regex alias(R"((\w+) = CoopCallerOrders::k\w+;)");
    const std::regex literal(R"(CallerOrder\s*=\s*[0-9])");
    for (const auto& p : sources) {
        const std::string t = Text(p);
        for (auto it = std::sregex_iterator(t.begin(), t.end(), alias); it != std::sregex_iterator(); ++it) {
            from_header.insert((*it)[1].str());
        }
        CHECK_MESSAGE(!std::regex_search(t, literal), p.filename().string() << " sets a caller order by number");
    }
    const std::regex call(R"(AddCaller\(\s*([^,()]+),)");
    std::size_t calls = 0;
    for (const auto& p : sources) {
        const std::string t = Text(p);
        for (auto it = std::sregex_iterator(t.begin(), t.end(), call); it != std::sregex_iterator(); ++it) {
            std::string arg = (*it)[1].str();
            if (arg.rfind("int ", 0) == 0) continue;  // AddCaller's own definition (coop_svc.h)
            ++calls;
            const auto last = arg.substr(arg.rfind(':') == std::string::npos ? 0 : arg.rfind(':') + 1);
            const bool ok = arg.rfind("CoopCallerOrders::", 0) == 0 || from_header.contains(last);
            CHECK_MESSAGE(ok, p.filename().string() << ": AddCaller(" << arg << ", ...) has an order of its own");
        }
    }
    CHECK(calls >= CoopCallerOrders::kAll.size());
}
