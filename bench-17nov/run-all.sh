#!/bin/bash
# run-all.sh: hardware A/B runs for aurora-silicon/mesa #417, #418 and the M4 Max 2x compression branch.
# Run on the Max as root, from anywhere: bash /root/gpu/b17/run-all.sh [section...]   (sections: info 417 418 2x t41)
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
section() { [ $# -eq 0 ] || [[ " ${SECTIONS} " == *" $1 "* ]]; }
SECTIONS="${*:-info 417 418 2x t41}"

info() {
  echo "date $(date -Is)"; uname -r; cat /proc/device-tree/model 2>/dev/null | tr -d '\0'; echo
  echo "AC online $(cat /sys/class/power_supply/macsmc-ac/online), battery $(cat /sys/class/power_supply/macsmc-battery/status) $(cat /sys/class/power_supply/macsmc-battery/capacity)%"
  aurora-fan status 2>&1 | head -2; uptime
  bash $B/gpubusy.sh 2
  for v in wip m417 m418 m420 mc2x; do
    echo "$v GL: $(LD_LIBRARY_PATH=$B/lib-$v timeout 60 $BIN/gltxf 64 64 1 1 | grep '^driver')"
    echo "$v VK: $(cd $B/vkmsaa2x && VK_DRIVER_FILES=$B/icd-$v.json timeout 60 ./vkmsaa2x 64 64 1 rgba8 4 | grep '^driver')"
  done
}

if section info; then guard; log "info"; info > $R/info-start.txt 2>&1; fi

if section 417; then
  guard; log "417: shader stats"
  for v in wip m417; do
    for h in 94f5593c 446e5584 96331194 7b77ac7a b4a3173d; do
      gl $v env AGX_MESA_DEBUG=shaderdb,precompile $T/glcomp $(ls /root/gpu/fsdump/FS_$h*.glsl) 2>&1 | grep -oE 'FS shader: [0-9]+ instrs.*' | sed "s/^/$v $h | /"
    done
    for k in 1 2; do for c in plain robust; do
      gl $v env AGX_MESA_DEBUG=shaderdb,precompile $BIN/gltxf 64 64 1 $k $( [ $c = robust ] && echo robust) 2>&1 | grep -oE 'FS shader: [0-9]+ instrs.*' | sed "s/^/$v gltxf-K$k-$c | /"
    done; done
  done > $R/417-shaderdb.txt
  ab 417-gltxf.txt gltxf-K1 "wip m417" -- $BIN/gltxf 3456 2234 300 1
  ab 417-gltxf.txt gltxf-K2 "wip m417" -- $BIN/gltxf 3456 2234 200 2
  N=5 ab 417-gltxf.txt gltxf-K1-robust "wip m417" -- $BIN/gltxf 3456 2234 300 1 robust
  guard; for v in wip m417; do echo "$v gldepthcomp-1920x1080 $(gl $v $T/gldepthcomp 1920 1080 | grep -v renderer | md5sum | cut -c1-8)"; done > $R/417-correctness.txt
fi

if section 418; then
  ab 418-depth.txt gldepthbench "wip m418 m418+ASAHI_G17P_GL_DEPTH_COMPRESS=0" -- $BIN/gldepthbench 3456 2234 200
  ab 418-sodium.txt sodium-terrain "wip m418" -- $T/sodiumpace $T/sodium.vert $T/sodium.frag 4096 256 600 3456 2234 real
  ab 418-sodium.txt sodium-game "wip+SB_GAME=1 m418+SB_GAME=1" -- $T/sodiumpace $T/sodium.vert $T/sodium.frag 4096 256 600 3456 2234 real
  guard
  {
    for sz in "17 33" "1920 1080" "3456 2234" "2048 2048"; do
      a=$(gl wip env ASAHI_MESA_DEBUG=1queue,resource $T/gldepthcomp $sz 2>/tmp/b17-w | grep -v renderer | md5sum | cut -c1-8)
      b=$(gl m418 env ASAHI_MESA_DEBUG=1queue,nocompress $T/gldepthcomp $sz | grep -v renderer | md5sum | cut -c1-8)
      c=$(gl m418 env ASAHI_MESA_DEBUG=1queue,resource $T/gldepthcomp $sz 2>/tmp/b17-r | grep -v renderer | md5sum | cut -c1-8)
      echo "gldepthcomp $sz: wip $a (z32 compressed $(grep -cE 'New: z32_float .* GPU COMP' /tmp/b17-w)) m418-nocompress $b m418 $c (z32 compressed $(grep -cE 'New: z32_float .* GPU COMP' /tmp/b17-r))"
    done
    for v in wip m418; do
      gl $v env ASAHI_MESA_DEBUG=1queue,resource $BIN/gldepthbench 3456 2234 2 >/dev/null 2>/tmp/b17-r
      echo "gldepthbench resource log $v: $(grep -E 'New: z32_float' /tmp/b17-r | sed -E 's/.*New: (z32_float [0-9x]+).*(GPU COMP|GPU|LINEAR).*/\1 \2/' | sort | uniq -c | tr '\n' ';')"
    done
  } > $R/418-correctness.txt 2>&1
fi

if section 2x; then
  cd $B/vkmsaa2x; guard
  {
    for f in rgba8 rgba16f; do
      vk mc2x env ASAHI_MESA_DEBUG=1queue,nocompress ./vkmsaa2x 3456 2234 1 $f 24 4 probe > /tmp/b17-p0 2>&1
      pc=$(awk '/^image ms-colour/ {sub("size=","",$5); print $5}' /tmp/b17-p0); pz=$(awk '/^image ms-depth/ {sub("size=","",$5); print $5}' /tmp/b17-p0)
      echo "## $f mc2x nocompress"; grep -E '^(image|probe)' /tmp/b17-p0
      for v in m420 mc2x; do
        echo "## $f $v (plain sizes $pc / $pz)"
        vk $v env ASAHI_MESA_DEBUG=1queue,perf VKM_PLAIN_C=$pc VKM_PLAIN_Z=$pz ./vkmsaa2x 3456 2234 1 $f 24 4 probe 2>&1 | grep -E '^(image|probe)|creating|compression' | sed -E 's/^.*(G17P compression enabled|No compression)/  driver: \1/'
      done
    done
  } > $R/2x-probe.txt 2>&1
  ab vk-2x.txt rgba8-d24 "m420 mc2x" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  ab vk-2x.txt rgba16f-d24 "m420 mc2x" -- ./vkmsaa2x 3456 2234 150 rgba16f 24 4
  ab vk-2x.txt rgba8-d200 "m420 mc2x" -- ./vkmsaa2x 3456 2234 100 rgba8 200 4
  N=5 ab vk-2x.txt rgba8-d24-nocompress "mc2x+ASAHI_MESA_DEBUG=1queue,nocompress" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
  cd /
fi

if section t41; then
  for v in m420 mc2x; do
    guard; log "t41 $v"
    sed -e "s|O=/root/gpu/t41;|O=$B/t41-$v;|" -e "s|export VK_DRIVER_FILES=\$O/icd.json T41_LOG=1|export VK_DRIVER_FILES=$B/icd-$v.json|" /root/gpu/t41test.sh > $B/t41test-$v.sh
    rm -rf $B/t41-$v; timeout 900 bash $B/t41test-$v.sh A B > /dev/null 2>&1
    S=$B/t41-$v/summary.txt; cp $S $R/t41-$v.txt
    echo "t41 $v: runs $(grep -c 'rc=' $S) nonzero $(grep 'rc=' $S | grep -vc 'rc=0 ') aborts $(grep -c ABORT $S): $(grep 'rc=' $S | grep -v 'rc=0 ' | awk '{print $1":"$2}' | tr '\n' ' ')" | tee -a $R/t41-summary.txt
  done
fi

guard
info > $R/info-end.txt 2>&1
log "DONE, GPU trouble lines new: $(( $(bad) - B0 ))"
