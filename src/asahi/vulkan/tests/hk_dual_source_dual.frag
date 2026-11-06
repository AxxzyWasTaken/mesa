/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2024 The Khronos Group Inc.
 * From VK-GL-CTS vulkan-cts-1.4.6.2, vktPipelineDualBlendTests.cpp.
 */
#version 450
    layout(push_constant) uniform PC
    {
        vec4 inColor0, inColor1, inColor2, inColor3;
    };
    layout(location = 0, index = 0) out vec4 outColor0;
    layout(location = 0, index = 1) out vec4 outColor1;
    void main() {
        outColor0 = inColor0;
        outColor1 = inColor1;
    }
