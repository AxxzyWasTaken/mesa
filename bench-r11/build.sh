#!/bin/bash
# build.sh: clc tools from aurora-wip 57df4db8, then one test-enabled release build dir used for both drivers:
# base = aurora-wip 57df4db8, fix = j616c/r11-2x-resolve. Copies each libvulkan_asahi.so + the hardware test
# programs to /root/gpu/r11/{base,fix}, runs meson test --suite asahi for each. Nothing installed.
cd /root/gpu; renice -n 19 -p $$ >/dev/null; ionice -c 3 -p $$
SRC=/root/gpu/mesa-r11; CLC=/root/gpu/clc-r11; CB=/root/gpu/clc-r11-build; B=/root/gpu/build-r11; R=/root/gpu/r11
BASE=57df4db849aac8f3a2815bb0875ed65868b01246; FIX=$(git -C $SRC rev-parse j616c/r11-2x-resolve)
space() { local a=$(df --output=avail -BG / | tail -1 | tr -dc 0-9); echo "df: ${a}G free before $1"; [ "$a" -ge 8 ] || { echo "STOP: under 8G free"; exit 2; }; }
echo "base $BASE fix $FIX $(date +%T)"
git -C $SRC checkout -q --detach $BASE || exit 1
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
for v in base fix; do
  [ $v = base ] && C=$BASE || C=$FIX
  git -C $SRC checkout -q --detach $C || exit 1
  echo "== $v $(git -C $SRC log --oneline -1) $(date +%T)"
  PATH=$CLC/bin:$PATH ninja -j6 -C $B > $R/ninja-$v.log 2>&1 || { grep -m5 -B2 -A8 error $R/ninja-$v.log; echo "BUILD-FAILED $v"; exit 1; }
  mkdir -p $R/$v; cp $B/src/asahi/vulkan/libvulkan_asahi.so $R/$v/
  printf '{"ICD":{"api_version":"1.4.363","library_arch":"64","library_path":"%s/libvulkan_asahi.so"},"file_format_version":"1.0.1"}\n' $R/$v > $R/$v/icd.json
  if [ $v = base ]; then mkdir -p $R/bin; for t in aurora-r11g11b10f aurora-tile-visit vulkan-msaa-mrt vulkan-msaa-xfer; do cp $B/src/asahi/vulkan/$t $R/bin/; done; fi
  echo "$v libvulkan_asahi.so $(sha256sum $R/$v/libvulkan_asahi.so | cut -c1-16)"
  ( cd $B && timeout 2400 meson test --num-processes 6 --suite asahi > $R/unit-$v.log 2>&1 )
  grep -E "^Ok:|^Fail:|^Expected Fail|^Unexpected Pass|^Skipped:|^Timeout:" $R/unit-$v.log
  grep -E " FAIL | TIMEOUT " $R/unit-$v.log | head
done
git -C $SRC checkout -q j616c/r11-2x-resolve
echo "ALL-DONE $(date +%T)"
