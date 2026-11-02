# Qualification status

This candidate includes the prerequisites in [Mesa PR 116](https://github.com/aurora-silicon/mesa/pull/116)
and the NIR initializer fix `99f3223c30c`. The default selector has not been
changed on a target. Current Neo numerical results, full suite qualification,
compositor/greeter checks and GL performance measurements remain pending.

## Remaining thirteen rendering cases

The same Zink and CTS inputs pass all thirteen cases with the CPU Vulkan
reference. This supports the existing HK attribution and provides no evidence
for a CTS-bug waiver. The no-op DRM shim validates compilation only; its pixels
cannot establish an HK rendering result. Hardware must recheck the candidate
before these cases can be closed.

| Case | CPU Vulkan reference | Current Neo | Native GL relevance |
| --- | --- | --- | --- |
| `dEQP-GL45-ES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_1` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GL45-ES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_2` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GL45-ES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_4` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GL45-ES31.functional.shaders.sample_variables.sample_mask.discard_half_per_two_samples.multisample_rbo_4` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GLES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_1` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GLES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_2` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GLES31.functional.shaders.sample_variables.sample_mask.discard_half_per_sample.multisample_rbo_4` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `dEQP-GLES31.functional.shaders.sample_variables.sample_mask.discard_half_per_two_samples.multisample_rbo_4` | Pass | Pending | Native historically passed; sample coverage needs separate HK verification |
| `KHR-GL46.sparse_buffer_tests.BufferStorageTest_case_j` | Pass | Pending | Native does not advertise sparse buffers; HK buffer reads remain open |
| `KHR-GL46.sparse_buffer_tests.BufferStorageTest_cases_a1-a2_1` | Pass | Pending | Native does not advertise sparse buffers; HK buffer reads remain open |
| `KHR-GL46.sparse_buffer_tests.BufferStorageTest_cases_a3-a4_2` | Pass | Pending | Native does not advertise sparse buffers; HK buffer reads remain open |
| `KHR-GL46.sparse_buffer_tests.BufferStorageTest_cases_a3-a4_3` | Pass | Pending | Native does not advertise sparse buffers; HK buffer reads remain open |
| `KHR-GL46.sparse_buffer_tests.BufferStorageTest_cases_a5-a6_4` | Pass | Pending | Native does not advertise sparse buffers; HK buffer reads remain open |

The sparse family covers vertex-buffer draws, indexed/ranged draws and colour
attributes, plus uniform-buffer reads from a vertex shader (`case_j`). A
resident-read failure is an implementation issue to minimize; decommitted-page
no-crash checks must not be replaced with invented pixel expectations. The
sample-mask family checks one-, two- and four-sample renderbuffers and the
specified half-coverage masks. No performance-costing tolerance was added.

## Software FP64 compilation

Zink delegates ordinary loop unrolling to Vulkan (`max_unroll_iterations=0`)
and separately requests early software-FP64 unrolling. Constant local double
array initialization contains stores and integer loop control, so it missed
that FP64 classification. The indirect initializer prevented constant
propagation before double operations expanded into emulation functions.

Loop analysis now recognizes constant stores to function-local double arrays
indexed by the loop induction variable. It uses the existing FP64 iteration
budget, preserving the trip-count bound. Hardware doubles, nonconstant data,
integer/float arrays and fixed array indices retain their previous policy.
Arithmetic precision and exposed capabilities are unchanged.

The following x86_64 host measurements run the same CTS binary/source
`f6a29701220f34dd1407513bfe80d74ca7b392ce`, with Mesa's shader cache disabled.
The baseline is PR 116's unchanged driver; the candidate column is the median
of three runs. The DRM shim ends the test at its first numerical check, so
these numbers measure that compilation prefix, not the whole CTS case or a
Neo frame. They include process/context startup, excluding harness hashing.

| `KHR-Single-GL46.arrays_of_arrays_gl.` suffix | Baseline Zink seconds | Candidate Zink seconds | Candidate native seconds |
| --- | ---: | ---: | ---: |
| `SubroutineFunctionCalls2_var_type_index_12` (dmat2) | 46.316 | 0.202 | 0.112 |
| `SubroutineFunctionCalls2_var_type_index_13` (dmat3) | 115.608 | 0.189 | 0.208 |
| `SubroutineFunctionCalls2_var_type_index_14` (dmat4) | timeout at 180 | 0.157 | 0.140 |

All **three complete cases pass numerically** through Zink over CPU Vulkan
with its supported `shaderFloat64` feature masked in a local fixture, forcing
Zink's full software-FP64 lowering. All 78 captured SPIR-V modules validate,
and none declares native Float64 capability. The fixture is excluded from
public commits and overlays. Full Neo numerical/performance checks remain
pending. Native Asahi advertises software doubles, so unreachable FP64 was
never used as a waiver.

Validation: **124/124** Asahi/poly/compiler host test entries pass, including
G13 controls and the new initializer checks. The initializer regression fails
before the fix; controls cover hardware doubles, nonconstant initialization,
integer/float arrays, fixed indices, and loops above the configured budget.
These are host/compiler results and do not establish reference-Mac GPU parity.

## Index alignment and greeter

Today's `agx_draw_vbo` path realigns byte/short element-buffer ranges before
acquiring a batch. `PIPE_MAP_READ` synchronizes a GPU writer before uploading
to a 64-byte-aligned buffer. The direct index-pointer helper also retains its
alignment handling. Source inspection confirms the fix is present.

The kit's fourteen-draw numerical probe passes on CPU Vulkan with every draw
covering 4096/4096 pixels. It exercises U8 offsets 0/1/2/3, U16 offsets 0/2 and
U32 offset 0 after both CPU upload and a GPU buffer-copy command. A separate
native Neo run, fresh user compositor and actual GDM avatar capture are still
required. See the kit's XR30 decoding notes and rollback instructions.

## Packaging and rollout

The overlay packager includes the GBM backend, checks source identity and ELF
architecture, rejects LLVM/SPIRV-Tools driver runtime dependencies, records
hashes, and carries Mesa license notices. AArch64 artifact identity and CI
links are recorded in the PR description and lead report.

The matching distro drafts are [Fedora 75](https://github.com/aurora-silicon/fedora-aurora/pull/75),
[ALARM 59](https://github.com/aurora-silicon/alarm-aurora/pull/59), and
[NixOS 62](https://github.com/aurora-silicon/nixos-aurora/pull/62). Current public
packaging has no global Zink assignment to delete. The drafts guard finished
factory images against global GL selectors and document qualification and
owner-controlled migration of existing configuration. They keep source pins
unchanged pending qualification.

There are no measured Neo glmark2-es2 scores in this run. The kit specifies
seven paired repetitions, cache handling, CPU times and renderer identity.
The public gpubench at `db3fec6d9107aaff99868e77b2811666d4a9f9d6` has Vulkan
and Metal executors with no GL executor. The requested gpubench GL comparison
remains unavailable; Vulkan scores cannot fill that metric.
