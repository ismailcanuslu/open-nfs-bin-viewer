#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include "MacFileDialog.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace NFSU2 {

std::string MacFileDialog::PickBinOrFolder(const std::string& initialDirectory) {
    char fileBuf[MAX_PATH] = {0};

    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "NFS Archives (*.BIN;*.BUN)\0*.BIN;*.BUN\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "NFSU2 BIN Opener — Select .BIN / .BUN File (Cancel to Pick Folder)";
    if (!initialDirectory.empty() && std::filesystem::exists(initialDirectory)) {
        ofn.lpstrInitialDir = initialDirectory.c_str();
    }
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(fileBuf);
    }

    // Fallback to Folder Browser if user cancelled file dialog to pick a workspace directory
    BROWSEINFOA bi{};
    bi.lpszTitle = "Select Need for Speed: Underground 2 Game Folder or Car Directory:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != nullptr) {
        if (SHGetPathFromIDListA(pidl, fileBuf)) {
            CoTaskMemFree(pidl);
            return std::string(fileBuf);
        }
        CoTaskMemFree(pidl);
    }
    return "";
}

static constexpr float kHeaderStart = 62.0f;
static constexpr float kRowHeight = 24.0f;

SidebarExplorer::SidebarExplorer() {
    std::string defaultRoot = std::filesystem::current_path().string();
    setWorkspaceRoot(defaultRoot);
}

void SidebarExplorer::setWorkspaceRoot(const std::string& rootPath) {
    namespace fs = std::filesystem;
    if (rootPath.empty() || !std::filesystem::exists(rootPath)) return;

    std::error_code ec;
    fs::path p = fs::weakly_canonical(rootPath, ec);
    if (fs::is_regular_file(p, ec)) {
        p = p.parent_path();
    }
    if (p.filename() == "CARS" || p.filename() == "TRACKS" || p.filename() == "GLOBAL" || p.filename() == "FRONTEND") {
        if (fs::exists(p.parent_path() / "CARS", ec)) {
            p = p.parent_path();
        }
    } else if ((p.parent_path().filename() == "CARS" || p.parent_path().filename() == "FRONTEND") &&
               fs::exists(p.parent_path().parent_path() / "CARS", ec)) {
        p = p.parent_path().parent_path();
    }

    m_rootPath = p.string();
    m_expandedDirs.clear();

    if (fs::exists(p / "CARS", ec)) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "CARS", ec).string());
    }
    if (fs::exists(p / "FRONTEND", ec)) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "FRONTEND", ec).string());
        if (fs::exists(p / "FRONTEND" / "PLATFORMS", ec)) {
            m_expandedDirs.insert(fs::weakly_canonical(p / "FRONTEND" / "PLATFORMS", ec).string());
        }
    }
    if (fs::exists(p / "TRACKS", ec)) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "TRACKS", ec).string());
    }

    m_scrollY = 0.0f;
    rebuildTree();
    m_dirty = true;
}

void SidebarExplorer::rebuildTree() {
    m_nodes.clear();
    if (m_rootPath.empty() || !std::filesystem::exists(m_rootPath)) return;
    appendDirectoryNodes(m_rootPath, 0);
    m_dirty = true;
}

