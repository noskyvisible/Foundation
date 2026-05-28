// Editor shell, step D: Hammer-style quad-view + play window + gizmos. The cube
// is now a scene object with an editable transform; click it in the Perspective
// view to select, then use the ImGuizmo handles (W/E/R = move/rotate/scale).
//
// WebGL parallels:
//   - An FBO is like an offscreen WebGL framebuffer we render into, then
//     draw as a texture into a UI region. We have one per viewport.
//   - A Camera bundles the view + projection matrices; ortho cameras use
//     glm::ortho instead of glm::perspective (no perspective foreshortening).
//   - The grid is just a line mesh (GL_LINES) drawn with the same shader as the
//     cube, oriented into each view's plane by a per-camera model matrix.
//   - ImGui is "immediate mode": we rebuild the entire UI every frame inside
//     the render loop instead of creating persistent panel objects.

#include <glad/gl.h>      // before GLFW: defines the GL types/functions.
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder API for the default layout
#include "imgui_stdlib.h"     // ImGui::InputText with std::string
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"         // translate/rotate/scale gizmos

#include <assimp/Importer.hpp>   // GLB/GLTF/FBX/OBJ mesh loading
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "platform_file.h"    // native file-open dialog
#include "stb_image.h"        // decode embedded model textures

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ---- Shaders -------------------------------------------------------------
// "line" program: flat per-vertex colour (grid + selection box). "lit" program:
// per-object colour shaded by a fixed directional light using surface normals.

static const char* kLineVS = R"glsl(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
}
)glsl";

static const char* kLineFS = R"glsl(
#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)glsl";

static const char* kLitVS = R"glsl(
#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUv;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = transpose(inverse(mat3(uModel))) * aNormal;  // correct under scale
    vUv = aUv;
}
)glsl";

static const char* kLitFS = R"glsl(
#version 330 core
in vec3 vNormal;
in vec2 vUv;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
uniform int uHasTexture;
uniform vec3 uLightDir;     // direction TO the sun (normalized)
uniform vec3 uLightColor;   // sun colour * intensity (dims at night)
uniform vec3 uAmbient;      // sky ambient (dim blue at night, never black)
out vec4 FragColor;
void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 base = uColor;
    if (uHasTexture == 1) base *= texture(uAlbedo, vUv).rgb;
    FragColor = vec4(base * (uAmbient + uLightColor * diff), 1.0);
}
)glsl";

// Procedural sky: a fullscreen triangle (no VBO) shaded by view direction.
static const char* kSkyVS = R"glsl(
#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)glsl";

static const char* kSkyFS = R"glsl(
#version 330 core
in vec2 vNdc;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;       // direction TO the sun
uniform vec3 uHorizon;
uniform vec3 uZenith;
uniform vec3 uSunColor;
out vec4 FragColor;
void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCamPos);
    float up = clamp(dir.y, 0.0, 1.0);
    vec3 col = mix(uHorizon, uZenith, pow(up, 0.45));
    float s = max(dot(dir, normalize(uSunDir)), 0.0);
    col += uSunColor * pow(s, 350.0);          // sun disc
    col += uSunColor * 0.25 * pow(s, 6.0);     // glow
    FragColor = vec4(col, 1.0);
}
)glsl";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Shader compile error:\n%s\n", log);
    }
    return shader;
}

static GLuint createProgram(const char* vsSrc, const char* fsSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Program link error:\n%s\n", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

// ---- Cube geometry (pos + normal per vertex) -----------------------------
static const float kCubeVertices[] = {
    // back face (-Z)
    -0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f,-0.5f,-0.5f, 0,0,-1,  0.5f, 0.5f,-0.5f, 0,0,-1,
     0.5f, 0.5f,-0.5f, 0,0,-1, -0.5f, 0.5f,-0.5f, 0,0,-1, -0.5f,-0.5f,-0.5f, 0,0,-1,
    // front face (+Z)
    -0.5f,-0.5f, 0.5f, 0,0,1,   0.5f,-0.5f, 0.5f, 0,0,1,   0.5f, 0.5f, 0.5f, 0,0,1,
     0.5f, 0.5f, 0.5f, 0,0,1,  -0.5f, 0.5f, 0.5f, 0,0,1,  -0.5f,-0.5f, 0.5f, 0,0,1,
    // left face (-X)
    -0.5f, 0.5f, 0.5f, -1,0,0, -0.5f, 0.5f,-0.5f, -1,0,0, -0.5f,-0.5f,-0.5f, -1,0,0,
    -0.5f,-0.5f,-0.5f, -1,0,0, -0.5f,-0.5f, 0.5f, -1,0,0, -0.5f, 0.5f, 0.5f, -1,0,0,
    // right face (+X)
     0.5f, 0.5f, 0.5f, 1,0,0,   0.5f, 0.5f,-0.5f, 1,0,0,   0.5f,-0.5f,-0.5f, 1,0,0,
     0.5f,-0.5f,-0.5f, 1,0,0,   0.5f,-0.5f, 0.5f, 1,0,0,   0.5f, 0.5f, 0.5f, 1,0,0,
    // bottom face (-Y)
    -0.5f,-0.5f,-0.5f, 0,-1,0,  0.5f,-0.5f,-0.5f, 0,-1,0,  0.5f,-0.5f, 0.5f, 0,-1,0,
     0.5f,-0.5f, 0.5f, 0,-1,0, -0.5f,-0.5f, 0.5f, 0,-1,0, -0.5f,-0.5f,-0.5f, 0,-1,0,
    // top face (+Y)
    -0.5f, 0.5f,-0.5f, 0,1,0,   0.5f, 0.5f,-0.5f, 0,1,0,   0.5f, 0.5f, 0.5f, 0,1,0,
     0.5f, 0.5f, 0.5f, 0,1,0,  -0.5f, 0.5f, 0.5f, 0,1,0,  -0.5f, 0.5f,-0.5f, 0,1,0,
};

// ---- Mesh assets (CPU side) ----------------------------------------------
// A mesh asset is a list of submeshes (one per material), each a vertex/index
// buffer + a base colour. The built-in cube is asset #0; loaded models append.
struct Vertex { glm::vec3 pos; glm::vec3 normal; glm::vec2 uv; };
struct AABB    { glm::vec3 min{0.0f}; glm::vec3 max{0.0f}; };
struct SubMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    glm::vec3 baseColor{1.0f};
    std::vector<uint8_t>  albedo;        // RGBA8 pixels, empty if untextured
    int albedoW = 0, albedoH = 0;
};
struct MeshAsset {
    std::string name;
    std::vector<SubMesh> subs;
    AABB bounds;
};

static AABB computeBounds(const std::vector<SubMesh>& subs) {
    AABB b{ glm::vec3(1e30f), glm::vec3(-1e30f) };
    bool any = false;
    for (const SubMesh& s : subs)
        for (const Vertex& v : s.vertices) {
            b.min = glm::min(b.min, v.pos);
            b.max = glm::max(b.max, v.pos);
            any = true;
        }
    if (!any) { b.min = glm::vec3(-0.5f); b.max = glm::vec3(0.5f); }
    return b;
}

// Build the built-in unit cube as a MeshAsset (from kCubeVertices: pos+normal).
static MeshAsset buildCubeMesh() {
    MeshAsset m;
    m.name = "Cube";
    SubMesh s;
    const int vertCount = (int)(sizeof(kCubeVertices) / sizeof(float)) / 6;
    for (int i = 0; i < vertCount; ++i) {
        const float* p = &kCubeVertices[i * 6];
        Vertex v;
        v.pos    = { p[0], p[1], p[2] };
        v.normal = { p[3], p[4], p[5] };
        v.uv     = { 0.0f, 0.0f };
        s.vertices.push_back(v);
        s.indices.push_back((uint32_t)i);
    }
    s.baseColor = glm::vec3(1.0f);   // white: object tint colour shows through
    m.subs.push_back(std::move(s));
    m.bounds = computeBounds(m.subs);
    return m;
}

// Filename without directory or extension, e.g. "C:/x/teapot.glb" -> "teapot".
static std::string fileStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;
    return path.substr(start, end - start);
}

