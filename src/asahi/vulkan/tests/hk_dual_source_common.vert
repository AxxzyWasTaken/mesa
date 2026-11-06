/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 The Khronos Group Inc.
 * From VK-GL-CTS vulkan-cts-1.4.6.2, vktPipelineDualBlendTests.cpp.
 */
#version 450
    layout(location = 0) in vec4 pos;
    void main() {
        gl_Position = vec4(pos.xy, 0, 1);
    }
