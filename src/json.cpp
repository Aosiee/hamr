// JSON. A table encodes as an array when its metatable says so
// (`__json = "array"`), when it is empty, or when its keys are exactly
// 1..n; otherwise as an object with its keys sorted, so the same value
// always encodes to the same text. `__json = "object"` forces an object.

#include "host.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace hamr {

namespace {

void EncodeString(std::string& out, const char* s, size_t n) {
    out += '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

void EncodeNumber(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out += "null";
        return;
    }
    if (std::floor(v) == v && std::fabs(v) < 1e15) {
        out += std::to_string((long long)v);
        return;
    }
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), v);
    out.append(buffer, result.ptr);
}

void Newline(std::string& out, int indent, int depth) {
    if (indent <= 0)
        return;
    out += '\n';
    out.append((size_t)(indent * depth), ' ');
}

void Encode(lua_State* L, int index, std::string& out, int indent, int depth);

std::string MetaMark(lua_State* L, int index) {
    std::string mark;
    if (lua_getmetatable(L, index)) {
        lua_getfield(L, -1, "__json");
        if (lua_isstring(L, -1))
            mark = lua_tostring(L, -1);
        lua_pop(L, 2);
    }
    return mark;
}

void EncodeTable(lua_State* L, int index, std::string& out, int indent, int depth) {
    if (depth > 200)
        luaL_error(L, "json: nested too deeply (a cycle?)");
    std::string mark = MetaMark(L, index);
    int length = lua_objlen(L, index);
    int keys = 0;
    bool sequence = true;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        keys++;
        if (!lua_isnumber(L, -2) || lua_type(L, -2) != LUA_TNUMBER) {
            sequence = false;
        } else {
            double k = lua_tonumber(L, -2);
            if (k < 1 || k > length || std::floor(k) != k)
                sequence = false;
        }
        lua_pop(L, 1);
    }
    bool array = mark == "array" || (mark != "object" && (keys == 0 || (sequence && keys == length)));
    if (array) {
        if (length == 0) {
            out += "[]";
            return;
        }
        out += '[';
        for (int i = 1; i <= length; i++) {
            if (i > 1)
                out += ',';
            Newline(out, indent, depth + 1);
            lua_rawgeti(L, index, i);
            Encode(L, lua_gettop(L), out, indent, depth + 1);
            lua_pop(L, 1);
        }
        Newline(out, indent, depth);
        out += ']';
        return;
    }
    std::vector<std::string> names;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            names.push_back(lua_tostring(L, -1));
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            std::string key;
            EncodeNumber(key, lua_tonumber(L, -1));
            names.push_back("#" + key);
        }
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        out += "{}";
        return;
    }
    out += '{';
    bool first = true;
    for (const std::string& name : names) {
        if (!first)
            out += ',';
        first = false;
        Newline(out, indent, depth + 1);
        if (name[0] == '#') {
            std::string key = name.substr(1);
            EncodeString(out, key.data(), key.size());
            lua_pushnumber(L, std::strtod(key.c_str(), nullptr));
        } else {
            EncodeString(out, name.data(), name.size());
            lua_pushlstring(L, name.data(), name.size());
        }
        out += indent > 0 ? ": " : ":";
        lua_rawget(L, index);
        Encode(L, lua_gettop(L), out, indent, depth + 1);
        lua_pop(L, 1);
    }
    Newline(out, indent, depth);
    out += '}';
}

void Encode(lua_State* L, int index, std::string& out, int indent, int depth) {
    switch (lua_type(L, index)) {
    case LUA_TNIL: out += "null"; break;
    case LUA_TBOOLEAN: out += lua_toboolean(L, index) ? "true" : "false"; break;
    case LUA_TNUMBER: EncodeNumber(out, lua_tonumber(L, index)); break;
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, index, &n);
        EncodeString(out, s, n);
        break;
    }
    case LUA_TTABLE: EncodeTable(L, index, out, indent, depth); break;
    default: luaL_error(L, "json: cannot encode a %s", luaL_typename(L, index));
    }
}

