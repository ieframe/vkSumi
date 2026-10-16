#pragma once

#include <vulkan/vulkan.h>

#include "vkfuncs.hpp"

#define FORVKFUNC(func) PFN_vk##func func = nullptr;

namespace vksumi
{
    struct InstanceDispatch
    {
        VK_INSTANCE_FUNCS
    };

    struct DeviceDispatch
    {
        VK_DEVICE_FUNCS
    };

    template<typename T>
    inline void* dispatchKey(T handle)
    {
        return *reinterpret_cast<void**>(handle);
    }

    void fillInstanceDispatch(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, InstanceDispatch* table);
    void fillDeviceDispatch(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, DeviceDispatch* table);
}

#undef FORVKFUNC
