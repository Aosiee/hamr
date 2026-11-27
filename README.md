# hamr

[![Linux](https://github.com/Aosiee/hamr/actions/workflows/linux.yml/badge.svg?branch=dev)](https://github.com/Aosiee/hamr/actions/workflows/linux.yml)
[![Windows: not supported](https://img.shields.io/badge/Windows-not%20supported-lightgrey)](#building)
[![macOS: not supported](https://img.shields.io/badge/macOS-not%20supported-lightgrey)](#building)

[![Release](https://img.shields.io/github/v/release/Aosiee/hamr)](https://github.com/Aosiee/hamr/releases/latest)

A test runner scripted in [cluau](https://github.com/Aosiee/cluau). A
project declares its checks once, in a `hamr.cluau` suite. hamr runs them
side by side where it can and times every step. It writes one report shape
for every project and remembers each step's recent times, so a suite that
starts to slow down says so.

```
$ hamr run                  # hamr's own suite
hamr: hamr, fast tier, 7 of 7 steps, -j 32
[  1/7] pass           0.1s  build  (budget 1m00s)
[  2/22] pass           0.0s  selftest  (budget 5.0s)
...

  status      time  share     cpu    rss  budget  vs med  step
  pass         6.3s    99%    0.0s     0M     15s   +103% *samples
  pass         5.0s           0.6s   209M       -   +4.5s    measure
  pass         3.1s           0.0s    28M       -     -0%    history
  ...
  pass         3.0s    47%    0.1s    28M     15s     +0%  test-library
  pass         0.1s     1%    0.0s    28M   1m00s   -0.1s *build
  ...
  * critical path: build -> samples

hamr: 22 passed in 6.3s (budget 2m00s); cpu 2.0s
hamr: report .hamr/runs/2026-11-24T21-00-13_2764914/report.json
```

The step count grows as it runs: a step that fans out into sub-steps
(`samples` here) adds them when it starts.

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

hamr also runs test cases: `hamr.tests(...)` in a suite (or `hamr test
<binary>`) spreads a test binary's cases over worker processes, so a crash
or a hang costs only its case. C++ tests are written against hamr's own
library, `include/hamr/test.h` (one header, C++17); other languages plug in
as adapters written in cluau.

## Installing

Each release on the [releases page](https://github.com/Aosiee/hamr/releases)
has a Linux x86-64 download: the `hamr` binary (its standard library is
built in, so it is the only file needed), the reference, and
`include/hamr/test.h`, the C++ test library.

```bash
curl -fsSL https://github.com/Aosiee/hamr/releases/download/v0.1.0/hamr-v0.1.0-linux-x86_64.tar.gz | tar -xz
./hamr-v0.1.0-linux-x86_64/hamr --version
```

Or build it from source (below). How branches, streams and releases work is
in [CONTRIBUTING.md](CONTRIBUTING.md).

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
| `hamr test <binary> [--case G] [--workers N] [-- args]` | run a test binary's cases side by side, a crash or a hang costing only its case |

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

hamr is released under the Odd Fish Society License ([`LICENSE`](LICENSE)), which is MIT
with one added condition. A product that includes hamr, or is built or tested with it,
credits it in its credits, about box or documentation, as "Uses hamr - Copyright (c)
2026 Odd Fish Society / Felipe Lara". A product that uses several Odd Fish Society
libraries can credit them together, on one line or several, such as "Uses kiln, cluau
and wisp - Copyright (c) 2026 Odd Fish Society / Felipe Lara". No logo or splash screen
is required.