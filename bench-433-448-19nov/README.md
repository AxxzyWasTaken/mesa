# M4 Max re-runs for mesa#448 and mesa#433 on aurora-wip e66c01e7, 19 Nov 2026

Sources, runner scripts and raw output for the reworked #448 ("hk: store R11G11B10F through its own class on G17P",
commit 47273442) and #433 ("hk: compress 2x colour and 2x depth wherever a 2x reload keeps the sample", commit
68a70dc1), both one commit on aurora-wip e66c01e7.

Hardware: MacBook Pro 16" M4 Max (J616C, T6041, 32-core GPU, kernel reports G16C B1), GPU firmware 25G83,
Linux 7.3.0-j616c-ia2, on AC (battery charging, 77 %), fans on SMC automatic (0 rpm), SoC die 56 C at the start. The desktop was
logged in (a browser open, no game). Three builds from one build dir with the same options (`build.sh`), clc tools
from e66c01e7, so they differ only by the commit: `base` = e66c01e7, `r11` = 47273442, `c2x` = 68a70dc1. Builds
alternate run by run. Each driver's commit string is in `raw/info-start.txt`.

- `build.sh`: the clc tools, the three builds, `meson test --suite asahi` on each (`raw/build.txt`, `raw/unit-*.txt`).
- `run.sh`: the runner (sections info, r11, visit, r11bench, probe, 2x, t41, end), 19 Nov 18:20-18:23.
- `rerun.sh`: the rgba8-d24 configuration again, 7 + 7 (18:23), because its round in `run.sh` had a wide spread on
  `c2x` (it did in an earlier, discarded round too, with bdef7e78, which differs from 68a70dc1 only in a comment).
- `vkmsaa2x/`: the [bench-r11](../bench-r11/vkmsaa2x) benchmark, unchanged (without `VKM_EOT` it records the
  [bench-17nov](../bench-17nov/vkmsaa2x) frames).
- The in-tree test programs `aurora-r11g11b10f`, `aurora-tile-visit`, `vulkan-msaa-mrt`, `vulkan-msaa-xfer`
  (`src/asahi/vulkan/tests/`, `-Dbuild-tests=true`) come from the `base` build.
- `summarize.py raw` makes `summary.txt`.

Raw files: `raw/r11-2x.txt`, `raw/r11-1x.txt`, `raw/visit.txt`, `raw/vk-r11.txt`, `raw/vk-other.txt` (#448);
`raw/2x-probe.txt`, `raw/vk-2x.txt` (#433); `raw/t41-<build>-<pass>/` (the 55-case mesa#129
suite, one pass for `r11`, two for `base` and `c2x`) and `raw/t41-compare.txt`; `raw/info-*.txt`, `raw/run.log`.
Every vkmsaa2x line is tagged `<label> v=<build> i=<run> rc=<rc> | <output>`.
