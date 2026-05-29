#pragma once
#include <glad/gl.h>

// GLSL shader sources used by the renderer. "line" = flat per-vertex colour
// (grid + selection box). "lit" = directional-lit objects. "skin" = skinned
// (bone-deformed) objects sharing the lit fragment shader. "sky" = procedural
// day/night sky on a fullscreen triangle.
extern const char* kLineVS;
extern const char* kLineFS;
extern const char* kLitVS;
extern const char* kLitFS;
extern const char* kSkinVS;
extern const char* kSkyVS;
extern const char* kSkyFS;

// Compile a single shader stage; logs errors to stderr. Returns the shader id.
GLuint compileShader(GLenum type, const char* src);

// Link a vertex+fragment program; logs errors to stderr. Returns the program id.
GLuint createProgram(const char* vsSrc, const char* fsSrc);
