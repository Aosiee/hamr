// Processes. Every child runs in its own process group with its output in
// a log file, and is measured when it is reaped (wait4: wall time, user +
// system CPU, peak RSS). A timeout terminates the whole group, then kills
// it five seconds later. host.poll is the event loop's only wait.

#include "host.h"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "lua.h"
#include "lualib.h"

namespace hamr {

namespace {

struct Child {
    pid_t Pid = 0;
    double Start = 0;
    double Deadline = 0; // 0 = none
    bool TimedOut = false;
    bool Killed = false;
    double KillAt = 0;
};

struct Children {
    std::map<int, Child> Live;
    int NextId = 1;
};

Children& Kids() {
    static Children children;
    return children;
}

// The process groups, readable from a signal handler.
constexpr int kMaxGroups = 1024;
volatile pid_t g_Groups[kMaxGroups] = {};
volatile sig_atomic_t g_Interrupts = 0;

void TrackGroup(pid_t pid) {
    for (int i = 0; i < kMaxGroups; i++)
        if (g_Groups[i] == 0) {
            g_Groups[i] = pid;
            return;
        }
}

void ForgetGroup(pid_t pid) {
    for (int i = 0; i < kMaxGroups; i++)
        if (g_Groups[i] == pid)
            g_Groups[i] = 0;
}

void OnSignal(int) {
    g_Interrupts = g_Interrupts + 1;
    if (g_Interrupts >= 2) {
        for (int i = 0; i < kMaxGroups; i++)
            if (g_Groups[i] > 0)
                kill(-g_Groups[i], SIGKILL);
        _exit(130);
    }
}

void SleepSeconds(double seconds) {
    if (seconds <= 0)
        return;
    timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, nullptr);
}

std::vector<char*> CArgv(std::vector<std::string>& argv) {
    std::vector<char*> out;
    for (std::string& arg : argv)
        out.push_back(arg.data());
    out.push_back(nullptr);
    return out;
}

// host.spawn { argv, cwd?, env?, log?, append?, timeout? } -> id
int Spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "argv");
    std::vector<std::string> argv = StringList(L, -1);
    lua_pop(L, 1);
    if (argv.empty())
        luaL_error(L, "host.spawn: argv is empty");
    std::optional<std::string> cwd = FieldString(L, 1, "cwd");
    std::string log = FieldString(L, 1, "log").value_or("/dev/null");
    bool append = FieldBool(L, 1, "append");
    double timeout = FieldNumber(L, 1, "timeout").value_or(0);
    std::vector<std::pair<std::string, std::string>> env;
    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1))
                env.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    std::vector<char*> cargv = CArgv(argv);
    double start = MonotonicSeconds();
    pid_t pid = fork();
    if (pid < 0)
        luaL_error(L, "host.spawn: fork failed: %s", std::strerror(errno));
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0)
            dup2(in, 0);
        int out = open(log.c_str(), O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        if (out < 0)
            out = open("/dev/null", O_WRONLY);
        dup2(out, 1);
        dup2(out, 2);
        if (cwd && chdir(cwd->c_str()) != 0) {
            dprintf(2, "hamr: cannot enter %s: %s\n", cwd->c_str(), std::strerror(errno));
            _exit(127);
        }
        for (const auto& [key, value] : env)
            setenv(key.c_str(), value.c_str(), 1);
        execvp(cargv[0], cargv.data());
        dprintf(2, "hamr: cannot run %s: %s\n", cargv[0], std::strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    TrackGroup(pid);
    Child child;
    child.Pid = pid;
    child.Start = start;
    child.Deadline = timeout > 0 ? start + timeout : 0;
    int id = Kids().NextId++;
    Kids().Live[id] = child;
    lua_pushinteger(L, id);
    return 1;
}

void PushResult(lua_State* L, int id, const Child& child, int status, const rusage& usage, double now) {
    lua_newtable(L);
    lua_pushinteger(L, id);
    lua_setfield(L, -2, "id");
    if (WIFEXITED(status)) {
        lua_pushinteger(L, WEXITSTATUS(status));
        lua_setfield(L, -2, "code");
    } else if (WIFSIGNALED(status)) {
        lua_pushinteger(L, WTERMSIG(status));
        lua_setfield(L, -2, "signal");
    }
    lua_pushboolean(L, child.TimedOut);
    lua_setfield(L, -2, "timedOut");
    lua_pushboolean(L, child.Killed);
    lua_setfield(L, -2, "killed");
    lua_pushnumber(L, now - child.Start);
    lua_setfield(L, -2, "seconds");
    double cpu = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec * 1e-6 +
                 (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec * 1e-6;
    lua_pushnumber(L, cpu);
    lua_setfield(L, -2, "cpuSeconds");
    lua_pushnumber(L, (double)usage.ru_maxrss / 1024.0);
    lua_setfield(L, -2, "peakRssMB");
    // A process the child left behind in its group is a stray: report it
    // and end it, so it cannot skew the next step.
    bool left = kill(-child.Pid, 0) == 0;
    if (left)
        kill(-child.Pid, SIGKILL);
    lua_pushboolean(L, left);
    lua_setfield(L, -2, "leftProcesses");
}

// host.poll(timeout?) -> ({ result }, interrupted). Waits until a child
// ends, the timeout passes (a negative or absent timeout: until a child
// ends), or the user interrupts.
int Poll(lua_State* L) {
    double timeout = luaL_optnumber(L, 1, -1);
    double until = timeout < 0 ? INFINITY : MonotonicSeconds() + timeout;
    lua_newtable(L);
    int count = 0;
    for (;;) {
        double now = MonotonicSeconds();
        for (auto it = Kids().Live.begin(); it != Kids().Live.end();) {
            Child& child = it->second;
            int status = 0;
            rusage usage = {};
            pid_t reaped = wait4(child.Pid, &status, WNOHANG, &usage);
            if (reaped == child.Pid || (reaped < 0 && errno == ECHILD)) {
                if (reaped < 0)
                    status = 255 << 8;
                PushResult(L, it->first, child, status, usage, now);
                lua_rawseti(L, -2, ++count);
                ForgetGroup(child.Pid);
                it = Kids().Live.erase(it);
                continue;
            }
            if (child.Deadline > 0 && now >= child.Deadline && !child.TimedOut && !child.Killed) {
                child.TimedOut = true;
                kill(-child.Pid, SIGTERM);
                child.KillAt = now + 5;
            } else if ((child.TimedOut || child.Killed) && now >= child.KillAt) {
                kill(-child.Pid, SIGKILL);
                child.KillAt = now + 5;
            }
            ++it;
        }
        if (count > 0 || g_Interrupts > 0 || now >= until || Kids().Live.empty())
            break;
        SleepSeconds(std::min(0.01, until - now));
    }
    lua_pushboolean(L, g_Interrupts > 0);
    return 2;
}

// host.kill(id): terminate the child's group (killed five seconds later).
int Kill(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    auto it = Kids().Live.find(id);
    if (it != Kids().Live.end() && !it->second.Killed) {
        it->second.Killed = true;
        kill(-it->second.Pid, SIGTERM);
        it->second.KillAt = MonotonicSeconds() + 5;
    }
    return 0;
}

int Running(lua_State* L) {
    lua_pushinteger(L, (int)Kids().Live.size());
    return 1;
}

int Interrupted(lua_State* L) {
    lua_pushboolean(L, g_Interrupts > 0);
    return 1;
}

// host.capture(argv, cwd?) -> (code, stdout): a short command run to its
// end, its standard output returned (standard error discarded).
int Capture(lua_State* L) {
    std::vector<std::string> argv = StringList(L, 1);
    if (argv.empty())
        luaL_error(L, "host.capture: argv is empty");
    std::optional<std::string> cwd;
    if (lua_isstring(L, 2))
        cwd = lua_tostring(L, 2);
    std::vector<char*> cargv = CArgv(argv);
    int fds[2];
    if (pipe(fds) != 0)
        luaL_error(L, "host.capture: pipe failed");
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(fds[1], 1);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 2);
        close(fds[0]);
        if (cwd && chdir(cwd->c_str()) != 0)
            _exit(127);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n > 0)
            output.append(buffer, (size_t)n);
        else if (n == 0 || errno != EINTR)
            break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    lua_pushinteger(L, WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    lua_pushlstring(L, output.data(), output.size());
    return 2;
}

} // namespace

void KillAllChildren() {
    for (auto& [id, child] : Kids().Live) {
        kill(-child.Pid, SIGKILL);
        int status = 0;
        waitpid(child.Pid, &status, 0);
        ForgetGroup(child.Pid);
    }
    Kids().Live.clear();
}

void InstallSignalHandlers() {
    struct sigaction action = {};
    action.sa_handler = OnSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void OpenProc(lua_State* L) {
    SetFunction(L, "spawn", Spawn);
    SetFunction(L, "poll", Poll);
    SetFunction(L, "kill", Kill);
    SetFunction(L, "running", Running);
    SetFunction(L, "interrupted", Interrupted);
    SetFunction(L, "capture", Capture);
}

} // namespace hamr
