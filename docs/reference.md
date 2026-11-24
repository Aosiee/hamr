# hamr reference

## Suites

A suite is a cluau module, by convention `hamr.cluau` at a project's root.
It creates a suite, declares its steps and returns it:

```
const hamr = require("@hamr")
const suite = hamr.suite("Name", { out = ".hamr", budgets = { fast = 600 }, env = { CI = "1" } })
...
return suite
```

| Option | Meaning |
|---|---|
| `out` | The output directory, relative to the suite (default `.hamr`). `--out` overrides it, relative to the current directory. |
| `budgets` | Each tier's total wall-time budget in seconds. A run over it is a warning listing the longest steps. |
| `env` | Environment variables for every step. |

`suite:var(name, default)` declares a variable and returns its value:
`--set name=value` overrides the default. A command's argument may name a
variable as `{name}`; a function step reads `ctx.vars`.

`suite:onReport(function(report, runDir) ... end)` runs after the report is
written, for a suite's own outputs (a findings file in another format, a
`latest` link). A hook that raises an error is reported and does not change
the run's exit code.

`suite:strays({ "name", ... })` names processes that must not already be
running when a run starts (the preflight reports each one it finds).
`suite:preflight(function(p) ... end)` adds a check of its own: `p:warn(msg)`
reports, `p:fail(msg)` stops the run (`--force` runs anyway).

Modules are required by path: `require("./helpers")` (`.cluau`, then `.luau`,
then `helpers/init.cluau`), `require("@hamr")` and `require("@hamr/<module>")`
for the standard library.

## Steps

`suite:step(name, spec)`:

| Field | Meaning |
|---|---|
| `run` | A command (a list of arguments, run without a shell) or a function `(ctx) -> ()`. |
| `timeout` | Required. Seconds before the step's processes are ended (SIGTERM to the process group, SIGKILL 5 s later). |
| `budget` | Required. Seconds the step should take; over it is a warning. |
| `tier` | `fast` (default), `nightly` or `manual`. `--tier nightly` runs the fast and nightly steps. |
| `needs` | Steps that must pass first, declared earlier. A step whose need failed is `blocked`. |
| `resources` | `{ exclusive = true }` runs alone; `cpu = n` is its share of `-j` (default 1); `tags = { "ports" }` are held by one step at a time. |
| `cwd` | Where it runs (default: the suite's directory). |
| `env` | Environment variables for this step. |
| `findings` | A rule turning what the step did into findings (below). |
| `paths` | The source globs the step covers; `--changed` runs only the steps whose paths changed. A step without `paths` always runs. |
| `doc` | One line saying what the step checks. |
| `meta` | Labels of the suite's own (`{ app = "sandbox" }`), copied into the step's report entry for a report hook. |

A step's status is `pass`, `fail` (an error finding, or a command that
exited non-zero), `timeout`, `blocked`, `skipped` (not selected) or
`interrupted`.

### The context

A function step gets a context:

