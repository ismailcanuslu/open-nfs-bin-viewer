#include "GeometryParser.hpp"
#include "TextureParser.hpp"
#include "MapParser.hpp"
#include "VulkanRenderer.hpp"
#include "MacFileDialog.hpp"
#include <iostream>
#include <filesystem>
#include <vector>

int main(int argc, char* argv[]) {
    std::vector<std::string> targets;
    int maxFrames = 0;
    int initialWireframeMode = 1; // Default: Solid + Wireframe Overlay
    bool initialUseTextures = true;
    bool forcePicker = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            maxFrames = std::stoi(argv[++i]);
        } else if (arg == "--solid") {
            initialWireframeMode = 0;
        } else if (arg == "--overlay") {
            initialWireframeMode = 1;
        } else if (arg == "--wireframe") {
            initialWireframeMode = 2;
        } else if (arg == "--no-textures") {
            initialUseTextures = false;
        } else if (arg == "--picker" || arg == "--open") {
            forcePicker = true;
        } else if (arg.rfind("-psn_", 0) == 0) {
            continue;
        } else if (arg.rfind("--", 0) != 0) {
            targets.push_back(arg);
        }
    }

    std::string defaultWorkspaceDir = "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2";
    if (!std::filesystem::exists(defaultWorkspaceDir)) {
        defaultWorkspaceDir = std::filesystem::current_path().string();
    }

    if (forcePicker && maxFrames == 0) {
        std::string picked = NFSU2::MacFileDialog::PickBinOrFolder(defaultWorkspaceDir);
        if (!picked.empty()) {
            targets.push_back(picked);
        }
    }

    if (targets.empty()) {
        targets.push_back("350Z");
    }

    std::cout << ">>> Initializing NFSU2 BIN Opener (Native Vulkan Engine on Apple Silicon)..." << std::endl;

    NFSU2::VulkanRenderer renderer(1380, 800, "NFSU2 BIN Opener");
    if (!renderer.init()) {
        std::cerr << "[Error] Failed to initialize Vulkan renderer" << std::endl;
        return 1;
    }

    renderer.setWireframeMode(initialWireframeMode);
    renderer.setUseTextures(initialUseTextures);

    bool anyLoaded = false;
    std::string lastFailed;
    for (const auto& t : targets) {
        if (renderer.loadTarget(t)) {
            anyLoaded = true;
        } else {
            lastFailed = t;
        }
    }

    if (!anyLoaded) {
        renderer.loadTarget("350Z");
        if (!lastFailed.empty()) {
            renderer.loadTarget(lastFailed);
        }
    }

    renderer.run(maxFrames);
    return 0;
}
