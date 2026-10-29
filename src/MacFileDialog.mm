#import <Cocoa/Cocoa.h>
#include "MacFileDialog.hpp"
#include <filesystem>
#include <algorithm>
#include <cstring>

namespace NFSU2 {

std::string MacFileDialog::PickBinOrFolder(const std::string& initialDirectory) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];

        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:@"NFSU2 BIN Opener — Select Workspace Folder or .BIN / .BUN File"];
        [panel setMessage:@"Select the game root folder, a car folder (e.g. CARS/350Z), or any .BIN / .BUN file."];
        [panel setPrompt:@"Open in Viewer"];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setResolvesAliases:YES];

        if (!initialDirectory.empty() && std::filesystem::exists(initialDirectory)) {
            NSString* dirStr = [NSString stringWithUTF8String:initialDirectory.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:dirStr isDirectory:YES]];
        }

        NSModalResponse response = [panel runModal];
        if (response == NSModalResponseOK) {
            NSURL* url = [[panel URLs] firstObject];
            if (url && [url isFileURL]) {
                const char* utf8Path = [[url path] UTF8String];
                if (utf8Path) {
                    return std::string(utf8Path);
                }
            }
        }
        return "";
    }
}

static constexpr float kHeaderStart = 62.0f;
static constexpr float kRowHeight = 24.0f;

SidebarExplorer::SidebarExplorer() {
    std::string defaultRoot = "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2";
    if (!std::filesystem::exists(defaultRoot)) {
        defaultRoot = std::filesystem::current_path().string();
    }
    setWorkspaceRoot(defaultRoot);
}

