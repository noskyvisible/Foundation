#include "shaders.h"

#include <cstdio>

// ---- Shaders -------------------------------------------------------------
// "line" program: flat per-vertex colour (grid + selection box). "lit" program:
// per-object colour shaded by a fixed directional light using surface normals.

const char* kLineVS = R"glsl(
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

const char* kLineFS = R"glsl(
#version 330 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)glsl";

const char* kLitVS = R"glsl(
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

const char* kLitFS = R"glsl(
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

// Skinned vertex shader: deforms the mesh by a palette of bone matrices, then
// shades with the same kLitFS fragment shader as static meshes. MAX_BONES must
// be >= the largest skeleton we load (Scarlet has 36).
const char* kSkinVS = R"glsl(
#version 330 core
layout (location = 0) in vec3  aPos;
layout (location = 1) in vec3  aNormal;
layout (location = 2) in vec2  aUv;
layout (location = 3) in ivec4 aBoneIds;
layout (location = 4) in vec4  aWeights;
uniform mat4 uMVP;     // proj * view * (model * importFix)
uniform mat4 uModel;   // model * importFix (for normals)
const int MAX_BONES = 64;
uniform mat4 uBones[MAX_BONES];
out vec3 vNormal;
out vec2 vUv;
void main() {
    mat4 skin = aWeights.x * uBones[aBoneIds.x] + aWeights.y * uBones[aBoneIds.y]
              + aWeights.z * uBones[aBoneIds.z] + aWeights.w * uBones[aBoneIds.w];
    vec4 sp = skin * vec4(aPos, 1.0);
    gl_Position = uMVP * sp;
    vNormal = mat3(uModel) * mat3(skin) * aNormal;   // approx; good enough for lighting
    vUv = aUv;
}
)glsl";

// Procedural sky: a fullscreen triangle (no VBO) shaded by view direction.
const char* kSkyVS = R"glsl(
#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)glsl";

// Physically-based sky: Rayleigh + Mie single-scattering, raymarched per pixel
// (Nishita / ScratchAPixel model). No assets, no compute shaders -- runs on the
// GL 3.3 core fragment stage. Produces a blue daytime dome, warm sunrise/sunset
// near the horizon, and a naturally dark sky once the sun drops below it.
const char* kSkyFS = R"glsl(
#version 330 core
in vec2 vNdc;
uniform mat4  uInvViewProj;
uniform vec3  uCamPos;
uniform vec3  uSunDir;       // direction TO the sun
uniform vec3  uSunColor;
uniform vec3  uMoonDir;      // direction TO the moon (reserved: stars/moon phase)
uniform float uTime;         // wind/anim clock (reserved: clouds phase)
uniform float uCloudCover;   // 0..1 (reserved: clouds phase)
uniform float uExposure;
out vec4 FragColor;

const float PI = 3.141592653589793;

// Earth-like atmosphere, metres.
const float Rp = 6371000.0;            // planet radius
const float Ra = 6471000.0;            // atmosphere top (Rp + 100 km)
const float Hr = 7994.0;               // Rayleigh scale height
const float Hm = 1200.0;               // Mie scale height
const vec3  betaR = vec3(5.5e-6, 13.0e-6, 22.4e-6);  // Rayleigh scattering
const vec3  betaM = vec3(21e-6);                     // Mie scattering
const float gMie = 0.76;
const float sunI = 22.0;               // sun intensity
const int   VIEW_STEPS  = 16;
const int   LIGHT_STEPS = 8;

// Ray (o,d) vs sphere of radius r centred at the origin. Returns near/far t,
// or vec2(-1) on a miss. d must be normalized.
vec2 raySphere(vec3 o, vec3 d, float r) {
    float b = dot(o, d);
    float c = dot(o, o) - r * r;
    float disc = b * b - c;
    if (disc < 0.0) return vec2(-1.0);
    disc = sqrt(disc);
    return vec2(-b - disc, -b + disc);
}

