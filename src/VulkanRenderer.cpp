#include "VulkanRenderer.hpp"
#include "MapParser.hpp"
#include "MacFileDialog.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <set>
#include <algorithm>
#include <chrono>
#include <array>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace NFSU2 {

static std::vector<char> readFile(const std::string& filename) {
    std::vector<std::string> candidates = {
        filename,
        "shaders/" + filename,
        "build/shaders/" + filename,
        "../shaders/" + filename,
        "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2/nfsu2_vulkan/build/shaders/" + filename,
        "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2/nfsu2_vulkan/shaders/" + filename
    };

    for (const auto& path : candidates) {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (file.is_open()) {
            size_t fileSize = (size_t)file.tellg();
            if (fileSize > 0) {
                std::vector<char> buffer(fileSize);
                file.seekg(0);
                file.read(buffer.data(), fileSize);
                return buffer;
            }
        }
    }

    std::cerr << "[VulkanRenderer] Failed to open shader file in any search path: " << filename << std::endl;
    return {};
}

static std::string diagnoseUnopenableFile(const std::string& filePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
        return "File or directory does not exist.";
    }
    if (fs::is_directory(filePath, ec)) {
        return "Folder does not contain a GEOMETRY.BIN or 3D map archive.";
    }
    auto fsize = fs::file_size(filePath, ec);
    if (ec || fsize == 0) {
        return "File is empty (0 bytes) — contains no geometry or textures.";
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        return "Unable to read file from disk.";
    }

    uint32_t topId = 0;
    file.read(reinterpret_cast<char*>(&topId), 4);
    char hexBuf[64];
    std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", topId);
    return std::string("Unsupported binary data archive (top chunk ") + hexBuf + ") — contains no 3D meshes or TPK textures.";
}

VulkanRenderer::VulkanRenderer(int width, int height, const std::string& title)
    : m_width(width), m_height(height), m_title(title) {}

void VulkanRenderer::destroyGpuTexture(GpuTexture& tex) {
    if (m_device == VK_NULL_HANDLE) return;
    if (tex.descriptorSet != VK_NULL_HANDLE && m_descriptorPool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &tex.descriptorSet);
        tex.descriptorSet = VK_NULL_HANDLE;
    }
    if (tex.view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, tex.view, nullptr);
        tex.view = VK_NULL_HANDLE;
    }
    if (tex.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, tex.image, nullptr);
        tex.image = VK_NULL_HANDLE;
    }
    if (tex.memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, tex.memory, nullptr);
        tex.memory = VK_NULL_HANDLE;
    }
    tex.width = 0;
    tex.height = 0;
}

void VulkanRenderer::rebindPersistentDescriptor(GpuTexture& tex) {
    if (m_device == VK_NULL_HANDLE || tex.view == VK_NULL_HANDLE) return;

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = m_descriptorPool;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkAllocateDescriptorSets(m_device, &setAllocInfo, &tex.descriptorSet) == VK_SUCCESS) {
        VkDescriptorImageInfo imageDescriptorInfo{};
        imageDescriptorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageDescriptorInfo.imageView = tex.view;
        imageDescriptorInfo.sampler = m_textureSampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = tex.descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageDescriptorInfo;
        vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
    } else {
        tex.descriptorSet = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::updateGpuTexture(const uint8_t* pixels, uint32_t width, uint32_t height, GpuTexture& tex, const std::string& name) {
    if (!pixels || width == 0 || height == 0 || m_device == VK_NULL_HANDLE) return false;

    // Fast in-place update when dimensions match — never reallocates VkImage or VkDescriptorSet!
    if (tex.image != VK_NULL_HANDLE && tex.view != VK_NULL_HANDLE &&
        tex.descriptorSet != VK_NULL_HANDLE && tex.width == width && tex.height == height) {
        VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
        if (!createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stagingBuffer, stagingBufferMemory)) {
            return false;
        }
        void* data = nullptr;
        vkMapMemory(m_device, stagingBufferMemory, 0, imageSize, 0, &data);
        std::memcpy(data, pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(m_device, stagingBufferMemory);

        transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(stagingBuffer, tex.image, width, height);
        transitionImageLayout(tex.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
        return true;
    }

    vkDeviceWaitIdle(m_device);
    destroyGpuTexture(tex);
    return createGpuTexture(pixels, width, height, tex, name);
}

VulkanRenderer::~VulkanRenderer() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(m_device, m_renderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(m_device, m_imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(m_device, m_inFlightFences[i], nullptr);
        }

        if (m_vertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
        }
        if (m_indexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
            vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
        }
        if (m_uiVertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_uiVertexBuffer, nullptr);
            vkFreeMemory(m_device, m_uiVertexBufferMemory, nullptr);
        }

        cleanupSwapchain();

        for (auto& [hash, tex] : m_gpuTextures) {
            destroyGpuTexture(tex);
        }
        m_gpuTextures.clear();
        destroyGpuTexture(m_sidebarTexture);
        destroyGpuTexture(m_tabBarTexture);
        destroyGpuTexture(m_defaultTexture);

        if (m_textureSampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_textureSampler, nullptr);
        if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        if (m_descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);

        if (m_solidPipeline != VK_NULL_HANDLE && m_solidPipeline != m_wireframePipeline) {
            vkDestroyPipeline(m_device, m_solidPipeline, nullptr);
        }
        if (m_wireframePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, m_wireframePipeline, nullptr);
        }
        if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);

        if (m_commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }

    if (m_surface != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }

    if (m_window) {
        glfwDestroyWindow(m_window);
        glfwTerminate();
    }
}

void VulkanRenderer::updateWindowTitle() {
    if (!m_window) return;
    const char* wfText = (m_wireframeMode == 0) ? "Solid Only"
                       : (m_wireframeMode == 1) ? "Solid + Wireframe"
                                                : "Wireframe Only";
    const char* texText = m_useTextures ? "Textures: ON" : "Textures: OFF";
    std::string fullTitle = m_title + "  —  [WASD+QE] Move  |  [Z] " + wfText + "  |  [T] " + texText + "  |  [R] Reset View";
    glfwSetWindowTitle(m_window, fullTitle.c_str());
    m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
}

