cmake_minimum_required(VERSION 3.16)
project(Foundation LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put the built .exe in build/ so it sits next to the DLLs CMake copies for it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(FetchContent)

# --- GLFW: window + OpenGL context + input -------------------------------
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
# We only want the library, not its examples/tests/docs.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# --- GLM: header-only vector / matrix math -------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)

# --- GLAD: loads modern OpenGL function pointers at runtime ---------------
# glad2 generates its source via CMake (uses the Python on your PATH).
FetchContent_Declare(
    glad
    GIT_REPOSITORY https://github.com/Dav1dde/glad.git
    GIT_TAG        v2.0.6
    SOURCE_SUBDIR  cmake
)

# --- Dear ImGui (docking branch): immediate-mode editor UI ----------------
# ImGui ships no CMake build, so MakeAvailable only downloads it; we compile
# its handful of source files ourselves into a static "imgui" library below.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)

# --- Assimp: import GLB/GLTF/FBX/OBJ meshes -------------------------------
# Big library; disable everything we don't need to keep the build lean.
FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER  ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER  ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)

# --- ImGuizmo: translate/rotate/scale gizmos for ImGui --------------------
# Its bundled CMakeLists expects a global imgui target, so we only populate the
# source and compile ImGuizmo.cpp into our imgui library below.
FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        master
)

# --- stb: single-header image decoding (for embedded model textures) -------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)

FetchContent_MakeAvailable(glfw glm glad imgui assimp)

FetchContent_GetProperties(imguizmo)
if(NOT imguizmo_POPULATED)
    FetchContent_Populate(imguizmo)
endif()

FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# Generate an OpenGL 3.3 Core loader target called "glad_gl_core_33".
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

# Build ImGui core + the GLFW/OpenGL3 backends + ImGuizmo as one static library.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp
    ${imguizmo_SOURCE_DIR}/src
)
# The GLFW backend needs GLFW's headers; the OpenGL3 backend uses its own loader.
target_link_libraries(imgui PUBLIC glfw)

# --- Our executable -------------------------------------------------------
add_executable(Foundation
    src/main.cpp
    src/shaders.cpp
    src/mesh.cpp
    src/skinned.cpp
    src/camera.cpp
    src/environment.cpp
    src/scene.cpp
    src/renderer.cpp
    src/editor.cpp
    src/navigation.cpp
    src/npc_sim.cpp
    src/platform_file.cpp
    src/stb_impl.cpp
)
target_include_directories(Foundation PRIVATE ${stb_SOURCE_DIR})

target_link_libraries(Foundation PRIVATE
    glfw
    glad_gl_core_33
    glm::glm
    imgui
    assimp
    comdlg32          # Win32 native file-open dialog
)