void SidebarExplorer::appendDirectoryNodes(const std::string& dirPath, int depth) {
    namespace fs = std::filesystem;
    std::error_code ec;

    std::vector<fs::directory_entry> dirs;
    std::vector<fs::directory_entry> files;

    for (const auto& entry : fs::directory_iterator(dirPath, ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (depth == 0 && (name == "nfsu2_vulkan" || name == "nfsu2_recomp" || name == "build" || name == "reshade-shaders")) {
            continue;
        }

        if (entry.is_directory(ec)) {
            dirs.push_back(entry);
        } else if (entry.is_regular_file(ec)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            if (ext == ".BIN" || ext == ".BUN") {
                files.push_back(entry);
            }
        }
    }

    auto sortByName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(dirs.begin(), dirs.end(), sortByName);
    std::sort(files.begin(), files.end(), sortByName);

    for (const auto& d : dirs) {
        std::string fullPath = fs::weakly_canonical(d.path(), ec).string();
        bool expanded = (m_expandedDirs.count(fullPath) > 0);
        bool hasCarGeom = fs::exists(d.path() / "GEOMETRY.BIN", ec);

        SidebarTreeNode node;
        node.path = fullPath;
        node.name = d.path().filename().string();
        node.depth = depth;
        node.isDirectory = true;
        node.isExpanded = expanded;
        node.hasCarGeometry = hasCarGeom;
        node.failedToOpen = (m_failedPaths.count(fullPath) > 0);
        m_nodes.push_back(node);

        if (expanded) {
            appendDirectoryNodes(fullPath, depth + 1);
        }
    }

    for (const auto& f : files) {
        std::string fullPath = fs::weakly_canonical(f.path(), ec).string();
        SidebarTreeNode node;
        node.path = fullPath;
        node.name = f.path().filename().string();
        node.depth = depth;
        node.isDirectory = false;
        node.isExpanded = false;
        node.hasCarGeometry = false;
        node.failedToOpen = (m_failedPaths.count(fullPath) > 0);
        m_nodes.push_back(node);
    }
}

void SidebarExplorer::setActiveModelInfo(const std::string& modelName, const std::string& activePath,
                                         size_t vertexCount, size_t triangleCount,
                                         size_t submeshCount, size_t textureCount) {
    m_activeModelName = modelName;
    std::error_code ec;
    m_activePath = activePath.empty() ? "" : std::filesystem::weakly_canonical(activePath, ec).string();
    m_vertexCount = vertexCount;
    m_triangleCount = triangleCount;
    m_submeshCount = submeshCount;
    m_textureCount = textureCount;
    m_dirty = true;
}

void SidebarExplorer::setTabs(const std::vector<std::string>& tabTitles, int activeTabIndex) {
    m_tabTitles = tabTitles;
    m_activeTabIndex = activeTabIndex;
    m_tabBarDirty = true;
}

void SidebarExplorer::setError(const std::string& fileName, const std::string& errorReason, const std::string& failedPath) {
    m_errorFile = fileName;
    m_errorReason = errorReason;
    if (!failedPath.empty()) {
        std::error_code ec;
        std::string canon = std::filesystem::weakly_canonical(failedPath, ec).string();
        m_failedPaths.insert(canon);
        for (auto& n : m_nodes) {
            if (n.path == canon) n.failedToOpen = true;
        }
    }
    m_dirty = true;
}

void SidebarExplorer::clearError() {
    if (!m_errorFile.empty() || !m_errorReason.empty()) {
        m_errorFile.clear();
        m_errorReason.clear();
        m_dirty = true;
    }
}

void SidebarExplorer::setRenderState(int wireframeMode, bool useTextures, bool autoRotate, int activePaintIndex,
                                     float camDist, float baseCamDist) {
    bool zoomChanged = (std::abs(m_camDist - camDist) > 0.005f || std::abs(m_baseCamDist - baseCamDist) > 0.005f);
    if (m_wireframeMode != wireframeMode || m_useTextures != useTextures ||
        m_autoRotate != autoRotate || m_activePaintIndex != activePaintIndex || zoomChanged) {
        m_wireframeMode = wireframeMode;
        m_useTextures = useTextures;
        m_autoRotate = autoRotate;
        m_activePaintIndex = activePaintIndex;
        m_camDist = camDist;
        m_baseCamDist = std::max(0.1f, baseCamDist);
        m_tabBarDirty = true;
    }
}

float SidebarExplorer::computeTabWidth(int windowWidth) const {
    float availW = std::max(200.0f, static_cast<float>(windowWidth - SIDEBAR_WIDTH) - 12.0f);
    if (m_tabTitles.empty()) return 165.0f;
    float w = availW / static_cast<float>(m_tabTitles.size());
    return std::clamp(w, 105.0f, 185.0f);
}

bool SidebarExplorer::onMouseMove(double x, double y, int windowWidth, int windowHeight) {
    int newHoveredRow = -1;
    int newHoveredBtn = -1;
    int newHoveredTab = -1;
    bool newHoveredClose = false;
    int newHoveredVpBtn = -1;

    if (x >= 0 && x < SIDEBAR_WIDTH) {
        float bottomH = m_errorReason.empty() ? 58.0f : 136.0f;
        float treeEnd = std::max(kHeaderStart + 20.0f, (float)windowHeight - bottomH);

        if (y >= 7 && y <= 31 && x >= 184 && x <= 302) {
            newHoveredBtn = 1;
        } else if (y >= kHeaderStart && y < treeEnd) {
            float relY = (float)y - kHeaderStart + m_scrollY;
            int idx = static_cast<int>(relY / kRowHeight);
            if (idx >= 0 && idx < static_cast<int>(m_nodes.size())) {
                newHoveredRow = idx;
            }
        }
    } else if (x >= SIDEBAR_WIDTH && y >= 0 && y < TAB_BAR_HEIGHT) {
        float relX = static_cast<float>(x - SIDEBAR_WIDTH);
        if (y < 36.0 && !m_tabTitles.empty()) {
            float tabW = computeTabWidth(windowWidth);
            int tidx = static_cast<int>(relX / tabW);
            if (tidx >= 0 && tidx < static_cast<int>(m_tabTitles.size())) {
                newHoveredTab = tidx;
                float tabLocalX = relX - tidx * tabW;
                if (tabLocalX >= tabW - 26.0f && tabLocalX <= tabW - 4.0f && m_tabTitles.size() > 1) {
                    newHoveredClose = true;
                }
            }
        } else if (y >= 38.0 && y <= 74.0) {
            if (relX >= 10.0f && relX <= 38.0f) newHoveredVpBtn = 1;
            else if (relX >= 42.0f && relX <= 162.0f) newHoveredVpBtn = 2;
            else if (relX >= 166.0f && relX <= 194.0f) newHoveredVpBtn = 3;
            else if (relX >= 202.0f && relX <= 298.0f) newHoveredVpBtn = 4;
            else if (relX >= 308.0f && relX <= 424.0f) newHoveredVpBtn = 5;
            else if (relX >= 430.0f && relX <= 524.0f) newHoveredVpBtn = 6;
            else if (relX >= 530.0f && relX <= 618.0f) newHoveredVpBtn = 7;
            else {
                for (int i = 0; i < 7; ++i) {
                    float sx = 668.0f + i * 30.0f;
                    if (relX >= sx && relX <= sx + 24.0f) {
                        newHoveredVpBtn = 10 + i;
                        break;
                    }
                }
            }
        }
    }

    if (newHoveredRow != m_hoveredRow || newHoveredBtn != m_hoveredButton) {
        m_hoveredRow = newHoveredRow;
        m_hoveredButton = newHoveredBtn;
        m_dirty = true;
    }
    if (newHoveredTab != m_hoveredTab || newHoveredClose != m_hoveredTabClose || newHoveredVpBtn != m_hoveredViewportBtn) {
        m_hoveredTab = newHoveredTab;
        m_hoveredTabClose = newHoveredClose;
        m_hoveredViewportBtn = newHoveredVpBtn;
        m_tabBarDirty = true;
    }
    return (x < SIDEBAR_WIDTH || y < TAB_BAR_HEIGHT);
}

bool SidebarExplorer::onScroll(double x, double y, double scrollY, int windowHeight) {
    (void)y;
    if (x < 0 || x >= SIDEBAR_WIDTH) return false;

    float bottomH = m_errorReason.empty() ? 58.0f : 136.0f;
    float treeViewHeight = std::max(40.0f, (float)windowHeight - kHeaderStart - bottomH);
    float maxScroll = std::max(0.0f, static_cast<float>(m_nodes.size()) * kRowHeight - treeViewHeight + 12.0f);

    m_scrollY = std::clamp(m_scrollY - static_cast<float>(scrollY) * 28.0f, 0.0f, maxScroll);
    m_dirty = true;
    return true;
}

SidebarAction SidebarExplorer::onMouseClick(double x, double y, int windowWidth, int windowHeight) {
    SidebarAction action;

    if (x >= SIDEBAR_WIDTH && y >= 0 && y < TAB_BAR_HEIGHT) {
        float relX = static_cast<float>(x - SIDEBAR_WIDTH);
        if (y < 36.0 && !m_tabTitles.empty()) {
            float tabW = computeTabWidth(windowWidth);
            int tidx = static_cast<int>(relX / tabW);
            if (tidx >= 0 && tidx < static_cast<int>(m_tabTitles.size())) {
                float tabLocalX = relX - tidx * tabW;
                if (tabLocalX >= tabW - 26.0f && tabLocalX <= tabW - 4.0f && m_tabTitles.size() > 1) {
                    action.type = SidebarActionType::CloseTab;
                    action.intParam = tidx;
                } else {
                    action.type = SidebarActionType::SwitchTab;
                    action.intParam = tidx;
                }
            }
            return action;
        }
        if (y >= 36.0 && y < TAB_BAR_HEIGHT) {
            if (relX >= 10.0f && relX <= 38.0f) action.type = SidebarActionType::ZoomOut;
            else if (relX >= 42.0f && relX <= 162.0f) action.type = SidebarActionType::ResetZoom;
            else if (relX >= 166.0f && relX <= 194.0f) action.type = SidebarActionType::ZoomIn;
            else if (relX >= 202.0f && relX <= 298.0f) action.type = SidebarActionType::ResetCamera;
            else if (relX >= 308.0f && relX <= 424.0f) action.type = SidebarActionType::ToggleWireframe;
            else if (relX >= 430.0f && relX <= 524.0f) action.type = SidebarActionType::ToggleTextures;
            else if (relX >= 530.0f && relX <= 618.0f) action.type = SidebarActionType::ToggleAutoRotate;
            else {
                for (int i = 0; i < 7; ++i) {
                    float sx = 668.0f + i * 30.0f;
                    if (relX >= sx && relX <= sx + 24.0f) {
                        action.type = SidebarActionType::SelectPaintColor;
                        action.intParam = i + 1;
                        break;
                    }
                }
            }
            return action;
        }
    }

    if (x < 0 || x >= SIDEBAR_WIDTH) return action;

    if (y >= 7 && y <= 31 && x >= 184 && x <= 302) {
        action.type = SidebarActionType::OpenFolderDialog;
        return action;
    }

    float bottomH = m_errorReason.empty() ? 58.0f : 136.0f;
    if (!m_errorReason.empty()) {
        float errTop = (float)windowHeight - bottomH + 6.0f;
        if (y >= errTop && y <= errTop + 24.0f && x >= SIDEBAR_WIDTH - 36.0f) {
            clearError();
            return action;
        }
    }

    float treeEnd = std::max(kHeaderStart + 20.0f, (float)windowHeight - bottomH);
    if (y >= kHeaderStart && y < treeEnd) {
        float relY = (float)y - kHeaderStart + m_scrollY;
        int idx = static_cast<int>(relY / kRowHeight);
        if (idx >= 0 && idx < static_cast<int>(m_nodes.size())) {
            SidebarTreeNode clicked = m_nodes[idx];
            float indentX = 10.0f + clicked.depth * 14.0f;

            if (clicked.isDirectory) {
                bool clickedArrowOnly = (x < indentX + 18.0f);
                if (m_expandedDirs.count(clicked.path)) {
                    if (clickedArrowOnly || !clicked.hasCarGeometry) {
                        m_expandedDirs.erase(clicked.path);
                    }
                } else {
                    m_expandedDirs.insert(clicked.path);
                }
                rebuildTree();

                if (clicked.hasCarGeometry && !clickedArrowOnly) {
                    clearError();
                    action.type = SidebarActionType::LoadPath;
                    action.path = clicked.path;
                }
            } else {
                clearError();
                action.type = SidebarActionType::LoadPath;
                action.path = clicked.path;
            }
            return action;
        }
    }

    return action;
}

namespace {

struct WinDibSurface {
    HDC hdc{nullptr};
    HBITMAP hbm{nullptr};
    HGDIOBJ oldBm{nullptr};
    uint8_t* bits{nullptr};
    int width{0};
    int height{0};

    WinDibSurface(int w, int h) : width(w), height(h) {
        hdc = CreateCompatibleDC(nullptr);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        oldBm = SelectObject(hdc, hbm);
        SetBkMode(hdc, TRANSPARENT);
    }

    ~WinDibSurface() {
        if (hdc) {
            SelectObject(hdc, oldBm);
            if (hbm) DeleteObject(hbm);
            DeleteDC(hdc);
        }
    }

    void fillRect(int x, int y, int w, int h, int r, int g, int b) {
        RECT rc{x, y, x + w, y + h};
        HBRUSH br = CreateSolidBrush(RGB(r, g, b));
        FillRect(hdc, &rc, br);
        DeleteObject(br);
    }

    void drawBox(int x, int y, int w, int h, int r, int g, int b, int brR, int brG, int brB) {
        fillRect(x, y, w, h, r, g, b);
        RECT rc{x, y, x + w, y + h};
        HBRUSH frameBr = CreateSolidBrush(RGB(brR, brG, brB));
        FrameRect(hdc, &rc, frameBr);
        DeleteObject(frameBr);
    }

    void drawText(int x, int y, int w, int h, const char* text, HFONT font, int r, int g, int b, UINT align = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS) {
        HGDIOBJ oldFont = SelectObject(hdc, font);
        SetTextColor(hdc, RGB(r, g, b));
        RECT rc{x, y, x + w, y + h};
        DrawTextA(hdc, text, -1, &rc, align);
        SelectObject(hdc, oldFont);
    }

    void copyToRgba(std::vector<uint8_t>& outRgba) {
        size_t total = static_cast<size_t>(width) * height * 4;
        outRgba.resize(total);
        for (size_t i = 0; i < total; i += 4) {
            outRgba[i + 0] = bits[i + 2]; // R
            outRgba[i + 1] = bits[i + 1]; // G
            outRgba[i + 2] = bits[i + 0]; // B
            outRgba[i + 3] = 255;         // A
        }
    }
};

} // namespace

void SidebarExplorer::renderToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba) {
    if (pixelWidth == 0 || pixelHeight == 0) return;

    WinDibSurface surf(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight));
    int W = static_cast<int>(pixelWidth);
    int H = static_cast<int>(pixelHeight);

    HFONT fontBold = CreateFontA(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT fontReg  = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    surf.fillRect(0, 0, W, H, 24, 24, 24);
    surf.fillRect(0, 0, W, 36, 32, 32, 33);
    surf.drawText(12, 0, 160, 36, "EXPLORER", fontBold, 187, 187, 187);

    surf.drawBox(184, 6, 118, 24, (m_hoveredButton == 1) ? 17 : 14, (m_hoveredButton == 1) ? 119 : 99, (m_hoveredButton == 1) ? 187 : 156, 0, 127, 212);
    surf.drawText(184, 6, 118, 24, "Open Folder...", fontBold, 255, 255, 255, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    surf.fillRect(0, 36, W, 24, 37, 37, 38);
    std::string rootFolderName = std::filesystem::path(m_rootPath).filename().string();
    std::transform(rootFolderName.begin(), rootFolderName.end(), rootFolderName.begin(), ::toupper);
    std::string wsHeader = "v  " + rootFolderName;
    surf.drawText(10, 36, W - 20, 24, wsHeader.c_str(), fontBold, 204, 204, 204);

    int bottomH = m_errorReason.empty() ? 58 : 136;
    int treeEnd = std::max(static_cast<int>(kHeaderStart) + 20, H - bottomH);

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        int rowY = static_cast<int>(kHeaderStart + i * kRowHeight - m_scrollY);
        if (rowY + static_cast<int>(kRowHeight) < static_cast<int>(kHeaderStart) || rowY >= treeEnd) continue;

        const auto& node = m_nodes[i];
        bool isSelected = (!m_activePath.empty() && node.path == m_activePath);
        bool isHovered = (static_cast<int>(i) == m_hoveredRow);

        if (isSelected) {
            surf.fillRect(0, rowY, W, static_cast<int>(kRowHeight), 9, 71, 113);
            surf.fillRect(0, rowY, 3, static_cast<int>(kRowHeight), 0, 127, 212);
        } else if (isHovered) {
            surf.fillRect(0, rowY, W, static_cast<int>(kRowHeight), 42, 45, 46);
        }

        int indentX = 10 + node.depth * 14;
        std::string label = (node.isDirectory ? (node.isExpanded ? "v " : "> ") : "  ") + node.name;
        surf.drawText(indentX, rowY, W - indentX - 8, static_cast<int>(kRowHeight), label.c_str(),
                      (node.hasCarGeometry || isSelected) ? fontBold : fontReg,
                      node.failedToOpen ? 244 : 220, node.failedToOpen ? 135 : 225, node.failedToOpen ? 113 : 230);
    }

    surf.fillRect(0, H - 50, W, 50, 0, 122, 204);
    std::string activeTitle = "Active Tab: " + m_activeModelName;
    surf.drawText(10, H - 46, W - 20, 20, activeTitle.c_str(), fontBold, 255, 255, 255);

    char statsBuf[160];
    std::snprintf(statsBuf, sizeof(statsBuf), "%zu verts | %zu tris | %zu submeshes | %zu tex",
                  m_vertexCount, m_triangleCount, m_submeshCount, m_textureCount);
    surf.drawText(10, H - 24, W - 20, 18, statsBuf, fontReg, 225, 240, 255);

    DeleteObject(fontBold);
    DeleteObject(fontReg);

    surf.copyToRgba(outRgba);
    m_dirty = false;
}