bool VulkanRenderer::init() {
    if (!glfwInit()) {
        std::cerr << "[VulkanRenderer] Failed to initialize GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    m_window = glfwCreateWindow(m_width, m_height, m_title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "[VulkanRenderer] Failed to create GLFW window" << std::endl;
        return false;
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
    glfwSetMouseButtonCallback(m_window, mouseButtonCallback);
    glfwSetCursorPosCallback(m_window, cursorPosCallback);
    glfwSetScrollCallback(m_window, scrollCallback);
    glfwSetKeyCallback(m_window, keyCallback);
    glfwSetDropCallback(m_window, dropCallback);

    if (!createInstance()) return false;
    if (glfwCreateWindowSurface(m_instance, m_window, nullptr, &m_surface) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create window surface" << std::endl;
        return false;
    }

    if (!selectPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createDescriptorSetLayout()) return false;
    if (!createDepthResources()) return false;
    if (!createGraphicsPipelines()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createTextureSampler()) return false;
    if (!createDescriptorPool()) return false;
    if (!createDefaultTexture()) return false;
    if (!createUiQuadBuffer()) return false;
    if (!createSyncObjects()) return false;

    updateWindowTitle();
    std::cout << "[VulkanRenderer] Vulkan initialized successfully on Apple Silicon / MoltenVK!" << std::endl;
    return true;
}

bool VulkanRenderer::createUiQuadBuffer() {
    std::array<Vertex, 6> quad = {{
        {Vec3(-1.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(0.0f, 0.0f)},
        {Vec3( 1.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(1.0f, 0.0f)},
        {Vec3( 1.0f,  1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(1.0f, 1.0f)},

        {Vec3(-1.0f, -1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(0.0f, 0.0f)},
        {Vec3( 1.0f,  1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(1.0f, 1.0f)},
        {Vec3(-1.0f,  1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 1.0f, 1.0f, 1.0f), Vec2(0.0f, 1.0f)}
    }};

    VkDeviceSize sz = sizeof(quad);
    if (!createBuffer(sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      m_uiVertexBuffer, m_uiVertexBufferMemory)) {
        return false;
    }

    void* data = nullptr;
    vkMapMemory(m_device, m_uiVertexBufferMemory, 0, sz, 0, &data);
    std::memcpy(data, quad.data(), static_cast<size_t>(sz));
    vkUnmapMemory(m_device, m_uiVertexBufferMemory);
    return true;
}

void VulkanRenderer::updateSidebarTextureIfNeeded() {
    int winW = m_width, winH = m_height;
    glfwGetWindowSize(m_window, &winW, &winH);
    float scaleX = (winW > 0) ? (static_cast<float>(m_swapchainExtent.width) / static_cast<float>(winW)) : 1.0f;
    float scaleY = (winH > 0) ? (static_cast<float>(m_swapchainExtent.height) / static_cast<float>(winH)) : 1.0f;

    uint32_t sideW = std::max(1u, static_cast<uint32_t>(SidebarExplorer::SIDEBAR_WIDTH * scaleX));
    uint32_t sideH = std::max(1u, m_swapchainExtent.height);

    if (m_sidebar.isDirty() ||
        m_sidebarTexture.descriptorSet == VK_NULL_HANDLE ||
        m_sidebarTexture.width != sideW ||
        m_sidebarTexture.height != sideH) {
        m_sidebar.renderToRgba(sideW, sideH, m_sidebarPixels);
        updateGpuTexture(m_sidebarPixels.data(), sideW, sideH, m_sidebarTexture, "SIDEBAR_UI");
    }

    uint32_t tabW = (m_swapchainExtent.width > sideW) ? (m_swapchainExtent.width - sideW) : 1u;
    uint32_t tabH = std::max(1u, static_cast<uint32_t>(SidebarExplorer::TAB_BAR_HEIGHT * scaleY));

    if (m_sidebar.isTabBarDirty() ||
        m_tabBarTexture.descriptorSet == VK_NULL_HANDLE ||
        m_tabBarTexture.width != tabW ||
        m_tabBarTexture.height != tabH) {
        m_sidebar.renderTabBarToRgba(tabW, tabH, m_tabBarPixels);
        updateGpuTexture(m_tabBarPixels.data(), tabW, tabH, m_tabBarTexture, "TABBAR_UI");
    }
}

bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "NFSU2 BIN Opener";
    appInfo.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    appInfo.pEngineName = "NFSU2-Vulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(2, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

#ifdef __APPLE__
    extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
#ifdef __APPLE__
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    if (vkCreateInstance(&createInfo, nullptr, &m_instance) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create Vulkan instance" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::selectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::cerr << "[VulkanRenderer] Failed to find GPUs with Vulkan support" << std::endl;
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    for (const auto& device : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int graphicsIndex = -1;
        int presentIndex = -1;

        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsIndex = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
            if (presentSupport) {
                presentIndex = i;
            }
            if (graphicsIndex >= 0 && presentIndex >= 0) break;
        }

        if (graphicsIndex >= 0 && presentIndex >= 0) {
            m_physicalDevice = device;
            m_graphicsQueueFamily = static_cast<uint32_t>(graphicsIndex);
            m_presentQueueFamily = static_cast<uint32_t>(presentIndex);
            std::cout << "[VulkanRenderer] Selected GPU: " << properties.deviceName << std::endl;
            return true;
        }
    }

    std::cerr << "[VulkanRenderer] Failed to find a suitable GPU" << std::endl;
    return false;
}

bool VulkanRenderer::createLogicalDevice() {
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {m_graphicsQueueFamily, m_presentQueueFamily};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.fillModeNonSolid = VK_TRUE;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, availableExtensions.data());

    for (const auto& ext : availableExtensions) {
        if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            deviceExtensions.push_back("VK_KHR_portability_subset");
            break;
        }
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create logical device" << std::endl;
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);
    return true;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& availableFormat : formats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = availableFormat;
            break;
        }
    }
    m_swapchainFormat = surfaceFormat.format;

    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    m_swapchainExtent.width = std::clamp(static_cast<uint32_t>(fbWidth), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    m_swapchainExtent.height = std::clamp(static_cast<uint32_t>(fbHeight), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = m_swapchainExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queueFamilyIndices[] = {m_graphicsQueueFamily, m_presentQueueFamily};
    if (m_graphicsQueueFamily != m_presentQueueFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create swapchain" << std::endl;
        return false;
    }

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());

    return true;
}

bool VulkanRenderer::createImageViews() {
    m_swapchainImageViews.resize(m_swapchainImages.size());
    for (size_t i = 0; i < m_swapchainImages.size(); i++) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = m_swapchainImages[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = m_swapchainFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &createInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanRenderer] Failed to create image views" << std::endl;
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createDepthResources() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_swapchainExtent.width;
    imageInfo.extent.height = m_swapchainExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_depthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_depthImage) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create depth image" << std::endl;
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_depthImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_depthImageMemory) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to allocate depth image memory" << std::endl;
        return false;
    }
    vkBindImageMemory(m_device, m_depthImage, m_depthImageMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_depthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_depthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthImageView) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create depth image view" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = m_swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create render pass" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create descriptor set layout" << std::endl;
        return false;
    }
    return true;
}

VkShaderModule VulkanRenderer::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create shader module" << std::endl;
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

bool VulkanRenderer::createGraphicsPipelines() {
    auto vertShaderCode = readFile("vert.spv");
    auto fragShaderCode = readFile("frag.spv");

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE) {
        return false;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(Vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions{};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(Vertex, pos);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(Vertex, normal);

    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(Vertex, color);

    attributeDescriptions[3].binding = 0;
    attributeDescriptions[3].location = 3;
    attributeDescriptions[3].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[3].offset = offsetof(Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstantData);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create pipeline layout" << std::endl;
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_solidPipeline) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create solid graphics pipeline" << std::endl;
        return false;
    }

    rasterizer.polygonMode = VK_POLYGON_MODE_LINE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = -1.5f;
    rasterizer.depthBiasSlopeFactor = -1.5f;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_wireframePipeline) != VK_SUCCESS) {
        m_wireframePipeline = m_solidPipeline;
    }

    vkDestroyShaderModule(m_device, fragShaderModule, nullptr);
    vkDestroyShaderModule(m_device, vertShaderModule, nullptr);
    return true;
}

bool VulkanRenderer::createFramebuffers() {
    m_framebuffers.resize(m_swapchainImageViews.size());
    for (size_t i = 0; i < m_swapchainImageViews.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            m_swapchainImageViews[i],
            m_depthImageView
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = m_swapchainExtent.width;
        framebufferInfo.height = m_swapchainExtent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanRenderer] Failed to create framebuffer" << std::endl;
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;

    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create command pool" << std::endl;
        return false;
    }

    m_commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());

    if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to allocate command buffers" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createTextureSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_textureSampler) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create texture sampler" << std::endl;
        return false;
    }
    return true;
}

