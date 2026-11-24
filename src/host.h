#pragma once

// The native host's shared state and the binding helpers. Every function a
// script can call is a field of the global `host` table (types/host.d.luau
// declares them); the standard library wraps them.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace hamr {

struct HostState {
    // When set, the standard library and host.d.luau are read from this
    // directory (and ../types) instead of the embedded copies: editing the
    // library without rebuilding hamr.
    std::string StdDir;
};

HostState& State();

// Modules. A module's name is "@hamr/<name>" for the standard library and
// the absolute path of a file otherwise.
std::optional<std::string> ResolveModule(const std::string& from, const std::string& request, std::string& error);
std::optional<std::string> ReadModuleSource(const std::string& name);
std::optional<std::string> ReadHostDeclarations();
std::vector<std::string> StdModuleNames();
bool IsCluauModule(const std::string& name);

// Loads (once) and returns the module's value on the stack; raises a Lua
// error when it cannot.
void RequireModule(lua_State* L, const std::string& name);

// The bindings, each adding its functions to the table on top of the stack.
void OpenProc(lua_State* L);
void OpenFs(lua_State* L);
void OpenJson(lua_State* L);
void OpenSys(lua_State* L);
void OpenCheck(lua_State* L);

// Kills every child process group still running (the exit paths).
void KillAllChildren();
void InstallSignalHandlers();

// Helpers for the bindings.
void SetFunction(lua_State* L, const char* name, int (*fn)(lua_State*));
std::string CheckString(lua_State* L, int index);
std::optional<std::string> FieldString(lua_State* L, int table, const char* key);
std::optional<double> FieldNumber(lua_State* L, int table, const char* key);
bool FieldBool(lua_State* L, int table, const char* key);
std::vector<std::string> StringList(lua_State* L, int index);
void PushStringList(lua_State* L, const std::vector<std::string>& values);

double MonotonicSeconds();
double UnixSeconds();

} // namespace hamr
