#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

#include "vkdispatch.hpp"

namespace vksumi
{
    struct DeviceState
    {
        VkDevice          device         = VK_NULL_HANDLE;
        VkPhysicalDevice  physicalDevice = VK_NULL_HANDLE;
        VkQueue           graphicsQueue  = VK_NULL_HANDLE;
        uint32_t          graphicsFamily = UINT32_MAX;
        DeviceDispatch    vkd{};
        InstanceDispatch  vki{};
    };

    class SwapchainState
    {
    public:
        static std::shared_ptr<SwapchainState> create(const DeviceState&              dev,
                                                      VkSwapchainKHR                  realSwapchain,
                                                      const VkSwapchainCreateInfoKHR& createInfo);

        ~SwapchainState();

        SwapchainState(const SwapchainState&)            = delete;
        SwapchainState& operator=(const SwapchainState&) = delete;

        VkResult getImages(uint32_t* pCount, VkImage* pImages);
        VkSemaphore submitBlit(VkQueue                     submitQueue,
                               uint32_t                    imageIndex,
                               const VkSemaphore*          waitSemaphores,
                               uint32_t                    waitCount);

        VkSwapchainKHR realSwapchain() const { return m_realSwapchain; }

    private:
        SwapchainState(const DeviceState& dev, VkSwapchainKHR sc, VkExtent2D extent, VkFormat fmt);

        bool initImages(const VkSwapchainCreateInfoKHR& createInfo);
        bool initParamBuffers();
        bool initSampler();
        bool initRenderPass();
        bool initDescriptorLayout();
        bool initPipeline();
        bool initFramebuffersAndDescriptors();
        bool initCommandResources();
        bool initSemaphores();
        bool initFences();

        void recordCommandBuffer(uint32_t imageIndex);

        DeviceState                  m_dev;
        VkSwapchainKHR               m_realSwapchain = VK_NULL_HANDLE;
        VkExtent2D                   m_extent{};
        VkFormat                     m_format = VK_FORMAT_UNDEFINED;

        std::vector<VkImage>         m_realImages;
        std::vector<VkImageView>     m_realViews;
        std::vector<VkFramebuffer>   m_framebuffers;

        std::vector<VkImage>         m_fakeImages;
        std::vector<VkDeviceMemory>  m_fakeMemory;
        std::vector<VkImageView>     m_fakeViews;

        std::vector<VkBuffer>        m_paramBuffers;
        std::vector<VkDeviceMemory>  m_paramMemory;
        std::vector<void*>           m_paramMapped;

        VkSampler                    m_sampler        = VK_NULL_HANDLE;
        VkRenderPass                 m_renderPass     = VK_NULL_HANDLE;
        VkDescriptorSetLayout        m_dsLayout       = VK_NULL_HANDLE;
        VkDescriptorPool             m_dsPool         = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_dsets;
        VkPipelineLayout             m_pipeLayout     = VK_NULL_HANDLE;
        VkShaderModule               m_vert           = VK_NULL_HANDLE;
        VkShaderModule               m_frag           = VK_NULL_HANDLE;
        VkPipeline                   m_pipeline       = VK_NULL_HANDLE;

        VkCommandPool                m_cmdPool        = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_cmdBuffers;
        std::vector<VkSemaphore>     m_renderDone;
        std::vector<VkFence>         m_inFlight;

        std::vector<bool>            m_realFirstUse;
        uint32_t                     m_presentCount = 0;
    };

    void                            registerSwapchain(VkSwapchainKHR sc, std::shared_ptr<SwapchainState> state);
    std::shared_ptr<SwapchainState> findSwapchain(VkSwapchainKHR sc);
    void                            unregisterSwapchain(VkSwapchainKHR sc);

    void  registerDevice(VkDevice device, DeviceState state);
    DeviceState* findDevice(VkDevice device);
    DeviceState* findDeviceByQueue(VkQueue queue);
    void  unregisterDevice(VkDevice device);
}