bool VulkanRenderer::createDescriptorPool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2048;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 2048;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to create descriptor pool" << std::endl;
        return false;
    }
    return true;
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = m_commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(m_device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanRenderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);

    vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
}

void VulkanRenderer::transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
    (void)format;
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    endSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    endSingleTimeCommands(commandBuffer);
}

bool VulkanRenderer::createGpuTexture(const uint8_t* pixels, uint32_t width, uint32_t height, GpuTexture& outTex, const std::string& name) {
    if (!pixels || width == 0 || height == 0) return false;

    outTex.width = width;
    outTex.height = height;
    outTex.name = name;

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(m_device, stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_device, stagingBufferMemory);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &outTex.image) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, outTex.image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &outTex.memory) != VK_SUCCESS) {
        vkDestroyImage(m_device, outTex.image, nullptr);
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingBufferMemory, nullptr);
        return false;
    }
    vkBindImageMemory(m_device, outTex.image, outTex.memory, 0);

    transitionImageLayout(outTex.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copyBufferToImage(stagingBuffer, outTex.image, width, height);
    transitionImageLayout(outTex.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingBufferMemory, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outTex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &outTex.view) != VK_SUCCESS) {
        return false;
    }

    rebindPersistentDescriptor(outTex);
    return (outTex.descriptorSet != VK_NULL_HANDLE);
}

bool VulkanRenderer::createDefaultTexture() {
    const uint8_t whitePixels[16] = {
        255, 255, 255, 255,
        255, 255, 255, 255,
        255, 255, 255, 255,
        255, 255, 255, 255
    };
    return createGpuTexture(whitePixels, 2, 2, m_defaultTexture, "DEFAULT_WHITE");
}

void VulkanRenderer::setTextures(const std::unordered_map<uint32_t, DecodedTexture>& textures) {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    for (auto& [hash, gpuTex] : m_gpuTextures) {
        gpuTex.descriptorSet = VK_NULL_HANDLE; // Pool reset below frees all sets at once
        destroyGpuTexture(gpuTex);
    }
    m_gpuTextures.clear();
    m_doorlineHash = 0;

    vkResetDescriptorPool(m_device, m_descriptorPool, 0);

    // Re-bind persistent UI & fallback textures immediately so Sidebar and TabBar never disappear!
    rebindPersistentDescriptor(m_defaultTexture);
    rebindPersistentDescriptor(m_sidebarTexture);
    rebindPersistentDescriptor(m_tabBarTexture);

    if (textures.empty()) return;

    size_t uploaded = 0;
    for (const auto& [hash, tex] : textures) {
        if (tex.rgba.empty() || tex.width == 0 || tex.height == 0) continue;
        if (uploaded >= 1800) break;

        GpuTexture gpuTex;
        if (createGpuTexture(tex.rgba.data(), tex.width, tex.height, gpuTex, tex.name)) {
            m_gpuTextures[hash] = gpuTex;
            if (tex.name.find("DOORLINE") != std::string::npos) {
                m_doorlineHash = hash;
            }
            uploaded++;
        }
    }

    std::cout << "[VulkanRenderer] Uploaded " << uploaded << " textures to Vulkan GPU descriptor sets." << std::endl;
}

void VulkanRenderer::saveActiveTabState() {
    if (m_activeTabIndex >= 0 && m_activeTabIndex < static_cast<int>(m_tabs.size())) {
        auto& tab = m_tabs[m_activeTabIndex];
        tab.camYaw = m_camYaw;
        tab.camPitch = m_camPitch;
        tab.camDist = m_camDist;
        tab.baseCamDist = m_baseCamDist;
        tab.camTarget = m_camTarget;
        tab.autoRotate = m_autoRotate;
        tab.wireframeMode = m_wireframeMode;
        tab.useTextures = m_useTextures;
        tab.carColor = m_carColor;
        tab.carMetallic = m_carMetallic;
        tab.activePaintIndex = m_activePaintIndex;
    }
}

void VulkanRenderer::syncSidebarTabs() {
    std::vector<std::string> titles;
    titles.reserve(m_tabs.size());
    for (const auto& t : m_tabs) {
        titles.push_back(t.title);
    }
    m_sidebar.setTabs(titles, m_activeTabIndex);
}

void VulkanRenderer::switchToTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(m_tabs.size())) return;
    if (tabIndex != m_activeTabIndex) {
        saveActiveTabState();
    }

    m_activeTabIndex = tabIndex;
    const auto& tab = m_tabs[m_activeTabIndex];

    m_camYaw = tab.camYaw;
    m_camPitch = tab.camPitch;
    m_camDist = tab.camDist;
    m_baseCamDist = tab.baseCamDist;
    m_camTarget = tab.camTarget;
    m_autoRotate = tab.autoRotate;
    m_wireframeMode = tab.wireframeMode;
    m_useTextures = tab.useTextures;
    m_carColor = tab.carColor;
    m_carMetallic = tab.carMetallic;
    m_activePaintIndex = tab.activePaintIndex;

    m_title = "NFSU2 BIN Opener: " + tab.title;
    setCarModel(tab.model);
    setTextures(tab.textures);

    m_sidebar.setActiveModelInfo(tab.title, tab.canonicalPath,
                                 tab.model.vertices.size(),
                                 tab.model.getTriangleCount(),
                                 tab.model.submeshes.size(),
                                 tab.textures.size());
    syncSidebarTabs();
    updateWindowTitle();
}

void VulkanRenderer::closeTab(int tabIndex) {
    if (m_tabs.size() <= 1) return;
    if (tabIndex < 0 || tabIndex >= static_cast<int>(m_tabs.size())) return;

    saveActiveTabState();
    m_tabs.erase(m_tabs.begin() + tabIndex);

    int nextActive = m_activeTabIndex;
    if (tabIndex < m_activeTabIndex) {
        nextActive = m_activeTabIndex - 1;
    } else if (nextActive >= static_cast<int>(m_tabs.size())) {
        nextActive = static_cast<int>(m_tabs.size()) - 1;
    }
    m_activeTabIndex = -1; // Force full reload of new active tab
    switchToTab(nextActive);
}

