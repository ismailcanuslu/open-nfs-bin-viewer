#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>

namespace NFSU2 {

enum class SidebarActionType {
    None,
    LoadPath,
    OpenFolderDialog,
    ToggleWireframe,
    ToggleTextures,
    ToggleAutoRotate,
    SelectPaintColor,
    SwitchTab,
    CloseTab,
    ZoomIn,
    ZoomOut,
    ResetZoom,
    ResetCamera
};

struct SidebarAction {
    SidebarActionType type{SidebarActionType::None};
    std::string path;
    int intParam{0};
};

struct SidebarTreeNode {
    std::string path;
    std::string name;
    int depth{0};
    bool isDirectory{false};
    bool isExpanded{false};
    bool hasCarGeometry{false};
    bool failedToOpen{false};
};

class MacFileDialog {
public:
    static std::string PickBinOrFolder(const std::string& initialDirectory = "");
};

class SidebarExplorer {
public:
    static constexpr int SIDEBAR_WIDTH = 310;
    static constexpr int TAB_BAR_HEIGHT = 76; // 36px Multi-Tab Bar + 40px Blender 3D Viewport Header Bar

    SidebarExplorer();

    void setWorkspaceRoot(const std::string& rootPath);
    std::string getWorkspaceRoot() const { return m_rootPath; }

    void setActiveModelInfo(const std::string& modelName, const std::string& activePath,
                            size_t vertexCount, size_t triangleCount,
                            size_t submeshCount, size_t textureCount);
    void setTabs(const std::vector<std::string>& tabTitles, int activeTabIndex);
    void setError(const std::string& fileName, const std::string& errorReason, const std::string& failedPath = "");
    void clearError();

    void setRenderState(int wireframeMode, bool useTextures, bool autoRotate, int activePaintIndex,
                        float camDist = 4.5f, float baseCamDist = 4.5f);

    // Mouse / Scroll events (coordinates in logical window pixels)
    bool onMouseMove(double x, double y, int windowWidth, int windowHeight);
    SidebarAction onMouseClick(double x, double y, int windowWidth, int windowHeight);
    bool onScroll(double x, double y, double scrollY, int windowHeight);

    bool isDirty() const { return m_dirty; }
    bool isTabBarDirty() const { return m_tabBarDirty; }
    void markDirty() { m_dirty = true; m_tabBarDirty = true; }

    // Renders the left sidebar at pixel dimensions (pixelWidth x pixelHeight) into outRgba (4 bytes/pixel)
    void renderToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba);

    // Renders the top tab bar at pixel dimensions (pixelWidth x pixelHeight) into outRgba (4 bytes/pixel)
    void renderTabBarToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba);

private:
    float computeTabWidth(int windowWidth) const;
    void rebuildTree();
    void appendDirectoryNodes(const std::string& dirPath, int depth);

    std::string m_rootPath;
    std::vector<SidebarTreeNode> m_nodes;
    std::unordered_set<std::string> m_expandedDirs;
    std::unordered_set<std::string> m_failedPaths;

    std::vector<std::string> m_tabTitles;
    int m_activeTabIndex{0};
    int m_hoveredTab{-1};
    bool m_hoveredTabClose{false};

    std::string m_activeModelName{"None"};
    std::string m_activePath;
    size_t m_vertexCount{0};
    size_t m_triangleCount{0};
    size_t m_submeshCount{0};
    size_t m_textureCount{0};

    std::string m_errorFile;
    std::string m_errorReason;

    int m_wireframeMode{1};
    bool m_useTextures{true};
    bool m_autoRotate{true};
    int m_activePaintIndex{1};
    float m_camDist{4.5f};
    float m_baseCamDist{4.5f};

    float m_scrollY{0.0f};
    int m_hoveredRow{-1};
    int m_hoveredButton{-1};
    int m_hoveredViewportBtn{-1};
    bool m_dirty{true};
    bool m_tabBarDirty{true};
};

} // namespace NFSU2
