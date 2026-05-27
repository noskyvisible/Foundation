#pragma once
#include <string>

// Opens a native "Open file" dialog filtered to model formats. Returns the
// chosen absolute path, or "" if the user cancelled. `initialDir` may be null.
std::string openModelFileDialog(const char* initialDir);