bool VulkanRenderer::loadTarget(const std::string& pathOrName) {
    if (pathOrName.empty()) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(pathOrName);

    bool isMap = false;
    std::string geomPath;
    std::string displayTitle = pathOrName;
    std::string activeHighlightPath = pathOrName;

    if (fs::exists(p, ec)) {
        if (fs::is_directory(p, ec)) {
            if (fs::exists(p / "GEOMETRY.BIN", ec)) {
                geomPath = (p / "GEOMETRY.BIN").string();
                displayTitle = p.filename().string();
                activeHighlightPath = fs::weakly_canonical(p, ec).string();
                m_currentBrowseDir = p.parent_path().string();
            } else if (fs::exists(p / "STREAML4RC.BUN", ec)) {
                geomPath = (p / "STREAML4RC.BUN").string();
                isMap = true;
                displayTitle = "STREAML4RC.BUN";
                activeHighlightPath = fs::weakly_canonical(geomPath, ec).string();
                m_currentBrowseDir = p.string();
            } else if (fs::exists(p / "CARS", ec) || p.filename() == "CARS" || p.filename() == "TRACKS") {
                m_sidebar.setWorkspaceRoot(p.string());
                m_sidebar.clearError();
                return true;
            } else {
                std::string reason = diagnoseUnopenableFile(p.string());
                m_sidebar.setError(p.filename().string(), reason, p.string());
                std::cerr << "[NFSU2 BIN Opener] Cannot open \"" << p.filename().string() << "\": " << reason << std::endl;
                return false;
            }
        } else if (fs::is_regular_file(p, ec)) {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            std::string fname = p.filename().string();
            std::transform(fname.begin(), fname.end(), fname.begin(), ::toupper);

            activeHighlightPath = fs::weakly_canonical(p, ec).string();
            m_currentBrowseDir = p.parent_path().string();

            // If user clicked L4RA.BUN..L4RH.BUN in TRACKS/, pair it with STREAML4RA.BUN..STREAML4RH.BUN
            if (fname.rfind("L4R", 0) == 0 && ext == ".BUN") {
                fs::path streamCandidate = p.parent_path() / ("STREAM" + fname);
                if (fs::exists(streamCandidate, ec)) {
                    geomPath = streamCandidate.string();
                    isMap = true;
                    displayTitle = fname + " (Stream)";
                } else {
                    geomPath = p.string();
                    isMap = true;
                    displayTitle = p.filename().string();
                }
            } else if (ext == ".BUN" || fname.find("STREAM") != std::string::npos) {
                geomPath = p.string();
                isMap = true;
                displayTitle = p.filename().string();
            } else {
                geomPath = p.string();
                if (fname == "GEOMETRY.BIN") {
                    displayTitle = p.parent_path().filename().string();
                } else {
                    displayTitle = p.parent_path().filename().string() + "/" + p.filename().string();
                }
            }
        }
    } else {
        if (pathOrName == "map" || pathOrName == "bayview" ||
            pathOrName.find("STREAM") != std::string::npos ||
            pathOrName.find("L4R") != std::string::npos) {
            isMap = true;
            std::vector<std::string> mapSearch = {
                "../TRACKS/STREAML4RC.BUN",
                "TRACKS/STREAML4RC.BUN",
                "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2/TRACKS/STREAML4RC.BUN",
                "../TRACKS/STREAML4RA.BUN"
            };
            for (const auto& mp : mapSearch) {
                if (fs::exists(mp, ec)) {
                    geomPath = mp;
                    displayTitle = fs::path(mp).filename().string();
                    activeHighlightPath = fs::weakly_canonical(mp, ec).string();
                    m_currentBrowseDir = fs::path(mp).parent_path().string();
                    break;
                }
            }
        } else {
            std::vector<std::string> searchPaths = {
                "../CARS/" + pathOrName + "/GEOMETRY.BIN",
                "CARS/" + pathOrName + "/GEOMETRY.BIN",
                "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2/CARS/" + pathOrName + "/GEOMETRY.BIN"
            };
            for (const auto& sp : searchPaths) {
                if (fs::exists(sp, ec)) {
                    geomPath = sp;
                    displayTitle = pathOrName;
                    activeHighlightPath = fs::weakly_canonical(fs::path(sp).parent_path(), ec).string();
                    m_currentBrowseDir = fs::path(sp).parent_path().parent_path().string();
                    break;
                }
            }
        }
    }

    if (geomPath.empty()) {
        std::string reason = diagnoseUnopenableFile(pathOrName);
        m_sidebar.setError(fs::path(pathOrName).filename().string(), reason, pathOrName);
        std::cerr << "[NFSU2 BIN Opener] Cannot open \"" << pathOrName << "\": " << reason << std::endl;
        return false;
    }

    // Check if a tab for this exact file/folder is already open!
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].canonicalPath == activeHighlightPath) {
            m_sidebar.clearError();
            switchToTab(static_cast<int>(i));
            return true;
        }
    }

    CarModel sceneModel;
    std::unordered_map<uint32_t, DecodedTexture> sceneTextures;

    if (isMap) {
        std::cout << "\n>>> Loading Bayview District / Archive from: " << geomPath << std::endl;
        sceneModel = MapParser::LoadMapDistrict(geomPath, 400);
        TextureParser::LoadTexturesInto(geomPath, sceneTextures);

        // If this .BUN has no 3D map geometry but contains TPK textures (e.g. GLOBALB.BUN), open as Texture Showcase!
        if (sceneModel.vertices.empty() && !sceneTextures.empty()) {
            sceneModel = GeometryParser::CreateTextureShowcase(sceneTextures, displayTitle);
        }
    } else {
        std::cout << "\n>>> Loading NFSU2 Archive from: " << geomPath << std::endl;
        sceneModel = GeometryParser::AssembleCar(geomPath, 'A');

        if (!sceneModel.vertices.empty()) {
            fs::path carDir = fs::path(geomPath).parent_path();
            fs::path carsRoot = carDir.parent_path();

            if (fs::exists(carsRoot / "TEXTURES.BIN", ec)) {
                TextureParser::LoadTexturesInto((carsRoot / "TEXTURES.BIN").string(), sceneTextures);
            }
            if (fs::exists(carsRoot / "WHEELS" / "TEXTURES.BIN", ec)) {
                TextureParser::LoadTexturesInto((carsRoot / "WHEELS" / "TEXTURES.BIN").string(), sceneTextures);
            }
            if (fs::exists(carDir / "TEXTURES.BIN", ec)) {
                TextureParser::LoadTexturesInto((carDir / "TEXTURES.BIN").string(), sceneTextures);
            }
            // Also load any textures embedded inside geomPath itself (e.g. CarLot01.BIN, SKYLINE/GEOMETRY.BIN)
            TextureParser::LoadTexturesInto(geomPath, sceneTextures);

            m_siblingDirs.clear();
            m_currentSiblingIndex = -1;
            if (fs::exists(carsRoot, ec) && fs::is_directory(carsRoot, ec)) {
                for (const auto& entry : fs::directory_iterator(carsRoot, ec)) {
                    if (entry.is_directory(ec) && fs::exists(entry.path() / "GEOMETRY.BIN", ec)) {
                        m_siblingDirs.push_back(entry.path().string());
                    }
                }
                std::sort(m_siblingDirs.begin(), m_siblingDirs.end());
                std::string canonicalCarDir = fs::weakly_canonical(carDir, ec).string();
                for (size_t i = 0; i < m_siblingDirs.size(); ++i) {
                    if (fs::weakly_canonical(m_siblingDirs[i], ec).string() == canonicalCarDir) {
                        m_currentSiblingIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
        } else {
            // No 3D geometry chunks (0x80134000) — check if it is a Texture Pack / TrackMap (e.g. TRACKMAP4001.BIN, TEXTURES.BIN, VINYLS.BIN)
            TextureParser::LoadTexturesInto(geomPath, sceneTextures);
            if (!sceneTextures.empty()) {
                std::cout << "[TextureParser] Opening \"" << displayTitle << "\" as Texture / TrackMap Showcase ("
                          << sceneTextures.size() << " textures)." << std::endl;
                sceneModel = GeometryParser::CreateTextureShowcase(sceneTextures, displayTitle);
            }
        }
    }

    if (sceneModel.vertices.empty()) {
        std::string reason = diagnoseUnopenableFile(geomPath);
        m_sidebar.setError(fs::path(geomPath).filename().string(), reason, activeHighlightPath);
        std::cerr << "[NFSU2 BIN Opener] Cannot open \"" << fs::path(geomPath).filename().string() << "\": " << reason << std::endl;
        return false;
    }

    saveActiveTabState();

    OpenTab newTab;
    newTab.title = displayTitle;
    newTab.canonicalPath = activeHighlightPath;
    newTab.model = std::move(sceneModel);
    newTab.textures = std::move(sceneTextures);

    if (newTab.model.isTextureShowcase) {
        newTab.camYaw = 0.0f;
        newTab.camPitch = 0.0f;
        newTab.camDist = 3.8f;
        newTab.baseCamDist = 3.8f;
        newTab.camTarget = Vec3(0.0f, 0.0f, 0.0f);
        newTab.autoRotate = false;
        newTab.wireframeMode = 0; // Crisp unencumbered view for TrackMaps & Texture Galleries
        newTab.useTextures = true;
    } else if (newTab.model.carName == "Showroom") {
        newTab.camYaw = 0.45f;
        newTab.camPitch = 0.16f;
        newTab.camDist = 6.5f;
        newTab.baseCamDist = 6.5f;
        newTab.camTarget = Vec3(0.0f, 0.0f, 0.0f);
        newTab.autoRotate = false; // Start stationary inside shop so WASD navigation feels immediate
        newTab.wireframeMode = 0;
        newTab.useTextures = true;
    } else {
        newTab.camYaw = 0.65f;
        newTab.camPitch = 0.28f;
        newTab.camDist = 4.5f;
        newTab.baseCamDist = 4.5f;
        newTab.camTarget = Vec3(0.0f, 0.0f, 0.0f);
        newTab.autoRotate = true;
        newTab.wireframeMode = m_wireframeMode;
        newTab.useTextures = m_useTextures;
    }
    newTab.carColor = m_carColor;
    newTab.carMetallic = m_carMetallic;
    newTab.activePaintIndex = m_activePaintIndex;

    m_tabs.push_back(std::move(newTab));
    m_sidebar.clearError();
    switchToTab(static_cast<int>(m_tabs.size()) - 1);
    return true;
}

void VulkanRenderer::openFileDialog() {
    std::string chosen = MacFileDialog::PickBinOrFolder(m_sidebar.getWorkspaceRoot());
    if (!chosen.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::is_directory(chosen, ec) && !fs::exists(fs::path(chosen) / "GEOMETRY.BIN", ec)) {
            m_sidebar.setWorkspaceRoot(chosen);
        } else {
            loadTarget(chosen);
        }
    }
}

void VulkanRenderer::stepSiblingTarget(int delta) {
    if (m_siblingDirs.empty()) return;
    int n = static_cast<int>(m_siblingDirs.size());
    if (m_currentSiblingIndex < 0) m_currentSiblingIndex = 0;
    m_currentSiblingIndex = (m_currentSiblingIndex + delta % n + n) % n;
    loadTarget(m_siblingDirs[m_currentSiblingIndex]);
}

float VulkanRenderer::classifySubmeshMaterial(const SubMesh& sub, VkDescriptorSet& outDescSet) const {
    outDescSet = m_defaultTexture.descriptorSet;

    auto it = m_gpuTextures.find(sub.textureHash);
    bool hasExactTex = (it != m_gpuTextures.end() && it->second.descriptorSet != VK_NULL_HANDLE);
    std::string texName = hasExactTex ? it->second.name : "";

    // Texture Pack / TrackMap Showcase Tile
    if (sub.shaderHash == 0x54455832u) {
        if (hasExactTex && m_useTextures) {
            outDescSet = it->second.descriptorSet;
        }
        return 5.0f;
    }

    if (m_isMap) {
        if (hasExactTex && m_useTextures) {
            outDescSet = it->second.descriptorSet;
        }
        return 0.0f;
    }

    const std::string& p = sub.partName;

    if (p.find("WINDOW") != std::string::npos ||
        p.find("WINDSHIELD") != std::string::npos ||
        sub.shaderHash == 0x6273197c ||
        texName.find("WINDSHIELD") != std::string::npos) {
        if (hasExactTex && m_useTextures) outDescSet = it->second.descriptorSet;
        return 2.0f;
    }

    if (sub.shaderHash == 0x2952c15d ||
        texName.find("TIRE") != std::string::npos ||
        texName.find("CARBOTTOM") != std::string::npos) {
        if (hasExactTex && m_useTextures) outDescSet = it->second.descriptorSet;
        return 3.0f;
    }

    if (p.find("WHEEL") != std::string::npos ||
        p.find("BRAKE") != std::string::npos ||
        p.find("EXHAUST") != std::string::npos ||
        p.find("MUFFLER") != std::string::npos) {
        if (hasExactTex && m_useTextures) outDescSet = it->second.descriptorSet;
        return 4.0f;
    }

    if (hasExactTex && texName.find("DOORLINE") == std::string::npos) {
        if (m_useTextures) outDescSet = it->second.descriptorSet;
        return 0.0f;
    }

    if (m_useTextures && m_doorlineHash != 0) {
        auto dit = m_gpuTextures.find(m_doorlineHash);
        if (dit != m_gpuTextures.end()) {
            outDescSet = dit->second.descriptorSet;
        }
    }
    return 1.0f;
}

bool VulkanRenderer::createSyncObjects() {
    m_imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    m_inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS) {
            std::cerr << "[VulkanRenderer] Failed to create sync objects" << std::endl;
            return false;
        }
    }
    return true;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, buffer, bufferMemory, 0);
    return true;
}

