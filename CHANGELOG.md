# Changelog

Each release is an annotated tag (`v0.x.0`, or `v0.x.y` for a release of
fixes only) whose message is the entry below. Versions before 1.0 may change
the suite API; every entry says whether existing suites need to change.

## 0.1.0

The first release.

- `hamr run`, `list`, `history`, `check` and `exec`.
- Suites in cluau: steps as commands or functions, sub-steps, needs, tiers,
  variables, the preflight and stray-process checks.
- The scheduler: resources (exclusive, CPU shares, tags), longest-first by
  the recent medians, and `--only`, `--skip`, `--changed`.
- Every process measured (wall time, CPU time, peak memory). A timeout ends
  the whole process group, and a step's leftover processes are reported and
  ended.
- The findings rules: exit codes, `FAIL` lines, log gates, JUnit XML.
- The `hamr/1` report and `hamr validate`, report hooks (`suite:onReport`),
  the run history, the budget and regression warnings, and the end-of-run
  table with the critical path.
- A step's timeout also stops cluau code that never yields.
- Background processes in a step (`ctx:start`), `ctx:sleep` and
  `ctx:waitUntil`, free ports, unset environment variables.
- A step with sub-steps reports the time they spent queued apart
  (`queuedSeconds`); its budget is held to its active time.
