#!/bin/bash
# gpubusy.sh [seconds]: GPU busy share per DRM engine over all processes, from /proc/*/fdinfo drm-engine-* counters.
# Prints "engine pct" lines and the top clients. Read-only.
T=${1:-2}
snap() { for f in /proc/[0-9]*/fdinfo/*; do grep -sH '^drm-engine-' "$f"; done 2>/dev/null | awk -F'[:\t ]+' '{split($1,p,"/"); key=p[3]"|"$2; v[key]+=$3} END {for (k in v) print k, v[k]}'; }
a=$(snap); sleep "$T"; b=$(snap)
join <(echo "$a" | sort) <(echo "$b" | sort) | awk -v T="$T" '{split($1,k,"|"); d=($3-$2)/(T*1e9)*100; if (d>0) {eng[k[2]]+=d; cl[k[2]" "k[1]]+=d}} END {for (e in eng) printf "engine %s %.1f%%\n", e, eng[e]; for (c in cl) if (cl[c] > 0.5) printf "  client %s %.1f%%\n", c, cl[c]}' | sort
