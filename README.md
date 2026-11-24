# hamr

A test runner scripted in [cluau](https://github.com/Aosiee/cluau). A
project declares its checks once, in a `hamr.cluau` suite. hamr runs them
side by side where it can and times every step. It writes one report shape
for every project and remembers each step's recent times, so a suite that
starts to slow down says so.

```
$ hamr run                  # hamr's own suite
hamr: hamr, fast tier, 5 of 5 steps, -j 32
[  1/5] pass            25s  build  (budget 1m00s)
[  2/16] pass           0.0s  selftest  (budget 5.0s)
...
[ 16/16] pass           3.6s  samples  (budget 15s)

  status      time  share     cpu    rss  budget  vs med  step
  pass          25s    87%   5m51s   521M   1m00s     new *build
  pass         3.6s    13%    0.0s     0M     15s     new *samples
  pass         3.1s           0.0s    28M       -     new    history
  pass         1.5s           0.0s    28M       -     new    parallel
  ...
  * critical path: build -> samples

hamr: 16 passed in 28s (budget 2m00s); cpu 5m52s
hamr: report .hamr/runs/2026-11-24T00-46-15_343518/report.json
```

hamr has two parts:

- **A small native host (C++, `src/`):** Luau's VM and compiler, the cluau
  translator and its strict checker, and what a script cannot do itself:
  - start processes, each in its own process group, with a timeout that
    ends the whole group, and measure each one (wall time, CPU time, peak
    memory);
  - files, clocks, JSON and the machine's state.
- **The standard library, in cluau (`std/`):** suites and steps, the
  scheduler, the findings rules, the live lines, the end table, the report
  and the history. It is embedded in the binary, so changing how hamr
  behaves is a script edit.

hamr knows nothing about any build system or project. A suite builds with
whatever its project uses.

## Building

hamr needs CMake 3.24, a C++20 compiler and Linux. It builds its own copy of
Luau and cluau, fetched at the commits pinned in `CMakeLists.txt`, or the
checkouts you name:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hamr --version
```

```bash
cmake -S . -B build -DHAMR_LUAU_DIR=<luau checkout> -DHAMR_CLUAU_DIR=<cluau checkout>
```

## A suite

```
// hamr.cluau, at the project's root
const hamr = require("@hamr")
const suite = hamr.suite("MyLibrary", { budgets = { fast = 300 } })
const config = suite:var("config", "Release")

suite:step("build", {
	run = { "cmake", "--build", "build", "--config", "{config}", "-j" },
	resources = { exclusive = true },          // builds never run side by side
	timeout = 1800, budget = 120,
})

suite:step("tests", {
	run = { "ctest", "--test-dir", "build", "--output-junit", "junit.xml" },
	needs = { "build" },
	findings = hamr.findings.junit("build/junit.xml"),   // every test's time in the report
	timeout = 600, budget = 60,
	paths = { "src/**", "tests/**" },          // what --changed looks at
})

// A function step: its phases are sub-steps, scheduled and timed one by one.
suite:step("integration", {
	needs = { "build" },
	timeout = 600, budget = 90,
	run = function(ctx: hamr.Context) {
		for (_, phase in { "udp", "tcp", "loss" }) {
			ctx:step(phase, { run = { "./build/integration", "--phase", phase }, resources = { tags = { "ports" } } })
		}
	},
})

return suite
```

Every step says what it may cost: a `budget` (seconds; over it is a
warning) and a `timeout` (seconds; the step is ended). A suite without them
does not load.

## Commands

| | |
|---|---|
| `hamr run [suite] [--tier T] [--only G] [--skip G] [--changed] [-j N] [--set k=v] [--dry-run]` | run the suite (default: the nearest `hamr.cluau`) |
| `hamr list [suite]` | the steps with their budgets, medians and last times |
| `hamr history [suite] [step]` | recent runs, or one step's trend |
| `hamr check [files...]`, `hamr check --std` | Luau's type checker under cluau's strict rules |
| `hamr exec <script.cluau> [args]` | run a cluau script |
| `hamr validate <report.json> ...` | check reports against the `hamr/1` schema |

Exit codes: 0 when everything passed, 1 when a step failed, timed out or was
blocked, 2 for a usage or preflight error, 130 when interrupted (Ctrl-C
ends the running steps and still writes the report).

`docs/reference.md` describes the step fields, the scheduler, the findings
rules, the report schema and the host API.

## Testing hamr

hamr tests itself: `hamr run` here builds a fresh hamr in `build/suite`,
type-checks the library and every suite, runs the unit checks, and runs each
sample suite in `tests/suites/` with the new binary, checking what it
reports.

## License

The hamr License (`LICENSE`): MIT-style, with an attribution line for products
that include hamr.