// Load a model file via Assimp into a MeshAsset (one submesh per Assimp mesh,
// with its material base colour). Geometry + colours only for now.
static bool loadModel(const std::string& path, MeshAsset& out) {
    Assimp::Importer imp;
    const aiScene* sc = imp.ReadFile(path,
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_PreTransformVertices | aiProcess_JoinIdenticalVertices);
    if (!sc || (sc->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !sc->mRootNode) {
        std::fprintf(stderr, "Assimp load error: %s\n", imp.GetErrorString());
        return false;
    }
    out = MeshAsset{};
    out.name = fileStem(path);
    for (unsigned mi = 0; mi < sc->mNumMeshes; ++mi) {
        const aiMesh* m = sc->mMeshes[mi];
        SubMesh s;
        s.vertices.reserve(m->mNumVertices);
        for (unsigned v = 0; v < m->mNumVertices; ++v) {
            Vertex vert;
            vert.pos = { m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z };
            vert.normal = m->HasNormals()
                ? glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z)
                : glm::vec3(0, 1, 0);
            vert.uv = m->mTextureCoords[0]
                ? glm::vec2(m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y)
                : glm::vec2(0.0f);
            s.vertices.push_back(vert);
        }
        for (unsigned f = 0; f < m->mNumFaces; ++f) {
            const aiFace& face = m->mFaces[f];
            for (unsigned k = 0; k < face.mNumIndices; ++k)
                s.indices.push_back(face.mIndices[k]);
        }
        glm::vec3 col(0.8f);
        bool hasTex = false;
        if (m->mMaterialIndex < sc->mNumMaterials) {
            aiMaterial* mat = sc->mMaterials[m->mMaterialIndex];
            aiColor4D base; aiColor3D diff;
            if (mat->Get(AI_MATKEY_BASE_COLOR, base) == AI_SUCCESS)        col = { base.r, base.g, base.b };
            else if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diff) == AI_SUCCESS) col = { diff.r, diff.g, diff.b };

            // Base-colour / diffuse texture (embedded in the GLB). Some exporters
            // don't link it through the standard slots, so fall back to the
            // scene's single embedded texture when the material lookup is empty.
            const aiTexture* t = nullptr;
            aiString texPath;
            if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
                mat->GetTexture(aiTextureType_DIFFUSE,    0, &texPath) == AI_SUCCESS)
                t = sc->GetEmbeddedTexture(texPath.C_Str());
            if (!t && sc->mNumTextures > 0) t = sc->mTextures[0];

            if (t && t->mHeight == 0) {                // compressed (PNG/JPG) bytes
                stbi_set_flip_vertically_on_load(true);
                int w, h, n;
                unsigned char* px = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(t->pcData), (int)t->mWidth, &w, &h, &n, 4);
                if (px) {
                    s.albedo.assign(px, px + (size_t)w * h * 4);
                    s.albedoW = w; s.albedoH = h; hasTex = true;
                    stbi_image_free(px);
                }
            } else if (t) {                            // raw aiTexel (BGRA)
                int w = (int)t->mWidth, h = (int)t->mHeight;
                s.albedo.resize((size_t)w * h * 4);
                for (int i = 0; i < w * h; ++i) {
                    s.albedo[i*4+0] = t->pcData[i].r;
                    s.albedo[i*4+1] = t->pcData[i].g;
                    s.albedo[i*4+2] = t->pcData[i].b;
                    s.albedo[i*4+3] = t->pcData[i].a;
                }
                s.albedoW = w; s.albedoH = h; hasTex = true;
            }
        }
        if (hasTex)                              col = glm::vec3(1.0f);   // show texture fully
        else if (glm::dot(col, col) < 0.0009f)   col = glm::vec3(0.8f);   // black factor -> gray
        s.baseColor = col;
        out.subs.push_back(std::move(s));
    }
    out.bounds = computeBounds(out.subs);
    return !out.subs.empty();
}

// ---- Environment: game clock + day/night sky + sun-driven light ----------
struct Environment {
    // Authored / controllable state.
    float timeOfDay   = 9.0f;     // hours, 0-24
    int   day         = 1;
    float dayLengthSec = 600.0f;  // real seconds for a full 24h game day
    bool  running     = true;
    // Derived each frame from timeOfDay.
    glm::vec3 sunDir     {0, 1, 0};   // direction TO the sun
    glm::vec3 sunColor   {1.0f};
    glm::vec3 horizonColor{0.7f, 0.8f, 0.95f};
    glm::vec3 zenithColor {0.3f, 0.55f, 0.9f};
    glm::vec3 lightColor {1.0f};      // directional light colour (0 at night)
    glm::vec3 ambient    {0.3f};      // sky ambient colour (dim blue at night)
};

// `advance` should be true only in play mode; the editor freezes the clock
// (you scrub the time slider to preview). The sky/light are recomputed from
// timeOfDay regardless, so scrubbing updates the view.
static void updateEnvironment(Environment& e, float dt, bool advance) {
    if (advance && e.running && e.dayLengthSec > 0.0f) {
        e.timeOfDay += dt * (24.0f / e.dayLengthSec);
        while (e.timeOfDay >= 24.0f) { e.timeOfDay -= 24.0f; ++e.day; }
        while (e.timeOfDay < 0.0f)   { e.timeOfDay += 24.0f; --e.day; }
    }

    const float PI = 3.14159265f;
    float a = (e.timeOfDay - 6.0f) / 12.0f * PI;   // 0 at 06:00, PI at 18:00
    float elev = std::sin(a);                       // sun elevation, <0 at night
    e.sunDir = glm::normalize(glm::vec3(std::cos(a), elev, 0.35f));

    float dayAmt = glm::clamp((elev + 0.05f) / 0.30f, 0.0f, 1.0f);  // 0 night -> 1 day
    float duskAmt = (elev > 0.0f && elev < 0.30f) ? (1.0f - elev / 0.30f) : 0.0f;

    glm::vec3 dayZenith (0.20f, 0.45f, 0.85f), dayHorizon(0.70f, 0.82f, 0.95f);
    glm::vec3 nightZenith(0.02f, 0.03f, 0.07f), nightHorizon(0.05f, 0.06f, 0.12f);
    glm::vec3 dusk(0.95f, 0.45f, 0.18f);

    e.zenithColor  = glm::mix(nightZenith,  dayZenith,  dayAmt);
    e.horizonColor = glm::mix(nightHorizon, dayHorizon, dayAmt) + dusk * duskAmt * 0.8f;
    e.sunColor     = glm::mix(glm::vec3(1.0f, 0.55f, 0.25f), glm::vec3(1.0f, 0.97f, 0.9f),
                              glm::clamp(elev / 0.4f, 0.0f, 1.0f));
    e.lightColor   = e.sunColor * dayAmt;
    // Night keeps a dim blue ambient so the world stays visible (Skyrim-style),
    // rising to a neutral grey during the day.
    glm::vec3 nightAmbient(0.16f, 0.18f, 0.27f);
    glm::vec3 dayAmbient(0.30f, 0.30f, 0.30f);
    e.ambient      = glm::mix(nightAmbient, dayAmbient, dayAmt);
}

// ---- Offscreen render target (one per viewport, later) -------------------
struct RenderTarget {
    GLuint fbo = 0;
    GLuint color = 0;   // texture we display in the UI
    GLuint depth = 0;   // depth-stencil renderbuffer
    int width = 0;
    int height = 0;
};

// (Re)allocate the FBO's attachments when the panel size changes.
static void resizeTarget(RenderTarget& rt, int w, int h) {
    if (rt.fbo && w == rt.width && h == rt.height) return;
    rt.width = w;
    rt.height = h;

    if (!rt.fbo) glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);

    if (!rt.color) glGenTextures(1, &rt.color);
    glBindTexture(GL_TEXTURE_2D, rt.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.color, 0);

    if (!rt.depth) glGenRenderbuffers(1, &rt.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rt.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt.depth);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void destroyTarget(RenderTarget& rt) {
    if (rt.fbo)   glDeleteFramebuffers(1, &rt.fbo);
    if (rt.color) glDeleteTextures(1, &rt.color);
    if (rt.depth) glDeleteRenderbuffers(1, &rt.depth);
    rt = RenderTarget{};
}

// ---- Camera: bundles where we look from and how we project ---------------
enum class Projection { Perspective, Ortho };

struct Camera {
    Projection type = Projection::Perspective;
    glm::vec3 eye{0, 0, 3};
    glm::vec3 target{0, 0, 0};
    glm::vec3 up{0, 1, 0};
    float fovDeg = 45.0f;           // perspective only
    float orthoHalfHeight = 1.3f;   // ortho only: half view height in world units
    glm::mat4 gridModel{1.0f};      // orients the grid mesh into this view's plane
    const char* label = "";         // shown in the panel corner
    // Free-fly navigation state (perspective): eye/target are derived from these.
    glm::vec3 flyPos{0.0f};
    float yaw = 0.0f, pitch = 0.0f, flySpeed = 4.0f;
};

// FPS-style forward vector from yaw (around Y) and pitch (around X).
static glm::vec3 forwardFromYawPitch(float yaw, float pitch) {
    return glm::vec3(std::cos(pitch) * std::sin(yaw),
                     std::sin(pitch),
                    -std::cos(pitch) * std::cos(yaw));
}

static glm::mat4 viewMatrix(const Camera& c) {
    return glm::lookAt(c.eye, c.target, c.up);
}

static glm::mat4 projMatrix(const Camera& c, float aspect) {
    if (c.type == Projection::Perspective)
        return glm::perspective(glm::radians(c.fovDeg), aspect, 0.1f, 100.0f);
    float h = c.orthoHalfHeight;
    float w = h * aspect;
    return glm::ortho(-w, w, -h, h, -100.0f, 100.0f);
}

// ---- Reference grid: a line mesh (pos + colour) in the XY plane (z=0). ----
// Per-camera gridModel rotates it into each view's plane. Minor lines are dim,
// every 5th line brighter, and the two centre axes are tinted red/green.
static constexpr float kGridStep = 0.5f;   // base mesh spacing; scaled at draw time

