#!/bin/bash
# build.sh: build aurora-wip 57df4db8 and the four test trees in ONE build dir (same options), copying each
# variant's driver libraries out. Run on the Max as root. Incremental between variants: only the change differs.
set -euo pipefail
renice -n 19 -p $$ >/dev/null; ionice -c 3 -p $$
SRC=/root/gpu/mesa-b17 B=/root/gpu/build-b17 OUT=/root/gpu/b17
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
for v in ${VARIANTS:-wip m417 m418 m420 mc2x}; do
  git -C $SRC checkout -q --detach refs/b17/$v
  echo "== $v $(git -C $SRC rev-parse --short HEAD) tree $(git -C $SRC rev-parse --short HEAD^{tree}) $(date +%T)"
  nice -n 19 ninja -C $B > $OUT/ninja-$v.log 2>&1 || { echo "BUILD FAILED $v"; tail -30 $OUT/ninja-$v.log; exit 1; }
  L=$OUT/lib-$v; rm -rf $L; mkdir -p $L
  cp -a /opt/aurora-mesa/lib/. $L/            # same set as the installed driver, then the build's own libraries on top
  cp $B/src/gallium/targets/dri/libgallium-*.so $L/
  cp -P $B/src/egl/libEGL_mesa.so* $L/ 2>/dev/null || true
  cp -P $B/src/gbm/libgbm.so* $L/ 2>/dev/null || true
  cp $B/src/asahi/vulkan/libvulkan_asahi.so $L/
  printf '{"ICD":{"api_version":"1.4.363","library_arch":"64","library_path":"%s/libvulkan_asahi.so"},"file_format_version":"1.0.1"}\n' $L > $OUT/icd-$v.json
  ( cd $L && sha256sum libgallium-*.so libvulkan_asahi.so libEGL_mesa.so.0.0.0 | cut -c1-16,65- ) | sed "s/^/  $v /"
done
git -C $SRC checkout -q --detach refs/b17/wip
echo "BUILD-DONE $(date +%T)"