void SidebarExplorer::setWorkspaceRoot(const std::string& rootPath) {
    namespace fs = std::filesystem;
    if (rootPath.empty() || !std::filesystem::exists(rootPath)) return;

    fs::path p = fs::weakly_canonical(rootPath);
    if (fs::is_regular_file(p)) {
        p = p.parent_path();
    }
    if (p.filename() == "CARS" || p.filename() == "TRACKS" || p.filename() == "GLOBAL" || p.filename() == "FRONTEND") {
        if (fs::exists(p.parent_path() / "CARS")) {
            p = p.parent_path();
        }
    } else if ((p.parent_path().filename() == "CARS" || p.parent_path().filename() == "FRONTEND") &&
               fs::exists(p.parent_path().parent_path() / "CARS")) {
        p = p.parent_path().parent_path();
    }

    m_rootPath = p.string();
    m_expandedDirs.clear();

    if (fs::exists(p / "CARS")) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "CARS").string());
    }
    if (fs::exists(p / "FRONTEND")) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "FRONTEND").string());
        if (fs::exists(p / "FRONTEND" / "PLATFORMS")) {
            m_expandedDirs.insert(fs::weakly_canonical(p / "FRONTEND" / "PLATFORMS").string());
        }
    }
    if (fs::exists(p / "TRACKS")) {
        m_expandedDirs.insert(fs::weakly_canonical(p / "TRACKS").string());
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
        if (depth == 0 && (name == "nfsu2_vulkan" || name == "nfsu2_recomp" ||
                           name.find(".app") != std::string::npos || name == "reshade-shaders")) {
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
            if (relX >= 10.0f && relX <= 38.0f) newHoveredVpBtn = 1;        // Zoom Out [-]
            else if (relX >= 42.0f && relX <= 162.0f) newHoveredVpBtn = 2;  // Zoom Readout (Click to 100%)
            else if (relX >= 166.0f && relX <= 194.0f) newHoveredVpBtn = 3; // Zoom In [+]
            else if (relX >= 202.0f && relX <= 298.0f) newHoveredVpBtn = 4; // Reset View
            else if (relX >= 308.0f && relX <= 424.0f) newHoveredVpBtn = 5; // Wireframe Mode
            else if (relX >= 430.0f && relX <= 524.0f) newHoveredVpBtn = 6; // Textures ON/OFF
            else if (relX >= 530.0f && relX <= 618.0f) newHoveredVpBtn = 7; // Auto-Rotate ON/OFF
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

    // Top Tab Bar + Blender 3D Viewport Bar Clicks (x >= SIDEBAR_WIDTH, y < TAB_BAR_HEIGHT)
    if (x >= SIDEBAR_WIDTH && y >= 0 && y < TAB_BAR_HEIGHT) {
        float relX = static_cast<float>(x - SIDEBAR_WIDTH);

        // Row 1 (y = 0..36): Chrome / VS Code Multi-Tab Bar
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

        // Row 2 (y = 36..76): Blender-Style 3D Viewport Controls Bar
        if (y >= 36.0 && y < TAB_BAR_HEIGHT) {
            if (relX >= 10.0f && relX <= 38.0f) {
                action.type = SidebarActionType::ZoomOut;
            } else if (relX >= 42.0f && relX <= 162.0f) {
                action.type = SidebarActionType::ResetZoom;
            } else if (relX >= 166.0f && relX <= 194.0f) {
                action.type = SidebarActionType::ZoomIn;
            } else if (relX >= 202.0f && relX <= 298.0f) {
                action.type = SidebarActionType::ResetCamera;
            } else if (relX >= 308.0f && relX <= 424.0f) {
                action.type = SidebarActionType::ToggleWireframe;
            } else if (relX >= 430.0f && relX <= 524.0f) {
                action.type = SidebarActionType::ToggleTextures;
            } else if (relX >= 530.0f && relX <= 618.0f) {
                action.type = SidebarActionType::ToggleAutoRotate;
            } else {
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

    // 1. Header Open Folder button
    if (y >= 7 && y <= 31 && x >= 184 && x <= 302) {
        action.type = SidebarActionType::OpenFolderDialog;
        return action;
    }

    // 2. Error Banner Dismiss button
    float bottomH = m_errorReason.empty() ? 58.0f : 136.0f;
    if (!m_errorReason.empty()) {
        float errTop = (float)windowHeight - bottomH + 6.0f;
        if (y >= errTop && y <= errTop + 24.0f && x >= SIDEBAR_WIDTH - 36.0f) {
            clearError();
            return action;
        }
    }

    // 3. File Tree Rows
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

static inline NSRect topRect(float x, float y, float w, float h, float H) {
    return NSMakeRect(x, H - y - h, w, h);
}

static inline NSColor* rgb(int r, int g, int b, float a = 1.0f) {
    return [NSColor colorWithSRGBRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a];
}

void SidebarExplorer::renderToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba) {
    if (pixelWidth == 0 || pixelHeight == 0) return;

    @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:pixelWidth
                          pixelsHigh:pixelHeight
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                        bitmapFormat:0
                         bytesPerRow:pixelWidth * 4
                        bitsPerPixel:32];

        NSGraphicsContext* nsCtx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsCtx];

        CGContextRef ctx = [nsCtx CGContext];
        float scale = static_cast<float>(pixelWidth) / static_cast<float>(SIDEBAR_WIDTH);
        CGContextScaleCTM(ctx, scale, scale);

        float W = static_cast<float>(SIDEBAR_WIDTH);
        float H = static_cast<float>(pixelHeight) / scale;

        // 1. Sidebar Background (#181818 VS Code Dark+ Sidebar)
        [rgb(24, 24, 24) setFill];
        NSRectFill(NSMakeRect(0, 0, W, H));

        NSFont* fontBold11 = [NSFont systemFontOfSize:11.0 weight:NSFontWeightBold];
        NSFont* fontSemi11 = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
        NSFont* fontReg11  = [NSFont systemFontOfSize:11.5 weight:NSFontWeightRegular];
        NSFont* fontMono9  = [NSFont monospacedSystemFontOfSize:9.5 weight:NSFontWeightBold];
        NSFont* fontSmall  = [NSFont systemFontOfSize:10.5 weight:NSFontWeightRegular];

        // 2. Top Header Bar (y = 0..36)
        [rgb(32, 32, 33) setFill];
        NSRectFill(topRect(0, 0, W, 36, H));

        [@"EXPLORER" drawInRect:topRect(12, 10, 160, 18, H)
                 withAttributes:@{NSFontAttributeName: fontBold11,
                                  NSForegroundColorAttributeName: rgb(187, 187, 187)}];

        NSColor* openBtnColor = (m_hoveredButton == 1) ? rgb(17, 119, 187) : rgb(14, 99, 156);
        NSBezierPath* openBtnPath = [NSBezierPath bezierPathWithRoundedRect:topRect(184, 6, 118, 24, H) xRadius:4 yRadius:4];
        [openBtnColor setFill];
        [openBtnPath fill];
        [@"Open Folder..." drawInRect:topRect(195, 10, 102, 18, H)
                       withAttributes:@{NSFontAttributeName: fontSemi11,
                                        NSForegroundColorAttributeName: [NSColor whiteColor]}];

        // 3. Workspace Header Bar (y = 36..60)
        [rgb(37, 37, 38) setFill];
        NSRectFill(topRect(0, 36, W, 24, H));

        std::string rootFolderName = std::filesystem::path(m_rootPath).filename().string();
        std::transform(rootFolderName.begin(), rootFolderName.end(), rootFolderName.begin(), ::toupper);
        NSString* wsHeader = [NSString stringWithFormat:@"▾  %s", rootFolderName.c_str()];
        [wsHeader drawInRect:topRect(10, 40, W - 20, 18, H)
              withAttributes:@{NSFontAttributeName: fontBold11,
                               NSForegroundColorAttributeName: rgb(204, 204, 204)}];

        // 4. Scrollable File Tree Area
        float bottomH = m_errorReason.empty() ? 58.0f : 136.0f;
        float treeEnd = std::max(kHeaderStart + 20.0f, H - bottomH);
        float treeHeight = treeEnd - kHeaderStart;

        [NSGraphicsContext saveGraphicsState];
        NSRectClip(topRect(0, kHeaderStart, W, treeHeight, H));

        for (size_t i = 0; i < m_nodes.size(); ++i) {
            float rowY = kHeaderStart + i * kRowHeight - m_scrollY;
            if (rowY + kRowHeight < kHeaderStart || rowY > treeEnd) continue;

            const auto& node = m_nodes[i];
            bool isSelected = (!m_activePath.empty() && node.path == m_activePath);
            bool isHovered = (static_cast<int>(i) == m_hoveredRow);

            if (isSelected) {
                [rgb(9, 71, 113) setFill];
                NSRectFill(topRect(0, rowY, W, kRowHeight, H));
                [rgb(0, 127, 212) setFill];
                NSRectFill(topRect(0, rowY, 3, kRowHeight, H));
            } else if (isHovered) {
                [rgb(42, 45, 46) setFill];
                NSRectFill(topRect(0, rowY, W, kRowHeight, H));
            }

            float indentX = 10.0f + node.depth * 14.0f;

            if (node.isDirectory) {
                NSString* arrow = node.isExpanded ? @"▾" : @"▸";
                [arrow drawInRect:topRect(indentX, rowY + 4, 14, 18, H)
                   withAttributes:@{NSFontAttributeName: fontReg11,
                                    NSForegroundColorAttributeName: rgb(190, 190, 190)}];
            }

            float badgeX = indentX + 14.0f;
            NSString* badgeText = @"BIN";
            NSColor* badgeBg = rgb(58, 62, 70);
            NSColor* badgeFg = rgb(210, 215, 225);

            if (node.failedToOpen) {
                badgeText = @" ! ";
                badgeBg = rgb(140, 38, 38);
                badgeFg = rgb(255, 220, 220);
            } else if (node.isDirectory && node.hasCarGeometry) {
                badgeText = @"CAR";
                badgeBg = rgb(12, 105, 130);
                badgeFg = rgb(160, 240, 255);
            } else if (node.isDirectory) {
                badgeText = @"DIR";
                badgeBg = rgb(75, 65, 35);
                badgeFg = rgb(255, 215, 120);
            } else if (node.name.find("GEOMETRY") != std::string::npos ||
                       node.name.find("STREAM") != std::string::npos) {
                badgeText = @"3D ";
                badgeBg = rgb(22, 110, 65);
                badgeFg = rgb(165, 250, 195);
            } else if (node.name.find("TRACKMAP") != std::string::npos) {
                badgeText = @"MAP";
                badgeBg = rgb(15, 95, 125);
                badgeFg = rgb(150, 235, 255);
            } else if (node.name.find("TEXTURE") != std::string::npos ||
                       node.name.find("VINYL") != std::string::npos ||
                       node.name.find("DYNTEX") != std::string::npos) {
                badgeText = @"TEX";
                badgeBg = rgb(95, 50, 115);
                badgeFg = rgb(235, 190, 255);
            }

            NSBezierPath* badgePath = [NSBezierPath bezierPathWithRoundedRect:topRect(badgeX, rowY + 4, 28, 15, H) xRadius:3 yRadius:3];
            [badgeBg setFill];
            [badgePath fill];
            [badgeText drawInRect:topRect(badgeX + 3, rowY + 5, 24, 14, H)
                   withAttributes:@{NSFontAttributeName: fontMono9,
                                    NSForegroundColorAttributeName: badgeFg}];

            float textX = badgeX + 34.0f;
            NSColor* nameColor = node.failedToOpen ? rgb(244, 135, 113)
                               : isSelected        ? [NSColor whiteColor]
                               : node.hasCarGeometry ? rgb(235, 245, 255)
                                                   : rgb(212, 212, 212);
            NSString* nsName = [NSString stringWithUTF8String:node.name.c_str()];
            [nsName drawInRect:topRect(textX, rowY + 4, W - textX - 10, 18, H)
                withAttributes:@{NSFontAttributeName: (node.hasCarGeometry || isSelected) ? fontSemi11 : fontReg11,
                                 NSForegroundColorAttributeName: nameColor}];
        }

        float totalContentHeight = static_cast<float>(m_nodes.size()) * kRowHeight;
        if (totalContentHeight > treeHeight && treeHeight > 20.0f) {
            float thumbH = std::max(24.0f, (treeHeight / totalContentHeight) * treeHeight);
            float maxScroll = totalContentHeight - treeHeight;
            float thumbY = kHeaderStart + (m_scrollY / std::max(1.0f, maxScroll)) * (treeHeight - thumbH);
            NSBezierPath* sb = [NSBezierPath bezierPathWithRoundedRect:topRect(W - 7, thumbY, 4, thumbH, H) xRadius:2 yRadius:2];
            [rgb(120, 120, 125, 0.55f) setFill];
            [sb fill];
        }

        [NSGraphicsContext restoreGraphicsState];

        // 7. Bottom Status & Warning Panel
        [rgb(30, 30, 32) setFill];
        NSRectFill(topRect(0, treeEnd, W, bottomH, H));
        [rgb(55, 55, 60) setFill];
        NSRectFill(topRect(0, treeEnd, W, 1, H));

        if (!m_errorReason.empty()) {
            float errY = treeEnd + 6.0f;
            float errH = 72.0f;
            NSBezierPath* errBox = [NSBezierPath bezierPathWithRoundedRect:topRect(8, errY, W - 16, errH, H) xRadius:5 yRadius:5];
            [rgb(90, 29, 29) setFill];
            [errBox fill];
            [rgb(241, 76, 76) setStroke];
            [errBox setLineWidth:1.2];
            [errBox stroke];

            NSString* errTitle = [NSString stringWithFormat:@"⚠ Cannot Open: %s", m_errorFile.c_str()];
            [errTitle drawInRect:topRect(16, errY + 6, W - 48, 16, H)
                  withAttributes:@{NSFontAttributeName: fontBold11,
                                   NSForegroundColorAttributeName: rgb(255, 185, 175)}];

            [@"✕" drawInRect:topRect(W - 28, errY + 6, 16, 16, H)
              withAttributes:@{NSFontAttributeName: fontBold11,
                               NSForegroundColorAttributeName: rgb(255, 185, 175)}];

            NSString* errBody = [NSString stringWithUTF8String:m_errorReason.c_str()];
            [errBody drawInRect:topRect(16, errY + 24, W - 32, 44, H)
                 withAttributes:@{NSFontAttributeName: fontSmall,
                                  NSForegroundColorAttributeName: rgb(245, 225, 225)}];
        }

        float statY = H - 50.0f;
        [rgb(0, 122, 204) setFill];
        NSRectFill(topRect(0, statY, W, 50.0f, H));

        NSString* activeTitle = [NSString stringWithFormat:@"Active Tab: %s", m_activeModelName.c_str()];
        [activeTitle drawInRect:topRect(10, statY + 7, W - 20, 18, H)
                 withAttributes:@{NSFontAttributeName: fontBold11,
                                  NSForegroundColorAttributeName: [NSColor whiteColor]}];

        NSString* statsLine = [NSString stringWithFormat:@"%zu verts  •  %zu tris  •  %zu submeshes  •  %zu tex",
                               m_vertexCount, m_triangleCount, m_submeshCount, m_textureCount];
        [statsLine drawInRect:topRect(10, statY + 26, W - 20, 18, H)
               withAttributes:@{NSFontAttributeName: fontSmall,
                                NSForegroundColorAttributeName: rgb(225, 240, 255)}];

        [rgb(48, 48, 52) setFill];
        NSRectFill(topRect(W - 1, 0, 1, H, H));

        [NSGraphicsContext restoreGraphicsState];

        size_t totalBytes = static_cast<size_t>(pixelWidth) * pixelHeight * 4;
        outRgba.resize(totalBytes);
        std::memcpy(outRgba.data(), [rep bitmapData], totalBytes);
    }

    m_dirty = false;
}

void SidebarExplorer::renderTabBarToRgba(uint32_t pixelWidth, uint32_t pixelHeight, std::vector<uint8_t>& outRgba) {
    if (pixelWidth == 0 || pixelHeight == 0) return;

    @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:pixelWidth
                          pixelsHigh:pixelHeight
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                        bitmapFormat:0
                         bytesPerRow:pixelWidth * 4
                        bitsPerPixel:32];

        NSGraphicsContext* nsCtx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:nsCtx];

        CGContextRef ctx = [nsCtx CGContext];
        float scale = static_cast<float>(pixelHeight) / static_cast<float>(TAB_BAR_HEIGHT);
        CGContextScaleCTM(ctx, scale, scale);

        float W = static_cast<float>(pixelWidth) / scale;
        float H = static_cast<float>(TAB_BAR_HEIGHT);

        // --- ROW 1 (y = 0..36): Chrome / VS Code Multi-Tab Bar ---
        [rgb(37, 37, 38) setFill];
        NSRectFill(topRect(0, 0, W, 36, H));
        [rgb(45, 45, 48) setFill];
        NSRectFill(topRect(0, 35, W, 1, H));

        NSFont* fontTabActive = [NSFont systemFontOfSize:11.5 weight:NSFontWeightSemibold];
        NSFont* fontTabIdle   = [NSFont systemFontOfSize:11.5 weight:NSFontWeightRegular];
        NSFont* fontBtn       = [NSFont systemFontOfSize:11.0 weight:NSFontWeightSemibold];
        NSFont* fontSmall     = [NSFont systemFontOfSize:10.5 weight:NSFontWeightRegular];

        float availW = std::max(200.0f, W - 12.0f);
        float tabW = m_tabTitles.empty() ? 165.0f
                   : std::clamp(availW / static_cast<float>(m_tabTitles.size()), 105.0f, 185.0f);

        for (size_t i = 0; i < m_tabTitles.size(); ++i) {
            float tx = i * tabW;
            if (tx + 30.0f > W) break;

            bool isActive = (static_cast<int>(i) == m_activeTabIndex);
            bool isHovered = (static_cast<int>(i) == m_hoveredTab);

            NSColor* tabBg = isActive ? rgb(24, 26, 31)
                           : isHovered ? rgb(50, 50, 53)
                                       : rgb(43, 43, 45);
            [tabBg setFill];
            NSRectFill(topRect(tx, 0, tabW - 1.0f, 35.0f, H));

            // Right separator between tabs
            [rgb(32, 32, 34) setFill];
            NSRectFill(topRect(tx + tabW - 1.0f, 0, 1.0f, 36.0f, H));

            if (isActive) {
                // VS Code / Chrome active tab top blue accent line
                [rgb(0, 122, 204) setFill];
                NSRectFill(topRect(tx, 0, tabW - 1.0f, 2.5f, H));
            }

            // Tab icon dot
            NSColor* dotColor = isActive ? rgb(0, 185, 255) : rgb(110, 115, 125);
            NSBezierPath* dot = [NSBezierPath bezierPathWithOvalInRect:topRect(tx + 10.0f, 14.0f, 7.0f, 7.0f, H)];
            [dotColor setFill];
            [dot fill];

            // Tab Title
            NSString* titleStr = [NSString stringWithUTF8String:m_tabTitles[i].c_str()];
            NSMutableParagraphStyle* pstyle = [[NSMutableParagraphStyle alloc] init];
            [pstyle setLineBreakMode:NSLineBreakByTruncatingTail];

            [titleStr drawInRect:topRect(tx + 22.0f, 9.0f, tabW - 48.0f, 18.0f, H)
                  withAttributes:@{NSFontAttributeName: isActive ? fontTabActive : fontTabIdle,
                                   NSForegroundColorAttributeName: isActive ? [NSColor whiteColor] : rgb(165, 165, 170),
                                   NSParagraphStyleAttributeName: pstyle}];

            // Close button '✕' (shown when > 1 tab)
            if (m_tabTitles.size() > 1) {
                bool closeHover = (isHovered && m_hoveredTabClose);
                if (closeHover) {
                    NSBezierPath* cb = [NSBezierPath bezierPathWithRoundedRect:topRect(tx + tabW - 24.0f, 9.0f, 18.0f, 18.0f, H) xRadius:3 yRadius:3];
                    [rgb(80, 80, 86) setFill];
                    [cb fill];
                }
                [@"✕" drawInRect:topRect(tx + tabW - 20.0f, 9.0f, 14.0f, 18.0f, H)
                  withAttributes:@{NSFontAttributeName: fontTabIdle,
                                   NSForegroundColorAttributeName: (isActive || closeHover) ? rgb(230, 230, 235) : rgb(135, 135, 140)}];
            }
        }

        // --- ROW 2 (y = 36..76): Blender-Style 3D Viewport Header Controls ---
        [rgb(28, 30, 35) setFill];
        NSRectFill(topRect(0, 36, W, 40, H));
        [rgb(52, 55, 62) setFill];
        NSRectFill(topRect(0, H - 1, W, 1, H));

        auto drawVpButton = [&](float bx, float by, float bw, float bh, NSString* label, bool active, bool hovered) {
            NSColor* bg = active ? (hovered ? rgb(24, 132, 208) : rgb(14, 99, 156))
                                 : (hovered ? rgb(62, 65, 74)   : rgb(43, 46, 52));
            NSColor* border = active ? rgb(56, 165, 245) : rgb(72, 76, 86);
            NSBezierPath* bp = [NSBezierPath bezierPathWithRoundedRect:topRect(bx, by, bw, bh, H) xRadius:5 yRadius:5];
            [bg setFill];
            [bp fill];
            [border setStroke];
            [bp setLineWidth:1.0];
            [bp stroke];

            NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
            [style setAlignment:NSTextAlignmentCenter];
            [label drawInRect:topRect(bx + 2, by + 4, bw - 4, bh - 4, H)
               withAttributes:@{NSFontAttributeName: fontBtn,
                                NSForegroundColorAttributeName: rgb(242, 244, 248),
                                NSParagraphStyleAttributeName: style}];
        };

        // 1. Zoom Out [-] Button (x = 10..38)
        drawVpButton(10.0f, 44.0f, 28.0f, 24.0f, @"−", false, m_hoveredViewportBtn == 1);

        // 2. Live Zoom Percentage & Distance Readout (x = 42..162, click resets zoom to 100%)
        float zoomPct = std::clamp((m_baseCamDist / std::max(0.02f, m_camDist)) * 100.0f, 1.0f, 9999.0f);
        NSString* zoomStr = [NSString stringWithFormat:@"Zoom %.0f%% (%.1fm)", zoomPct, m_camDist];
        drawVpButton(42.0f, 44.0f, 120.0f, 24.0f, zoomStr, false, m_hoveredViewportBtn == 2);

        // 3. Zoom In [+] Button (x = 166..194)
        drawVpButton(166.0f, 44.0f, 28.0f, 24.0f, @"+", false, m_hoveredViewportBtn == 3);

        // 4. Reset View Button (x = 202..298)
        drawVpButton(202.0f, 44.0f, 96.0f, 24.0f, @"⌖ Reset View", false, m_hoveredViewportBtn == 4);

        // 5. Wireframe Mode Selector (x = 308..424)
        NSString* wfLabel = (m_wireframeMode == 0) ? @"Z: Solid Only"
                          : (m_wireframeMode == 1) ? @"Z: Solid+Wire"
                                                   : @"Z: Wire Only";
        drawVpButton(308.0f, 44.0f, 116.0f, 24.0f, wfLabel, m_wireframeMode > 0, m_hoveredViewportBtn == 5);

        // 6. Textures ON/OFF Toggle (x = 430..524)
        drawVpButton(430.0f, 44.0f, 94.0f, 24.0f, m_useTextures ? @"T: Tex ON" : @"T: Tex OFF", m_useTextures, m_hoveredViewportBtn == 6);

        // 7. Auto-Rotate ON/OFF Toggle (x = 530..618)
        drawVpButton(530.0f, 44.0f, 88.0f, 24.0f, m_autoRotate ? @"↻ Rotate" : @"⏸ Paused", m_autoRotate, m_hoveredViewportBtn == 7);

        // 8. Paint Color Swatches (x = 628..876)
        [@"Paint:" drawInRect:topRect(628.0f, 48.0f, 38.0f, 16.0f, H)
               withAttributes:@{NSFontAttributeName: fontSmall,
                                NSForegroundColorAttributeName: rgb(175, 180, 190)}];

        const int swatchColors[7][3] = {
            {26, 115, 242},  // 1: Electric Blue
            {38, 235, 56},   // 2: Neon Lime Green
            {245, 97, 13},   // 3: Sunset Orange
            {224, 13, 38},   // 4: Cherry Red
            {28, 28, 34},    // 5: Midnight Black
            {242, 242, 245}, // 6: Pearl White
            {191, 26, 217}   // 7: Cyber Magenta
        };
        for (int i = 0; i < 7; ++i) {
            float sx = 668.0f + i * 30.0f;
            if (sx + 24.0f > W) break;
            NSBezierPath* sp = [NSBezierPath bezierPathWithRoundedRect:topRect(sx, 46.0f, 24.0f, 20.0f, H) xRadius:4 yRadius:4];
            [rgb(swatchColors[i][0], swatchColors[i][1], swatchColors[i][2]) setFill];
            [sp fill];
            if (m_activePaintIndex == i + 1) {
                [[NSColor whiteColor] setStroke];
                [sp setLineWidth:2.0];
            } else if (m_hoveredViewportBtn == 10 + i) {
                [rgb(190, 210, 240) setStroke];
                [sp setLineWidth:1.5];
            } else {
                [rgb(80, 84, 92) setStroke];
                [sp setLineWidth:1.0];
            }
            [sp stroke];
        }

        // 9. WASD + Mouse Navigation Hint Pill (right side if space allows)
        if (W > 1030.0f) {
            [@"WASD+QE: Move (Shift: Fast)  •  R-Drag: Pan" drawInRect:topRect(890.0f, 49.0f, W - 898.0f, 18.0f, H)
                           withAttributes:@{NSFontAttributeName: fontSmall,
                                            NSForegroundColorAttributeName: rgb(155, 165, 180)}];
        }

        [NSGraphicsContext restoreGraphicsState];

        size_t totalBytes = static_cast<size_t>(pixelWidth) * pixelHeight * 4;
        outRgba.resize(totalBytes);
        std::memcpy(outRgba.data(), [rep bitmapData], totalBytes);
    }

    m_tabBarDirty = false;
}

} // namespace NFSU2