| | |
|---|---|
| `ctx:start(argv, options?)` | Starts a command in the background and returns a `Proc`: `proc:wait()` (its `Run`), `proc:kill()`, `proc:done()`. A process the step leaves running is ended when the step ends (a `stray` warning). |
| `ctx:sleep(seconds)`, `ctx:waitUntil(ready, timeout, interval?)` | Wait without holding up other steps; `waitUntil` polls `ready()` and says whether it held in time. |
| `ctx:run(argv, options?)` | Runs a command to its end and returns its `Run` (`code`, `signal`, `timedOut`, `seconds`, `cpuSeconds`, `peakRssMB`, `leftProcesses`, `log`). A non-zero exit is an error finding unless `options.allowFail`. Options: `cwd`, `env`, `unset` (variables it must not inherit), `timeout`, `log` (another file than the step's log). |
| `ctx:step(name, spec)` | Starts a sub-step (the step fields; `budget` and `timeout` optional). It is scheduled like any step; its name is `<step>/<name>`; `needs` may name a sibling. |
| `ctx:wait()` | Waits for the sub-steps started so far. A function step also waits for them when it returns. |
| `ctx:finding(kind, message, severity?)` | `severity` is `error` (default) or `warning`. |
| `ctx:fail(message)` | An error finding of kind `fail`. |
| `ctx:log(text)`, `ctx:readLog()` | The step's log. |
| `ctx.name`, `ctx.root`, `ctx.out`, `ctx.vars`, `ctx.result` | The step's name, the suite's directory, this run's directory, the variables, the result being built. |

A step's coroutine must not wait inside a coroutine of its own: `ctx:run`
and `ctx:wait` yield to hamr's loop. cluau code that runs past the step's
timeout without yielding raises an error in that step.

### Findings rules

`hamr.findings`:

| | |
|---|---|
| `exitCode()` | The default for a command: a non-zero exit is the finding. |
| `failLines(kind, marker?)` | When the command fails, each output line containing `marker` (default `FAIL`) is a finding. |
| `logGate({ file?, match, ignore?, kind?, severity? })` | After the step, pass or fail, each line of `file` (default: the step's own output) matching a `match` pattern and no `ignore` pattern is a finding. Lua patterns. |
| `junit(file, kind?)` | Reads a JUnit XML file: every test case's time goes into the report, each failed case is a finding. |
| `all({ rule, ... })` | Every rule. |

A rule is any function `(result, ctx) -> ()`. Whatever a rule decides, a
command step that exited non-zero fails.

Processes a step leaves running in its process group are a `stray` warning,
and hamr ends them.

## The scheduler

Every step is a coroutine. A step waits by yielding, and hamr's loop resumes
it when its process ends or its sub-steps finish. A ready step (its needs
passed) starts while its resources fit:

- an `exclusive` step runs with nothing else; while one is waiting to start,
  nothing behind it starts, so it is not starved;
- the `cpu` shares of the running steps stay within `-j` (one step wider
  than `-j` still runs, alone);
- a tag is held by one step at a time;
- a step never waits for its own ancestors: a function step lends its share
  to its sub-steps.

Ready steps start longest first, by their median time over the recent runs
(else their budget). The long steps start early, which is what shortens the
wall time.

## Output

Each run writes `<out>/runs/<stamp>/`:

- `report.json`, with each step's log under `logs/`;
- a copy of the report at `<out>/latest.json`;
- a line appended to `<out>/history.jsonl` (the newest 500 lines are kept),
  which `list`, `history`, the medians and the regression warnings read.

The newest 20 runs are kept.

### Timing warnings

These never fail a run:

- `budget`: a step took longer than its budget;
- `regression`: a step took over twice its budget, or 50 % and 10 s over its
  median of at least three passing runs;
- `tier_budget`: the whole run took longer than the tier's budget. This is
  only checked when the run selected the whole tier (no `--only` or
  `--changed`).

### `report.json` (schema `hamr/1`)

`hamr validate <report.json> ...` checks files against the schema, which
is `std/schema.cluau` (`require("@hamr/schema").validate(value)` from a
script).

| Field | |
|---|---|
| `schema` | `"hamr/1"` |
| `suite`, `tier`, `args`, `jobs` | What ran. |
| `started`, `startedUnix`, `wallSeconds` | When it ran and how long it took. |
| `machine` | `hostname`, `platform`, `cores`, `memoryMB`. |
| `revision` | The jj or git revision, when there is one. |
| `interrupted` | Whether Ctrl-C ended the run. |
| `preflight` | `[{ severity, message }]`. |
| `steps` | Each step, its sub-steps nested in `children`: `name`, `tier`, `status`, `reason`, `startedAt` and `endedAt` (seconds from the run's start), `seconds`, `queuedSeconds` (a step with sub-steps: the part of its time when nothing of it ran, its sub-steps waiting for resources; budgets are held to the rest), `cpuSeconds`, `peakRssMB`, `budget`, `timeout`, `command`, `log`, `findings`, `runs`, `tests` (`[{ name, seconds, status }]`), `children`, `meta`. |
| `findings` | Every finding: `{ step, kind, message, severity }`. |
| `summary` | `steps`, `passed`, `failed`, `timedOut`, `blocked`, `skipped`, `interrupted`, `errors`, `warnings`, `cpuSeconds`, `overBudget`, `criticalPath`, `tierBudget`. |

`cpuSeconds` and `peakRssMB` come from `wait4`. They cover a process and the
children it waited for; the peak is the largest single process.

The critical path is the chain of top-level steps that set the wall time.
It starts at the step that ended last and goes back through what ended just
before each one started: a need, or the step whose resources it waited for.

## The host API

`host` is a global (declared in `types/host.d.luau`). The standard library
wraps it; suites may call it directly:

- processes: `spawn`, `poll`, `kill`, `running`, `interrupted`, `capture`,
  `freePort`;
- files: `read`, `write`, `append`, `exists`, `isDir`, `mkdir`, `remove`,
  `copy`, `list`, `glob`, `globMatch`, `stat`;
- paths: `cwd`, `absolute`, `relative`, `join`, `parent`, `basename`,
  `symlink`;
- JSON: `jsonEncode`, `jsonDecode`, `jsonMark`;
- the machine: `cores`, `memory`, `diskFreeMB`, `processes`, `getenv`,
  `hostname`, `platform`, `pid`, `executable`;
- time and the terminal: `now`, `monotonic`, `isoTime`, `sleep`, `isTty`,
  `terminalWidth`, `stdout`, `stderr`, `setDeadline` (the runner's);
- modules and checking: `require`, `check`, `stdModules`, `resolve`.

Globs: `*` and `?` stay inside a path segment, `**` crosses segments, and
`**/` also matches no segment.

`hamr --std-dir <dir>` loads the standard library from `<dir>` (and
`<dir>/../types/host.d.luau`) instead of the embedded copy, for working on
the library without rebuilding.