void VulkanRenderer::setCarModel(const CarModel& model) {
    if (m_device == VK_NULL_HANDLE || model.vertices.empty()) return;

    vkDeviceWaitIdle(m_device);

    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
    }

    m_indexCount = static_cast<uint32_t>(model.indices.size());
    m_submeshes = model.submeshes;
    m_isMap = model.isMap;

    if (m_submeshes.empty() && m_indexCount > 0) {
        SubMesh fallback;
        fallback.firstIndex = 0;
        fallback.indexCount = m_indexCount;
        m_submeshes.push_back(fallback);
    }

    VkDeviceSize vertexBufferSize = sizeof(Vertex) * model.vertices.size();
    createBuffer(vertexBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 m_vertexBuffer, m_vertexBufferMemory);

    void* data;
    vkMapMemory(m_device, m_vertexBufferMemory, 0, vertexBufferSize, 0, &data);
    memcpy(data, model.vertices.data(), (size_t)vertexBufferSize);
    vkUnmapMemory(m_device, m_vertexBufferMemory);

    VkDeviceSize indexBufferSize = sizeof(uint32_t) * model.indices.size();
    createBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 m_indexBuffer, m_indexBufferMemory);

    vkMapMemory(m_device, m_indexBufferMemory, 0, indexBufferSize, 0, &data);
    memcpy(data, model.indices.data(), (size_t)indexBufferSize);
    vkUnmapMemory(m_device, m_indexBufferMemory);

    std::cout << "[VulkanRenderer] Uploaded " << model.vertices.size()
              << " vertices, " << m_indexCount << " indices ("
              << m_submeshes.size() << " submeshes) to Vulkan GPU buffers." << std::endl;
}

void VulkanRenderer::setCarColor(const Vec3& color, float metallic, int paintIndex) {
    m_carColor = color;
    m_carMetallic = metallic;
    m_activePaintIndex = paintIndex;
    saveActiveTabState();
    m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
}

