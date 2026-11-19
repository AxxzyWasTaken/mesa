#!/bin/bash
# run.sh: M4 Max hardware runs for aurora-silicon/mesa #448 (R11G11B10F own class on G17P) and #433 (2x colour and
# 2x depth compression by the 2x reload property), both rebased on aurora-wip e66c01e7.
# Run on the Max as root: bash /root/gpu/p19/run.sh [section...]
#   sections: info r11 visit r11bench probe 2x t41 end
# Off-screen only (Vulkan without a surface). Drivers come from /root/gpu/p19/<build>/icd.json (VK_DRIVER_FILES);
# nothing installed is touched. One test process at a time. Before every run: no game may be running (else wait,
# rechecking every 5 min, until DEADLINE, then stop), and no new GPU recovery/fault line may have appeared in the
# kernel log since the start (else stop at once).
# Builds (one build dir, same options, clc tools from e66c01e7, see build.sh):
#   base = aurora-wip e66c01e7    r11 = 47273442 (#448)    c2x = 68a70dc1 (#433)
set -uo pipefail
B=/root/gpu/p19; R=$B/raw; BIN=$B/bin; mkdir -p $R
N=${N:-7}
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3 * 3600 ))}
GAME_RE='SCPSL|Roblox|cordial|sober|java.*(minecraft|prism)|AppId=|\.exe|linuxsteamrt64/cs2'
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
# ab <file> <label> <builds> -- cmd...: N rounds, builds in turn (A B A B ...), every stdout line tagged.
# A build may carry extra environment after a '+', e.g. c2x+ASAHI_MESA_DEBUG=1queue,nocompress.
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
SECTIONS="${*:-info r11 visit r11bench probe 2x t41 end}"

info() {
  echo "date $(date -Is)"; uname -r; tr -d '\0' < /proc/device-tree/model; echo
  echo "AC online $(cat /sys/class/power_supply/macsmc-ac/online), battery $(cat /sys/class/power_supply/macsmc-battery/status) $(cat /sys/class/power_supply/macsmc-battery/capacity)%"
  aurora-fan status 2>&1 | head -2; uptime
  bash $B/gpubusy.sh 2
  for v in base r11 c2x; do
    echo "$v: $(sha256sum $B/$v/libvulkan_asahi.so | cut -c1-16) $(cd $B/vkmsaa2x && vk $v ./vkmsaa2x 64 64 1 rgba8 4 | grep '^driver')"
  done
}

if section info; then guard; log "info"; info > $R/info-start.txt 2>&1; fi

if section r11; then
  # #448: the in-tree test (src/asahi/vulkan/tests/aurora_r11g11b10f.c). res-eot is the end-of-tile resolve.
  N=2 ab r11-2x.txt r11-2x "base r11" -- $BIN/aurora-r11g11b10f 2
  N=1 ab r11-1x.txt r11-1x "base r11" -- $BIN/aurora-r11g11b10f 1
fi

if section visit; then
  # #448: aurora-tile-visit with ONE target: its resolve-nostore shapes take the end-of-tile resolve.
  for c in "ms=2 fmts=r11f" "ms=4 fmts=r11f" "ms=2 fmts=rgba16f"; do
    N=1 ab visit.txt "visit-$(tr ' =' '_-' <<<"$c")" "base r11" -- $BIN/aurora-tile-visit $c
  done
fi

if section r11bench; then
  # #448: R11G11B10F 2x, the last pass resolved after the pass (storeOp STORE), then at the end of the tile.
  cd $B/vkmsaa2x
  N=2 ab vk-r11.txt r11-d24 "base r11" -- ./vkmsaa2x 3456 2234 200 r11g11b10f 24 4
  N=2 ab vk-r11.txt r11-d24-eot "base+VKM_EOT=1 r11+VKM_EOT=1" -- ./vkmsaa2x 3456 2234 200 r11g11b10f 24 4
  N=1 ab vk-other.txt rgba8-d24 "base r11" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  cd /
