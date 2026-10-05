#include "mini_json.h"

#include <cctype>

namespace osc {

void MiniJson::SkipSpace(const std::string &s, size_t &pos)
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        pos++;
    }
}

bool MiniJson::ParseString(const std::string &s, size_t &pos, std::string &out)
{
    if (pos >= s.size() || s[pos] != '"') {
        return false;
    }
    pos++;
    out.clear();
    while (pos < s.size() && s[pos] != '"') {
        char c = s[pos];
        if (c == '\\' && pos + 1 < s.size()) {
            pos++;
            char e = s[pos];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    // 仅支持 BMP 基本区，转成 UTF-8
                    if (pos + 4 < s.size()) {
                        unsigned code = 0;
                        for (int i = 1; i <= 4; i++) {
                            char h = s[pos + i];
                            code <<= 4;
                            if (h >= '0' && h <= '9') {
                                code |= static_cast<unsigned>(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                code |= static_cast<unsigned>(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                code |= static_cast<unsigned>(h - 'A' + 10);
                            }
                        }
                        pos += 4;
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                    }
                    break;
                }
                default: out += e; break;
            }
            pos++;
        } else {
            out += c;
            pos++;
        }
    }
    if (pos >= s.size()) {
        return false;
    }
    pos++; // 跳过结尾引号
    return true;
}

bool MiniJson::ParseValue(const std::string &s, size_t &pos, JsonValue &out)
{
    SkipSpace(s, pos);
    if (pos >= s.size()) {
        return false;
    }
    char c = s[pos];
    if (c == '{') {
        out.type = JsonValue::Type::OBJECT;
        pos++;
        SkipSpace(s, pos);
        if (pos < s.size() && s[pos] == '}') {
            pos++;
            return true;
        }
        while (pos < s.size()) {
            std::string key;
            if (!ParseString(s, pos, key)) {
                return false;
            }
            SkipSpace(s, pos);
            if (pos >= s.size() || s[pos] != ':') {
                return false;
            }
            pos++;
            JsonValue val;
            if (!ParseValue(s, pos, val)) {
                return false;
            }
            out.obj[key] = val;
            SkipSpace(s, pos);
            if (pos < s.size() && s[pos] == ',') {
                pos++;
                SkipSpace(s, pos);
                continue;
            }
            if (pos < s.size() && s[pos] == '}') {
                pos++;
                return true;
            }
            return false;
        }
        return false;
    }
    if (c == '[') {
        out.type = JsonValue::Type::ARRAY;
        pos++;
        SkipSpace(s, pos);
        if (pos < s.size() && s[pos] == ']') {
            pos++;
            return true;
        }
        while (pos < s.size()) {
            JsonValue val;
            if (!ParseValue(s, pos, val)) {
                return false;
            }
            out.arr.push_back(val);
            SkipSpace(s, pos);
            if (pos < s.size() && s[pos] == ',') {
                pos++;
                SkipSpace(s, pos);
                continue;
            }
            if (pos < s.size() && s[pos] == ']') {
                pos++;
                return true;
            }
            return false;
        }
        return false;
    }
    if (c == '"') {
        out.type = JsonValue::Type::STRING;
        return ParseString(s, pos, out.strVal);
    }
    if (c == 't' || c == 'f') {
        out.type = JsonValue::Type::BOOL;
        out.boolVal = (c == 't');
        pos += (c == 't') ? 4 : 5;
        return pos <= s.size();
    }
    if (c == 'n') {
        out.type = JsonValue::Type::NUL;
        pos += 4;
        return pos <= s.size();
    }
    // number
    out.type = JsonValue::Type::NUMBER;
    size_t start = pos;
    while (pos < s.size() && (isdigit(s[pos]) || s[pos] == '-' || s[pos] == '+' ||
        s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E')) {
        pos++;
    }
    if (pos == start) {
        return false;
    }
    out.numVal = std::stod(s.substr(start, pos - start));
    return true;
}

bool MiniJson::Parse(const std::string &text, JsonValue &out)
{
    size_t pos = 0;
    return ParseValue(text, pos, out);
}

} // namespace osc
