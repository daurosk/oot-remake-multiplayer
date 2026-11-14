// coop_mod_json.h: the reader is a plain recursive descent over a
// string_view; every step checks the end of the text.
#include "coop_mod_json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace CoopModJson {

const Value* Value::Get(std::string_view key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    const Value* found = nullptr;
    for (const auto& [name, value] : object) {
        if (name == key) {
            found = &value;
        }
    }
    return found;
}

std::optional<std::string> Value::Text(std::string_view key) const {
    const Value* v = Get(key);
    return v != nullptr && v->kind == Kind::String ? std::optional<std::string>(v->string) : std::nullopt;
}

std::optional<double> Value::Number(std::string_view key) const {
    const Value* v = Get(key);
    return v != nullptr && v->kind == Kind::Number ? std::optional<double>(v->number) : std::nullopt;
}

std::optional<bool> Value::Bool(std::string_view key) const {
    const Value* v = Get(key);
    return v != nullptr && v->kind == Kind::Bool ? std::optional<bool>(v->boolean) : std::nullopt;
}

Value Value::Of(std::string text) {
    Value v;
    v.kind = Kind::String;
    v.string = std::move(text);
    return v;
}

Value Value::Of(double number) {
    Value v;
    v.kind = Kind::Number;
    v.number = number;
    return v;
}

Value Value::Of(bool boolean) {
    Value v;
    v.kind = Kind::Bool;
    v.boolean = boolean;
    return v;
}

namespace {

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    std::optional<Value> Document(std::string* error) {
        Value v;
        Space();
        if (!Any(v, 0)) {
            return Fail(error);
        }
        Space();
        if (at_ != text_.size()) {
            Say("text after the value");
            return Fail(error);
        }
        return v;
    }

private:
    std::optional<Value> Fail(std::string* error) const {
        if (error != nullptr) {
            *error = why_ + " at byte " + std::to_string(where_);
        }
        return std::nullopt;
    }

    bool Say(const char* why) {
        if (why_.empty()) {
            why_ = why;
            where_ = at_;
        }
        return false;
    }

    void Space() {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    bool Word(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) {
            return Say("not a JSON value");
        }
        at_ += word.size();
        return true;
    }

    bool Any(Value& out, int depth) {
        if (at_ >= text_.size()) {
            return Say("the text ends where a value should be");
        }
        switch (text_[at_]) {
        case '{':
            return Object(out, depth);
        case '[':
            return Array(out, depth);
        case '"':
            out.kind = Value::Kind::String;
            return String(out.string);
        case 't':
            out = Value::Of(true);
            return Word("true");
        case 'f':
            out = Value::Of(false);
            return Word("false");
        case 'n':
            out = Value{};
            return Word("null");
        default:
            return Number(out);
        }
    }

