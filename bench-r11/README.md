# M4 Max R11G11B10F end-of-tile resolve, 18 Nov 2026

Sources, runner scripts and raw output for "hk: store R11G11B10F through its own class on the M4 Max"
(branch `pr/m4max-r11-2x-resolve`, commit d2408a65 on aurora-wip 57df4db8).

Hardware: MacBook Pro 16" M4 Max (J616C, T6041, 32-core GPU, kernel reports G16C B1), GPU firmware 25G83,
Linux 7.3.0-j616c-ia2, on AC (battery 95 %, not charging), fans on SMC automatic. The desktop was logged in.
Two builds from one build dir with the same options (`build.sh`), so they differ only by the change:
`base` = aurora-wip 57df4db8, `fix` = d2408a65. The mesa-clc / precomp tools were built from 57df4db8 too.
Builds alternate run by run (base fix base fix ...).

- `build.sh`: the clc tools, both builds, `meson test --suite asahi` on each (`raw/build.txt`, `raw/unit-*.txt`).
- `run-r11.sh`: the runner (sections info, r11, visit, t41, bench, end). The test programs `aurora-r11g11b10f`,
  `aurora-tile-visit`, `vulkan-msaa-mrt` and `vulkan-msaa-xfer` are the in-tree ones
  (`src/asahi/vulkan/tests/`, built with `-Dbuild-tests=true`), unchanged.
- `vkmsaa2x/`: the [bench-17nov](../bench-17nov/vkmsaa2x) benchmark plus an `r11g11b10f` format, `VKM_EOT=1`
  (the last pass's 2x colour has storeOp DONT_CARE, so the driver resolves it at the end of the tile) and a
  resolve check (one more frame, resolved after the pass into a second image, compared pixel by pixel). Without
  `VKM_EOT` it records the bench-17nov frames.
- `summarize.py raw` makes `summary.txt`.
- `raw/r11-2x.txt`, `raw/r11-1x.txt`: `aurora-r11g11b10f 2` (3 runs per build) and `1` (1 run per build).
- `raw/visit.txt`: `aurora-tile-visit` with one target (`r11f` at 2x and 4x, `rgba16f` at 2x as a control).
- `raw/t41-{base,fix}/`: the 55-case mesa#129 suite as run on the daily drivers, one pass per build;
  `raw/t41-compare.txt` compares the logs.
- `raw/vk-r11.txt`, `raw/vk-other.txt`: every vkmsaa2x run, tagged `<label> v=<build> i=<run> rc=<rc> | <output>`.
- `raw/info-*.txt`, `raw/run.log`: machine state and the runner's log. The two `cd: ... vkmsaa2x` lines in
  `info-start.txt` are a path mistake of mine in the info section only (the directory was moved into place
  before the bench section ran); each build's driver string is in the `driver` lines of `raw/vk-*.txt`.