void VulkanRenderer::selectPaintPreset(int index) {
    switch (index) {
        case 1: setCarColor({0.10f, 0.45f, 0.95f}, 0.85f, 1); break;
        case 2: setCarColor({0.15f, 0.92f, 0.22f}, 0.75f, 2); break;
        case 3: setCarColor({0.96f, 0.38f, 0.05f}, 0.80f, 3); break;
        case 4: setCarColor({0.88f, 0.05f, 0.15f}, 0.90f, 4); break;
        case 5: setCarColor({0.04f, 0.04f, 0.05f}, 0.95f, 5); break;
        case 6: setCarColor({0.95f, 0.95f, 0.96f}, 0.60f, 6); break;
        case 7: setCarColor({0.75f, 0.10f, 0.85f}, 0.80f, 7); break;
        default: break;
    }
}

void VulkanRenderer::toggleWireframe() {
    m_wireframeMode = (m_wireframeMode + 1) % 3;
    saveActiveTabState();
    const char* modeStr = (m_wireframeMode == 0) ? "SOLID ONLY"
                        : (m_wireframeMode == 1) ? "SOLID + WIREFRAME OVERLAY"
                                                 : "WIREFRAME ONLY";
    std::cout << "[VulkanRenderer] Wireframe Mode: " << modeStr << std::endl;
    updateWindowTitle();
}

void VulkanRenderer::toggleTextures() {
    m_useTextures = !m_useTextures;
    saveActiveTabState();
    std::cout << "[VulkanRenderer] Textures: " << (m_useTextures ? "ON" : "OFF") << std::endl;
    updateWindowTitle();
}

void VulkanRenderer::setWireframeMode(int mode) {
    m_wireframeMode = std::clamp(mode, 0, 2);
    saveActiveTabState();
    updateWindowTitle();
}

void VulkanRenderer::setUseTextures(bool useTex) {
    m_useTextures = useTex;
    saveActiveTabState();
    updateWindowTitle();
}

void VulkanRenderer::toggleAutoRotate() {
    m_autoRotate = !m_autoRotate;
    saveActiveTabState();
    std::cout << "[VulkanRenderer] Auto-Rotate: " << (m_autoRotate ? "ON" : "OFF") << std::endl;
    updateWindowTitle();
}

void VulkanRenderer::zoomByFactor(float factor) {
    m_camDist = std::clamp(m_camDist * factor, 0.05f, 300.0f);
    saveActiveTabState();
    m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
}

void VulkanRenderer::resetZoom() {
    m_camDist = m_baseCamDist;
    saveActiveTabState();
    m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
}

void VulkanRenderer::resetCamera() {
    m_camTarget = Vec3(0.0f, 0.0f, 0.0f);
    m_camDist = m_baseCamDist;
    if (m_activeTabIndex >= 0 && m_activeTabIndex < static_cast<int>(m_tabs.size()) &&
        m_tabs[m_activeTabIndex].model.isTextureShowcase) {
        m_camYaw = 0.0f;
        m_camPitch = 0.0f;
    } else {
        m_camYaw = 0.65f;
        m_camPitch = 0.25f;
    }
    saveActiveTabState();
    m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
}

void VulkanRenderer::cleanupSwapchain() {
    if (m_depthImageView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_depthImageView, nullptr);
    if (m_depthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_depthImage, nullptr);
    if (m_depthImageMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_depthImageMemory, nullptr);

    for (auto framebuffer : m_framebuffers) {
        vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    }
    for (auto imageView : m_swapchainImageViews) {
        vkDestroyImageView(m_device, imageView, nullptr);
    }
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    }
}

