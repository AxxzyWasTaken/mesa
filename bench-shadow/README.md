# M4 Max feedback-loop benchmarks, 17 Nov 2026

Sources, runner scripts and raw output for "asahi: count only views the draw's shaders read as feedback loops"
(branch `pr/gl-feedback-loop-used-views`, commit 2e2647c4 on aurora-wip 57df4db8).

Hardware: MacBook Pro 16" M4 Max (J616C, T6041, 32-core GPU), on battery, fans on SMC automatic. The desktop was
in use: the `info-*.txt` snapshots show other GPU clients keeping the fragment engine 0-53 % busy.
Four variants, built in one build dir with the same options (`build.sh`), so they differ only by the change:
`wip` = aurora-wip 57df4db8, `fix` = 2e2647c4, `m418` = refs/pull/418/merge 1752a4d9 (#418, Z32F depth
compression), `m418fix` = local merge c719e0d8 of 2e2647c4 into 1752a4d9. Builds alternate run by run.

- `build.sh`: builds the four variants and runs `meson test --suite asahi` on each (`raw/build.txt`, `raw/unit-*.txt`).
- `run-shadow.sh`: the runner (sections info, check, log, bench, repeat). `summarize.py` and `gpubusy.sh` are the
  bench-17nov tools.
- `glfeedback.c`: real feedback loops and stale views (`check`), and two benchmarks (`bench-fs`, `bench-cs`).
- `gldepthcomp.c`, `sodiumpace.c`: the test programs from the #418 text. `gldepthbench.c` is
  [bench-17nov/gldepthbench.c](../bench-17nov/gldepthbench.c) (same file). Sodium's shaders are not attached.
- `summary.txt`: medians and spread, `summarize.py raw`.
- `raw/bench-*.txt`: every timed run, tagged `<label> v=<variant> i=<run> rc=<rc> | <output>`.
- `raw/check-{1,2,3}.txt`: three runs of the check section (glfeedback check, gldepthcomp). check-1 used an
  earlier glfeedback.c without the two `glTextureBarrier()` calls before loop-depth and loop-vs; its loop-vs
  then read texels the previous draw had just written, which GL leaves undefined, and under `nocompress` it
  differed. check-2 and check-3 are this glfeedback.c.
- `raw/repeat.txt`: gldepthcomp 2048x2048, 10 runs per variant, every step's hash (after check-2's one odd hash).
- `raw/log.txt`: the driver's resource/perf log per workload and variant (images created compressed or plain,
  feedback-loop decompressions). The first try's log was identical.
- `raw/info-*.txt`: machine state at the start and end of each runner invocation; `raw/run.log`, `raw/run-1.log`:
  the runner's log (including a 5-minute pause for a game process during check-2).