fi

if section probe; then
  # #433: is the 2x colour/depth image compressed (driver log, size, GPU writes into the metadata)?
  cd $B/vkmsaa2x; guard; log "probe"
  {
    for f in rgba8 rgba16f; do
      vk c2x env ASAHI_MESA_DEBUG=1queue,nocompress ./vkmsaa2x 3456 2234 1 $f 24 4 probe > /tmp/p19-p0 2>&1
      pc=$(awk '/^image ms-colour/ {sub("size=","",$5); print $5}' /tmp/p19-p0); pz=$(awk '/^image ms-depth/ {sub("size=","",$5); print $5}' /tmp/p19-p0)
      echo "## $f c2x nocompress"; grep -E '^(image|probe)' /tmp/p19-p0
      for v in base c2x; do
        guard
        echo "## $f $v (plain sizes $pc / $pz)"
        vk $v env ASAHI_MESA_DEBUG=1queue,perf VKM_PLAIN_C=$pc VKM_PLAIN_Z=$pz ./vkmsaa2x 3456 2234 1 $f 24 4 probe 2>&1 | grep -E '^(image|probe)|creating|compression' | sed -E 's/^.*(G17P compression enabled|No compression)/  driver: \1/'
      done
    done
  } > $R/2x-probe.txt 2>&1
  cd /
fi

if section 2x; then
  # #433: the bench-17nov frames (vkmsaa2x without VKM_EOT), 7 + 7 runs, alternating.
  cd $B/vkmsaa2x
  ab vk-2x.txt rgba8-d24 "base c2x" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  ab vk-2x.txt rgba16f-d24 "base c2x" -- ./vkmsaa2x 3456 2234 150 rgba16f 24 4
  ab vk-2x.txt rgba8-d200 "base c2x" -- ./vkmsaa2x 3456 2234 100 rgba8 200 4
  N=3 ab vk-2x.txt rgba8-d24-nocompress "c2x+ASAHI_MESA_DEBUG=1queue,nocompress" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  cd /
fi

if section t41; then
  # The mesa#129 suite (55 cases, as t41test.sh configuration A), per build; two passes for base and c2x.
  t41run() { local l=$1 t=$2; shift 2; guard
    timeout -k 5 $t "$@" > $O/$l.log 2>&1; local rc=$?
    printf "%-4s %-34s rc=%s\n" $v $l $rc >> $O/summary.txt; }
  for vp in base:1 r11:1 c2x:1 base:2 c2x:2; do
    v=${vp%:*}; p=${vp#*:}
    O=$R/t41-$v-$p; rm -rf $O; mkdir -p $O; log "t41 $v pass $p"
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
    log "t41 $v pass $p: $(grep -c 'rc=' $O/summary.txt) runs, nonzero: $(grep -v 'rc=0$' $O/summary.txt | awk '{print $2"("$3")"}' | tr '\n' ' ')"
  done
  { for a in r11-1 c2x-1 base-2 c2x-2; do
      for f in $R/t41-base-1/*.log; do l=$(basename $f); cmp -s $f $R/t41-$a/$l && echo "same      base-1 $a $l" || echo "DIFFERENT base-1 $a $l"; done
    done; } > $R/t41-compare.txt
  log "t41 compare against base pass 1: $(grep -c ^same $R/t41-compare.txt) logs identical, different: $(grep ^DIFF $R/t41-compare.txt | awk '{print $3":"$4}' | tr '\n' ' ')"
fi

if section end; then
  log "end"; { date -Is; uptime; echo "AC $(cat /sys/class/power_supply/macsmc-ac/online) battery $(cat /sys/class/power_supply/macsmc-battery/capacity)%"; aurora-fan status 2>&1 | head -2; bash $B/gpubusy.sh 2;
    echo "GPU trouble lines in dmesg: start $B0, now $(bad)"; } > $R/info-end.txt 2>&1
fi
log "RUN-DONE"