void VulkanRenderer::recreateSwapchain() {
    int width = 0, height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    while (width == 0 || height == 0) {
        glfwGetFramebufferSize(m_window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(m_device);
    cleanupSwapchain();

    createSwapchain();
    createImageViews();
    createDepthResources();
    createFramebuffers();
    m_sidebar.markDirty();
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = m_swapchainExtent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.06f, 0.07f, 0.09f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    int winW = m_width, winH = m_height;
    glfwGetWindowSize(m_window, &winW, &winH);
    float scaleX = (winW > 0) ? (static_cast<float>(m_swapchainExtent.width) / static_cast<float>(winW)) : 1.0f;
    float scaleY = (winH > 0) ? (static_cast<float>(m_swapchainExtent.height) / static_cast<float>(winH)) : 1.0f;

    uint32_t sidebarFbW = std::min(m_swapchainExtent.width / 2,
                                   static_cast<uint32_t>(SidebarExplorer::SIDEBAR_WIDTH * scaleX));
    uint32_t tabBarFbH = std::min(m_swapchainExtent.height / 4,
                                  static_cast<uint32_t>(SidebarExplorer::TAB_BAR_HEIGHT * scaleY));
    uint32_t sceneFbW = (m_swapchainExtent.width > sidebarFbW) ? (m_swapchainExtent.width - sidebarFbW) : 1u;
    uint32_t sceneFbH = (m_swapchainExtent.height > tabBarFbH) ? (m_swapchainExtent.height - tabBarFbH) : 1u;

    // --- PASS 0A: Left VS Code-style Explorer Sidebar (2D UI Quad) ---
    if (m_uiVertexBuffer != VK_NULL_HANDLE && m_sidebarTexture.descriptorSet != VK_NULL_HANDLE && sidebarFbW > 0) {
        VkViewport uiViewport{};
        uiViewport.x = 0.0f;
        uiViewport.y = 0.0f;
        uiViewport.width = static_cast<float>(sidebarFbW);
        uiViewport.height = static_cast<float>(m_swapchainExtent.height);
        uiViewport.minDepth = 0.0f;
        uiViewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &uiViewport);

        VkRect2D uiScissor{};
        uiScissor.offset = {0, 0};
        uiScissor.extent = {sidebarFbW, m_swapchainExtent.height};
        vkCmdSetScissor(commandBuffer, 0, 1, &uiScissor);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_solidPipeline);

        VkBuffer uiBuffers[] = {m_uiVertexBuffer};
        VkDeviceSize uiOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, uiBuffers, uiOffsets);

        PushConstantData uiPush{};
        uiPush.renderFlags = Vec4(1.0f, 3.0f, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstantData), &uiPush);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_sidebarTexture.descriptorSet, 0, nullptr);

        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // --- PASS 0B: Top Chrome / VS Code Multi-Tab Bar (2D UI Quad) ---
    if (m_uiVertexBuffer != VK_NULL_HANDLE && m_tabBarTexture.descriptorSet != VK_NULL_HANDLE && tabBarFbH > 0) {
        VkViewport tabViewport{};
        tabViewport.x = static_cast<float>(sidebarFbW);
        tabViewport.y = 0.0f;
        tabViewport.width = static_cast<float>(sceneFbW);
        tabViewport.height = static_cast<float>(tabBarFbH);
        tabViewport.minDepth = 0.0f;
        tabViewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &tabViewport);

        VkRect2D tabScissor{};
        tabScissor.offset = {static_cast<int32_t>(sidebarFbW), 0};
        tabScissor.extent = {sceneFbW, tabBarFbH};
        vkCmdSetScissor(commandBuffer, 0, 1, &tabScissor);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_solidPipeline);

        VkBuffer uiBuffers[] = {m_uiVertexBuffer};
        VkDeviceSize uiOffsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, uiBuffers, uiOffsets);

        PushConstantData uiPush{};
        uiPush.renderFlags = Vec4(1.0f, 3.0f, 0.0f, 0.0f);
        vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PushConstantData), &uiPush);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_tabBarTexture.descriptorSet, 0, nullptr);

        vkCmdDraw(commandBuffer, 6, 1, 0, 0);
    }

    // --- PASS 1 & 2: Active Tab 3D Model Viewport (ONLY active tab is rendered!) ---
    VkViewport viewport{};
    viewport.x = static_cast<float>(sidebarFbW);
    viewport.y = static_cast<float>(tabBarFbH);
    viewport.width = static_cast<float>(sceneFbW);
    viewport.height = static_cast<float>(sceneFbH);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {static_cast<int32_t>(sidebarFbW), static_cast<int32_t>(tabBarFbH)};
    scissor.extent = {sceneFbW, sceneFbH};
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    if (m_indexCount > 0 && m_vertexBuffer != VK_NULL_HANDLE) {
        VkBuffer vertexBuffers[] = {m_vertexBuffer};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        float eyeX = m_camDist * std::cos(m_camPitch) * std::sin(m_camYaw);
        float eyeY = m_camDist * std::sin(m_camPitch);
        float eyeZ = m_camDist * std::cos(m_camPitch) * std::cos(m_camYaw);

        Vec3 eye = m_camTarget + Vec3(eyeX, eyeY, eyeZ);
        Vec3 target = m_camTarget;
        Vec3 up(0.0f, 1.0f, 0.0f);

        Mat4 view = Mat4::lookAt(eye, target, up);
        float aspect = static_cast<float>(sceneFbW) / static_cast<float>(sceneFbH);
        Mat4 proj = Mat4::perspectiveVK(42.0f * 3.14159265f / 180.0f, aspect, 0.02f, 800.0f);
        Mat4 model = Mat4::identity();

        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

        PushConstantData pushData;
        pushData.model = model;
        pushData.view = view;
        pushData.proj = proj;
        pushData.cameraPos = Vec4(eye, time);
        pushData.carColor = Vec4(m_carColor, m_carMetallic);

        float texFlag = m_useTextures ? 1.0f : 0.0f;
        float mapFlag = m_isMap ? 1.0f : 0.0f;

        if (m_wireframeMode == 0 || m_wireframeMode == 1) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_solidPipeline);

            for (const auto& sub : m_submeshes) {
                if (sub.indexCount == 0) continue;
                VkDescriptorSet descSet = m_defaultTexture.descriptorSet;
                float matType = classifySubmeshMaterial(sub, descSet);

                pushData.renderFlags = Vec4(texFlag, 0.0f, matType, mapFlag);
                vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushConstantData), &pushData);

                if (descSet != VK_NULL_HANDLE) {
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipelineLayout, 0, 1, &descSet, 0, nullptr);
                }

                vkCmdDrawIndexed(commandBuffer, sub.indexCount, 1, sub.firstIndex, 0, 0);
            }
        }

        if (m_wireframeMode == 1 || m_wireframeMode == 2) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_wireframePipeline);
            float wireFlag = (m_wireframeMode == 1) ? 1.0f : 2.0f;

            for (const auto& sub : m_submeshes) {
                if (sub.indexCount == 0) continue;
                VkDescriptorSet descSet = m_defaultTexture.descriptorSet;
                float matType = classifySubmeshMaterial(sub, descSet);

                pushData.renderFlags = Vec4(texFlag, wireFlag, matType, mapFlag);
                vkCmdPushConstants(commandBuffer, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushConstantData), &pushData);

                if (descSet != VK_NULL_HANDLE) {
                    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipelineLayout, 0, 1, &descSet, 0, nullptr);
                }

                vkCmdDrawIndexed(commandBuffer, sub.indexCount, 1, sub.firstIndex, 0, 0);
            }
        }
    }

    vkCmdEndRenderPass(commandBuffer);
    vkEndCommandBuffer(commandBuffer);
}

