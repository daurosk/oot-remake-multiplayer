// SPDX-License-Identifier: GPL-3.0-or-later
// CoopClipboard (hookshot/src/coop_clipboard_posix.h), the module's Linux clipboard, with stand-in
// wl-copy / wl-paste / xclip scripts on PATH that keep the text in a file.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "coop_clipboard_posix.h"

namespace {

namespace fs = std::filesystem;

std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void Script(const fs::path& p, const std::string& body) {
    {
        std::ofstream f(p, std::ios::binary);
        f << "#!/bin/sh\n" << body;
    }
    fs::permissions(p, fs::perms::owner_all);
}

std::string Get(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void Put(const char* name, const std::string& value) {
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
}

// A folder of stand-in tools, put first on PATH; the desktop variables set as asked. Undone at the end.
struct Desktop {
    fs::path dir;
    std::string path, wayland, display;
    Desktop(const char* wayland_display, const char* x_display)
        : path(Get("PATH")), wayland(Get("WAYLAND_DISPLAY")), display(Get("DISPLAY")) {
        dir = fs::temp_directory_path() / ("coop-clip-test-" + std::to_string(getpid()) + "-" +
                                           std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir);
        const std::string clip = (dir / "clip.txt").string(), fds = (dir / "fds.txt").string();
        // (the copy tools also list their own open descriptors, to see that none of the game's came along)
        Script(dir / "wl-copy", "cat > '" + clip + "'\nls /proc/$$/fd > '" + fds + "'\n");
        Script(dir / "wl-paste", "[ \"$1\" = -n ] || exit 3\ncat '" + clip + "'\necho\n");
        Script(dir / "xclip",
               "[ \"$1 $2\" = '-selection clipboard' ] || exit 3\n"
               "if [ \"$3\" = -i ]; then cat > '" + clip + "'; ls /proc/$$/fd > '" + fds + "'; "
               "else cat '" + clip + "'; fi\n");
        Put("PATH", dir.string() + ":" + path);
        Put("WAYLAND_DISPLAY", wayland_display);
        Put("DISPLAY", x_display);
    }
    ~Desktop() {
        Put("PATH", path);
        Put("WAYLAND_DISPLAY", wayland);
        Put("DISPLAY", display);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string Clip() const { return Slurp(dir / "clip.txt"); }
    std::string Fds() const { return Slurp(dir / "fds.txt"); }
};

}  // namespace

TEST_CASE("Linux clipboard: Wayland's wl-copy and wl-paste") {
    Desktop d("wayland-0", "");
    CHECK(CoopClipboard::Copy("ABCD-EFGH-1234"));
    CHECK(d.Clip() == "ABCD-EFGH-1234");
    CHECK(CoopClipboard::Paste() == "ABCD-EFGH-1234");   // (wl-paste's line end dropped)
}

TEST_CASE("Linux clipboard: X11's xclip when there is no Wayland") {
    Desktop d("", ":7");
    CHECK(CoopClipboard::Copy("code with spaces and 'quotes'\n"));
    CHECK(d.Clip() == "code with spaces and 'quotes'\n");
    CHECK(CoopClipboard::Paste() == "code with spaces and 'quotes'");
}

TEST_CASE("Linux clipboard: no desktop, no tool: nothing, and the game goes on") {
    Desktop d("", "");
    CHECK_FALSE(CoopClipboard::Copy("x"));
    CHECK(CoopClipboard::Paste().empty());
    CHECK(d.Clip().empty());
}

TEST_CASE("Linux clipboard: the tool gets none of the game's descriptors") {
    Desktop d("wayland-0", "");
    const int game_fd = open("/dev/null", O_RDONLY);   // (as a socket of the game: no O_CLOEXEC)
    REQUIRE(game_fd >= 0);
    REQUIRE(dup2(game_fd, 200) == 200);
    CHECK(CoopClipboard::Copy("x"));
    close(200);
    close(game_fd);
    std::istringstream fds(d.Fds());
    std::string fd;
    int count = 0;
    while (fds >> fd) {
        ++count;
        CHECK_MESSAGE(fd != "200", "the clipboard tool kept a descriptor of the game");
    }
    CHECK(count >= 3);   // (0, 1, 2 and the shell's own script)
}

TEST_CASE("Linux clipboard: a long paste is cut at 4096 bytes") {
    Desktop d("wayland-0", "");
    REQUIRE(CoopClipboard::Copy(std::string(10000, 'a')));
    CHECK(CoopClipboard::Paste().size() == 4096);
}
