// The machine: cores, memory, disk, the process table, the environment,
// clocks and the terminal.

#include "host.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/statvfs.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lualib.h"

namespace hamr {

namespace {

int Cores(lua_State* L) {
    unsigned n = std::thread::hardware_concurrency();
    lua_pushinteger(L, n ? (int)n : 1);
    return 1;
}

// host.memory() -> { totalMB, availableMB }
int Memory(lua_State* L) {
    double total = 0, available = 0;
    std::ifstream in("/proc/meminfo");
    std::string key;
    double kb = 0;
    std::string unit;
    while (in >> key >> kb >> unit) {
        if (key == "MemTotal:")
            total = kb / 1024.0;
        else if (key == "MemAvailable:")
            available = kb / 1024.0;
    }
    lua_newtable(L);
    lua_pushnumber(L, total);
    lua_setfield(L, -2, "totalMB");
    lua_pushnumber(L, available);
    lua_setfield(L, -2, "availableMB");
    return 1;
}

int DiskFreeMB(lua_State* L) {
    struct statvfs info = {};
    std::string path = lua_isstring(L, 1) ? lua_tostring(L, 1) : ".";
    if (statvfs(path.c_str(), &info) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (double)info.f_bavail * (double)info.f_frsize / (1024.0 * 1024.0));
    return 1;
}

double BootTime() {
    std::ifstream in("/proc/stat");
    std::string key;
    while (in >> key) {
        if (key == "btime") {
            double t = 0;
            in >> t;
            return t;
        }
        std::string rest;
        std::getline(in, rest);
    }
    return 0;
}

// host.processes() -> { { pid, ppid, name, cmdline, started } }; empty
// where /proc does not exist.
int Processes(lua_State* L) {
    lua_newtable(L);
    DIR* dir = opendir("/proc");
    if (!dir)
        return 1;
    double boot = BootTime();
    double ticks = (double)sysconf(_SC_CLK_TCK);
    int count = 0;
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (!name[0] || name[0] < '0' || name[0] > '9')
            continue;
        std::string base = std::string("/proc/") + name;
        std::ifstream statFile(base + "/stat");
        std::string stat((std::istreambuf_iterator<char>(statFile)), std::istreambuf_iterator<char>());
        size_t open = stat.find('('), close = stat.rfind(')');
        if (open == std::string::npos || close == std::string::npos)
            continue;
        std::string comm = stat.substr(open + 1, close - open - 1);
        std::istringstream rest(stat.substr(close + 2));
        std::vector<std::string> fields;
        std::string field;
        while (rest >> field)
            fields.push_back(field);
        if (fields.size() < 20)
            continue;
        // After the name: state(0) ppid(1) ... starttime(19).
        int ppid = std::atoi(fields[1].c_str());
        double started = boot + std::atof(fields[19].c_str()) / ticks;
        std::ifstream cmdFile(base + "/cmdline", std::ios::binary);
        std::string cmdline((std::istreambuf_iterator<char>(cmdFile)), std::istreambuf_iterator<char>());
        for (char& c : cmdline)
            if (c == '\0')
                c = ' ';
        while (!cmdline.empty() && cmdline.back() == ' ')
            cmdline.pop_back();
        lua_newtable(L);
        lua_pushinteger(L, std::atoi(name));
        lua_setfield(L, -2, "pid");
        lua_pushinteger(L, ppid);
        lua_setfield(L, -2, "ppid");
        lua_pushstring(L, comm.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, cmdline.c_str());
        lua_setfield(L, -2, "cmdline");
        lua_pushnumber(L, started);
        lua_setfield(L, -2, "started");
        lua_rawseti(L, -2, ++count);
    }
    closedir(dir);
    return 1;
}

int Getenv(lua_State* L) {
    const char* value = std::getenv(CheckString(L, 1).c_str());
    if (value)
        lua_pushstring(L, value);
    else
        lua_pushnil(L);
    return 1;
}

int Hostname(lua_State* L) {
    char buffer[256] = {};
    gethostname(buffer, sizeof(buffer) - 1);
    lua_pushstring(L, buffer);
    return 1;
}

int Platform(lua_State* L) {
#if defined(__linux__)
    lua_pushstring(L, "linux");
#elif defined(__APPLE__)
    lua_pushstring(L, "macos");
#else
    lua_pushstring(L, "unknown");
#endif
    return 1;
}

int Now(lua_State* L) {
    lua_pushnumber(L, UnixSeconds());
    return 1;
}

int Monotonic(lua_State* L) {
    lua_pushnumber(L, MonotonicSeconds());
    return 1;
}

// host.isoTime(unix?) -> "2026-11-24T10:20:30" in local time.
int IsoTime(lua_State* L) {
    double t = luaL_optnumber(L, 1, UnixSeconds());
    time_t seconds = (time_t)t;
    tm local = {};
    localtime_r(&seconds, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &local);
    lua_pushstring(L, buffer);
    return 1;
}

int IsTty(lua_State* L) {
    lua_pushboolean(L, isatty((int)luaL_optinteger(L, 1, 1)));
    return 1;
}

int TerminalWidth(lua_State* L) {
    const char* columns = std::getenv("COLUMNS");
    lua_pushinteger(L, columns ? std::atoi(columns) : 100);
    return 1;
}

int Write(lua_State* L) {
    size_t n = 0;
    const char* text = luaL_checklstring(L, 1, &n);
    std::fwrite(text, 1, n, stdout);
    std::fflush(stdout);
    return 0;
}

int WriteErr(lua_State* L) {
    size_t n = 0;
    const char* text = luaL_checklstring(L, 1, &n);
    std::fwrite(text, 1, n, stderr);
    std::fflush(stderr);
    return 0;
}

int Sleep(lua_State* L) {
    double seconds = luaL_checknumber(L, 1);
    timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
    return 0;
}

int Pid(lua_State* L) {
    lua_pushinteger(L, (int)getpid());
    return 1;
}

int Executable(lua_State* L) {
    char buffer[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    lua_pushstring(L, n > 0 ? buffer : "hamr");
    return 1;
}

} // namespace

void OpenSys(lua_State* L) {
    SetFunction(L, "cores", Cores);
    SetFunction(L, "memory", Memory);
    SetFunction(L, "diskFreeMB", DiskFreeMB);
    SetFunction(L, "processes", Processes);
    SetFunction(L, "getenv", Getenv);
    SetFunction(L, "hostname", Hostname);
    SetFunction(L, "platform", Platform);
    SetFunction(L, "now", Now);
    SetFunction(L, "monotonic", Monotonic);
    SetFunction(L, "isoTime", IsoTime);
    SetFunction(L, "isTty", IsTty);
    SetFunction(L, "terminalWidth", TerminalWidth);
    SetFunction(L, "stdout", Write);
    SetFunction(L, "stderr", WriteErr);
    SetFunction(L, "sleep", Sleep);
    SetFunction(L, "pid", Pid);
    SetFunction(L, "executable", Executable);
}

} // namespace hamr