struct Parser {
    lua_State* L;
    const char* p;
    const char* end;
    std::string error;

    void Skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            p++;
    }
    bool Fail(const char* what) {
        if (error.empty())
            error = what;
        return false;
    }
    static void PutUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(unsigned& cp) {
        if (end - p < 4)
            return false;
        cp = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        p++; // the opening quote
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            p++;
            if (p >= end)
                return Fail("unterminated string");
            char c = *p++;
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(cp))
                    return Fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned low = 0;
                    if (!Hex4(low))
                        return Fail("bad \\u escape");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                PutUtf8(out, cp);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        if (p >= end)
            return Fail("unterminated string");
        p++;
        return true;
    }
    bool Value(int depth) {
        if (depth > 200)
            return Fail("nested too deeply");
        Skip();
        if (p >= end)
            return Fail("unexpected end");
        char c = *p;
        if (c == '{') {
            p++;
            lua_newtable(L);
            Skip();
            if (p < end && *p == '}') {
                p++;
                return true;
            }
            for (;;) {
                Skip();
                if (p >= end || *p != '"')
                    return Fail("expected a key");
                std::string key;
                if (!String(key))
                    return false;
                Skip();
                if (p >= end || *p != ':')
                    return Fail("expected ':'");
                p++;
                if (!Value(depth + 1))
                    return false;
                lua_setfield(L, -2, key.c_str());
                Skip();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == '}') {
                    p++;
                    return true;
                }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            p++;
            lua_newtable(L);
            Skip();
            if (p < end && *p == ']') {
                p++;
                return true;
            }
            for (int i = 1;; i++) {
                if (!Value(depth + 1))
                    return false;
                lua_rawseti(L, -2, i);
                Skip();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == ']') {
                    p++;
                    return true;
                }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!String(s))
                return false;
            lua_pushlstring(L, s.data(), s.size());
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
            p += 4;
            lua_pushboolean(L, 1);
            return true;
        }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
            p += 5;
            lua_pushboolean(L, 0);
            return true;
        }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
            p += 4;
            lua_pushnil(L);
            return true;
        }
        char* stop = nullptr;
        std::string number(p, (size_t)std::min<ptrdiff_t>(end - p, 64));
        double v = std::strtod(number.c_str(), &stop);
        if (stop == number.c_str())
            return Fail("unexpected character");
        p += stop - number.c_str();
        lua_pushnumber(L, v);
        return true;
    }
};

// host.jsonEncode(value, indent?) -> text
int JsonEncode(lua_State* L) {
    int indent = (int)luaL_optinteger(L, 2, 0);
    lua_settop(L, 1);
    std::string out;
    Encode(L, 1, out, indent, 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// host.jsonDecode(text) -> value | (nil, error)
int JsonDecode(lua_State* L) {
    size_t n = 0;
    const char* text = luaL_checklstring(L, 1, &n);
    Parser parser{L, text, text + n, {}};
    int top = lua_gettop(L);
    bool ok = parser.Value(0);
    if (ok) {
        parser.Skip();
        if (parser.p != parser.end)
            ok = parser.Fail("trailing text");
    }
    if (!ok) {
        lua_settop(L, top);
        lua_pushnil(L);
        std::string message = "json: " + parser.error + " at byte " + std::to_string(parser.p - text);
        lua_pushstring(L, message.c_str());
        return 2;
    }
    return 1;
}

// host.jsonMark(table, "array" | "object") -> table: how an ambiguous
// table (an empty one) encodes.
int JsonMark(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string mark = CheckString(L, 2);
    lua_newtable(L);
    lua_pushstring(L, mark.c_str());
    lua_setfield(L, -2, "__json");
    lua_setmetatable(L, 1);
    lua_settop(L, 1);
    return 1;
}

} // namespace

void OpenJson(lua_State* L) {
    SetFunction(L, "jsonMark", JsonMark);
    SetFunction(L, "jsonEncode", JsonEncode);
    SetFunction(L, "jsonDecode", JsonDecode);
}

} // namespace hamr
