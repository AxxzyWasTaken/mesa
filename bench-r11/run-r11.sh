#!/bin/bash
# run-r11.sh: M4 Max hardware runs for "hk: store R11G11B10F through its own class on the M4 Max".
# Run on the Max as root: bash /root/gpu/r11/run-r11.sh [section...]   (sections: info r11 visit t41 bench end)
# Off-screen only (Vulkan without a surface). Drivers come from /root/gpu/r11/<build>/icd.json (VK_DRIVER_FILES);
# nothing installed is touched. One test process at a time. Before every run: no game may be running (else wait,
# rechecking every 5 min, until DEADLINE, then stop), and no new GPU recovery/fault line may have appeared in the
# kernel log since the start (else stop at once).
# Builds (one build dir, same options, clc tools built from 57df4db8, see build.sh):
#   base = aurora-wip 57df4db8     fix = d2408a65 (57df4db8 + the PR's commit)
set -uo pipefail
B=/root/gpu/r11; R=$B/raw; BIN=$B/bin; mkdir -p $R
N=${N:-3}
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3 * 3600 ))}
GAME_RE='SCPSL|Roblox|cordial|sober|java.*(minecraft|prism)|AppId=|\.exe|steam_app'
BAD_RE='gpu recovery|gpu hang|GPU work of a client failed|(asahi|aurora|agx|g17).*(fault|timed out|stall|hang|recover)'
bad() { dmesg | grep -ciE "$BAD_RE"; }
B0=$(bad)
log() { echo "$(date +%T) $*" | tee -a $R/run.log; }
guard() {
  while pgrep -f "$GAME_RE" >/dev/null; do
    [ "$(date +%s)" -lt "$DEADLINE" ] || { log "STOP: a game is still running"; exit 3; }
    log "PAUSE: game running, recheck in 300 s"; sleep 300
  done
  [ "$(bad)" = "$B0" ] || { log "STOP: new GPU trouble in dmesg"; dmesg | grep -iE "$BAD_RE" | tail -5 | tee -a $R/run.log; exit 4; }
}
set -a; . /opt/aurora-mesa/session-env; set +a          # the desktop's Vulkan environment (ASAHI_MESA_DEBUG=1queue)
export MESA_SHADER_CACHE_DISABLE=true
unset VK_ICD_FILENAMES
vk() { local v=$1; shift; VK_DRIVER_FILES=$B/$v/icd.json timeout -k 5 180 "$@"; }
# ab <file> <label> <builds> -- cmd...: N rounds, builds in turn (base fix base fix ...), every stdout line tagged.
# A build may carry extra environment after a '+', e.g. fix+VKM_EOT=1.
ab() {
  local f=$R/$1 lab=$2 vs=$3; shift 4
  local i v vv out rc extra
  for i in $(seq 1 $N); do for v in $vs; do
    vv=${v%%+*}; extra=(); [[ $v == *+* ]] && extra=(${v#*+})
    guard
    out=$(env "${extra[@]}" VK_DRIVER_FILES=$B/$vv/icd.json timeout -k 5 180 "$@" 2>&1); rc=$?
    sed "s/^/$lab v=$v i=$i rc=$rc | /" <<<"$out" >> $f
  done; done
  log "$lab: $(grep -c "^$lab v=.* rc=0 " $f) lines with rc=0"
}
section() { [[ " ${SECTIONS} " == *" $1 "* ]]; }
SECTIONS="${*:-info r11 visit t41 bench end}"

info() {
  echo "date $(date -Is)"; uname -r; tr -d '\0' < /proc/device-tree/model; echo
  echo "AC online $(cat /sys/class/power_supply/macsmc-ac/online), battery $(cat /sys/class/power_supply/macsmc-battery/status) $(cat /sys/class/power_supply/macsmc-battery/capacity)%"
  aurora-fan status 2>&1 | head -2; uptime
  bash /root/gpu/b17/gpubusy.sh 2
  for v in base fix; do
    echo "$v: $(sha256sum $B/$v/libvulkan_asahi.so | cut -c1-16) $(cd $B/vkmsaa2x && vk $v ./vkmsaa2x 64 64 1 rgba8 4 | grep '^driver')"
  done
}

if section info; then guard; log "info"; info > $R/info-start.txt 2>&1; fi

if section r11; then
  # The in-tree test (src/asahi/vulkan/tests/aurora_r11g11b10f.c). res-eot is the end-of-tile resolve.
  ab r11-2x.txt r11-2x "base fix" -- $BIN/aurora-r11g11b10f 2
  N=1 ab r11-1x.txt r11-1x "base fix" -- $BIN/aurora-r11g11b10f 1
fi

if section visit; then
  # aurora-tile-visit with ONE target: its resolve-nostore shapes take the end-of-tile resolve.
  for c in "ms=2 fmts=r11f" "ms=4 fmts=r11f" "ms=2 fmts=rgba16f"; do
    N=1 ab visit.txt "visit-$(tr ' =' '_-' <<<"$c")" "base fix" -- $BIN/aurora-tile-visit $c
  done
fi

if section t41; then
  # The mesa#129 suite as run on the daily drivers (t41test-1119.sh), configuration A, per build.
  t41run() { local l=$1 t=$2; shift 2; guard
    timeout -k 5 $t "$@" > $O/$l.log 2>&1; local rc=$?
    printf "%-4s %-34s rc=%s\n" $v $l $rc >> $O/summary.txt; }
  for v in base fix; do
    O=$R/t41-$v; rm -rf $O; mkdir -p $O; log "t41 $v"
    export VK_DRIVER_FILES=$B/$v/icd.json
    t41run r11-1x 60 $BIN/aurora-r11g11b10f 1
    t41run r11-2x 60 $BIN/aurora-r11g11b10f 2
    for m in "rgba8,r11g11b10f,none 3 4" "rgba8,r11g11b10f,none 3 2" "rgba8,rgb10a2,rgba16f 3 2" "rgba16f,rgba32f,rgba32f,rgba32f,rgba32f,rgba16f 1 2" "rgba8,r11g11b10f,none 3 1"; do
      for w in clear draw none; do set -- $m; t41run "mrt-$3x-$(echo $1|cut -c1-22|tr , _)-$w" 60 $BIN/vulkan-msaa-mrt $1 $2 $3 load $w; done
    done
    for f in rgba8 rgb10a2 rgba16f r11g11b10f; do for d in d32 d32s8; do for x in copy clear mixed mixed-src; do
      t41run "xfer-$f-$d-$x" 60 $BIN/vulkan-msaa-xfer $d $f $x; done; done; done
    for f in rgba8,rgb10 r11f,rgba8 rgba8,r11f rgba16f,r11f rgba16f,rgba16f,rgba8 rgba16f,r11f,rgba8; do
      t41run "visit-2x-$(echo $f|tr , _)" 120 $BIN/aurora-tile-visit ms=2 fmts=$f; done
    unset VK_DRIVER_FILES
    log "t41 $v: $(grep -c 'rc=' $O/summary.txt) runs, nonzero: $(grep -v 'rc=0$' $O/summary.txt | awk '{print $2"("$3")"}' | tr '\n' ' ')"
  done
  { for f in $R/t41-base/*.log; do l=$(basename $f); cmp -s $f $R/t41-fix/$l && echo "same      $l" || echo "DIFFERENT $l"; done; } > $R/t41-compare.txt
  log "t41 compare: $(grep -c ^same $R/t41-compare.txt) logs identical, different: $(grep ^DIFF $R/t41-compare.txt | awk '{print $2}' | tr '\n' ' ')"
fi

if section bench; then
  cd $B/vkmsaa2x
  # R11G11B10F 2x: the last pass resolved after the pass (storeOp STORE), then at the end of the tile (VKM_EOT=1).
  ab vk-r11.txt r11-d24 "base fix" -- ./vkmsaa2x 3456 2234 200 r11g11b10f 24 4
  ab vk-r11.txt r11-d24-eot "base+VKM_EOT=1 fix+VKM_EOT=1" -- ./vkmsaa2x 3456 2234 200 r11g11b10f 24 4
  # Other formats: same work in both builds, hashes must match.
  N=1 ab vk-other.txt rgba8-d24 "base fix" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  N=1 ab vk-other.txt rgba16f-d24-eot "base+VKM_EOT=1 fix+VKM_EOT=1" -- ./vkmsaa2x 3456 2234 150 rgba16f 24 4
  cd /
fi

if section end; then
  log "end"; { date -Is; uptime; echo "AC $(cat /sys/class/power_supply/macsmc-ac/online) battery $(cat /sys/class/power_supply/macsmc-battery/capacity)%"; aurora-fan status 2>&1 | head -2; bash /root/gpu/b17/gpubusy.sh 2;
    echo "GPU trouble lines in dmesg: start $B0, now $(bad)"; } > $R/info-end.txt 2>&1
fi
log "RUN-DONE"