vec3 atmosphere(vec3 dir, vec3 sunDir) {
    // Virtual observer ~1 km above the ground, looking along dir.
    vec3 origin = vec3(0.0, Rp + 1000.0, 0.0);
    vec2 t = raySphere(origin, dir, Ra);
    if (t.y < 0.0) return vec3(0.0);                 // ray never enters atmosphere
    float tMax = t.y;
    vec2 tg = raySphere(origin, dir, Rp);            // clip at the ground if hit
    if (tg.x > 0.0) tMax = min(tMax, tg.x);
    float segLen = tMax / float(VIEW_STEPS);

    float mu = dot(dir, sunDir);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float g2 = gMie * gMie;
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + mu * mu)) /
                   ((2.0 + g2) * pow(1.0 + g2 - 2.0 * gMie * mu, 1.5));

    vec3 sumR = vec3(0.0), sumM = vec3(0.0);
    float odR = 0.0, odM = 0.0;                      // accumulated view optical depth
    float tCur = 0.0;
    for (int i = 0; i < VIEW_STEPS; ++i) {
        vec3 p = origin + dir * (tCur + segLen * 0.5);
        float h = length(p) - Rp;
        float hr = exp(-h / Hr) * segLen;
        float hm = exp(-h / Hm) * segLen;
        odR += hr;
        odM += hm;
        // Optical depth from this sample toward the sun.
        vec2 tl = raySphere(p, sunDir, Ra);
        float segLenL = tl.y / float(LIGHT_STEPS);
        float odLR = 0.0, odLM = 0.0;
        float tCurL = 0.0;
        bool blocked = false;
        for (int j = 0; j < LIGHT_STEPS; ++j) {
            vec3 pl = p + sunDir * (tCurL + segLenL * 0.5);
            float hl = length(pl) - Rp;
            if (hl < 0.0) { blocked = true; break; }  // sun occluded by the planet
            odLR += exp(-hl / Hr) * segLenL;
            odLM += exp(-hl / Hm) * segLenL;
            tCurL += segLenL;
        }
        if (!blocked) {
            vec3 tau = betaR * (odR + odLR) + betaM * 1.1 * (odM + odLM);
            vec3 atten = exp(-tau);
            sumR += hr * atten;
            sumM += hm * atten;
        }
        tCur += segLen;
    }
    return sunI * (sumR * betaR * phaseR + sumM * betaM * phaseM);
}

// ---- value-noise fBm (clouds) --------------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
// Rotated-octave fBm: the rotation between octaves breaks up the axis-aligned
// grid of the value noise, so large shapes look organic instead of streaky.
const mat2 M2 = mat2(1.6, 1.2, -1.2, 1.6);
float fbm(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 5; ++i) { v += amp * vnoise(p); p = M2 * p; amp *= 0.5; }
    return v;
}
// Billow turbulence: |signed noise| per octave gives puffy, cauliflower lumps --
// the building block of fluffy cumulus rather than smooth wisps.
float billow(vec2 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * abs(vnoise(p) * 2.0 - 1.0);
        p = M2 * p; amp *= 0.5;
    }
    return v;
}

// Volumetric clouds. They live in a horizontal slab between CLOUD_BOT and
// CLOUD_TOP above the viewer. We march the view ray through the slab,
// accumulating Beer's-law transmittance; at each step a short march toward the
// sun gives self-shadow, and a Henyey-Greenstein phase adds the forward-scatter
// silver lining. Thin clouds stay translucent so the sun and sky shine through;
// thick cores fully occlude. Wind drifts the noise field via uTime.
const float CLOUD_BOT = 600.0;
const float CLOUD_TOP = 850.0;     // thin slab: flat cloud layer, not a tall
                                   // edge-on "mountain range" at the horizon

float hg(float c, float g) {            // Henyey-Greenstein phase
    float g2 = g * g;
    return (1.0 - g2) / (12.566370 * pow(max(1.0 + g2 - 2.0 * g * c, 1e-3), 1.5));
}

// Domain warp: the dominant (first) fBm octave is axis-aligned value noise, so
// coverage tends to elongate into world-axis streaks; when one lines up with the
// view azimuth it reads as a vertical smear on screen. A low-frequency warp bends
// the field into organic curls so no straight radial fingers survive.
vec2 cloudWarp(vec2 q) {
    return q + 0.9 * (vec2(vnoise(q * 0.45), vnoise(q * 0.45 + 31.7)) - 0.5);
}

// Sample coordinate for the coverage field (wind drift + altitude drift + warp).
// Without the altitude term the field is a pure 2D extrusion, so every cloud --
// especially faint near-threshold ones -- reads as a vertical column (the "finger"
// smudges). Drifting the sample with height makes a vertical view ray cross
// different cloud at different altitudes, breaking the columns into broken wisps.
vec2 cloudCoord(vec3 pos) {
    vec2  q  = pos.xz * 0.0017 + vec2(0.010, 0.007) * uTime;
    float yn = (pos.y - CLOUD_BOT) / (CLOUD_TOP - CLOUD_BOT);   // 0 base .. 1 top
    q += (vnoise(q * 0.4 + yn * 5.0) - 0.5) * 1.2;
    return cloudWarp(q);
}

