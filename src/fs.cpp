// Files and paths. Globs: `*` and `?` stay inside one path segment, `**`
// crosses segments (and `**/` also matches no segment at all).

#include "host.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "lualib.h"

namespace hamr {

namespace {

namespace fs = std::filesystem;

bool Match(const char* p, const char* s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (*p == '/' && Match(p + 1, s))
                return true;
            for (const char* t = s;; t++) {
                if (Match(p, t))
                    return true;
                if (!*t)
                    return false;
            }
        }
        if (*p == '*') {
            p++;
            for (const char* t = s;; t++) {
                if (Match(p, t))
                    return true;
                if (!*t || *t == '/')
                    return false;
            }
        }
        if (!*s)
            return false;
        if (*p == '?') {
            if (*s == '/')
                return false;
            p++;
            s++;
            continue;
        }
        if (*p != *s)
            return false;
        p++;
        s++;
    }
    return !*s;
}

int Read(lua_State* L) {
    std::ifstream in(CheckString(L, 1), std::ios::binary);
    if (!in) {
        lua_pushnil(L);
        return 1;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string data = text.str();
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int WriteMode(lua_State* L, std::ios::openmode mode) {
    fs::path path = CheckString(L, 1);
    std::string data = CheckString(L, 2);
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | mode);
    if (!out)
        luaL_error(L, "cannot write %s", path.string().c_str());
    out.write(data.data(), (std::streamsize)data.size());
    return 0;
}

int Write(lua_State* L) { return WriteMode(L, std::ios::trunc); }
int Append(lua_State* L) { return WriteMode(L, std::ios::app); }

int Exists(lua_State* L) {
    std::error_code ec;
    lua_pushboolean(L, fs::exists(CheckString(L, 1), ec));
    return 1;
}

int IsDir(lua_State* L) {
    std::error_code ec;
    lua_pushboolean(L, fs::is_directory(CheckString(L, 1), ec));
    return 1;
}

int Mkdir(lua_State* L) {
    std::error_code ec;
    fs::create_directories(CheckString(L, 1), ec);
    if (ec)
        luaL_error(L, "cannot create %s: %s", CheckString(L, 1).c_str(), ec.message().c_str());
    return 0;
}

int Remove(lua_State* L) {
    std::error_code ec;
    auto count = fs::remove_all(CheckString(L, 1), ec);
    lua_pushinteger(L, ec ? 0 : (int)count);
    return 1;
}

int Copy(lua_State* L) {
    std::error_code ec;
    fs::path to = CheckString(L, 2);
    if (to.has_parent_path())
        fs::create_directories(to.parent_path(), ec);
    fs::copy_file(CheckString(L, 1), to, fs::copy_options::overwrite_existing, ec);
    if (ec)
        luaL_error(L, "cannot copy to %s: %s", to.string().c_str(), ec.message().c_str());
    return 0;
}

int List(lua_State* L) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(CheckString(L, 1), ec))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    PushStringList(L, names);
    return 1;
}

// host.glob(pattern, root?) -> the matching files, relative to root, sorted.
int Glob(lua_State* L) {
    std::string pattern = CheckString(L, 1);
    fs::path root = lua_isstring(L, 2) ? fs::path(lua_tostring(L, 2)) : fs::current_path();
    // Walk from the pattern's literal directory prefix.
    std::string prefix;
    size_t segment = 0;
    for (size_t i = 0; i <= pattern.size(); i++) {
        if (i == pattern.size() || pattern[i] == '/') {
            std::string part = pattern.substr(segment, i - segment);
            if (part.find_first_of("*?") != std::string::npos || i == pattern.size())
                break;
            prefix += part + "/";
            segment = i + 1;
        }
    }
    std::vector<std::string> out;
    std::error_code ec;
    fs::path start = root / prefix;
    if (fs::is_regular_file(root / pattern, ec)) {
        out.push_back(pattern);
    } else if (fs::is_directory(start, ec)) {
        fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec)
                break;
            std::string name = it->path().filename().string();
            if (it->is_directory(ec) && (name == ".git" || name == ".jj")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec))
                continue;
            std::string rel = it->path().lexically_relative(root).generic_string();
            if (Match(pattern.c_str(), rel.c_str()))
                out.push_back(rel);
        }
    }
    std::sort(out.begin(), out.end());
    PushStringList(L, out);
    return 1;
}

int GlobMatch(lua_State* L) {
    lua_pushboolean(L, Match(CheckString(L, 1).c_str(), CheckString(L, 2).c_str()));
    return 1;
}

int Stat(lua_State* L) {
    std::error_code ec;
    fs::path path = CheckString(L, 1);
    auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    bool dir = fs::is_directory(status);
    lua_pushboolean(L, dir);
    lua_setfield(L, -2, "isDir");
    lua_pushnumber(L, dir ? 0.0 : (double)fs::file_size(path, ec));
    lua_setfield(L, -2, "size");
    auto time = fs::last_write_time(path, ec);
    auto system = std::chrono::file_clock::to_sys(time);
    lua_pushnumber(L, std::chrono::duration<double>(system.time_since_epoch()).count());
    lua_setfield(L, -2, "mtime");
    return 1;
}

int Cwd(lua_State* L) {
    lua_pushstring(L, fs::current_path().string().c_str());
    return 1;
}

int Absolute(lua_State* L) {
    lua_pushstring(L, fs::absolute(CheckString(L, 1)).lexically_normal().string().c_str());
    return 1;
}

int Relative(lua_State* L) {
    fs::path path = fs::absolute(CheckString(L, 1)).lexically_normal();
    fs::path base = lua_isstring(L, 2) ? fs::absolute(lua_tostring(L, 2)).lexically_normal() : fs::current_path();
    lua_pushstring(L, path.lexically_relative(base).generic_string().c_str());
    return 1;
}

int Join(lua_State* L) {
    fs::path path;
    int top = lua_gettop(L);
    for (int i = 1; i <= top; i++)
        path /= CheckString(L, i);
    lua_pushstring(L, path.lexically_normal().generic_string().c_str());
    return 1;
}

int Parent(lua_State* L) {
    lua_pushstring(L, fs::path(CheckString(L, 1)).parent_path().generic_string().c_str());
    return 1;
}

int Basename(lua_State* L) {
    lua_pushstring(L, fs::path(CheckString(L, 1)).filename().string().c_str());
    return 1;
}

// host.symlink(target, link): replaces `link` with a symbolic link.
int Symlink(lua_State* L) {
    std::error_code ec;
    fs::path link = CheckString(L, 2);
    fs::remove(link, ec);
    ec.clear();
    fs::create_symlink(CheckString(L, 1), link, ec);
    lua_pushboolean(L, !ec);
    return 1;
}

} // namespace

void OpenFs(lua_State* L) {
    SetFunction(L, "symlink", Symlink);
    SetFunction(L, "read", Read);
    SetFunction(L, "write", Write);
    SetFunction(L, "append", Append);
    SetFunction(L, "exists", Exists);
    SetFunction(L, "isDir", IsDir);
    SetFunction(L, "mkdir", Mkdir);
    SetFunction(L, "remove", Remove);
    SetFunction(L, "copy", Copy);
    SetFunction(L, "list", List);
    SetFunction(L, "glob", Glob);
    SetFunction(L, "globMatch", GlobMatch);
    SetFunction(L, "stat", Stat);
    SetFunction(L, "cwd", Cwd);
    SetFunction(L, "absolute", Absolute);
    SetFunction(L, "relative", Relative);
    SetFunction(L, "join", Join);
    SetFunction(L, "parent", Parent);
    SetFunction(L, "basename", Basename);
}

} // namespace hamr