void VulkanRenderer::drawFrame() {
    updateSidebarTextureIfNeeded();

    vkWaitForFences(m_device, 1, &m_inFlightFences[m_currentFrame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
                                           m_imageAvailableSemaphores[m_currentFrame], VK_NULL_HANDLE, &imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return;
    }

    vkResetFences(m_device, 1, &m_inFlightFences[m_currentFrame]);

    vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);
    recordCommandBuffer(m_commandBuffers[m_currentFrame], imageIndex);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {m_imageAvailableSemaphores[m_currentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffers[m_currentFrame];

    VkSemaphore signalSemaphores[] = {m_renderFinishedSemaphores[m_currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_inFlightFences[m_currentFrame]) != VK_SUCCESS) {
        std::cerr << "[VulkanRenderer] Failed to submit draw command buffer" << std::endl;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    VkSwapchainKHR swapchains[] = {m_swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_framebufferResized) {
        m_framebufferResized = false;
        recreateSwapchain();
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::run(int maxFrames) {
    double lastTime = glfwGetTime();

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "  NEED FOR SPEED: UNDERGROUND 2 - BIN OPENER (VULKAN)" << std::endl;
    std::cout << "=======================================================" << std::endl;
    std::cout << "  Controls:" << std::endl;
    std::cout << "  • Left Sidebar      : Click folders/files to open in new Tabs" << std::endl;
    std::cout << "  • Top Tab + 3D Bar  : Blender-style Zoom [-/+], Reset, Wireframe, Tex, Rotate" << std::endl;
    std::cout << "  • W / A / S / D     : Move / Fly around 3D scene (Hold Shift for 3.5x Speed)" << std::endl;
    std::cout << "  • Q / E             : Move Down / Up" << std::endl;
    std::cout << "  • Left-Click Drag   : Orbit Camera  |  Right-Click Drag: Pan Camera" << std::endl;
    std::cout << "  • Scroll / + / -    : Wide-Range Zoom (0.05m to 300m)" << std::endl;
    std::cout << "  • R                 : Reset Camera Position & Zoom" << std::endl;
    std::cout << "  • Z                 : Cycle Wireframe (Solid -> Solid+Wireframe -> Wireframe)" << std::endl;
    std::cout << "  • T                 : Toggle Textures (ON / OFF)" << std::endl;
    std::cout << "  • SPACE             : Toggle Auto-Rotation" << std::endl;
    std::cout << "  • Keys 1 to 7       : Switch Underground Paint Colors" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    int frameCount = 0;
    while (!glfwWindowShouldClose(m_window)) {
        glfwPollEvents();

        double currentTime = glfwGetTime();
        float dt = static_cast<float>(std::min(currentTime - lastTime, 0.1));
        lastTime = currentTime;

        // WASD + Q/E Free-Flight / Pan Movement through 3D Scene
        Vec3 forward(-std::cos(m_camPitch) * std::sin(m_camYaw),
                     -std::sin(m_camPitch),
                     -std::cos(m_camPitch) * std::cos(m_camYaw));
        Vec3 right(std::cos(m_camYaw), 0.0f, -std::sin(m_camYaw));
        Vec3 worldUp(0.0f, 1.0f, 0.0f);

        bool shiftDown = (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                          glfwGetKey(m_window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        float moveSpeed = std::clamp(m_camDist * 1.5f, 3.0f, 65.0f) * (shiftDown ? 3.5f : 1.0f);

        Vec3 moveDelta(0.0f, 0.0f, 0.0f);
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) moveDelta = moveDelta + forward;
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) moveDelta = moveDelta - forward;
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) moveDelta = moveDelta + right;
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) moveDelta = moveDelta - right;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) moveDelta = moveDelta + worldUp;
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) moveDelta = moveDelta - worldUp;

        if (moveDelta.length() > 1e-4f) {
            m_camTarget = m_camTarget + moveDelta.normalized() * (moveSpeed * dt);
            if (m_autoRotate) {
                m_autoRotate = false;
                m_sidebar.setRenderState(m_wireframeMode, m_useTextures, m_autoRotate, m_activePaintIndex, m_camDist, m_baseCamDist);
            }
        }

        if (m_autoRotate && !m_mousePressed && !m_rightMousePressed) {
            m_camYaw += dt * 0.40f;
        }

        drawFrame();
        frameCount++;
        if (maxFrames > 0 && frameCount >= maxFrames) {
            std::cout << "[VulkanRenderer] Successfully rendered " << frameCount << " frames on Apple Silicon / Vulkan." << std::endl;
            break;
        }
    }

    vkDeviceWaitIdle(m_device);
}

void VulkanRenderer::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));
    double xpos = 0.0, ypos = 0.0;
    glfwGetCursorPos(window, &xpos, &ypos);
    int winW = renderer->m_width, winH = renderer->m_height;
    glfwGetWindowSize(window, &winW, &winH);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            if (xpos < SidebarExplorer::SIDEBAR_WIDTH || ypos < SidebarExplorer::TAB_BAR_HEIGHT) {
                SidebarAction act = renderer->m_sidebar.onMouseClick(xpos, ypos, winW, winH);
                switch (act.type) {
                    case SidebarActionType::LoadPath:
                        renderer->loadTarget(act.path);
                        break;
                    case SidebarActionType::OpenFolderDialog:
                        renderer->openFileDialog();
                        break;
                    case SidebarActionType::ToggleWireframe:
                        renderer->toggleWireframe();
                        break;
                    case SidebarActionType::ToggleTextures:
                        renderer->toggleTextures();
                        break;
                    case SidebarActionType::ToggleAutoRotate:
                        renderer->toggleAutoRotate();
                        break;
                    case SidebarActionType::SelectPaintColor:
                        renderer->selectPaintPreset(act.intParam);
                        break;
                    case SidebarActionType::SwitchTab:
                        renderer->switchToTab(act.intParam);
                        break;
                    case SidebarActionType::CloseTab:
                        renderer->closeTab(act.intParam);
                        break;
                    case SidebarActionType::ZoomIn:
                        renderer->zoomByFactor(0.80f);
                        break;
                    case SidebarActionType::ZoomOut:
                        renderer->zoomByFactor(1.25f);
                        break;
                    case SidebarActionType::ResetZoom:
                        renderer->resetZoom();
                        break;
                    case SidebarActionType::ResetCamera:
                        renderer->resetCamera();
                        break;
                    default:
                        break;
                }
            } else {
                renderer->m_mousePressed = true;
                renderer->m_lastMouseX = xpos;
                renderer->m_lastMouseY = ypos;
            }
        } else if (action == GLFW_RELEASE) {
            renderer->m_mousePressed = false;
        }
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS && xpos >= SidebarExplorer::SIDEBAR_WIDTH && ypos >= SidebarExplorer::TAB_BAR_HEIGHT) {
            renderer->m_rightMousePressed = true;
            renderer->m_lastMouseX = xpos;
            renderer->m_lastMouseY = ypos;
        } else if (action == GLFW_RELEASE) {
            renderer->m_rightMousePressed = false;
        }
    }
}

void VulkanRenderer::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));
    int winW = renderer->m_width, winH = renderer->m_height;
    glfwGetWindowSize(window, &winW, &winH);

    renderer->m_sidebar.onMouseMove(xpos, ypos, winW, winH);

    if (renderer->m_mousePressed) {
        float dx = static_cast<float>(xpos - renderer->m_lastMouseX);
        float dy = static_cast<float>(ypos - renderer->m_lastMouseY);

        renderer->m_camYaw += dx * 0.008f;
        renderer->m_camPitch += dy * 0.008f;
        renderer->m_camPitch = std::clamp(renderer->m_camPitch, -1.48f, 1.48f);

        renderer->m_lastMouseX = xpos;
        renderer->m_lastMouseY = ypos;
    } else if (renderer->m_rightMousePressed) {
        float dx = static_cast<float>(xpos - renderer->m_lastMouseX);
        float dy = static_cast<float>(ypos - renderer->m_lastMouseY);

        float panScale = std::max(0.002f, renderer->m_camDist * 0.0018f);
        Vec3 right(std::cos(renderer->m_camYaw), 0.0f, -std::sin(renderer->m_camYaw));
        Vec3 up(0.0f, 1.0f, 0.0f);
        renderer->m_camTarget = renderer->m_camTarget - right * (dx * panScale) + up * (dy * panScale);

        renderer->m_lastMouseX = xpos;
        renderer->m_lastMouseY = ypos;
    }
}

void VulkanRenderer::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)xoffset;
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));
    double xpos = 0.0, ypos = 0.0;
    glfwGetCursorPos(window, &xpos, &ypos);
    int winW = renderer->m_width, winH = renderer->m_height;
    glfwGetWindowSize(window, &winW, &winH);

    if (xpos < SidebarExplorer::SIDEBAR_WIDTH) {
        renderer->m_sidebar.onScroll(xpos, ypos, yoffset, winH);
    } else {
        float factor = std::pow(0.87f, static_cast<float>(yoffset));
        renderer->zoomByFactor(factor);
    }
}

void VulkanRenderer::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (action != GLFW_PRESS) return;
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));

    if (key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    } else if (key == GLFW_KEY_TAB) {
        if (!renderer->m_tabs.empty()) {
            int nextTab = (renderer->m_activeTabIndex + 1) % static_cast<int>(renderer->m_tabs.size());
            renderer->switchToTab(nextTab);
        }
    } else if (key == GLFW_KEY_O) {
        renderer->openFileDialog();
    } else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_N) {
        renderer->stepSiblingTarget(1);
    } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_P) {
        renderer->stepSiblingTarget(-1);
    } else if (key == GLFW_KEY_Z) {
        renderer->toggleWireframe();
    } else if (key == GLFW_KEY_T) {
        renderer->toggleTextures();
    } else if (key == GLFW_KEY_SPACE) {
        renderer->toggleAutoRotate();
    } else if (key == GLFW_KEY_R) {
        renderer->resetCamera();
    } else if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
        renderer->zoomByFactor(0.80f);
    } else if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
        renderer->zoomByFactor(1.25f);
    } else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_7) {
        renderer->selectPaintPreset(key - GLFW_KEY_1 + 1);
    }
}

void VulkanRenderer::dropCallback(GLFWwindow* window, int count, const char** paths) {
    if (count <= 0 || !paths || !paths[0]) return;
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < count; ++i) {
        if (paths[i]) {
            std::cout << "[VulkanRenderer] Dropped path: " << paths[i] << std::endl;
            renderer->loadTarget(paths[i]);
        }
    }
}

void VulkanRenderer::framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    (void)width;
    (void)height;
    auto renderer = reinterpret_cast<VulkanRenderer*>(glfwGetWindowUserPointer(window));
    renderer->m_framebufferResized = true;
}

} // namespace NFSU2
