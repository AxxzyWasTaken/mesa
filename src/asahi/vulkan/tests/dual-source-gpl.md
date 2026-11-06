The CTS `pipeline_library.blend.dual_source.multi_attachments` group uses two
different pipelines. In the public Vulkan CTS 1.4.6.2 source:

* The generic render pass has four colour slots, with slot 0 unused. Its
  fragment shader exports Locations 0 through 3 and its blend equations
  replace every SRC1 factor with the corresponding SRC factor.
* The dual-source render pass has one colour slot. Its fragment shader
  exports Location 0 with Index 0 and Index 1. Its blend-state attachment
  count is one. Both framebuffers contain four image views.

Supported cases should pass. Four framebuffer attachments do not make the
dual-source draw exceed `maxFragmentDualSrcAttachments`. Unsupported formats
or features can cause ordinary CTS support skips.

`hk-dual-source-gpl` compiles the CTS shaders and fixed-function state above,
using each of the 41 formats and four representative secondary-source factors
(two for formats without alpha). It checks the actual fragment link key
against the Vulkan blend state and compares linked executable bytes, register
counts and fragment controls across monolithic, four-library fast-link and
four-library LTO creation. Each command buffer switches generic/dual twice.
Library objects are destroyed after linking, before recording. It also checks
that the generic attachment hole and the dual-source attachment count survive
linking and rebinding.

This reproduces the pipeline-state shapes, shaders, vertex input and push
constants; it does not reproduce the CTS's complete seed-dependent blend
generator, transfer/readback sequence or image-result verification. It never
submits a queue. The no-op shim cannot reproduce GPU execution, firmware
crashes or concurrent kernel resource lifetimes.

Configure a native Linux build with `-Dvulkan-drivers=asahi -Dbuild-tests=true
-Dtools=drm-shim` and a Vulkan loader and `glslangValidator` installed. Build
normally, then run:

```
meson test -C build --no-rebuild hk_dual_source_gpl --print-errorlogs
```

The Meson runner explicitly selects the built no-op shim and the built Asahi
ICD in a temporary directory. It uses the G13G software identity. Other chip
identities can be tested manually with the same executable and shader modules.
G17P's implementation is unchanged.

`DualSourceEpilog.CTSGenericAndDualState` in `libasahi_tests` checks the AGX2
epilog's numerical result independently of pipeline creation. It substitutes
distinct primary, secondary and destination values into the lowered NIR and
compares 40 generic/dual, blend-factor and blend-operation combinations with
a CPU calculation. It verifies that dual-source exports produce only RT0
stores and that the generic pass preserves its RT0 hole.

The public source is
[vktPipelineDualBlendTests.cpp at f6a29701220f34dd1407513bfe80d74ca7b392ce](https://github.com/KhronosGroup/VK-GL-CTS/blob/f6a29701220f34dd1407513bfe80d74ca7b392ce/external/vulkancts/modules/vulkan/pipeline/vktPipelineDualBlendTests.cpp).
The three GLSL files retain Khronos's Apache-2.0 copyright and license;
`licenses/Apache-2.0` contains that license. The recording test and numerical
oracle are MIT-licensed.