// Coverage shape (fbm only): WHERE clouds sit, plus a rounded vertical profile
// so the slab has soft top/bottom edges. Cheap enough for the light march.
float cloudShape(vec3 pos) {
    float hN = clamp((pos.y - CLOUD_BOT) / (CLOUD_TOP - CLOUD_BOT), 0.0, 1.0);
    float vProfile = smoothstep(0.0, 0.18, hN) * smoothstep(1.0, 0.55, hN);
    vec2  q = cloudCoord(pos);
    float cover = clamp(uCloudCover, 0.0, 1.0);
    float thr   = mix(0.50, 0.05, cover);
    float base  = fbm(q * 0.7);
    return smoothstep(thr, thr + 0.20, base) * vProfile;
}

// Full density: adds billow erosion for fluffy, cauliflower lumps. View march.
float cloudDensity(vec3 pos) {
    float s = cloudShape(pos);
    if (s <= 0.001) return 0.0;
    // Billow is high-frequency erosion; warping it is invisible, so sample the
    // cheaper unwarped coord here.
    vec2  q   = pos.xz * 0.0017 + vec2(0.010, 0.007) * uTime;
    float det = billow(q * 2.6 + 11.0);
    return clamp(s - (1.0 - s) * det * 0.55, 0.0, 1.0);
}

// Cheap coverage for the self-shadow light march: 3 octaves, no domain warp.
// It runs 5x per view step, so it's the hot path; the shadow term tolerates a
// coarse estimate and the small mismatch with the view shape is invisible.
float cloudShapeLite(vec3 pos) {
    float hN = clamp((pos.y - CLOUD_BOT) / (CLOUD_TOP - CLOUD_BOT), 0.0, 1.0);
    float vProfile = smoothstep(0.0, 0.18, hN) * smoothstep(1.0, 0.55, hN);
    vec2  q = pos.xz * 0.0017 + vec2(0.010, 0.007) * uTime;
    float v = 0.0, amp = 0.5; vec2 p = q * 0.7;
    for (int i = 0; i < 3; ++i) { v += amp * vnoise(p); p = M2 * p; amp *= 0.5; }
    float cover = clamp(uCloudCover, 0.0, 1.0);
    float thr   = mix(0.50, 0.05, cover);
    return smoothstep(thr, thr + 0.20, v) * vProfile;
}

vec4 clouds(vec3 dir, vec3 sun, float dayF, vec3 sky) {
    if (dir.y < 0.03) return vec4(0.0);            // only skip rays at/below horizon

    float t0 = CLOUD_BOT / dir.y;
    float t1 = CLOUD_TOP / dir.y;
    const int STEPS = 40;          // fine dt kills the radial-finger aliasing
                                   // (vertical streaks); affordable now the light
                                   // march uses the cheap 3-tap sampler
    float dt = (t1 - t0) / float(STEPS);
    // Jitter the ray start by up to one step: turns slice banding into fine,
    // unobjectionable noise instead of visible stripes through the cloud.
    t0 += hash21(gl_FragCoord.xy) * dt;

    float mu    = dot(dir, sun);
    float phase = max(hg(mu, 0.35), hg(mu, -0.18) * 0.55);   // forward + soft back

    const float sigmaE   = 0.012;     // extinction per unit density
    const float SUN_GAIN = 11.0;
    const float lstep    = (CLOUD_TOP - CLOUD_BOT) * 0.225;   // 4 steps, same reach

    // Ambient on the cloud IS the sky (the atmosphere colour in this direction):
    // clouds are lit by the dome around them. No hand-picked colours -- one sky
    // model. Direct sunlight is the sun's own colour, scaled for the phase term.
    // It fades out with the day factor (uSunColor stays orange after dusk), so at
    // night clouds are lit only by the dark night sky and the scene fades to black
    // instead of glowing brown.
    vec3 sunLit = uSunColor * SUN_GAIN * dayF;
    // Faint cool moon/skyglow that only appears at night: keeps clouds dimly
    // visible (silvery) once the sun's direct light is gone, rather than vanishing
    // into the black sky.
    vec3 moonLit = vec3(0.12, 0.14, 0.20) * (1.0 - dayF);

    float T = 1.0;                    // transmittance along the view ray
    vec3  scatter = vec3(0.0);

    for (int i = 0; i < STEPS; ++i) {
        float t   = t0 + (float(i) + 0.5) * dt;
        vec3  pos = dir * t;
        float dens = cloudDensity(pos);
        if (dens > 0.001) {
            // March toward the sun, accumulating optical depth (self-shadow).
            float odL = 0.0;
            for (int j = 0; j < 4; ++j)
                odL += cloudShapeLite(pos + sun * (lstep * (float(j) + 0.5)));
            float lightT = exp(-odL * lstep * sigmaE);

            float hN  = clamp((pos.y - CLOUD_BOT) / (CLOUD_TOP - CLOUD_BOT), 0.0, 1.0);
            vec3  amb = sky * mix(0.35, 0.95, hN);    // skylight, brighter on top
            vec3  Light = sunLit * lightT * phase + moonLit + amb;

            float stepT = exp(-dens * sigmaE * dt);
            scatter += T * Light * (1.0 - stepT);     // energy-conserving in-scatter
            T *= stepT;
            if (T < 0.02) break;                       // ray is opaque, stop
        }
    }

    float alpha = 1.0 - T;
    // No vertical fade here: the aerial-perspective blend in main() dissolves
    // far/low clouds into the atmosphere colour, so the deck reaches the horizon
    // without a hard shelf.
    //
    // Night opacity: as the sun drops below the horizon the clouds thin out
    // sharply -- they keep a faint presence (so the deck doesn't pop away) but
    // mostly let the night sky and stars through. The narrow smoothstep makes the
    // fade aggressive around dusk/dawn.
    float dayOpacity = smoothstep(-0.04, 0.12, sun.y);
    float alphaMul   = mix(0.25, 1.0, dayOpacity);
    return vec4(scatter * mix(0.30, 1.0, dayF), alpha * alphaMul);
}

