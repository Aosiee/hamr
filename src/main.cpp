// hamr's entry point: a Luau VM with the host table and `require`, running
// the standard library's command line (std/cli.cluau). Everything hamr
// prints or decides is the library's; this file only starts it.

#include "host.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace hamr {

void SetFunction(lua_State* L, const char* name, int (*fn)(lua_State*)) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

std::string CheckString(lua_State* L, int index) {
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return std::string(text, length);
}

std::optional<std::string> FieldString(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    std::optional<std::string> value;
    if (lua_isstring(L, -1)) {
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        value = std::string(text, length);
    }
    lua_pop(L, 1);
    return value;
}

std::optional<double> FieldNumber(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    std::optional<double> value;
    if (lua_isnumber(L, -1))
        value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return value;
}

bool FieldBool(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

std::vector<std::string> StringList(lua_State* L, int index) {
    std::vector<std::string> values;
    if (!lua_istable(L, index))
        return values;
    int count = lua_objlen(L, index);
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, index, i);
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        if (text)
            values.emplace_back(text, length);
        lua_pop(L, 1);
    }
    return values;
}

void PushStringList(lua_State* L, const std::vector<std::string>& values) {
    lua_createtable(L, (int)values.size(), 0);
    for (size_t i = 0; i < values.size(); i++) {
        lua_pushlstring(L, values[i].data(), values[i].size());
        lua_rawseti(L, -2, (int)i + 1);
    }
}

double MonotonicSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

double UnixSeconds() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

namespace {

int Require(lua_State* L) {
    std::string request = CheckString(L, 1);
    std::string from;
    lua_Debug ar = {};
    if (lua_getinfo(L, 1, "s", &ar) && ar.source && ar.source[0] == '@')
        from = ar.source + 1;
    std::string error;
    std::optional<std::string> name = ResolveModule(from, request, error);
    if (!name) {
        lua_pushstring(L, error.c_str());
        lua_error(L);
    }
    RequireModule(L, *name);
    return 1;
}

// print, flushed at once: live progress must reach a pipe or a log as it
// happens, not when the buffer fills.
int Print(lua_State* L) {
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; i++) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1)
            line += '\t';
        line.append(text, length);
        lua_pop(L, 1);
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return 0;
}

// A step's cluau code that never yields would hang the run: the runner
// sets the running step's deadline around each resume, and the interrupt
// raises an error in that step once it passes. The clock is read every
// 4096 safepoints only.
double g_Deadline = 0;

int SetDeadline(lua_State* L) {
    g_Deadline = luaL_checknumber(L, 1);
    return 0;
}

void Interrupt(lua_State* L, int gc) {
    static unsigned count = 0;
    if (gc >= 0 || g_Deadline <= 0 || (++count & 4095) != 0)
        return;
    if (MonotonicSeconds() > g_Deadline) {
        g_Deadline = 0;
        luaL_error(L, "the step ran past its timeout inside cluau code");
    }
}

int Traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    std::string text = message ? message : "(an error that is not a string)";
    text += "\nstack traceback:\n";
    text += lua_debugtrace(L);
    lua_pushstring(L, text.c_str());
    return 1;
}

int RunMain(lua_State* L) {
    // Stack: the argument list.
    RequireModule(L, "@hamr/cli");
    lua_getfield(L, -1, "main");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

} // namespace

} // namespace hamr

int main(int argc, char** argv) {
    using namespace hamr;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--std-dir") == 0 && i + 1 < argc) {
            State().StdDir = argv[++i];
            continue;
        }
        args.emplace_back(argv[i]);
    }

    InstallSignalHandlers();
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    lua_callbacks(L)->interrupt = Interrupt;
    lua_newtable(L);
    SetFunction(L, "setDeadline", SetDeadline);
    lua_pushstring(L, HAMR_VERSION);
    lua_setfield(L, -2, "version");
    OpenProc(L);
    OpenFs(L);
    OpenJson(L);
    OpenSys(L);
    OpenCheck(L);
    lua_setglobal(L, "host");

    lua_pushcfunction(L, Print, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, Require, "require");
    lua_setglobal(L, "require");

    lua_pushcfunction(L, Traceback, "traceback");
    lua_pushcfunction(L, RunMain, "main");
    PushStringList(L, args);
    int status = lua_pcall(L, 1, 1, -3);
    int code = 0;
    if (status != 0) {
        std::fprintf(stderr, "hamr: %s\n", lua_tostring(L, -1));
        code = 2;
    } else if (lua_isnumber(L, -1)) {
        code = (int)lua_tointeger(L, -1);
    }
    KillAllChildren();
    std::fflush(stdout);
    lua_close(L);
    return code;
}
