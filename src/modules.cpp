#include "host.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cluau/cluau.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

namespace hamr {

struct EmbeddedFile {
    std::string_view Name;
    const unsigned char* Data;
    std::size_t Size;
};
extern const EmbeddedFile kEmbeddedFiles[];
extern const std::size_t kEmbeddedFileCount;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kStdPrefix = "@hamr/";

bool IsStd(const std::string& name) { return name.rfind(kStdPrefix, 0) == 0; }

std::optional<std::string> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::optional<std::string> Embedded(std::string_view file) {
    for (std::size_t i = 0; i < kEmbeddedFileCount; i++)
        if (kEmbeddedFiles[i].Name == file)
            return std::string(reinterpret_cast<const char*>(kEmbeddedFiles[i].Data), kEmbeddedFiles[i].Size);
    return std::nullopt;
}

bool IsFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

} // namespace

HostState& State() {
    static HostState state;
    return state;
}

bool IsCluauModule(const std::string& name) { return IsStd(name) || cluau::IsCluauPath(name); }

std::vector<std::string> StdModuleNames() {
    std::vector<std::string> names;
    if (!State().StdDir.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(State().StdDir, ec))
            if (entry.path().extension() == ".cluau")
                names.push_back(std::string(kStdPrefix) + entry.path().stem().string());
    } else {
        for (std::size_t i = 0; i < kEmbeddedFileCount; i++) {
            std::string_view file = kEmbeddedFiles[i].Name;
            if (file.size() > 6 && file.substr(file.size() - 6) == ".cluau")
                names.push_back(std::string(kStdPrefix) + std::string(file.substr(0, file.size() - 6)));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::optional<std::string> ResolveModule(const std::string& from, const std::string& request, std::string& error) {
    if (request == "@hamr")
        return std::string(kStdPrefix) + "hamr";
    if (request.rfind(kStdPrefix, 0) == 0) {
        if (!ReadModuleSource(request)) {
            error = "require(\"" + request + "\"): hamr's standard library has no such module";
            return std::nullopt;
        }
        return request;
    }
    fs::path base;
    if (request.rfind("./", 0) == 0 || request.rfind("../", 0) == 0) {
        if (from.empty() || IsStd(from))
            base = fs::current_path();
        else
            base = fs::path(from).parent_path();
    } else if (request.empty() || request[0] != '/') {
        error = "require(\"" + request + "\"): a module path starts with ./, ../ or /, or is @hamr or @hamr/<module>";
        return std::nullopt;
    }
    fs::path path = (base / request).lexically_normal();
    std::vector<fs::path> candidates;
    std::string ext = path.extension().string();
    if (ext == ".cluau" || ext == ".luau")
        candidates.push_back(path);
    candidates.push_back(fs::path(path.string() + ".cluau"));
    candidates.push_back(fs::path(path.string() + ".luau"));
    candidates.push_back(path / "init.cluau");
    for (const fs::path& candidate : candidates)
        if (IsFile(candidate))
            return fs::absolute(candidate).lexically_normal().string();
    error = "require(\"" + request + "\"): no module at " + path.string() + "(.cluau|.luau)";
    return std::nullopt;
}

std::optional<std::string> ReadModuleSource(const std::string& name) {
    if (IsStd(name)) {
        std::string file = name.substr(kStdPrefix.size()) + ".cluau";
        if (!State().StdDir.empty())
            return ReadFile(fs::path(State().StdDir) / file);
        return Embedded(file);
    }
    return ReadFile(name);
}

std::optional<std::string> ReadHostDeclarations() {
    if (!State().StdDir.empty())
        return ReadFile(fs::path(State().StdDir) / ".." / "types" / "host.d.luau");
    return Embedded("host.d.luau");
}

void RequireModule(lua_State* L, const std::string& name) {
    lua_getfield(L, LUA_REGISTRYINDEX, "hamr.modules");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "hamr.modules");
    }
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1)) {
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            std::string message = "require cycle through " + name;
            lua_pushstring(L, message.c_str());
            lua_error(L);
        }
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    lua_pushboolean(L, 0); // loading: a second require of it is a cycle
    lua_setfield(L, -2, name.c_str());

    std::optional<std::string> source = ReadModuleSource(name);
    if (!source) {
        std::string message = "cannot read module " + name;
        lua_pushstring(L, message.c_str());
        lua_error(L);
    }
    std::string luau = *source;
    if (IsCluauModule(name)) {
        cluau::TranslateResult translation = cluau::Translate(*source);
        if (translation.HasErrors()) {
            std::string message;
            for (const cluau::Diagnostic& d : translation.Diagnostics)
                if (!d.Warning)
                    message += (message.empty() ? "" : "\n") + name + ":" + std::to_string(d.At.Begin.Line + 1) + ":" +
                               std::to_string(d.At.Begin.Col + 1) + ": " + d.Message;
            lua_pushstring(L, message.c_str());
            lua_error(L);
        }
        luau = std::move(translation.Luau);
    }

    lua_CompileOptions options = {};
    options.optimizationLevel = 1;
    options.debugLevel = 2;
    size_t size = 0;
    char* bytecode = luau_compile(luau.data(), luau.size(), &options, &size);
    std::string chunkname = "@" + name;
    int status = luau_load(L, chunkname.c_str(), bytecode, size, 0);
    free(bytecode);
    if (status != 0)
        lua_error(L); // the message is on the stack
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name.c_str());
    lua_remove(L, -2);
}

} // namespace hamr
