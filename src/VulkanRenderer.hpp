#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "Math3D.hpp"
#include "GeometryParser.hpp"
#include "TextureParser.hpp"
#include "MacFileDialog.hpp"
#include <vector>
#include <string>
#include <unordered_map>

namespace NFSU2 {

struct PushConstantData {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Vec4 cameraPos;
    Vec4 carColor;
    Vec4 renderFlags; // x = useTextures, y = wireframePass (3.0 = 2D UI), z = materialType, w = isMap
};

struct GpuTexture {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    std::string name;
};

struct OpenTab {
    std::string title;
    std::string canonicalPath;
    CarModel model;
    std::unordered_map<uint32_t, DecodedTexture> textures;

    float camYaw{0.65f};
    float camPitch{0.28f};
    float camDist{4.5f};
    float baseCamDist{4.5f};
    Vec3 camTarget{0.0f, 0.0f, 0.0f};
    bool autoRotate{true};
    int wireframeMode{1};
    bool useTextures{true};
    Vec3 carColor{0.10f, 0.45f, 0.95f};
    float carMetallic{0.85f};
    int activePaintIndex{1};
};

class VulkanRenderer {
public:
    VulkanRenderer(int width = 1380, int height = 800, const std::string& title = "NFSU2 BIN Opener");
    ~VulkanRenderer();

    bool init();
    bool loadTarget(const std::string& pathOrName);
    void switchToTab(int tabIndex);
    void closeTab(int tabIndex);
    void openFileDialog();
    void stepSiblingTarget(int delta);
    void setCarModel(const CarModel& model);
    void setTextures(const std::unordered_map<uint32_t, DecodedTexture>& textures);
    void run(int maxFrames = 0);

    // Camera and visual controls
    void setCarColor(const Vec3& color, float metallic = 0.8f, int paintIndex = 1);
    void selectPaintPreset(int index);
    void toggleWireframe();
    void toggleTextures();
    void toggleAutoRotate();
    void setWireframeMode(int mode);
    void setUseTextures(bool useTex);
    void zoomByFactor(float factor);
    void resetZoom();
    void resetCamera();

private:
    int m_width;
    int m_height;
    std::string m_title;

    GLFWwindow* m_window{nullptr};

    // Vulkan Core
    VkInstance m_instance{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};
    VkQueue m_presentQueue{VK_NULL_HANDLE};
    uint32_t m_graphicsQueueFamily{0};
    uint32_t m_presentQueueFamily{0};

    // Swapchain
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
    std::vector<VkImage> m_swapchainImages;
    VkFormat m_swapchainFormat{VK_FORMAT_B8G8R8A8_UNORM};
    VkExtent2D m_swapchainExtent{};
    std::vector<VkImageView> m_swapchainImageViews;

    // Depth Buffer
    VkImage m_depthImage{VK_NULL_HANDLE};
    VkDeviceMemory m_depthImageMemory{VK_NULL_HANDLE};
    VkImageView m_depthImageView{VK_NULL_HANDLE};
    VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};

    // Render Pass & Pipeline
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_solidPipeline{VK_NULL_HANDLE};
    VkPipeline m_wireframePipeline{VK_NULL_HANDLE};

    // 0 = Solid Only, 1 = Solid + Wireframe Overlay, 2 = Wireframe Only
    int m_wireframeMode{1};
    bool m_useTextures{true};

    std::vector<VkFramebuffer> m_framebuffers;

    // Commands & Sync
    VkCommandPool m_commandPool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> m_commandBuffers;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    uint32_t m_currentFrame{0};

    // Active Tab GPU Geometry Buffers & Submeshes
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_vertexBufferMemory{VK_NULL_HANDLE};
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indexBufferMemory{VK_NULL_HANDLE};
    uint32_t m_indexCount{0};
    std::vector<SubMesh> m_submeshes;
    bool m_isMap{false};

    // 2D UI Sidebar & Top Tab Bar
    VkBuffer m_uiVertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_uiVertexBufferMemory{VK_NULL_HANDLE};
    SidebarExplorer m_sidebar;
    GpuTexture m_sidebarTexture;
    GpuTexture m_tabBarTexture;
    std::vector<uint8_t> m_sidebarPixels;
    std::vector<uint8_t> m_tabBarPixels;

    // Open Tabs (only m_tabs[m_activeTabIndex] is uploaded & rendered on GPU)
    std::vector<OpenTab> m_tabs;
    int m_activeTabIndex{-1};

    // Texturing & Descriptors
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkSampler m_textureSampler{VK_NULL_HANDLE};
    GpuTexture m_defaultTexture;
    std::unordered_map<uint32_t, GpuTexture> m_gpuTextures;
    uint32_t m_doorlineHash{0};

    // Camera & Interaction
    float m_camYaw{0.65f};
    float m_camPitch{0.28f};
    float m_camDist{4.5f};
    float m_baseCamDist{4.5f};
    Vec3 m_camTarget{0.0f, 0.0f, 0.0f};
    bool m_autoRotate{true};
    Vec3 m_carColor{0.10f, 0.45f, 0.95f};
    float m_carMetallic{0.85f};
    int m_activePaintIndex{1};

    bool m_mousePressed{false};
    bool m_rightMousePressed{false};
    double m_lastMouseX{0.0};
    double m_lastMouseY{0.0};

    std::string m_currentBrowseDir{"/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2"};
    std::vector<std::string> m_siblingDirs;
    int m_currentSiblingIndex{-1};

    // Helpers
    void saveActiveTabState();
    void syncSidebarTabs();
    bool createInstance();
    bool selectPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createDepthResources();
    bool createRenderPass();
    bool createDescriptorSetLayout();
    bool createGraphicsPipelines();
    bool createFramebuffers();
    bool createCommandPool();
    bool createTextureSampler();
    bool createDescriptorPool();
    bool createDefaultTexture();
    bool createUiQuadBuffer();
    void updateSidebarTextureIfNeeded();
    bool createGpuTexture(const uint8_t* pixels, uint32_t width, uint32_t height, GpuTexture& outTex, const std::string& name = "");
    bool updateGpuTexture(const uint8_t* pixels, uint32_t width, uint32_t height, GpuTexture& tex, const std::string& name = "");
    void destroyGpuTexture(GpuTexture& tex);
    void rebindPersistentDescriptor(GpuTexture& tex);
    void updateWindowTitle();
    float classifySubmeshMaterial(const SubMesh& sub, VkDescriptorSet& outDescSet) const;

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    bool createSyncObjects();
    void recreateSwapchain();
    void cleanupSwapchain();

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const std::vector<char>& code);

    void drawFrame();
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);

    // Callbacks
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void dropCallback(GLFWwindow* window, int count, const char** paths);
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    bool m_framebufferResized{false};
};

} // namespace NFSU2
