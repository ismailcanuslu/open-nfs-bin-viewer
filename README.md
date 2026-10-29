# NFS BIN Opener — Universal Vulkan 3D `.BIN` & `.BUN` Archive Inspector

![Vulkan 1.2+](https://img.shields.io/badge/Graphics-Vulkan_1.2%2B-red)
![Apple Silicon Native](https://img.shields.io/badge/macOS-Apple_Silicon_(MoltenVK)-blue)
![Windows x64](https://img.shields.io/badge/Windows-x64_Native-0078D6)
![License: MIT](https://img.shields.io/badge/License-MIT-green)

**NFS BIN Opener** is a fast, open-source, hardware-accelerated 3D asset viewer and archive inspector for **EA Black Box Need for Speed** games (**Need for Speed: Underground 2**, **Most Wanted (2005)**, and **Carbon (2006)** `.BIN` and `.BUN` archives).

Built from the ground up in **C++17** on top of the **Vulkan API**, it runs natively on **Windows (x64)** and **macOS (Apple Silicon M1/M2/M3/M4 & Intel)** via **MoltenVK**, allowing modders, reverse engineers, and graphics enthusiasts to inspect 3D car geometries, showroom platforms, map streaming districts, and `JDLZ`-compressed texture packs without needing legacy DirectX 9 tools or Windows-only wrappers.

---

## 🎯 Game Compatibility Status

Because EA Black Box built *Underground 2*, *Most Wanted (2005)*, and *Carbon (2006)* on the shared **EAGL / Speed** chunk architecture (`0x80134000` solid lists, `0x80134010` parts, `0xb3300000` TPK texture packs, and `JDLZ` compression), the parser is designed universally across all three titles:

| Game Title | 3D Geometry (`.BIN` / `.BUN`) | Textures & TrackMaps (`TPK` / `JDLZ`) | Verification Status |
| :--- | :---: | :---: | :--- |
| **Need for Speed: Underground 2** *(2004)* | ✅ 24 / 36-byte strides, Wheel Mounts, Showrooms & Bayview Maps | ✅ `DXT1` / `DXT3` / `DXT5` / `A8R8G8B8` / `P8` | **Fully Tested & Verified** *(Cars, Showrooms, TrackMaps & Bayview Districts)* |
| **Need for Speed: Most Wanted** *(2005)* | ⚡ `JDLZ` auto-decompression + 36 / 48 / 60-byte tangent strides | ⚡ `TPK` (`0xb3300000`) + `JDLZ` | **Expected to Work** *(Universal parser implemented; not yet tested against full retail assets)* |
| **Need for Speed: Carbon** *(2006)* | ⚡ `JDLZ` auto-decompression + 36 / 48 / 60 / 64-byte strides | ⚡ `TPK` (`0xb3300000`) + `JDLZ` | **Expected to Work** *(Universal parser implemented; `RAWZ`/`HUFF` chunks untested)* |

---

## ✨ Key Features

- **Native Vulkan & Apple Silicon Support (`MoltenVK`):**
  - Hardware-accelerated Vulkan rendering pipeline (`VK_KHR_portability_subset` & `MoltenVK` on macOS; native Vulkan 1.2+ on Windows).
  - Runs natively on **Apple Silicon (M1 / M2 / M3 / M4)** at high frame rates with Retina DPI scaling.
- **VS Code-Style File Explorer Sidebar:**
  - Browse your entire game directory (`CARS/`, `TRACKS/`, `FRONTEND/PLATFORMS/`, `GLOBAL/`) in a collapsible tree view with automatic asset type badges (`CAR`, `3D`, `MAP`, `TEX`, `DIR`, `!`).
  - Clear English diagnostic banners when clicking empty or non-3D/non-texture binary files.
- **Chrome / VS Code Multi-Tab Workspace:**
  - Open multiple `.BIN` and `.BUN` files side by side in tabs.
  - **Zero background GPU overhead:** Only the active tab is uploaded and rendered on the GPU; switching tabs restores each tab's exact camera position, zoom, wireframe mode, and paint preset.
- **Blender-Style 3D Viewport Controls & `WASD` Free-Flight Navigation:**
  - Interactive viewport header bar with clickable **`[ − ]` / `[ + ]` Zoom**, **Live Zoom Percentage & Distance Readout (`Zoom 100% (4.5m)`)**, **`[ ⌖ Reset View ]`**, **`[ Z: Solid+Wire ]`**, **`[ T: Tex ON ]`**, **`[ ↻ Rotate ]`**, and **7 Underground Paint Swatches**.
  - Fly freely inside large 3D environments (such as `FRONTEND/PLATFORMS/MegaloShop01.BIN`, `CarLot01.BIN`, or Bayview map districts `STREAML4RA..J.BUN`) using **`WASD` + `Q/E`** (hold **`Shift`** for `3.5x` turbo speed) and **Right-Click Drag** to pan.
  - Wide-range proportional zoom from **`0.05m` (5 cm)** up to **`300.0m`** with extended near/far clipping planes (`0.02m` – `800.0m`).
- **Complete EA Black Box `.BIN` / `.BUN` & `JDLZ` Decoder:**
  - **3D Cars (`CARS/*/GEOMETRY.BIN`):** Automatically filters stock `BASE` + `KIT00` LOD-A parts, resolves `0x00134b03` 4-byte aligned 16-bit triangle list padding, and mounts wheels and brake rotors at their exact `GLOBALB.BUN` wheel well coordinates.
  - **3D Showrooms & Map Districts (`MegaloShop01.BIN`, `CarLot01.BIN`, `STREAML4R*.BUN`):** Supports both 36-byte (`pos + normal + color + uv`) and 24-byte (`pos + color + uv`) vertex buffer strides with automatic face-normal reconstruction.
  - **Textures & Track Maps (`TEXTURES.BIN`, `VINYLS.BIN`, `TRACKMAP4000..4084.BIN`):** Built-in `JDLZ` decompressor and `TPK` (`0xb3300000`) parser supporting `DXT1`, `DXT3`, `DXT5`, `A8R8G8B8`, and `P8` paletted textures. Texture-only archives open automatically in a 2D/3D Texture Showcase Grid.

---

## 🎮 Viewport & Keyboard Controls

| Input / Control | Action |
| :--- | :--- |
| **Left Sidebar Click** | Expand folders or open `.BIN` / `.BUN` files in a new Tab |
| **Top Tab Bar Click** | Switch active tab or click **`✕`** to close a tab |
| **`W` / `A` / `S` / `D`** | Fly Forward / Left / Backward / Right through the 3D scene |
| **`Q` / `E`** | Move Camera Down / Up |
| **`Left Shift` (Hold)** | `3.5x` faster `WASD` + `Q/E` movement speed for large maps & shops |
| **Left-Click + Drag** | Orbit camera around the current target |
| **Right-Click + Drag** | Pan camera target horizontally and vertically |
| **Mouse Scroll / `+` / `-`** | Proportional Zoom In / Zoom Out (`0.05m` to `300.0m`) |
| **Viewport `[ − ]` / `[ + ]`** | Clickable Blender-style Zoom Out / Zoom In buttons |
| **Viewport `Zoom %` Badge** | Displays live zoom percentage & distance; click to reset zoom to `100%` |
| **`R` or `[ ⌖ Reset View ]`** | Reset `WASD` camera position, orbit angles, and zoom |
| **`Z` or `[ Z: Solid+Wire ]`** | Cycle Wireframe Mode (`Solid Only` → `Solid + Wireframe` → `Wireframe Only`) |
| **`T` or `[ T: Tex ON ]`** | Toggle decoded `.BIN`/`.BUN` textures `ON` / `OFF` |
| **`Space` or `[ ↻ Rotate ]`** | Toggle turntable auto-rotation |
| **`1` – `7` or Swatches** | Switch Underground car body metallic paint presets |
| **`Tab`** | Cycle through open tabs |
| **`Left` / `Right` Arrow** | Load previous / next car folder in `CARS/` |
| **`O`** | Open native macOS / Windows file & folder picker dialog |
| **Drag & Drop** | Drag any `.BIN`, `.BUN`, or folder from Finder / Explorer into the window |

---

## 🛠️ Installation & Building from Source

### 1. macOS (Apple Silicon M1/M2/M3/M4 & Intel)

#### Pre-built `.dmg` Release
1. Download **`NFSU2-BIN-Opener-macOS-AppleSilicon.dmg`** from the [Releases](../../releases) page (or GitHub Actions artifacts).
2. Install the runtime dependencies via [Homebrew](https://brew.sh):
   ```bash
   brew install glfw vulkan-loader molten-vk
   ```
3. Open the `.dmg`, drag **`NFSU2 BIN Opener.app`** to `/Applications`, and double-click to launch.

#### Building from Source on macOS
1. Install Xcode Command Line Tools and Homebrew dependencies:
   ```bash
   xcode-select --install
   brew install cmake glfw vulkan-headers vulkan-loader molten-vk shaderc
   ```
2. Configure and build:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(sysctl -n hw.ncpu)
   ```
3. Run the viewer:
   ```bash
   ./run.sh
   # Or open specific files directly in tabs:
   ./build/nfsu2_vulkan "/path/to/NFSU2/CARS/350Z/GEOMETRY.BIN" "/path/to/NFSU2/FRONTEND/PLATFORMS/MegaloShop01.BIN"
   ```

---

### 2. Windows (x64)

#### Pre-built `.exe` Release
1. Download **`NFSU2-BIN-Opener-Windows-x64.zip`** from the [Releases](../../releases) page (or GitHub Actions artifacts).
2. Extract the archive (keep `NFSU2_BIN_Opener.exe` alongside the `shaders/` folder) and run **`NFSU2_BIN_Opener.exe`**.
   *(Requires any modern GPU with Vulkan 1.2+ drivers installed — NVIDIA, AMD, or Intel).*

#### Building from Source on Windows
1. Install prerequisites:
   - **Visual Studio 2022** (with *"Desktop development with C++"* workload)
   - **CMake 3.16+**
   - **[LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** (includes `Vulkan-Headers`, `vulkan-1.lib`, and `glslc.exe`)
   - *(Note: **GLFW 3.4** is fetched and compiled automatically by CMake `FetchContent` if not already installed).*
2. Open PowerShell or a Developer Command Prompt and build:
   ```powershell
   cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release --parallel
   ```
3. Copy the compiled SPIR-V shaders next to the executable (or run from the repository root):
   ```powershell
   .\build\Release\nfsu2_vulkan.exe
   ```

---

## 📦 Continuous Integration (GitHub Actions)

This repository includes a ready-to-use GitHub Actions workflow ([`.github/workflows/build.yml`](.github/workflows/build.yml)) that automatically builds and packages both platforms on every push, pull request, or release tag (`v*`):

- **`build-macos-dmg` (`macos-14` Apple Silicon):** Compiles the native ARM64 Vulkan binary and SPIR-V shaders, bundles **`NFSU2 BIN Opener.app`**, and generates **`NFSU2-BIN-Opener-macOS-AppleSilicon.dmg`**.
- **`build-windows-exe` (`windows-latest` x64 MSVC):** Installs the LunarG Vulkan SDK, compiles **`NFSU2_BIN_Opener.exe`** and SPIR-V shaders, and packages **`NFSU2-BIN-Opener-Windows-x64.zip`**.
- **`release`:** Automatically attaches the `.dmg`, `.zip`, and `.exe` binaries to GitHub Releases when pushing a version tag (e.g., `git tag v2.0.0 && git push origin v2.0.0`).

---

## 📄 License

Released under the [MIT License](LICENSE).
*Need for Speed: Underground 2 is a trademark of Electronic Arts Inc. This project is an independent open-source reverse-engineering and visualization tool and contains no proprietary game assets.*