// ---- night: stars + moon -------------------------------------------------
float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
// Sparse round stars fixed to the sky dome (sampled in world direction so they
// don't swim as the camera moves), with a gentle per-star twinkle.
float starField(vec3 dir) {
    vec3 c  = dir * 300.0;
    vec3 id = floor(c);
    vec3 f  = fract(c) - 0.5;
    float h = hash31(id);
    if (h < 0.985) return 0.0;
    float d = length(f);
    float star = smoothstep(0.18, 0.0, d) * ((h - 0.985) / 0.015);
    star *= 0.6 + 0.4 * sin(uTime * 2.5 + h * 120.0);   // twinkle
    return star;
}
// Moon disc + soft glow in the moon direction.
vec3 moon(vec3 dir, vec3 moonDir) {
    float md   = dot(dir, moonDir);
    float disc = smoothstep(0.9990, 0.9994, md);
    float glow = pow(max(md, 0.0), 250.0) * 0.25;
    return (disc * 1.2 + glow) * vec3(0.90, 0.92, 1.0);
}

void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uCamPos);
    vec3 sun = normalize(uSunDir);
    float dayF   = smoothstep(-0.12, 0.25, sun.y);
    float nightF = smoothstep(0.08, -0.08, sun.y);   // 1 once the sun is down

    vec3 sky = atmosphere(dir, sun);   // single source of truth for sky colour
    vec3 col = sky;

    // Stars + moon (only at night, only above the horizon).
    if (nightF > 0.0) {
        if (dir.y > 0.0) col += starField(dir) * nightF;
        col += moon(dir, normalize(uMoonDir)) * nightF;
    }

    // Sun disc, faded out as the sun sinks below the horizon.
    float sd = dot(dir, sun);
    float disc = smoothstep(0.9997, 0.9998, sd);
    float aboveH = smoothstep(-0.04, 0.04, sun.y);
    col += disc * uSunColor * sunI * 0.7 * aboveH;

    // Volumetric clouds, lit by the sky itself (the atmosphere colour), then
    // composited by transmittance: cl.a is how much background the cloud blocks,
    // cl.rgb is the light it scatters toward us. Thin clouds let the sun disc and
    // sky leak through; thick cores occlude sun, moon & stars.
    vec4 cl = clouds(dir, sun, dayF, sky);
    // Aerial perspective: far (low) clouds take on the sky colour behind them so
    // they dissolve into the atmosphere with distance -- no hard deck edge and no
    // separate haze colour. Derived entirely from the atmosphere.
    float aerial = 1.0 - smoothstep(0.05, 0.42, dir.y);
    cl.rgb = mix(cl.rgb, sky * cl.a, aerial);
    col = col * (1.0 - cl.a) + cl.rgb;

    // Tonemap + gamma.
    col = vec3(1.0) - exp(-col * uExposure);
    col = pow(col, vec3(1.0 / 2.2));
    FragColor = vec4(col, 1.0);
}
)glsl";

GLuint compileShader(GLenum type, const char* src) {
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

GLuint createProgram(const char* vsSrc, const char* fsSrc) {
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
