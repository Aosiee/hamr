-- hamr's native host (src/). Every function a script can reach beyond Luau's
-- own libraries is a field of `host`; the standard library wraps them, and
-- a suite normally uses them through require("@hamr").

export type HostSpawnSpec = {
    argv: { string },
    cwd: string?,
    env: { [string]: string }?,
    log: string?,
    append: boolean?,
    timeout: number?,
}

export type HostProcResult = {
    id: number,
    code: number?,
    signal: number?,
    timedOut: boolean,
    killed: boolean,
    seconds: number,
    cpuSeconds: number,
    peakRssMB: number,
    leftProcesses: boolean,
}

export type HostCheckFinding = {
    file: string,
    line: number,
    col: number,
    code: string,
    message: string,
    warning: boolean,
}

export type HostFileStat = { isDir: boolean, size: number, mtime: number }
export type HostProcess = { pid: number, ppid: number, name: string, cmdline: string, started: number }
export type HostMemory = { totalMB: number, availableMB: number }

declare host: {
    version: string,

    -- Processes: each child in its own process group, its output in `log`.
    spawn: (spec: HostSpawnSpec) -> number,
    poll: (timeout: number?) -> ({ HostProcResult }, boolean),
    kill: (id: number) -> (),
    running: () -> number,
    interrupted: () -> boolean,
    capture: (argv: { string }, cwd: string?) -> (number, string),

    -- Files and paths.
    read: (path: string) -> string?,
    write: (path: string, data: string) -> (),
    append: (path: string, data: string) -> (),
    exists: (path: string) -> boolean,
    isDir: (path: string) -> boolean,
    mkdir: (path: string) -> (),
    remove: (path: string) -> number,
    copy: (from: string, to: string) -> (),
    list: (dir: string) -> { string },
    glob: (pattern: string, root: string?) -> { string },
    globMatch: (pattern: string, text: string) -> boolean,
    stat: (path: string) -> HostFileStat?,
    cwd: () -> string,
    absolute: (path: string) -> string,
    relative: (path: string, base: string?) -> string,
    join: (...string) -> string,
    parent: (path: string) -> string,
    basename: (path: string) -> string,
    symlink: (target: string, link: string) -> boolean,

    -- JSON.
    jsonEncode: (value: unknown, indent: number?) -> string,
    jsonDecode: (text: string) -> (unknown, string?),
    jsonMark: <T>(value: T, mark: string) -> T,

    -- The machine.
    cores: () -> number,
    memory: () -> HostMemory,
    diskFreeMB: (path: string?) -> number?,
    processes: () -> { HostProcess },
    getenv: (name: string) -> string?,
    hostname: () -> string,
    platform: () -> string,
    now: () -> number,
    monotonic: () -> number,
    isoTime: (unix: number?) -> string,
    isTty: (fd: number?) -> boolean,
    terminalWidth: () -> number,
    stdout: (text: string) -> (),
    stderr: (text: string) -> (),
    sleep: (seconds: number) -> (),
    pid: () -> number,
    executable: () -> string,
    -- The running step's deadline (monotonic seconds; 0 = none).
    setDeadline: (deadline: number) -> (),

    -- Modules and the type checker.
    require: (path: string) -> unknown,
    check: (modules: { string }) -> { HostCheckFinding },
    stdModules: () -> { string },
    resolve: (from: string, request: string) -> (string?, string?),
}
