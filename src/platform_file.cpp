// Win32 file-open dialog, isolated so <windows.h> stays out of main.cpp
// (avoids its min/max/near/far macros clashing with glm).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "platform_file.h"

std::string openModelFileDialog(const char* initialDir) {
    char file[1024] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Models (*.glb;*.gltf;*.fbx;*.obj)\0*.glb;*.gltf;*.fbx;*.obj\0All Files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrTitle  = "Load mesh";
    // NOCHANGEDIR keeps our working directory intact (relative scene.fdn path).
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(file);
    return std::string();
}