static void makeGrid(GLuint& vao, GLuint& vbo, int& outVertexCount) {
    const int   half = 10;          // lines from -10..+10
    const float step = kGridStep;   // base world units between lines
    const float ext  = half * step;
    const glm::vec3 minor(0.18f, 0.18f, 0.21f);
    const glm::vec3 major(0.30f, 0.30f, 0.36f);
    const glm::vec3 axisX(0.55f, 0.22f, 0.22f);  // line along X (y=0)
    const glm::vec3 axisY(0.22f, 0.55f, 0.28f);  // line along Y (x=0)

    std::vector<float> v;
    auto push = [&](float x, float y, glm::vec3 c) {
        v.insert(v.end(), {x, y, 0.0f, c.r, c.g, c.b});
    };
    for (int i = -half; i <= half; ++i) {
        float p = i * step;
        glm::vec3 cv = (i == 0) ? axisY : (i % 5 == 0 ? major : minor);
        push(p, -ext, cv); push(p, ext, cv);     // vertical line (const x)
        glm::vec3 ch = (i == 0) ? axisX : (i % 5 == 0 ? major : minor);
        push(-ext, p, ch); push(ext, p, ch);     // horizontal line (const y)
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    outVertexCount = (int)(v.size() / 6);
}

// The 12 edges of a slightly-enlarged cube, all in a bright highlight colour --
// drawn around the selected object as selection feedback (GL_LINES, 24 verts).
static void makeSelectionBox(GLuint& vao, GLuint& vbo, int& outVertexCount) {
    const float s = 0.5f;                        // unit box; scaled to the mesh AABB at draw
    const glm::vec3 hl(1.0f, 0.6f, 0.1f);        // orange
    const glm::vec3 c[8] = {
        {-s,-s,-s}, { s,-s,-s}, { s,-s, s}, {-s,-s, s},
        {-s, s,-s}, { s, s,-s}, { s, s, s}, {-s, s, s},
    };
    const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // bottom
        {4,5},{5,6},{6,7},{7,4},   // top
        {0,4},{1,5},{2,6},{3,7},   // verticals
    };
    std::vector<float> v;
    for (auto& edge : e)
        for (int k = 0; k < 2; ++k) {
            glm::vec3 p = c[edge[k]];
            v.insert(v.end(), {p.x, p.y, p.z, hl.r, hl.g, hl.b});
        }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    outVertexCount = (int)(v.size() / 6);
}

// Bundles the GPU resources the scene needs to draw. VAOs are NOT shared
// between GL contexts, so each window (editor / play) builds its own Renderer.
// A mesh asset uploaded to the CURRENT GL context (one per submesh).
struct GPUSubMesh { GLuint vao = 0, vbo = 0, ebo = 0; GLsizei count = 0; glm::vec3 baseColor{1.0f}; GLuint tex = 0; };
struct GPUMesh    { std::vector<GPUSubMesh> subs; AABB bounds; };

struct Renderer {
    GLuint lineProgram = 0;            // grid + selection box (pos+colour, uMVP)
    GLint  lineMvpLoc = -1;
    GLuint litProgram = 0;             // objects (pos+normal+uv, lit)
    GLint  litMvpLoc = -1, litModelLoc = -1, litColorLoc = -1, litHasTexLoc = -1, litAlbedoLoc = -1;
    GLint  litLightDirLoc = -1, litLightColLoc = -1, litAmbientLoc = -1;
    GLuint skyProgram = 0, skyVao = 0; // procedural sky (fullscreen triangle)
    GLint  skyInvVPLoc = -1, skyCamLoc = -1, skySunLoc = -1, skyHorizonLoc = -1, skyZenithLoc = -1, skySunColLoc = -1;
    GLuint gridVao = 0, gridVbo = 0;   // pos+colour
    GLuint selBoxVao = 0, selBoxVbo = 0;
    int    gridVertexCount = 0;
    int    selBoxVertexCount = 0;
    std::vector<GPUMesh> meshes;       // parallel to the CPU mesh library
};

