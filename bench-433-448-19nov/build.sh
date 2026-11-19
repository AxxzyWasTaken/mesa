#!/bin/bash
# build.sh: clc tools from aurora-wip e66c01e7, then ONE test-enabled release build dir for all three drivers:
#   base = aurora-wip e66c01e7    r11 = 47273442 (#448 on e66c01e7)    c2x = 68a70dc1 (#433 on e66c01e7)
# Incremental between builds, so the drivers differ only by the commit. Copies each libvulkan_asahi.so to
# /root/gpu/p19/<build>/ with an icd.json, the in-tree hardware test programs (from base) to /root/gpu/p19/bin,
# and runs `meson test --suite asahi` per build. Run on the Max as root. Nothing installed is touched.
cd /root/gpu; renice -n 19 -p $$ >/dev/null; ionice -c 3 -p $$
SRC=/root/gpu/mesa-p19; CLC=/root/gpu/clc-p19; CB=/root/gpu/clc-p19-build; B=/root/gpu/build-p19; R=/root/gpu/p19
declare -A REF=([base]=refs/p19/wip [r11]=refs/p19/r11 [c2x]=refs/p19/c2x)
space() { local a=$(df --output=avail -BG / | tail -1 | tr -dc 0-9); echo "df: ${a}G free before $1"; [ "$a" -ge 8 ] || { echo "STOP: under 8G free"; exit 2; }; }
git -C $SRC checkout -q --detach ${REF[base]} || exit 1
echo "base $(git -C $SRC rev-parse HEAD) r11 $(git -C $SRC rev-parse ${REF[r11]}) c2x $(git -C $SRC rev-parse ${REF[c2x]}) $(date +%T)"
if [ ! -x $CLC/bin/mesa_clc ]; then
  space clc; rm -rf $CB $CLC
  meson setup $SRC $CB -Dbuildtype=release -Dprefix=$CLC -Dplatforms=wayland -Dgallium-drivers=asahi -Dvulkan-drivers= -Dglx=disabled \
    -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dllvm=enabled -Dshared-llvm=enabled -Dvideo-codecs= \
    -Dbuild-tests=false -Dtools= -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
    > $CB.setup.log 2>&1 || { tail -30 $CB.setup.log; echo "CLC-FAILED setup"; exit 1; }
  ninja -j6 -C $CB > $CB.ninja.log 2>&1 || { grep -m5 -B2 -A8 error $CB.ninja.log; echo "CLC-FAILED build"; exit 1; }
  ninja -C $CB install > $CB.install.log 2>&1 || { tail -20 $CB.install.log; echo "CLC-FAILED install"; exit 1; }
  echo "CLC-DONE $(date +%T)"
fi
space build
[ -d $B ] || PATH=$CLC/bin:$PATH meson setup $SRC $B -Dbuildtype=release -Dprefix=/opt/aurora-mesa -Dlibdir=lib -Db_ndebug=if-release \
  -Dplatforms=wayland,x11 -Dgallium-drivers=asahi -Dvulkan-drivers=asahi -Dvulkan-layers= -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles1=disabled \
  -Dgles2=enabled -Dopengl=true -Dglvnd=enabled -Dglvnd-vendor-name=mesa -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dvideo-codecs= -Dgallium-va=disabled -Dbuild-tests=true -Dtools=drm-shim -Dmesa-clc=system -Dprecomp-compiler=system -Dgallium-rusticl=false > $B.setup.log 2>&1 \
  || { tail -30 $B.setup.log; echo "BUILD-FAILED setup"; exit 1; }
for v in ${BUILDS:-base r11 c2x}; do
  git -C $SRC checkout -q --detach ${REF[$v]} || exit 1
  echo "== $v $(git -C $SRC log --oneline -1) $(date +%T)"
  PATH=$CLC/bin:$PATH nice -n 19 ninja -j6 -C $B > $R/ninja-$v.log 2>&1 || { grep -m5 -B2 -A8 error $R/ninja-$v.log; echo "BUILD-FAILED $v"; exit 1; }
  mkdir -p $R/$v; cp $B/src/asahi/vulkan/libvulkan_asahi.so $R/$v/
  printf '{"ICD":{"api_version":"1.4.363","library_arch":"64","library_path":"%s/libvulkan_asahi.so"},"file_format_version":"1.0.1"}\n' $R/$v > $R/$v/icd.json
  if [ $v = base ]; then mkdir -p $R/bin; for t in aurora-r11g11b10f aurora-tile-visit vulkan-msaa-mrt vulkan-msaa-xfer; do cp $B/src/asahi/vulkan/$t $R/bin/; done; fi
  echo "$v libvulkan_asahi.so $(sha256sum $R/$v/libvulkan_asahi.so | cut -c1-16)"
  ( cd $B && timeout 2400 nice -n 19 meson test --num-processes 6 --suite asahi > $R/unit-$v.log 2>&1 )
  grep -E "^Ok:|^Fail:|^Expected Fail|^Unexpected Pass|^Skipped:|^Timeout:" $R/unit-$v.log
  grep -E " FAIL | TIMEOUT " $R/unit-$v.log | head
  grep -h "checks passed" $B/meson-logs/testlog.txt 2>/dev/null | sort -u | sed "s/^/  $v /"
done
git -C $SRC checkout -q --detach ${REF[base]}
echo "ALL-DONE $(date +%T)"
