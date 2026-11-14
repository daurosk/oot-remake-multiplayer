// A small JSON reader and writer for the mod loader (docs/mods.md): a mod's
// mod.json and the loader's own settings file. Strict JSON (RFC 8259), UTF-8,
// with limits instead of trust: a mod's file is somebody else's text. No game
// or co-op code here.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopModJson {

inline constexpr std::size_t kMaxText = 256 * 1024;  // a longer text is refused
inline constexpr int kMaxDepth = 16;                 // arrays and objects inside each other

struct Value {
    struct Member;  // (defined below: a member of a still incomplete Value is no std::pair for libstdc++)
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<Member> object;  // in the file's order; a key twice: the last counts in Get

    // An object's member, nullptr when this is no object or has no such key.
    const Value* Get(std::string_view key) const;
    // The member as text / number / bool when it is one, else nothing.
    std::optional<std::string> Text(std::string_view key) const;
    std::optional<double> Number(std::string_view key) const;
    std::optional<bool> Bool(std::string_view key) const;

    static Value Of(std::string text);
    static Value Of(double number);
    static Value Of(bool boolean);
};

// An object's member: its key and value, named as std::pair's were.
struct Value::Member {
    Member(std::string key, Value value) : first(std::move(key)), second(std::move(value)) {}
    std::string first;
    Value second;
};

// The text's value; nothing with `error` set (what and where, plain English) when it is not one JSON value.
std::optional<Value> Parse(std::string_view text, std::string* error = nullptr);

// The value as JSON text: objects one member per line (two spaces per level), so the file can be read and edited.
std::string Write(const Value& value);

}  // namespace CoopModJson