    bool Object(Value& out, int depth) {
        if (depth >= kMaxDepth) {
            return Say("nested too deep");
        }
        out.kind = Value::Kind::Object;
        ++at_;
        Space();
        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            return true;
        }
        for (;;) {
            Space();
            if (at_ >= text_.size() || text_[at_] != '"') {
                return Say("a member's name (in quotes) is missing");
            }
            std::string key;
            if (!String(key)) {
                return false;
            }
            Space();
            if (at_ >= text_.size() || text_[at_] != ':') {
                return Say("':' is missing after a member's name");
            }
            ++at_;
            Space();
            Value member;
            if (!Any(member, depth + 1)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(member));
            Space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            return Say("',' or '}' is missing in an object");
        }
    }

    bool Array(Value& out, int depth) {
        if (depth >= kMaxDepth) {
            return Say("nested too deep");
        }
        out.kind = Value::Kind::Array;
        ++at_;
        Space();
        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            return true;
        }
        for (;;) {
            Space();
            Value item;
            if (!Any(item, depth + 1)) {
                return false;
            }
            out.array.push_back(std::move(item));
            Space();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                continue;
            }
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            return Say("',' or ']' is missing in an array");
        }
    }

    bool Hex4(std::uint32_t& out) {
        if (text_.size() - at_ < 4) {
            return Say("a \\u escape is cut short");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return Say("a \\u escape needs four hex digits");
            }
            out = out * 16 + digit;
        }
        return true;
    }

    static void Utf8(std::uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        } else {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool String(std::string& out) {
        ++at_;  // the opening quote
        for (;;) {
            if (at_ >= text_.size()) {
                return Say("a text's closing quote is missing");
            }
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                --at_;
                return Say("a control character inside a text");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) {
                return Say("a text's closing quote is missing");
            }
            const char e = text_[at_++];
            switch (e) {
            case '"':
            case '\\':
            case '/':
                out += e;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!Hex4(cp)) {
                    return false;
                }
                if (cp >= 0xd800 && cp < 0xdc00) {  // the first half of a pair
                    std::uint32_t low = 0;
                    if (text_.substr(at_, 2) != "\\u") {
                        return Say("half of a \\u pair");
                    }
                    at_ += 2;
                    if (!Hex4(low)) {
                        return false;
                    }
                    if (low < 0xdc00 || low > 0xdfff) {
                        return Say("half of a \\u pair");
                    }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return Say("half of a \\u pair");
                }
                Utf8(cp, out);
                break;
            }
            default:
                --at_;
                return Say("an escape JSON does not have");
            }
        }
    }

    bool Number(Value& out) {
        const std::size_t start = at_;
        std::size_t i = at_;
        const auto digits = [&] {
            const std::size_t first = i;
            while (i < text_.size() && text_[i] >= '0' && text_[i] <= '9') {
                ++i;
            }
            return i - first;
        };
        if (i < text_.size() && text_[i] == '-') {
            ++i;
        }
        if (i < text_.size() && text_[i] == '0') {
            ++i;
        } else if (digits() == 0) {
            return Say("not a JSON value");
        }
        if (i < text_.size() && text_[i] == '.') {
            ++i;
            if (digits() == 0) {
                return Say("a number needs digits after its point");
            }
        }
        if (i < text_.size() && (text_[i] == 'e' || text_[i] == 'E')) {
            ++i;
            if (i < text_.size() && (text_[i] == '+' || text_[i] == '-')) {
                ++i;
            }
            if (digits() == 0) {
                return Say("a number needs digits in its exponent");
            }
        }
        double number = 0;
        const auto [end, ec] = std::from_chars(text_.data() + start, text_.data() + i, number);
        if (ec != std::errc() || end != text_.data() + i || !std::isfinite(number)) {
            return Say("a number out of range");
        }
        at_ = i;
        out = Value::Of(number);
        return true;
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string why_;
    std::size_t where_ = 0;
};

void Quote(const std::string& s, std::string& out) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void WriteAt(const Value& v, int level, std::string& out) {
    const auto indent = [&](int n) { out.append(static_cast<std::size_t>(n) * 2, ' '); };
    switch (v.kind) {
    case Value::Kind::Null:
        out += "null";
        break;
    case Value::Kind::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case Value::Kind::Number: {
        char buf[32];
        // Whole numbers without a point; the shortest text that reads back the same otherwise.
        if (v.number == std::floor(v.number) && std::fabs(v.number) < 1e15) {
            std::snprintf(buf, sizeof buf, "%.0f", v.number);
            out += buf;
        } else {
            const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v.number);
            out.append(buf, ec == std::errc() ? end : buf);
        }
        break;
    }
    case Value::Kind::String:
        Quote(v.string, out);
        break;
    case Value::Kind::Array:
        out += '[';
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            out += i ? ", " : "";
            WriteAt(v.array[i], level, out);
        }
        out += ']';
        break;
    case Value::Kind::Object:
        if (v.object.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (std::size_t i = 0; i < v.object.size(); ++i) {
            indent(level + 1);
            Quote(v.object[i].first, out);
            out += ": ";
            WriteAt(v.object[i].second, level + 1, out);
            out += i + 1 < v.object.size() ? ",\n" : "\n";
        }
        indent(level);
        out += '}';
        break;
    }
}

}  // namespace

std::optional<Value> Parse(std::string_view text, std::string* error) {
    if (text.size() > kMaxText) {
        if (error != nullptr) {
            *error = "the text is longer than " + std::to_string(kMaxText) + " bytes";
        }
        return std::nullopt;
    }
    if (text.substr(0, 3) == "\xef\xbb\xbf") {  // a byte order mark some editors write
        text.remove_prefix(3);
    }
    return Reader(text).Document(error);
}

std::string Write(const Value& value) {
    std::string out;
    WriteAt(value, 0, out);
    out += '\n';
    return out;
}

}  // namespace CoopModJson
