#pragma once

struct PushConstants {
  uint ResourceDescriptorIndex;
  uint ResourceDescriptorIndex2;
  float Param0;
  float Param1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants
    : register(b3, space4);
