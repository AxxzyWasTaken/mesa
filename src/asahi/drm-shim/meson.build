# Copyright 2019 Broadcom
# SPDX-License-Identifier: MIT

libasahi_noop_drm_shim = shared_library(
  'asahi_noop_drm_shim',
  ['asahi_noop.c', 'asahi_g15s_stream_gate.c',
   'asahi_g17p_stream_validator.c'],
  objects : libasahi_decode.extract_objects(
    'agx3_cdm_decode_core.c',
    'agx_ioto_core.c',
    'agx_ppp_hal200_graphics_core.c',
    'agx_ppp_viewport_payload_core.c',
    'agx_ppp_w_clamp_cull2_core.c',
    'agx_vdm_barrier_core.c',
    'agx_vdm_decode_core.c',
    'agx_vdm_hal200_bind_tail_core.c',
    'agx_vdm_index_list_core.c',
    'agx_vdm_mesh_core.c',
    'agx_vdm_ppp_state_core.c',
    'agx_vdm_state_core.c',
    'agx_vdm_state_update_core.c',
    'agx_vdm_stream_link_core.c',
    'agx_vdm_stream_terminate_core.c',
    'agx_vdm_tessellate_core.c',
  ),
  include_directories: [inc_include, inc_src, inc_asahi],
  dependencies: [dep_drm_shim, dep_valgrind],
  link_with : [libagx3b_disasm],
  gnu_symbol_visibility : 'hidden',
  install : true,
)

if with_tests
  asahi_vm_limits_test = executable(
    'asahi-vm-limits', 'test-vm-limits.c',
    include_directories : [inc_include],
    dependencies : [dep_libdrm],
  )
  foreach hal : ['legacy', 'hal200']
    test(
      'asahi-vm-limits-' + hal, prog_python,
      args : [files('../../gallium/drivers/asahi/tests/run-shim.py'),
              libasahi_noop_drm_shim, asahi_vm_limits_test],
      env : ['ASAHI_SHIM_CHIP=g15g',
             'ASAHI_SHIM_G15G_HAL200=' + (hal == 'hal200' ? '1' : '0')],
      suite : ['asahi'],
    )
  endforeach
endif