// Upload one CPU mesh asset into the current context.
static GPUMesh uploadMesh(const MeshAsset& asset) {
    GPUMesh gm;
    gm.bounds = asset.bounds;
    for (const SubMesh& s : asset.subs) {
        GPUSubMesh g;
        g.baseColor = s.baseColor;
        g.count = (GLsizei)s.indices.size();
        glGenVertexArrays(1, &g.vao);
        glGenBuffers(1, &g.vbo);
        glGenBuffers(1, &g.ebo);
        glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, s.vertices.size() * sizeof(Vertex), s.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, s.indices.size() * sizeof(uint32_t), s.indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
        glEnableVertexAttribArray(2);

        if (s.albedoW > 0 && !s.albedo.empty()) {
            glGenTextures(1, &g.tex);
            glBindTexture(GL_TEXTURE_2D, g.tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.albedoW, s.albedoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, s.albedo.data());
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        gm.subs.push_back(g);
    }
    return gm;
}

// Upload any library meshes this context hasn't uploaded yet (VAOs/buffers are
// per-context, so the editor and play windows each build their own copies).
static void syncMeshes(Renderer& r, const std::vector<MeshAsset>& lib) {
    for (size_t i = r.meshes.size(); i < lib.size(); ++i)
        r.meshes.push_back(uploadMesh(lib[i]));
}

// Build per-context shared resources (programs, grid, selection box).
static Renderer createRenderer() {
    Renderer r;
    r.lineProgram = createProgram(kLineVS, kLineFS);
    r.lineMvpLoc  = glGetUniformLocation(r.lineProgram, "uMVP");
    r.litProgram  = createProgram(kLitVS, kLitFS);
    r.litMvpLoc   = glGetUniformLocation(r.litProgram, "uMVP");
    r.litModelLoc = glGetUniformLocation(r.litProgram, "uModel");
    r.litColorLoc = glGetUniformLocation(r.litProgram, "uColor");
    r.litHasTexLoc = glGetUniformLocation(r.litProgram, "uHasTexture");
    r.litAlbedoLoc = glGetUniformLocation(r.litProgram, "uAlbedo");
    r.litLightDirLoc = glGetUniformLocation(r.litProgram, "uLightDir");
    r.litLightColLoc = glGetUniformLocation(r.litProgram, "uLightColor");
    r.litAmbientLoc  = glGetUniformLocation(r.litProgram, "uAmbient");

    r.skyProgram   = createProgram(kSkyVS, kSkyFS);
    r.skyInvVPLoc  = glGetUniformLocation(r.skyProgram, "uInvViewProj");
    r.skyCamLoc    = glGetUniformLocation(r.skyProgram, "uCamPos");
    r.skySunLoc    = glGetUniformLocation(r.skyProgram, "uSunDir");
    r.skyHorizonLoc= glGetUniformLocation(r.skyProgram, "uHorizon");
    r.skyZenithLoc = glGetUniformLocation(r.skyProgram, "uZenith");
    r.skySunColLoc = glGetUniformLocation(r.skyProgram, "uSunColor");
    glGenVertexArrays(1, &r.skyVao);   // empty VAO for the no-VBO fullscreen triangle

    makeGrid(r.gridVao, r.gridVbo, r.gridVertexCount);
    makeSelectionBox(r.selBoxVao, r.selBoxVbo, r.selBoxVertexCount);
    return r;
}

static void destroyRenderer(Renderer& r) {
    for (GPUMesh& m : r.meshes)
        for (GPUSubMesh& s : m.subs) {
            glDeleteVertexArrays(1, &s.vao);
            glDeleteBuffers(1, &s.vbo);
            glDeleteBuffers(1, &s.ebo);
            if (s.tex) glDeleteTextures(1, &s.tex);
        }
    glDeleteVertexArrays(1, &r.gridVao);
    glDeleteVertexArrays(1, &r.selBoxVao);
    glDeleteVertexArrays(1, &r.skyVao);
    glDeleteBuffers(1, &r.gridVbo);
    glDeleteBuffers(1, &r.selBoxVbo);
    glDeleteProgram(r.lineProgram);
    glDeleteProgram(r.litProgram);
    glDeleteProgram(r.skyProgram);
    r = Renderer{};
}

// Draw the grid + objects into the target, seen through the given camera.
// Ortho views always render wireframe; the 3D view follows `wireframe3D`.
// `models`, `colors`, `meshIds` are parallel per-object arrays. The renderer's
// meshes must already be synced to the library (see syncMeshes).
static void renderScene(GLuint fbo, int width, int height, const Camera& cam,
                        const glm::mat4& view, const glm::mat4& proj, const Renderer& r,
                        const Environment& env, bool wireframe3D, bool showGrid,
                        const std::vector<glm::mat4>& models,
                        const std::vector<glm::vec3>& colors, const std::vector<int>& meshIds,
                        int selectedIndex = -1, float gridSpacing = kGridStep) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glEnable(GL_DEPTH_TEST);
    if (cam.type == Projection::Ortho) glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
    else                               glClearColor(0.11f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Procedural sky behind everything (perspective views only).
    if (cam.type == Projection::Perspective && r.skyProgram) {
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(r.skyProgram);
        glUniformMatrix4fv(r.skyInvVPLoc, 1, GL_FALSE, glm::value_ptr(glm::inverse(proj * view)));
        glUniform3fv(r.skyCamLoc, 1, glm::value_ptr(cam.eye));
        glUniform3fv(r.skySunLoc, 1, glm::value_ptr(env.sunDir));
        glUniform3fv(r.skyHorizonLoc, 1, glm::value_ptr(env.horizonColor));
        glUniform3fv(r.skyZenithLoc, 1, glm::value_ptr(env.zenithColor));
        glUniform3fv(r.skySunColLoc, 1, glm::value_ptr(env.sunColor));
        glBindVertexArray(r.skyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

    // Grid (unlit lines), oriented into this view's plane and scaled to the
    // requested grid size.
    if (showGrid && r.gridVao) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glUseProgram(r.lineProgram);
        glm::mat4 gridM = cam.gridModel * glm::scale(glm::mat4(1.0f), glm::vec3(gridSpacing / kGridStep));
        glm::mat4 gridMVP = proj * view * gridM;
        glUniformMatrix4fv(r.lineMvpLoc, 1, GL_FALSE, glm::value_ptr(gridMVP));
        glBindVertexArray(r.gridVao);
        glDrawArrays(GL_LINES, 0, r.gridVertexCount);
    }

    // Objects: lit, per-object colour (tint) * submesh base colour. Wireframe in ortho.
    bool wire = (cam.type == Projection::Ortho) || wireframe3D;
    glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
    glUseProgram(r.litProgram);
    glUniform1i(r.litAlbedoLoc, 0);   // albedo sampler -> texture unit 0
    glUniform3fv(r.litLightDirLoc, 1, glm::value_ptr(env.sunDir));
    glUniform3fv(r.litLightColLoc, 1, glm::value_ptr(env.lightColor));
    glUniform3fv(r.litAmbientLoc, 1, glm::value_ptr(env.ambient));
    for (size_t i = 0; i < models.size(); ++i) {
        int mid = i < meshIds.size() ? meshIds[i] : 0;
        if (mid < 0 || mid >= (int)r.meshes.size()) continue;
        glm::vec3 tint = i < colors.size() ? colors[i] : glm::vec3(1.0f);
        glUniformMatrix4fv(r.litMvpLoc, 1, GL_FALSE, glm::value_ptr(proj * view * models[i]));
        glUniformMatrix4fv(r.litModelLoc, 1, GL_FALSE, glm::value_ptr(models[i]));
        for (const GPUSubMesh& s : r.meshes[mid].subs) {
            glm::vec3 c = s.baseColor * tint;
            glUniform3fv(r.litColorLoc, 1, glm::value_ptr(c));
            // Skip the texture in wireframe so the lines are plain, not textured.
            bool useTex = !wire && s.tex;
            glUniform1i(r.litHasTexLoc, useTex ? 1 : 0);
            if (useTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, s.tex); }
            glBindVertexArray(s.vao);
            glDrawElements(GL_TRIANGLES, s.count, GL_UNSIGNED_INT, 0);
        }
    }

    // Selection highlight: an orange wireframe box scaled to the object's AABB.
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    if (selectedIndex >= 0 && selectedIndex < (int)models.size() && r.selBoxVao) {
        int mid = selectedIndex < (int)meshIds.size() ? meshIds[selectedIndex] : 0;
        AABB b = (mid >= 0 && mid < (int)r.meshes.size()) ? r.meshes[mid].bounds : AABB{glm::vec3(-0.5f), glm::vec3(0.5f)};
        glm::vec3 center = 0.5f * (b.min + b.max);
        glm::vec3 size   = glm::max(b.max - b.min, glm::vec3(1e-3f)) * 1.03f;  // small margin
        glm::mat4 boxM = models[selectedIndex] * glm::translate(glm::mat4(1.0f), center) * glm::scale(glm::mat4(1.0f), size);
        glUseProgram(r.lineProgram);
        glUniformMatrix4fv(r.lineMvpLoc, 1, GL_FALSE, glm::value_ptr(proj * view * boxM));
        glBindVertexArray(r.selBoxVao);
        glDrawArrays(GL_LINES, 0, r.selBoxVertexCount);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Ray vs an axis-aligned box [bmin,bmax] in the object's local space. Returns
// the entry distance (0 if inside), or -1 on a miss. The ray parameter t is the
// same in world and local space, so values compare across objects for picking.
static float rayAabbT(glm::vec3 ro, glm::vec3 rd, const glm::mat4& model,
                      glm::vec3 bmin, glm::vec3 bmax) {
    glm::mat4 inv = glm::inverse(model);
    glm::vec3 o = glm::vec3(inv * glm::vec4(ro, 1.0f));
    glm::vec3 d = glm::vec3(inv * glm::vec4(rd, 0.0f));
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return -1.0f;
        } else {
            float t1 = (bmin[i] - o[i]) / d[i];
            float t2 = (bmax[i] - o[i]) / d[i];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            tmin = tmin > t1 ? tmin : t1;
            tmax = tmax < t2 ? tmax : t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    if (tmax < 0.0f) return -1.0f;
    return tmin >= 0.0f ? tmin : 0.0f;
}

// A scene object: a named transform + colour (tint) + which mesh asset it uses.
// npcTemplate >= 0 marks this object as a placed NPC instance of that template.
struct SceneObject {
    std::string name;
    glm::mat4 transform{1.0f};
    glm::vec3 color{0.75f, 0.75f, 0.8f};
    int meshId = 0;        // index into the mesh library (0 = built-in cube)
    int npcTemplate = -1;  // index into npcTemplates, or -1 if a plain object
};

// ---- NPC definition (template) -------------------------------------------
// One schedule entry: at `hour` the NPC goes to `location` (a scene object's
// name) and performs `activity`. Used by the simulation layer (later).
struct ScheduleEntry {
    float       hour = 8.0f;       // 0-24
    std::string activity = "work";
    std::string location = "";     // name of a scene object to go to
};

// Curated survival/RPG stats. Needs are 0-100 and deplete at a per-game-hour
// rate; the simulation will refill them by eating/drinking/resting.
struct NPCAttributes {
    float health = 100.0f;
    float hunger = 100.0f, hungerRate = 4.0f;
    float thirst = 100.0f, thirstRate = 6.0f;
    float energy = 100.0f, energyRate = 4.0f;
    int   gold = 50;
    float moveSpeed = 1.5f;        // world units / second
};

// A reusable NPC type: mesh + attributes + custom fields + a daily schedule.
struct NPCTemplate {
    std::string name = "NPC";
    int meshId = 0;                // which mesh asset this NPC uses
    NPCAttributes attr;
    std::vector<std::pair<std::string, float>> custom;  // user-defined attributes
    std::vector<ScheduleEntry> schedule;
};

// Plain-text scene format: a header + count, then per object a name line and a
// line of 16 floats (the column-major transform). No external dependency.
static bool saveScene(const std::vector<SceneObject>& scene, const char* path) {
    std::ofstream f(path);
    if (!f) return false;
    f << "FOUNDATION_SCENE 2\n" << scene.size() << "\n";
    for (const SceneObject& o : scene) {
        f << o.name << "\n";
        const float* m = glm::value_ptr(o.transform);
        for (int i = 0; i < 16; ++i) f << m[i] << (i == 15 ? '\n' : ' ');
        f << o.color.r << ' ' << o.color.g << ' ' << o.color.b << '\n';
    }
    return (bool)f;
}

static bool loadScene(std::vector<SceneObject>& out, const char* path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string header;
    if (!std::getline(f, header) || header.rfind("FOUNDATION_SCENE", 0) != 0) return false;
    std::string countLine;
    if (!std::getline(f, countLine)) return false;
    int count = std::atoi(countLine.c_str());

    std::vector<SceneObject> loaded;
    for (int i = 0; i < count; ++i) {
        SceneObject o;
        if (!std::getline(f, o.name)) return false;
        std::string nums;
        if (!std::getline(f, nums)) return false;
        std::istringstream ss(nums);
        float* m = glm::value_ptr(o.transform);
        for (int k = 0; k < 16; ++k) ss >> m[k];
        std::string colLine;            // version 2+ stores an RGB colour line
        if (std::getline(f, colLine)) {
            std::istringstream cs(colLine);
            cs >> o.color.r >> o.color.g >> o.color.b;
        }
        loaded.push_back(o);
    }
    out = std::move(loaded);
    return true;
}

// Editing context for the active viewport: the scene + current selection index.
struct EditContext {
    std::vector<SceneObject>* scene = nullptr;
    const std::vector<MeshAsset>* lib = nullptr;  // for per-object AABB picking
    int* selected = nullptr;          // index into scene, -1 = none
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool  snap = false;               // snap gizmo drags to increments
    float snapTranslate = 1.0f;       // world units (== grid size)
    float snapRotate = 15.0f;         // degrees
    float snapScale = 0.25f;
};

// One dockable panel that renders `cam`'s view of the scene into `rt`, with the
// image filling the panel edge-to-edge and the view label in the top-left. If
// `edit` is supplied, draws a gizmo for the selected object and handles
// click-to-select. Returns true if the viewport image was hovered this frame.
static bool drawViewportPanel(const char* name, RenderTarget& rt, const Camera& cam,
                              const Renderer& r, const Environment& env, bool wireframe3D, bool showGrid,
                              const std::vector<glm::mat4>& models, const std::vector<glm::vec3>& colors,
                              const std::vector<int>& meshIds,
                              int selectedIndex, float gridSpacing, EditContext* edit = nullptr) {
    bool imageHovered = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(name);
    ImGui::PopStyleVar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = (int)avail.x, h = (int)avail.y;
    if (w > 0 && h > 0) {
        float aspect = h == 0 ? 1.0f : (float)w / (float)h;
        glm::mat4 view = viewMatrix(cam);
        glm::mat4 proj = projMatrix(cam, aspect);

        resizeTarget(rt, w, h);
        renderScene(rt.fbo, rt.width, rt.height, cam, view, proj, r, env, wireframe3D, showGrid, models, colors, meshIds, selectedIndex, gridSpacing);
        // Flip V (uv 0,1 -> 1,0): GL textures have origin at bottom-left.
        ImGui::Image((ImTextureID)(intptr_t)rt.color, avail, ImVec2(0, 1), ImVec2(1, 0));
        imageHovered = ImGui::IsItemHovered();

        ImVec2 p0 = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p0.x + 6, p0.y + 4), IM_COL32(210, 210, 220, 255), cam.label);

        if (edit && edit->scene && edit->selected) {
            std::vector<SceneObject>& scene = *edit->scene;
            int sel = *edit->selected;
            // Gizmo for the selected object, clipped to this viewport's rect.
            if (sel >= 0 && sel < (int)scene.size()) {
                ImGuizmo::SetOrthographic(cam.type == Projection::Ortho);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(p0.x, p0.y, (float)w, (float)h);
                float snapVal[3] = {0, 0, 0};
                const float* snapPtr = nullptr;
                if (edit->snap) {
                    if (edit->op == ImGuizmo::TRANSLATE)   { snapVal[0] = snapVal[1] = snapVal[2] = edit->snapTranslate; }
                    else if (edit->op == ImGuizmo::ROTATE) { snapVal[0] = edit->snapRotate; }
                    else                                   { snapVal[0] = edit->snapScale; }
                    snapPtr = snapVal;
                }
                ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                                     edit->op, edit->mode, glm::value_ptr(scene[sel].transform),
                                     nullptr, snapPtr);
            }
            // Click-to-select: nearest object hit by the ray, unless over the gizmo.
            if (imageHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
                ImVec2 m = ImGui::GetIO().MousePos;
                float nx = 2.0f * (m.x - p0.x) / (float)w - 1.0f;
                float ny = 1.0f - 2.0f * (m.y - p0.y) / (float)h;
                glm::mat4 invVP = glm::inverse(proj * view);
                glm::vec4 pn = invVP * glm::vec4(nx, ny, -1.0f, 1.0f); pn /= pn.w;
                glm::vec4 pf = invVP * glm::vec4(nx, ny,  1.0f, 1.0f); pf /= pf.w;
                glm::vec3 ro = glm::vec3(pn);
                glm::vec3 rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
                int best = -1; float bestT = 1e30f;
                for (int i = 0; i < (int)scene.size(); ++i) {
                    glm::vec3 bmin(-0.5f), bmax(0.5f);
                    if (edit->lib) {
                        int mid = scene[i].meshId;
                        if (mid >= 0 && mid < (int)edit->lib->size()) {
                            bmin = (*edit->lib)[mid].bounds.min;
                            bmax = (*edit->lib)[mid].bounds.max;
                        }
                    }
                    float t = rayAabbT(ro, rd, scene[i].transform, bmin, bmax);
                    if (t >= 0.0f && t < bestT) { bestT = t; best = i; }
                }
                *edit->selected = best;
            }
        }
    }
    ImGui::End();
    return imageHovered;
}

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Foundation - Editor", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);   // vsync

    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "Failed to load OpenGL via GLAD\n");
        glfwTerminate();
        return 1;
    }

    // ---- ImGui setup ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // enable dockable panels
    // Don't load/save imgui.ini while we're still changing the panel set: a
    // stale layout file silently overrides our DockBuilder default and leaves
    // newly-renamed panels floating/collapsed. Build a fresh layout each launch.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    // Squared-off, darker Hammer-ish theme.
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = style.FrameRounding = style.TabRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg]       = ImVec4(0.10f, 0.10f, 0.11f, 1.0f);
    style.Colors[ImGuiCol_TitleBg]        = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive]  = ImVec4(0.16f, 0.16f, 0.19f, 1.0f);
    style.Colors[ImGuiCol_Tab]            = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TabActive]      = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Editor's own scene resources (built in the editor GL context).
    Renderer renderer = createRenderer();

    bool  wireframe3D = false;  // 3D view solid by default; ortho always wireframe
    bool  showGrid    = true;
    bool  snapEnabled = false;  // snap gizmo drags to the grid
    float gridSize    = 1.0f;   // grid spacing AND translate snap increment

    Environment env;            // game clock + day/night sky + sun light

    // Mesh library: asset #0 is the built-in cube; loaded models append. Each
    // Renderer uploads these into its own GL context lazily (syncMeshes).
    std::vector<MeshAsset> meshLibrary;
    meshLibrary.push_back(buildCubeMesh());

    // NPC templates (reusable types). Instances are SceneObjects whose
    // npcTemplate indexes into this list.
    std::vector<NPCTemplate> npcTemplates;
    int selectedTemplate = -1;

    // The scene is a list of objects referencing mesh assets. The gizmo edits
    // the selected object's transform.
    std::vector<SceneObject> scene;
    {
        // Start by loading models/testModel.glb (try a few cwd-relative paths).
        const char* candidates[] = { "models/testModel.glb", "../models/testModel.glb", "testModel.glb" };
        MeshAsset asset;
        bool loaded = false;
        for (const char* c : candidates) if (loadModel(c, asset)) { loaded = true; break; }
        if (loaded) {
            meshLibrary.push_back(std::move(asset));
            SceneObject o;
            o.name   = meshLibrary.back().name;
            o.meshId = (int)meshLibrary.size() - 1;
            o.color  = glm::vec3(1.0f);
            scene.push_back(o);
        } else {
            std::fprintf(stderr, "Could not load testModel.glb; starting with a cube.\n");
            scene.push_back({"Cube", glm::mat4(1.0f)});
        }
    }
    int selected = scene.empty() ? -1 : 0;   // index into scene, -1 = none
    ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      gizmoMode = ImGuizmo::WORLD;

    // Undo/redo: snapshot the whole (tiny) scene before each change. Copy/paste
    // holds one object on a clipboard.
    std::vector<std::vector<SceneObject>> undoStack, redoStack;
    SceneObject clipboard;
    bool hasClipboard = false;
    bool prevUsingGizmo = false;   // edge-detect gizmo drag start for undo

    const char* kScenePath = "scene.fdn";
    std::string sceneStatus;       // last save/load result, shown in Outliner

    // Play-mode state. Play opens a SEPARATE OS window with its own GL context
    // running the game; the editor stays static.
    GLFWwindow* playWindow   = nullptr;
    Renderer    playRenderer{};
    bool        prevF5       = false;  // edge-detect the F5 toggle (editor window)
    bool        prevPlayF11  = false;  // edge-detect F11 (play window fullscreen)
    bool        playFullscreen = false;
    int         playWinX = 0, playWinY = 0, playWinW = 1280, playWinH = 720;
    double      lastTime = glfwGetTime();

    // Game camera used by the play window: a simple perspective view.
    Camera gameCam;
    gameCam.type = Projection::Perspective;
    gameCam.eye  = {0.0f, 1.2f, 3.5f};
    gameCam.fovDeg = 60.0f;
    gameCam.gridModel = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));

    // Open/close the play window. createRenderer/destroyRenderer must run with
    // the play window's context current (VAOs are per-context).
    auto openPlay = [&]() {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        playWindow = glfwCreateWindow(playWinW, playWinH, "Foundation - Play", nullptr, nullptr);
        if (!playWindow) { std::fprintf(stderr, "Failed to create play window\n"); return; }
        glfwMakeContextCurrent(playWindow);
        glfwSwapInterval(1);
        playRenderer = createRenderer();
        glfwMakeContextCurrent(window);   // restore editor context
        playFullscreen = false;
    };
    auto closePlay = [&]() {
        glfwMakeContextCurrent(playWindow);
        destroyRenderer(playRenderer);
        glfwMakeContextCurrent(window);
        glfwDestroyWindow(playWindow);
        playWindow = nullptr;
    };

    // Four viewports: a perspective 3/4 view plus three orthographic views
    // looking straight down each world axis. gridModel rotates the XY grid mesh
    // into each view's plane (perspective uses the XZ ground plane).
    RenderTarget rtPersp, rtTop, rtFront, rtRight;
    glm::mat4 toXZ = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0));
    glm::mat4 toYZ = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));

    Camera camPersp;
    camPersp.type = Projection::Perspective;
    camPersp.eye = {2.5f, 2.0f, 3.0f};
    camPersp.gridModel = toXZ;          // ground plane
    camPersp.label = "3D camera";
    // Seed fly state so it initially looks at the origin.
    {
        camPersp.flyPos = camPersp.eye;
        glm::vec3 d0 = glm::normalize(camPersp.target - camPersp.eye);
        camPersp.yaw   = std::atan2(d0.x, -d0.z);
        camPersp.pitch = std::asin(d0.y);
    }
    bool flying = false;   // true while right-mouse fly is active in Perspective

    Camera camTop;   // looking straight down -Y; "up" can't be Y, so use -Z
    camTop.type = Projection::Ortho;
    camTop.eye = {0, 3, 0};
    camTop.up  = {0, 0, -1};
    camTop.gridModel = toXZ;
    camTop.label = "top (x/z)";

    Camera camFront; // looking down -Z from +Z
    camFront.type = Projection::Ortho;
    camFront.eye = {0, 0, 3};
    camFront.label = "front (x/y)";     // grid stays in XY plane (identity)

    Camera camRight; // looking down -X from +X
    camRight.type = Projection::Ortho;
    camRight.eye = {3, 0, 0};
    camRight.gridModel = toYZ;
    camRight.label = "side (z/y)";

    // Central-area layout: either the 2x2 quad grid, or one viewport maximized
    // to fill the area (NOT OS fullscreen -- the Controls panel still shows).
    // Engine starts with the Perspective view maximized.
    const char* viewName[4] = {"Perspective", "Top", "Front", "Right"};
    bool layoutSingle = true;     // true = one maximized view, false = quad grid
    int  activeView   = 0;        // index into viewName when maximized
    bool layoutDirty  = true;     // rebuild the dock layout next frame

    auto rebuildLayout = [&](ImGuiID dockId) {
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);
        ImGuiID mainId = dockId;
        ImGuiID leftId  = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.18f, nullptr, &mainId);
        ImGuiID rightId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.22f, nullptr, &mainId);
        ImGuiID controlsId;
        ImGuiID outlinerId = ImGui::DockBuilderSplitNode(leftId, ImGuiDir_Up, 0.4f, nullptr, &controlsId);
        ImGui::DockBuilderDockWindow("Outliner", outlinerId);
        ImGui::DockBuilderDockWindow("Controls", controlsId);
        ImGui::DockBuilderDockWindow("Environment", controlsId);   // tabbed with Controls
        ImGui::DockBuilderDockWindow("Properties", rightId);
        ImGui::DockBuilderDockWindow("NPC Editor", rightId);   // tabbed with Properties
        if (layoutSingle) {
            ImGui::DockBuilderDockWindow(viewName[activeView], mainId);
        } else {
            ImGuiID bottomId;
            ImGuiID topId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Up, 0.5f, nullptr, &bottomId);
            ImGuiID trId, brId;
            ImGuiID tlId = ImGui::DockBuilderSplitNode(topId, ImGuiDir_Left, 0.5f, nullptr, &trId);
            ImGuiID blId = ImGui::DockBuilderSplitNode(bottomId, ImGuiDir_Left, 0.5f, nullptr, &brId);
            ImGui::DockBuilderDockWindow("Perspective", tlId);
            ImGui::DockBuilderDockWindow("Top", trId);
            ImGui::DockBuilderDockWindow("Front", blId);
            ImGui::DockBuilderDockWindow("Right", brId);
        }
        ImGui::DockBuilderFinish(dockId);
    };

    auto clampSelected = [&]() {
        if (selected >= (int)scene.size()) selected = (int)scene.size() - 1;
    };
    auto snapshot = [&]() {            // record current scene for undo
        undoStack.push_back(scene);
        redoStack.clear();
        if (undoStack.size() > 64) undoStack.erase(undoStack.begin());
    };
    auto doUndo = [&]() {
        if (undoStack.empty()) return;
        redoStack.push_back(scene);
        scene = undoStack.back(); undoStack.pop_back();
        clampSelected();
    };
    auto doRedo = [&]() {
        if (redoStack.empty()) return;
        undoStack.push_back(scene);
        scene = redoStack.back(); redoStack.pop_back();
        clampSelected();
    };
    auto doCopy = [&]() {
        if (selected >= 0 && selected < (int)scene.size()) { clipboard = scene[selected]; hasClipboard = true; }
    };
    auto doPaste = [&]() {
        if (!hasClipboard) return;
        snapshot();
        SceneObject o = clipboard;
        float off = gridSize > 0.0f ? gridSize : 1.0f;
        o.transform = glm::translate(glm::mat4(1.0f), glm::vec3(off, 0.0f, off)) * o.transform;
        o.name += " copy";
        scene.push_back(o);
        selected = (int)scene.size() - 1;
    };
    auto doAddCube = [&]() {
        snapshot();
        static const glm::vec3 palette[6] = {
            {0.85f,0.35f,0.35f}, {0.4f,0.7f,0.4f}, {0.4f,0.55f,0.9f},
            {0.9f,0.8f,0.35f},   {0.7f,0.45f,0.85f}, {0.4f,0.8f,0.8f},
        };
        SceneObject o;
        o.name = "Cube " + std::to_string(scene.size());
        o.transform = glm::translate(glm::mat4(1.0f), glm::vec3((float)scene.size() * 1.5f, 0.0f, 0.0f));
        o.color = palette[scene.size() % 6];
        scene.push_back(o);
        selected = (int)scene.size() - 1;
    };
    auto doDelete = [&]() {
        if (selected < 0 || selected >= (int)scene.size()) return;
        snapshot();
        scene.erase(scene.begin() + selected);
        if (selected >= (int)scene.size()) selected = (int)scene.size() - 1;
    };
    auto doSave = [&]() {
        sceneStatus = saveScene(scene, kScenePath) ? "Saved scene.fdn" : "Save FAILED";
    };
    auto doLoad = [&]() {
        std::vector<SceneObject> loaded;
        if (loadScene(loaded, kScenePath)) {
            snapshot();
            scene = std::move(loaded);
            selected = scene.empty() ? -1 : 0;
            sceneStatus = "Loaded scene.fdn";
        } else sceneStatus = "Load FAILED (no scene.fdn?)";
    };
    auto doLoadMesh = [&]() {
        std::string path = openModelFileDialog("models");
        if (path.empty()) return;
        MeshAsset asset;
        if (loadModel(path, asset)) {
            snapshot();
            meshLibrary.push_back(std::move(asset));
            SceneObject o;
            o.name   = meshLibrary.back().name;
            o.meshId = (int)meshLibrary.size() - 1;
            o.color  = glm::vec3(1.0f);
            scene.push_back(o);
            selected = (int)scene.size() - 1;
            sceneStatus = "Loaded " + o.name;
        } else sceneStatus = "Mesh load FAILED";
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

        // The play window can be closed via its own [X]; treat that as Stop.
        if (playWindow && glfwWindowShouldClose(playWindow)) closePlay();

        // F5 (editor) toggles the play window open/closed (edge-detected).
        bool f5 = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5 && !prevF5) { if (playWindow) closePlay(); else openPlay(); }
        prevF5 = f5;

        // Frame timing.
        double now = glfwGetTime();
        float  dt  = (float)(now - lastTime);
        lastTime   = now;
        updateEnvironment(env, dt, playWindow != nullptr);   // clock ticks only in play mode

        // ---- Editor render (editor GL context) ----
        glfwMakeContextCurrent(window);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // W/E/R switch gizmo operation (Unity-style) -- but not while flying,
        // where WASD drives the camera instead.
        if (!io.WantTextInput && !flying) {
            if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmoOp = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E, false)) gizmoOp = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R, false)) gizmoOp = ImGuizmo::SCALE;
        }

        // Ctrl shortcuts: Z undo, Y redo, C copy, V paste, S save.
        if (io.KeyCtrl && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) doUndo();
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) doRedo();
            if (ImGui::IsKeyPressed(ImGuiKey_C, false)) doCopy();
            if (ImGui::IsKeyPressed(ImGuiKey_V, false)) doPaste();
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) doSave();
        }

        // Derive the perspective camera's eye/target from its fly state.
        {
            glm::vec3 f = forwardFromYawPitch(camPersp.yaw, camPersp.pitch);
            camPersp.eye = camPersp.flyPos;
            camPersp.target = camPersp.flyPos + f;
            camPersp.up = glm::vec3(0, 1, 0);
        }

        // ---- Top menu bar ----
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save", "Ctrl+S"))            doSave();
                if (ImGui::MenuItem("Load"))                      doLoad();
                if (ImGui::MenuItem("Load Mesh (GLB/FBX)..."))    doLoadMesh();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit"))                      glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undoStack.empty())) doUndo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redoStack.empty())) doRedo();
                ImGui::Separator();
                bool hasSel = selected >= 0 && selected < (int)scene.size();
                if (ImGui::MenuItem("Copy",   "Ctrl+C", false, hasSel))        doCopy();
                if (ImGui::MenuItem("Paste",  "Ctrl+V", false, hasClipboard))  doPaste();
                if (ImGui::MenuItem("Delete", nullptr,  false, hasSel))        doDelete();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Create")) {
                if (ImGui::MenuItem("Cube"))                   doAddCube();
                if (ImGui::MenuItem("Mesh (GLB/FBX)..."))      doLoadMesh();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Layout")) {
                if (ImGui::MenuItem("Quad view", "Space", !layoutSingle)) { layoutSingle = false; layoutDirty = true; }
                ImGui::Separator();
                for (int i = 0; i < 4; ++i)
                    if (ImGui::MenuItem(viewName[i], nullptr, layoutSingle && activeView == i))
                        { activeView = i; layoutSingle = true; layoutDirty = true; }
                ImGui::Separator();
                ImGui::MenuItem("Show grid",         nullptr, &showGrid);
                ImGui::MenuItem("Wireframe 3D view", nullptr, &wireframe3D);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Play")) {
                if (ImGui::MenuItem(playWindow ? "Stop" : "Play", "F5")) { if (playWindow) closePlay(); else openPlay(); }
                ImGui::EndMenu();
            }
            // Right-aligned clock + mode indicator.
            int hh = (int)env.timeOfDay;
            int mm = (int)((env.timeOfDay - hh) * 60.0f);
            ImGui::SameLine(ImGui::GetWindowWidth() - 230.0f);
            ImGui::Text("Day %d  %02d:%02d", env.day, hh, mm);
            ImGui::SameLine();
            ImGui::TextColored(playWindow ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                               playWindow ? "  PLAYING" : "  EDIT");
            ImGui::EndMainMenuBar();
        }

        // Full-window dockspace so panels can be docked/tabbed/resized.
        ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (layoutDirty) { rebuildLayout(dockId); layoutDirty = false; }

        // Editor views show each object at its authored transform (the gizmo
        // edits the selected one); the simulation spin runs only in the play
        // window. The gizmo + click-select live in the Perspective view.
        EditContext edit;
        edit.scene = &scene;
        edit.lib = &meshLibrary;
        edit.selected = &selected;
        edit.op = gizmoOp;
        edit.mode = gizmoMode;
        edit.snap = snapEnabled;
        edit.snapTranslate = gridSize;

        syncMeshes(renderer, meshLibrary);   // upload any newly-loaded meshes (editor context)
        std::vector<glm::mat4> editorModels;
        std::vector<glm::vec3> editorColors;
        std::vector<int>       editorMeshIds;
        editorModels.reserve(scene.size());
        editorColors.reserve(scene.size());
        editorMeshIds.reserve(scene.size());
        for (const SceneObject& o : scene) {
            editorModels.push_back(o.transform);
            editorColors.push_back(o.color);
            editorMeshIds.push_back(o.meshId);
        }

        // Snapshot before any gizmo edit this frame, so a drag that starts now
        // can be undone back to this pre-drag state.
        std::vector<SceneObject> frameStartScene = scene;

        // Submit only the viewports the current layout shows; track which one
        // the mouse is over so Space can maximize/restore it.
        int hoveredView = -1;
        auto drawView = [&](int i) -> bool {
            switch (i) {
                case 0: return drawViewportPanel("Perspective", rtPersp, camPersp, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize, &edit);
                case 1: return drawViewportPanel("Top",   rtTop,   camTop,   renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize);
                case 2: return drawViewportPanel("Front", rtFront, camFront, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize);
                default:return drawViewportPanel("Right", rtRight, camRight, renderer, env, wireframe3D, showGrid, editorModels, editorColors, editorMeshIds, selected, gridSize);
            }
        };
        if (layoutSingle) {
            if (drawView(activeView)) hoveredView = activeView;
        } else {
            for (int i = 0; i < 4; ++i) if (drawView(i)) hoveredView = i;
        }
        // One undo entry per gizmo drag: record the pre-drag scene on the frame
        // the manipulation starts.
        bool usingGizmo = ImGuizmo::IsUsing();
        if (usingGizmo && !prevUsingGizmo) {
            undoStack.push_back(frameStartScene);
            redoStack.clear();
            if (undoStack.size() > 64) undoStack.erase(undoStack.begin());
        }
        prevUsingGizmo = usingGizmo;

        // Space toggles maximize: in quad, maximize the hovered view; in single,
        // return to the quad grid.
        if (!io.WantTextInput && !flying && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (layoutSingle)            { layoutSingle = false; layoutDirty = true; }
            else if (hoveredView >= 0)   { activeView = hoveredView; layoutSingle = true; layoutDirty = true; }
        }

        // ---- Camera navigation ----
        // Right-mouse over the Perspective view = Unreal-style flythrough; the
        // mouse wheel zooms whichever view is hovered (ortho zoom / persp dolly).
        {
            Camera* cams[4] = {&camPersp, &camTop, &camFront, &camRight};
            bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (!flying && rmb && hoveredView == 0) {
                flying = true;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  // lock + hide
            } else if (flying && !rmb) {
                flying = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            }

            if (flying) {
                camPersp.yaw   += io.MouseDelta.x * 0.0025f;
                camPersp.pitch -= io.MouseDelta.y * 0.0025f;          // invert Y
                camPersp.pitch  = glm::clamp(camPersp.pitch, -1.54f, 1.54f);
                if (io.MouseWheel != 0.0f)                            // wheel = fly speed
                    camPersp.flySpeed = glm::clamp(camPersp.flySpeed * (1.0f + 0.1f * io.MouseWheel), 0.25f, 60.0f);
                glm::vec3 f   = forwardFromYawPitch(camPersp.yaw, camPersp.pitch);
                glm::vec3 rgt = glm::normalize(glm::cross(f, glm::vec3(0, 1, 0)));
                glm::vec3 wup(0, 1, 0);
                float v = camPersp.flySpeed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_W)) camPersp.flyPos += f   * v;
                if (ImGui::IsKeyDown(ImGuiKey_S)) camPersp.flyPos -= f   * v;
                if (ImGui::IsKeyDown(ImGuiKey_D)) camPersp.flyPos += rgt * v;
                if (ImGui::IsKeyDown(ImGuiKey_A)) camPersp.flyPos -= rgt * v;
                if (ImGui::IsKeyDown(ImGuiKey_E)) camPersp.flyPos += wup * v;
                if (ImGui::IsKeyDown(ImGuiKey_Q)) camPersp.flyPos -= wup * v;
            } else if (io.MouseWheel != 0.0f && hoveredView >= 0) {
                Camera* c = cams[hoveredView];
                if (c->type == Projection::Ortho) {
                    c->orthoHalfHeight = glm::clamp(c->orthoHalfHeight * (1.0f - 0.1f * io.MouseWheel), 0.05f, 50.0f);
                } else {
                    glm::vec3 f = forwardFromYawPitch(c->yaw, c->pitch);  // dolly = zoom
                    c->flyPos += f * (io.MouseWheel * 0.4f);
                }
            }
        }

        // --- Outliner: scene object list + add/delete ---
        ImGui::Begin("Outliner");
        if (ImGui::Button("Add Cube"))   doAddCube();
        ImGui::SameLine();
        ImGui::BeginDisabled(selected < 0 || selected >= (int)scene.size());
        if (ImGui::Button("Delete"))     doDelete();
        ImGui::EndDisabled();
        ImGui::Separator();
        for (int i = 0; i < (int)scene.size(); ++i) {
            if (ImGui::Selectable(scene[i].name.c_str(), selected == i))
                selected = i;
        }
        if (!sceneStatus.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", sceneStatus.c_str());
        }
        ImGui::End();

        // --- Controls: tools + display settings + help ---
        ImGui::Begin("Controls");
        ImGui::SeparatorText("Gizmo");
        if (ImGui::RadioButton("Move (W)",   gizmoOp == ImGuizmo::TRANSLATE)) gizmoOp = ImGuizmo::TRANSLATE;
        if (ImGui::RadioButton("Rotate (E)", gizmoOp == ImGuizmo::ROTATE))    gizmoOp = ImGuizmo::ROTATE;
        if (ImGui::RadioButton("Scale (R)",  gizmoOp == ImGuizmo::SCALE))     gizmoOp = ImGuizmo::SCALE;
        bool worldMode = (gizmoMode == ImGuizmo::WORLD);
        if (ImGui::Checkbox("World space", &worldMode))
            gizmoMode = worldMode ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

        ImGui::SeparatorText("Grid");
        ImGui::Checkbox("Show grid", &showGrid);
        ImGui::Checkbox("Snap to grid", &snapEnabled);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::DragFloat("Grid size", &gridSize, 0.05f, 0.05f, 16.0f, "%.2f"))
            gridSize = glm::clamp(gridSize, 0.05f, 16.0f);

        ImGui::SeparatorText("Stats");
        ImGui::Text("%.1f FPS  (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);

        ImGui::SeparatorText("Help");
        ImGui::TextWrapped("Wheel zooms the hovered view. In Perspective hold RIGHT mouse to fly (WASD, E/Q up/down, wheel = speed). Space maximizes the hovered view.");
        ImGui::End();

        // --- Environment: time of day + day/night ---
        ImGui::Begin("Environment");
        int hh = (int)env.timeOfDay;
        int mm = (int)((env.timeOfDay - hh) * 60.0f);
        ImGui::Text("Day %d   %02d:%02d", env.day, hh, mm);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##time", &env.timeOfDay, 0.0f, 24.0f, "%.2f h");
        ImGui::Checkbox("Clock runs in Play", &env.running);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Day length (s)", &env.dayLengthSec, 10.0f, 3600.0f, "%.0f");
        ImGui::TextDisabled("Real seconds per full 24h day.");
        ImGui::Separator();
        ImGui::TextWrapped("Time only advances in Play mode (F5). In the editor, drag the time slider to preview the sky. NPC schedules use this clock.");
        ImGui::End();

        // --- Properties: inspector for the selected object (right side) ---
        ImGui::Begin("Properties");
        if (selected >= 0 && selected < (int)scene.size()) {
            SceneObject& o = scene[selected];
            bool propActivated = false;   // snapshot for undo on the first edit

            ImGui::InputText("Name", &o.name);
            propActivated |= ImGui::IsItemActivated();

            ImGui::SeparatorText("Transform");
            float t[3], rot[3], s[3];
            ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(o.transform), t, rot, s);
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", t,   0.05f);
            propActivated |= ImGui::IsItemActivated();
            changed |= ImGui::DragFloat3("Rotation", rot, 0.5f);
            propActivated |= ImGui::IsItemActivated();
            changed |= ImGui::DragFloat3("Scale",    s,   0.05f);
            propActivated |= ImGui::IsItemActivated();
            if (changed)
                ImGuizmo::RecomposeMatrixFromComponents(t, rot, s, glm::value_ptr(o.transform));

            ImGui::SeparatorText("Material");
            ImGui::ColorEdit3("Color", glm::value_ptr(o.color));
            propActivated |= ImGui::IsItemActivated();
            ImGui::TextDisabled("Texture: drop files in materials/ (coming soon)");

            if (o.npcTemplate >= 0 && o.npcTemplate < (int)npcTemplates.size()) {
                ImGui::SeparatorText("NPC");
                ImGui::Text("Template: %s", npcTemplates[o.npcTemplate].name.c_str());
                ImGui::TextDisabled("Edit the type in the NPC Editor tab.");
            }

            if (propActivated) snapshot();
        } else {
            ImGui::TextDisabled("No object selected.");
            ImGui::TextWrapped("Click an object in a viewport or the Outliner to edit it here.");
        }
        ImGui::End();

        // --- NPC Editor: author reusable NPC templates ---
        ImGui::Begin("NPC Editor");
        if (ImGui::Button("New Template")) {
            NPCTemplate t;
            t.name = "NPC " + std::to_string(npcTemplates.size());
            npcTemplates.push_back(t);
            selectedTemplate = (int)npcTemplates.size() - 1;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedTemplate < 0 || selectedTemplate >= (int)npcTemplates.size());
        if (ImGui::Button("Delete Template")) {
            npcTemplates.erase(npcTemplates.begin() + selectedTemplate);
            for (SceneObject& so : scene) {     // keep instance links valid
                if (so.npcTemplate == selectedTemplate)      so.npcTemplate = -1;
                else if (so.npcTemplate > selectedTemplate)  so.npcTemplate--;
            }
            if (selectedTemplate >= (int)npcTemplates.size()) selectedTemplate = (int)npcTemplates.size() - 1;
        }
        ImGui::EndDisabled();

        ImGui::BeginChild("##tpllist", ImVec2(0, 90), true);
        for (int i = 0; i < (int)npcTemplates.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(npcTemplates[i].name.c_str(), selectedTemplate == i)) selectedTemplate = i;
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (selectedTemplate >= 0 && selectedTemplate < (int)npcTemplates.size()) {
            NPCTemplate& t = npcTemplates[selectedTemplate];
            ImGui::InputText("Name", &t.name);
            const char* curMesh = (t.meshId >= 0 && t.meshId < (int)meshLibrary.size())
                                  ? meshLibrary[t.meshId].name.c_str() : "(none)";
            if (ImGui::BeginCombo("Mesh", curMesh)) {
                for (int m = 0; m < (int)meshLibrary.size(); ++m) {
                    ImGui::PushID(m);
                    if (ImGui::Selectable(meshLibrary[m].name.c_str(), t.meshId == m)) t.meshId = m;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            ImGui::SeparatorText("Attributes");
            ImGui::DragFloat("Health", &t.attr.health, 1.0f, 0.0f, 100.0f);
            ImGui::DragFloat("Hunger", &t.attr.hunger, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##hu", &t.attr.hungerRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Thirst", &t.attr.thirst, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##th", &t.attr.thirstRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragFloat("Energy", &t.attr.energy, 1.0f, 0.0f, 100.0f);
            ImGui::SameLine(); ImGui::SetNextItemWidth(60); ImGui::DragFloat("/h##en", &t.attr.energyRate, 0.1f, 0.0f, 50.0f);
            ImGui::DragInt("Gold", &t.attr.gold, 1.0f, 0, 1000000);
            ImGui::DragFloat("Move speed", &t.attr.moveSpeed, 0.1f, 0.0f, 20.0f);

            ImGui::SeparatorText("Custom attributes");
            int removeC = -1;
            for (int c = 0; c < (int)t.custom.size(); ++c) {
                ImGui::PushID(2000 + c);
                ImGui::SetNextItemWidth(120); ImGui::InputText("##cn", &t.custom[c].first);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::DragFloat("##cv", &t.custom[c].second, 0.1f);
                ImGui::SameLine(); if (ImGui::SmallButton("x")) removeC = c;
                ImGui::PopID();
            }
            if (removeC >= 0) t.custom.erase(t.custom.begin() + removeC);
            if (ImGui::Button("Add attribute")) t.custom.push_back({"attribute", 0.0f});

            ImGui::SeparatorText("Schedule (hour / activity / location)");
            int removeS = -1;
            for (int e = 0; e < (int)t.schedule.size(); ++e) {
                ImGui::PushID(3000 + e);
                ScheduleEntry& se = t.schedule[e];
                ImGui::SetNextItemWidth(64); ImGui::DragFloat("##hr", &se.hour, 0.25f, 0.0f, 24.0f, "%.2f");
                ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputText("##act", &se.activity);
                ImGui::SameLine(); ImGui::SetNextItemWidth(90);
                if (ImGui::BeginCombo("##loc", se.location.empty() ? "(where)" : se.location.c_str())) {
                    for (int o = 0; o < (int)scene.size(); ++o) {
                        ImGui::PushID(o);
                        if (ImGui::Selectable(scene[o].name.c_str(), se.location == scene[o].name)) se.location = scene[o].name;
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(); if (ImGui::SmallButton("x")) removeS = e;
                ImGui::PopID();
            }
            if (removeS >= 0) t.schedule.erase(t.schedule.begin() + removeS);
            if (ImGui::Button("Add schedule entry")) t.schedule.push_back({8.0f, "work", ""});

            ImGui::Separator();
            if (ImGui::Button("Place NPC in world")) {
                SceneObject o;
                o.name = t.name;
                o.meshId = t.meshId;
                o.npcTemplate = selectedTemplate;
                o.color = glm::vec3(1.0f);
                scene.push_back(o);
                selected = (int)scene.size() - 1;
            }
        } else {
            ImGui::TextDisabled("No template selected. Click 'New Template'.");
        }
        ImGui::End();

        // --- Present: render ImGui to the real window ---
        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);

        // ---- Play render (separate window, its own GL context) ----
        if (playWindow) {
            glfwMakeContextCurrent(playWindow);

            // Esc stops play (back to editor); F11 toggles borderless fullscreen.
            if (glfwGetKey(playWindow, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                closePlay();
            } else {
                bool f11 = glfwGetKey(playWindow, GLFW_KEY_F11) == GLFW_PRESS;
                if (f11 && !prevPlayF11) {
                    if (!playFullscreen) {
                        glfwGetWindowPos(playWindow, &playWinX, &playWinY);
                        glfwGetWindowSize(playWindow, &playWinW, &playWinH);
                        GLFWmonitor* mon = glfwGetPrimaryMonitor();
                        const GLFWvidmode* vm = glfwGetVideoMode(mon);
                        glfwSetWindowMonitor(playWindow, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
                    } else {
                        glfwSetWindowMonitor(playWindow, nullptr, playWinX, playWinY, playWinW, playWinH, 0);
                    }
                    playFullscreen = !playFullscreen;
                }
                prevPlayF11 = f11;

                int gw, gh;
                glfwGetFramebufferSize(playWindow, &gw, &gh);
                float gAspect = gh == 0 ? 1.0f : (float)gw / (float)gh;
                glm::mat4 gView = viewMatrix(gameCam);
                glm::mat4 gProj = projMatrix(gameCam, gAspect);
                syncMeshes(playRenderer, meshLibrary);   // upload meshes into the play context
                std::vector<glm::mat4> playModels;
                std::vector<glm::vec3> playColors;
                std::vector<int>       playMeshIds;
                playModels.reserve(scene.size());
                playColors.reserve(scene.size());
                playMeshIds.reserve(scene.size());
                for (const SceneObject& o : scene) {
                    playModels.push_back(o.transform);
                    playColors.push_back(o.color);
                    playMeshIds.push_back(o.meshId);
                }
                renderScene(0, gw, gh, gameCam, gView, gProj, playRenderer, env, false, true, playModels, playColors, playMeshIds, -1, gridSize);
                glfwSwapBuffers(playWindow);
            }
        }
    }

    if (playWindow) closePlay();
    destroyRenderer(renderer);
    destroyTarget(rtPersp);
    destroyTarget(rtTop);
    destroyTarget(rtFront);
    destroyTarget(rtRight);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