void SidebarExplorer::renderTabBarToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba) {
    if (pixelWidth == 0 || pixelHeight == 0) return;

    WinDibSurface surf(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight));
    int W = static_cast<int>(pixelWidth);
    int H = static_cast<int>(pixelHeight);

    HFONT fontBold = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    surf.fillRect(0, 0, W, 36, 37, 37, 38);
    int tabW = m_tabTitles.empty() ? 165 : std::clamp(static_cast<int>((W - 12) / m_tabTitles.size()), 105, 185);
    for (size_t i = 0; i < m_tabTitles.size(); ++i) {
        int tx = static_cast<int>(i) * tabW;
        bool isActive = (static_cast<int>(i) == m_activeTabIndex);
        surf.fillRect(tx, 0, tabW - 1, 35, isActive ? 24 : 43, isActive ? 26 : 43, isActive ? 31 : 45);
        if (isActive) surf.fillRect(tx, 0, tabW - 1, 3, 0, 122, 204);
        surf.drawText(tx + 12, 0, tabW - 32, 35, m_tabTitles[i].c_str(), fontBold, 240, 240, 245);
        if (m_tabTitles.size() > 1) {
            surf.drawText(tx + tabW - 22, 0, 18, 35, "X", fontBold, 190, 190, 195, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    surf.fillRect(0, 36, W, H - 36, 28, 30, 35);
    auto drawBtn = [&](int bx, int by, int bw, int bh, const char* text, bool active) {
        surf.drawBox(bx, by, bw, bh, active ? 14 : 43, active ? 99 : 46, active ? 156 : 52, active ? 56 : 72, active ? 165 : 76, active ? 245 : 86);
        surf.drawText(bx, by, bw, bh, text, fontBold, 242, 244, 248, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    float zoomPct = std::clamp((m_baseCamDist / std::max(0.02f, m_camDist)) * 100.0f, 1.0f, 9999.0f);
    char zoomBuf[64];
    std::snprintf(zoomBuf, sizeof(zoomBuf), "Zoom %.0f%% (%.1fm)", zoomPct, m_camDist);

    drawBtn(10, 44, 28, 24, "-", false);
    drawBtn(42, 44, 120, 24, zoomBuf, false);
    drawBtn(166, 44, 28, 24, "+", false);
    drawBtn(202, 44, 96, 24, "Reset View", false);
    drawBtn(308, 44, 116, 24, (m_wireframeMode == 0) ? "Z: Solid Only" : (m_wireframeMode == 1) ? "Z: Solid+Wire" : "Z: Wire Only", m_wireframeMode > 0);
    drawBtn(430, 44, 94, 24, m_useTextures ? "T: Tex ON" : "T: Tex OFF", m_useTextures);
    drawBtn(530, 44, 88, 24, m_autoRotate ? "Rotate" : "Paused", m_autoRotate);

    const int swatchColors[7][3] = {
        {26, 115, 242}, {38, 235, 56}, {245, 97, 13}, {224, 13, 38},
        {28, 28, 34}, {242, 242, 245}, {191, 26, 217}
    };
    for (int i = 0; i < 7; ++i) {
        int sx = 668 + i * 30;
        surf.drawBox(sx, 46, 24, 20, swatchColors[i][0], swatchColors[i][1], swatchColors[i][2],
                     (m_activePaintIndex == i + 1) ? 255 : 80, (m_activePaintIndex == i + 1) ? 255 : 84, (m_activePaintIndex == i + 1) ? 255 : 92);
    }

    DeleteObject(fontBold);
    surf.copyToRgba(outRgba);
    m_tabBarDirty = false;
}

} // namespace NFSU2
#endif

