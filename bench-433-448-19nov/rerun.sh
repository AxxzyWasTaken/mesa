#!/bin/bash
# rerun.sh: the rgba8-d24 configuration again (7 + 7), after its round in run.sh showed a wide spread on c2x.
. <(sed -n '/^set -uo/,/^section()/p' /root/gpu/p19/run.sh)
cd $B/vkmsaa2x; log "rerun rgba8-d24 (uptime: $(uptime))"
ab vk-2x.txt rgba8-d24-rerun "base c2x" -- ./vkmsaa2x 3456 2234 200 rgba8 24 4
log "RERUN-DONE, GPU trouble lines new: $(( $(bad) - B0 ))"
