#!/bin/bash
# run-418b.sh: second #418 round after run-all.sh: gldepthbench as before and with its composite pass, which
# keeps the shadow map compressed (see gldepthbench.c). Same guards, variants and environment as run-all.sh.
# Run on the Max as root: bash /root/gpu/b17/run-418b.sh
# Off-screen only (EGL surfaceless, Vulkan without a surface). Test drivers come from /root/gpu/b17/lib-<variant>
# (LD_LIBRARY_PATH, GL) and /root/gpu/b17/icd-<variant>.json (VK_DRIVER_FILES); nothing installed is touched.
# Before every run: no game may be running (else wait, rechecking every 5 min, until DEADLINE, then stop), and
# no new GPU recovery/fault line may have appeared in the kernel log since the start (else stop at once).
# Variants (all on aurora-wip 57df4db8, built in one build dir with the same options, see build.sh):
#   wip  = aurora-wip 57df4db8                     m417 = refs/pull/417/merge 56a5252e (57df4db8 + e16b80ed)
#   m418 = refs/pull/418/merge 1752a4d9 (+ ef69f599) m420 = refs/pull/420/merge 67f4306d (+ c8f9bc4e)
#   mc2x = local merge db2a2b3b of 8be57389 (hold/m4max-2x-compression, on #420) into 57df4db8 (tree 899c47cf)
set -uo pipefail
B=/root/gpu/b17; R=$B/raw; T=/root/gpu/tests; BIN=$B/bin; mkdir -p $R
N=${N:-7}
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
GAME_RE='SCPSL|Roblox|cordial|sober|java.*(minecraft|prism)|steam_app|\.exe'
BAD_RE='gpu recovery|gpu hang|GPU work of a client failed|(asahi|aurora|agx|g17).*(fault|timed out|stall|hang|recover)'
bad() { dmesg | grep -ciE "$BAD_RE"; }
B0=$(bad)
log() { echo "$(date +%T) $*" | tee -a $R/run.log; }
guard() {
  while pgrep -f "$GAME_RE" >/dev/null; do
    [ "$(date +%s)" -lt "$DEADLINE" ] || { log "STOP: a game is still running: $(pgrep -fa "$GAME_RE" | grep -oE '[^/ \\]+\.exe|SCPSL|Roblox|sober|java' | sort -u | tr '\n' ' ')"; exit 3; }
    log "PAUSE: game running, recheck in 300 s"; sleep 300
  done
  [ "$(bad)" = "$B0" ] || { log "STOP: new GPU trouble in dmesg"; dmesg | grep -iE "$BAD_RE" | tail -5 | tee -a $R/run.log; exit 4; }
}
set -a; . /opt/aurora-mesa/session-env; set +a          # the desktop's GL/Vulkan environment (ASAHI_MESA_DEBUG=1queue, MESA_GLTHREAD=false)
export MESA_SHADER_CACHE_DISABLE=true
unset VK_ICD_FILENAMES
gl() { local v=$1; shift; LD_LIBRARY_PATH=$B/lib-$v timeout -k 5 180 "$@"; }
vk() { local v=$1; shift; VK_DRIVER_FILES=$B/icd-$v.json timeout -k 5 180 "$@"; }
# ab <file> <label> <variants> -- cmd...: N rounds, variants in turn (A B A B ...), every stdout line tagged.
# A variant may carry extra environment after a '+', e.g. m418+ASAHI_G17P_GL_DEPTH_COMPRESS=0.
ab() {
  local f=$R/$1 lab=$2 vs=$3; shift 4
  local i v vv out rc extra
  for i in $(seq 1 $N); do for v in $vs; do
    vv=${v%%+*}; extra=(); [[ $v == *+* ]] && extra=(${v#*+})
    guard
    out=$(env "${extra[@]}" LD_LIBRARY_PATH=$B/lib-$vv VK_DRIVER_FILES=$B/icd-$vv.json timeout -k 5 180 "$@" 2>/dev/null); rc=$?
    sed "s/^/$lab v=$v i=$i rc=$rc | /" <<<"$out" >> $f
  done; done
  log "$lab: $(grep -c "^$lab v=.* rc=0 " $f) lines with rc=0"
}
guard; log "418b"
ab 418b-depth.txt gldepthbench "wip m418 m418+ASAHI_G17P_GL_DEPTH_COMPRESS=0" -- $BIN/gldepthbench 3456 2234 200
ab 418b-depth.txt gldepthbench-composite "wip m418 m418+ASAHI_G17P_GL_DEPTH_COMPRESS=0" -- $BIN/gldepthbench 3456 2234 200 2048 3000 composite
guard
for v in wip m418; do for m in "" composite; do
  gl $v env ASAHI_MESA_DEBUG=1queue,resource,perf $BIN/gldepthbench 3456 2234 3 2048 3000 $m >/dev/null 2>/tmp/b17-r
  echo "gldepthbench ${m:-default} $v: $(grep -cE 'New: z32_float .* GPU COMP' /tmp/b17-r) Z32F images created compressed, $(grep -c 'Decompressing resource due to Texture feedback loop' /tmp/b17-r) decompressed by the feedback-loop rule"
done; done > $R/418b-resource.txt 2>&1
log "418b DONE, GPU trouble lines new: $(( $(bad) - B0 ))"
