#!/bin/bash
# run-shadow.sh: hardware A/B runs for "asahi: count only views the draw's shaders read as feedback loops".
# Run on the Max as root: bash /root/gpu/fbl/run-shadow.sh [section...]   (sections: info check log bench repeat)
# Off-screen only (EGL surfaceless). Test drivers come from /root/gpu/fbl/lib-<variant> (LD_LIBRARY_PATH);
# nothing installed is touched. One runner at a time (flock). Before every run: no game may be running (else wait,
# rechecking every 5 min, 45 min in all, then stop), and no new GPU recovery/fault line may have appeared in the
# kernel log since the start (else stop at once).
# Variants (built in one build dir with the same options, see build.sh):
#   wip  = aurora-wip 57df4db8                      fix     = 2e2647c4 (this change on 57df4db8)
#   m418 = refs/pull/418/merge 1752a4d9 (+ ef69f599) m418fix = local merge c719e0d8 of 2e2647c4 into 1752a4d9
set -uo pipefail
B=/root/gpu/fbl; R=$B/raw; T=/root/gpu/tests; BIN=$B/bin; mkdir -p $R
exec 9>$B/.runner.lock; flock -n 9 || { echo "another run-shadow.sh holds $B/.runner.lock" >&2; exit 5; }
N=${N:-7}
WAIT_MAX=${WAIT_MAX:-2700}; WAITED=0
GAME_RE='SCPSL|Roblox|cordial|sober|java.*(minecraft|prism)|steam_app|\.exe|AppId='
BAD_RE='gpu recovery|gpu hang|GPU work of a client failed|(asahi|aurora|agx|g17).*(fault|timed out|stall|hang|recover)'
bad() { dmesg | grep -ciE "$BAD_RE"; }
B0=$(bad)
log() { echo "$(date +%T) $*" | tee -a $R/run.log; }
guard() {
  while pgrep -f "$GAME_RE" >/dev/null; do
    [ "$WAITED" -lt "$WAIT_MAX" ] || { log "STOP: a game is still running after $WAITED s: $(pgrep -fa "$GAME_RE" | grep -oE '[^/ \\]+\.exe|SCPSL|Roblox|sober|java|AppId=[0-9]+' | sort -u | tr '\n' ' ')"; exit 3; }
    log "PAUSE: game running, recheck in 300 s"; sleep 300; WAITED=$((WAITED + 300))
  done
  [ "$(bad)" = "$B0" ] || { log "STOP: new GPU trouble in dmesg"; dmesg | grep -iE "$BAD_RE" | tail -5 | tee -a $R/run.log; exit 4; }
}
set -a; . /opt/aurora-mesa/session-env; set +a          # the desktop's GL environment (ASAHI_MESA_DEBUG=1queue, MESA_GLTHREAD=false)
export MESA_SHADER_CACHE_DISABLE=true
gl() { local v=$1; shift; guard; LD_LIBRARY_PATH=$B/lib-$v timeout -k 5 180 "$@"; }
# ab <file> <label> <variants> -- cmd...: N rounds, variants in turn (A B A B ...), every stdout line tagged.
# A variant may carry extra environment after a '+', e.g. wip+SB_GAME=1.
ab() {
  local f=$R/$1 lab=$2 vs=$3; shift 4
  local i v vv out rc extra
  for i in $(seq 1 $N); do for v in $vs; do
    vv=${v%%+*}; extra=(); [[ $v == *+* ]] && extra=(${v#*+})
    guard
    out=$(env "${extra[@]}" LD_LIBRARY_PATH=$B/lib-$vv timeout -k 5 180 "$@" 2>/dev/null); rc=$?
    sed "s/^/$lab v=$v i=$i rc=$rc | /" <<<"$out" >> $f
  done; done
  log "$lab: $(grep -c "^$lab v=.* rc=0 " $f) lines with rc=0"
}
section() { [[ " ${SECTIONS} " == *" $1 "* ]]; }
SECTIONS="${*:-info check log bench}"
FBL='Decompressing resource due to Texture feedback loop'
# steps <stderr file>: feedback-loop decompressions per glfeedback step, e.g. "loop-colour=1 loop-depth=0 ..."
steps() { awk -v pat="$FBL" '/^STEP / {sub(/^STEP /, ""); s = $0; order[++n] = s; c[s] = 0; next} index($0, pat) {c[s]++}
  END {for (i = 1; i <= n; i++) if (order[i] != "end") printf "%s=%d ", order[i], c[order[i]]; print ""}' "$1"; }
# images <stderr file>: the images a run created, compressed or not, e.g. "2x z32_float 2048x2048 COMP; 1x z32_float 2048x2048 plain;"
images() { awk '/New: / {for (i = 1; i < NF; i++) if ($i == "New:") break; d = $(i + 2); sub(/x[0-9]+$/, "", d)
  k = $(i + 1) " " d " " (index($0, " COMP ") ? "COMP" : "plain"); if (!(k in n)) o[++m] = k; n[k]++}
  END {for (j = 1; j <= m; j++) printf "%dx %s; ", n[o[j]], o[j]; print ""}' "$1"; }
# z32order <stderr file>: Z32F images created and feedback-loop decompressions, in order.
z32order() { awk -v pat="$FBL" '/New: z32_float/ {for (i = 1; i < NF; i++) if ($i == "New:") break; d = $(i + 2); sub(/x[0-9]+$/, "", d)
  printf "z32_float %s %s; ", d, (index($0, " COMP ") ? "COMP" : "plain")} index($0, pat) {printf "[feedback-loop decompression]; "} END {print ""}' "$1"; }

info() {
  echo "date $(date -Is)"; uname -r; cat /proc/device-tree/model 2>/dev/null | tr -d '\0'; echo
  echo "AC online $(cat /sys/class/power_supply/macsmc-ac/online), battery $(cat /sys/class/power_supply/macsmc-battery/status) $(cat /sys/class/power_supply/macsmc-battery/capacity)%"
  aurora-fan status 2>&1 | head -2; uptime
  bash $B/gpubusy.sh 2
  for v in wip fix m418 m418fix; do
    echo "$v GL: $(LD_LIBRARY_PATH=$B/lib-$v timeout 60 $BIN/glfeedback check 16 16 2>/dev/null | grep '^driver')"
  done
}

if section info; then guard; log "info"; info > $R/info-start.txt 2>&1; fi

if section check; then
  log "check"
  {
    # glfeedback check: real loops and stale views; same images everywhere, decompressions per step per build.
    for sz in "256 256" "1920 1080" "3456 2234"; do
      for v in wip fix m418 m418fix m418fix+nocompress; do
        vv=${v%%+*}; dbg=1queue,resource,perf; [[ $v == *+nocompress ]] && dbg=1queue,nocompress,perf
        out=$(gl $vv env ASAHI_MESA_DEBUG=$dbg $BIN/glfeedback check $sz 2>/tmp/fbl-err); rc=$?
        echo "glfeedback check $sz $v rc=$rc md5=$(grep -v '^driver' <<<"$out" | md5sum | cut -c1-8) | decompressions: $(steps /tmp/fbl-err)"
        [ "$sz" != "256 256" ] || grep -v '^driver' <<<"$out" | sed "s/^/    $v: /"
      done
    done
    # gldepthcomp (bench-17nov): depth through every path; the last step (scissored clear + draws) runs after a
    # sampler-less program is bound while the sampling pass's views of the depth image are still bound.
    for sz in "17 33" "1920 1080" "3456 2234" "2048 2048"; do
      line="gldepthcomp $sz:"
      for v in wip fix m418 m418fix m418fix+nocompress; do
        vv=${v%%+*}; dbg=1queue,resource,perf; [[ $v == *+nocompress ]] && dbg=1queue,nocompress,perf
        h=$(gl $vv env ASAHI_MESA_DEBUG=$dbg $T/gldepthcomp $sz 2>/tmp/fbl-err | grep -v renderer | md5sum | cut -c1-8)
        line="$line $v $h (z32 compressed $(grep -cE 'New: z32_float .* COMP' /tmp/fbl-err), loop decompressions $(grep -c "$FBL" /tmp/fbl-err))"
      done
      echo "$line"
    done
  } > $R/check.txt 2>&1
  log "check: $(grep -c 'rc=0' $R/check.txt) glfeedback runs rc=0"
fi

if section log; then
  log "log"
  {
    # Which images each workload creates compressed, and how many the feedback-loop rule decompresses.
    run_log() { local lab=$1; shift; for v in wip fix m418 m418fix; do
      gl $v env ASAHI_MESA_DEBUG=1queue,resource,perf "$@" >/dev/null 2>/tmp/fbl-err
      echo "$lab $v: loop decompressions $(grep -c "$FBL" /tmp/fbl-err); images: $(images /tmp/fbl-err)"
    done; }
    run_log "gldepthbench default (50 frames)" $BIN/gldepthbench 3456 2234 50
    run_log "gldepthbench composite (50 frames)" $BIN/gldepthbench 3456 2234 50 2048 3000 composite
    run_log "gldepthbench shadow 4096 (50 frames)" $BIN/gldepthbench 3456 2234 50 4096 3000
    run_log "glfeedback bench-fs (50 frames)" $BIN/glfeedback bench-fs 3456 2234 50
    run_log "glfeedback bench-cs (50 frames)" $BIN/glfeedback bench-cs 3456 2234 50
    run_log "sodiumpace SB_GAME=1 (60 frames)" env SB_GAME=1 $T/sodiumpace $T/sodium.vert $T/sodium.frag 4096 256 60 3456 2234 real
    # When the shadow map loses compression: gldepthbench draws 10 warm-up frames, then the timed ones.
    for v in m418 m418fix; do
      gl $v env ASAHI_MESA_DEBUG=1queue,resource,perf $BIN/gldepthbench 3456 2234 3 >/dev/null 2>/tmp/fbl-err
      echo "gldepthbench default (3 + 10 warm-up frames) $v, in order: $(z32order /tmp/fbl-err)"
    done
  } > $R/log.txt 2>&1
  log "log done"
fi

if section bench; then
  log "bench"
  ab bench-depth.txt gldepthbench "wip fix m418 m418fix" -- $BIN/gldepthbench 3456 2234 200
  ab bench-depth.txt gldepthbench-composite "wip fix m418 m418fix" -- $BIN/gldepthbench 3456 2234 200 2048 3000 composite
  ab bench-depth.txt gldepthbench-shadow4096 "m418 m418fix" -- $BIN/gldepthbench 3456 2234 200 4096 3000
  ab bench-sodium.txt sodium-game "wip+SB_GAME=1 fix+SB_GAME=1 m418+SB_GAME=1 m418fix+SB_GAME=1" -- $T/sodiumpace $T/sodium.vert $T/sodium.frag 4096 256 600 3456 2234 real
  ab bench-feedback.txt glfeedback-fs "wip fix" -- $BIN/glfeedback bench-fs 3456 2234 200
  ab bench-feedback.txt glfeedback-cs "wip fix" -- $BIN/glfeedback bench-cs 3456 2234 200
fi

if section repeat; then
  # gldepthcomp 2048x2048 again, every step's hash, all four builds in turn (after one odd hash in the check section).
  log "repeat"
  for i in $(seq 1 ${NREP:-10}); do for v in wip fix m418 m418fix; do
    out=$(gl $v $T/gldepthcomp 2048 2048 2>/dev/null | grep -v renderer); rc=$?
    echo "gldepthcomp 2048 2048 $v i=$i rc=$rc md5=$(md5sum <<<"$out" | cut -c1-8) steps: $(awk '{print $NF}' <<<"$out" | tr '\n' ' ')"
  done; done > $R/repeat.txt 2>&1
  log "repeat: $(grep -oE 'md5=[0-9a-f]+' $R/repeat.txt | sort | uniq -c | tr -s ' ' | tr '\n' ' ')"
fi

guard
info > $R/info-end.txt 2>&1
log "DONE, GPU trouble lines new: $(( $(bad) - B0 ))"
