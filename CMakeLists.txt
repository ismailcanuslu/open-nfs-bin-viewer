cmake_minimum_required(VERSION 3.16)
project(nfsu2_vulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

find_package(Vulkan REQUIRED)

include_directories(
    src
    ${Vulkan_INCLUDE_DIRS}
)

if(APPLE)
    include_directories(/opt/homebrew/include /usr/local/include)
    link_directories(/opt/homebrew/lib /usr/local/lib)
endif()

set(SOURCES
    src/main.cpp
    src/GeometryParser.cpp
    src/TextureParser.cpp
    src/MapParser.cpp
    src/VulkanRenderer.cpp
)

if(APPLE)
    list(APPEND SOURCES src/MacFileDialog.mm)
elseif(WIN32)
    list(APPEND SOURCES src/WinFileDialog.cpp)
endif()

set(HEADERS
    src/Math3D.hpp
    src/GeometryParser.hpp
    src/TextureParser.hpp
    src/MapParser.hpp
    src/VulkanRenderer.hpp
    src/MacFileDialog.hpp
)

add_executable(nfsu2_vulkan ${SOURCES} ${HEADERS})

if(APPLE)
    target_link_libraries(nfsu2_vulkan
        PRIVATE
        ${Vulkan_LIBRARIES}
        glfw
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework QuartzCore"
    )
elseif(WIN32)
    find_package(glfw3 QUIET)
    if(NOT glfw3_FOUND)
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
        )
        FetchContent_MakeAvailable(glfw)
    endif()

    target_link_libraries(nfsu2_vulkan
        PRIVATE
        ${Vulkan_LIBRARIES}
        glfw
        gdi32
        comdlg32
        shell32
        ole32
    )
endif()

# Locate glslc shader compiler across macOS (Homebrew) and Windows (Vulkan SDK)
find_program(GLSLC_EXECUTABLE
    NAMES glslc glslc.exe
    HINTS
        /opt/homebrew/bin
        /usr/local/bin
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
    REQUIRED
)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shaders)

add_custom_target(compile_shaders ALL
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shader.vert -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/vert.spv
    COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shader.frag -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/frag.spv
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/shaders/vert.spv ${CMAKE_CURRENT_BINARY_DIR}/shaders/frag.spv
    COMMENT "Compiling NFSU2 GLSL shaders to SPIR-V using ${GLSLC_EXECUTABLE}..."
)

add_dependencies(nfsu2_vulkan compile_shaders)
