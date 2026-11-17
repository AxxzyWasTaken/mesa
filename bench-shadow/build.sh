#!/bin/bash
# build.sh: build the four variants in ONE build dir (same options as bench-17nov/build.sh), copying each
# variant's driver libraries out, and run the asahi unit tests on each. Run on the Max as root.
# Incremental between variants: only the change differs.
#   wip     = aurora-wip 57df4db8
#   fix     = 2e2647c4 (this change on 57df4db8)
#   m418    = refs/pull/418/merge 1752a4d9 (57df4db8 + ef69f599, #418)
#   m418fix = local merge of 2e2647c4 into 1752a4d9 (#418 + this change)
set -euo pipefail
renice -n 19 -p $$ >/dev/null; ionice -c 3 -p $$
SRC=/root/gpu/mesa-fbl B=/root/gpu/build-fbl OUT=/root/gpu/fbl
export PATH=/root/gpu/clc-17/bin:$PATH
mkdir -p $OUT
if [ ! -f $B/build.ninja ]; then
  meson setup $B $SRC -Dbuildtype=release -Dprefix=/opt/aurora-mesa -Dlibdir=lib -Db_ndebug=if-release \
    -Dplatforms=wayland,x11 -Dgallium-drivers=asahi -Dvulkan-drivers=asahi -Dvulkan-layers= -Dglx=disabled \
    -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglvnd=enabled \
    -Dglvnd-vendor-name=mesa -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
    -Dvideo-codecs= -Dgallium-va=disabled -Dbuild-tests=true -Dtools= -Dmesa-clc=system \
    -Dprecomp-compiler=system -Dgallium-rusticl=false > $OUT/setup.log 2>&1
fi
for v in ${VARIANTS:-wip fix m418 m418fix}; do
  git -C $SRC checkout -q --detach refs/fbl/$v
  echo "== $v $(git -C $SRC rev-parse --short HEAD) tree $(git -C $SRC rev-parse --short HEAD^{tree}) $(date +%T)"
  nice -n 19 ninja -C $B > $OUT/ninja-$v.log 2>&1 || { echo "BUILD FAILED $v"; tail -30 $OUT/ninja-$v.log; exit 1; }
  L=$OUT/lib-$v; rm -rf $L; mkdir -p $L
  cp -a /opt/aurora-mesa/lib/. $L/            # same set as the installed driver, then the build's own libraries on top
  cp $B/src/gallium/targets/dri/libgallium-*.so $L/
  cp -P $B/src/egl/libEGL_mesa.so* $L/ 2>/dev/null || true
  cp -P $B/src/gbm/libgbm.so* $L/ 2>/dev/null || true
  ( cd $L && sha256sum libgallium-*.so libEGL_mesa.so.0.0.0 | cut -c1-16,65- ) | sed "s/^/  $v /"
  nice -n 19 meson test -C $B --suite asahi --no-rebuild --print-errorlogs > $OUT/unit-$v.log 2>&1 || true
  echo "  $v unit: $(grep -E '^(Ok|Fail|Expected Fail|Unexpected Pass|Skipped|Timeout):' $OUT/unit-$v.log | tr -s ' ' | tr '\n' ' ')"
done
git -C $SRC checkout -q --detach refs/fbl/fix
echo "BUILD-DONE $(date +%T)"
