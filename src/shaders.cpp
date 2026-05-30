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
out vec3 vWorld;
void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormal = transpose(inverse(mat3(uModel))) * aNormal;  // correct under scale
    vUv = aUv;
    vWorld = world.xyz;
}
)glsl";

const char* kLitFS = R"glsl(
#version 330 core
in vec3 vNormal;
in vec2 vUv;
in vec3 vWorld;
uniform vec3 uColor;
uniform sampler2D uAlbedo;
uniform int uHasTexture;
uniform vec3 uLightDir;     // direction TO the sun (normalized)
uniform vec3 uLightColor;   // sun colour * intensity (dims at night)
uniform vec3 uAmbient;      // sky ambient (dim blue at night, never black)
uniform vec3 uCamPos;       // for distance fog
uniform vec3 uFogColor;     // weather haze colour
uniform float uFogDensity;  // fog thickness at ground level
uniform float uFogFalloff;  // how fast fog thins with height
out vec4 FragColor;
// Exponential height fog: density rho(y) = D * exp(-k*y), analytically integrated
// along the camera->fragment ray. Thick low, thin high; more fog with distance.
float heightFog(vec3 c, vec3 w, float D, float k) {
    vec3 v = w - c;
    float dist = length(v);
    if (dist < 1e-4) return 0.0;
    float ry = v.y / dist;
    float baseD = D * exp(-k * c.y);
    float kry = k * ry;
    float od = (abs(kry) > 1e-5) ? baseD * (1.0 - exp(-kry * dist)) / kry
                                 : baseD * dist;
    return 1.0 - exp(-max(od, 0.0));
}
void main() {
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 base = uColor;
    if (uHasTexture == 1) base *= texture(uAlbedo, vUv).rgb;
    vec3 lit = base * (uAmbient + uLightColor * diff);
    float fog = heightFog(uCamPos, vWorld, uFogDensity, uFogFalloff);
    FragColor = vec4(mix(lit, uFogColor, fog), 1.0);
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
out vec3 vWorld;
void main() {
    mat4 skin = aWeights.x * uBones[aBoneIds.x] + aWeights.y * uBones[aBoneIds.y]
              + aWeights.z * uBones[aBoneIds.z] + aWeights.w * uBones[aBoneIds.w];
    vec4 sp = skin * vec4(aPos, 1.0);
    gl_Position = uMVP * sp;
    vNormal = mat3(uModel) * mat3(skin) * aNormal;   // approx; good enough for lighting
    vUv = aUv;
    vWorld = vec3(uModel * sp);
}
)glsl";

// Ground plane: a large quad recentred on the camera each frame (so it always
// reaches the horizon), textured in world space and fogged so its far edge melts
// into the haze rather than showing a hard boundary.
const char* kGroundVS = R"glsl(
#version 330 core
layout (location = 0) in vec2 aCorner;   // [-1,1] quad corner
uniform mat4  uViewProj;
uniform vec3  uCamPos;
uniform float uHalfSize;
out vec3 vWorld;
void main() {
    vec3 w = vec3(uCamPos.x + aCorner.x * uHalfSize, -0.5, uCamPos.z + aCorner.y * uHalfSize);
    vWorld = w;
    gl_Position = uViewProj * vec4(w, 1.0);
}
)glsl";

const char* kGroundFS = R"glsl(
#version 330 core
in vec3 vWorld;
uniform vec3  uCamPos;
uniform vec3  uLightDir;
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform sampler2D uAlbedo;
uniform float uUvScale;
uniform vec3  uFogColor;
uniform float uFogDensity;
uniform float uFogFalloff;
out vec4 FragColor;
// Exponential height fog (same model as the lit shader).
float heightFog(vec3 c, vec3 w, float D, float k) {
    vec3 v = w - c;
    float dist = length(v);
    if (dist < 1e-4) return 0.0;
    float ry = v.y / dist;
    float baseD = D * exp(-k * c.y);
    float kry = k * ry;
    float od = (abs(kry) > 1e-5) ? baseD * (1.0 - exp(-kry * dist)) / kry
                                 : baseD * dist;
    return 1.0 - exp(-max(od, 0.0));
}
void main() {
    vec3 n = vec3(0.0, 1.0, 0.0);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 tex = texture(uAlbedo, vWorld.xz * uUvScale).rgb;   // world-anchored tiling
    vec3 lit = tex * (uAmbient + uLightColor * diff);
    float fog = heightFog(uCamPos, vWorld, uFogDensity, uFogFalloff);
    FragColor = vec4(mix(lit, uFogColor, fog), 1.0);
}
)glsl";

// Heightmap terrain: a world-space grid mesh. The splatmap (RGBA layer weights)
// blends four tiling textures (sand/dirt/rock/4th) by world XZ, lit by the sun
// and fogged with the same exponential height fog as the rest of the scene.
const char* kTerrainVS = R"glsl(
#version 330 core
layout (location = 0) in vec3 aPos;     // already in world space
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;       // 0..1 across the terrain (splatmap)
uniform mat4 uViewProj;
out vec3 vNormal;
out vec2 vUv;
out vec3 vWorld;
void main() {
    vWorld  = aPos;
    vNormal = aNormal;
    vUv     = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)glsl";

const char* kTerrainFS = R"glsl(
#version 330 core
in vec3 vNormal;
in vec2 vUv;
in vec3 vWorld;
uniform vec3  uLightDir;
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform sampler2D uSplat;                // RGBA layer weights
uniform sampler2D uTex0;                 // sand
uniform sampler2D uTex1;                 // dirt
uniform sampler2D uTex2;                 // rock
uniform sampler2D uTex3;                 // 4th
uniform float uTileScale;
uniform vec3  uCamPos;
uniform vec3  uFogColor;
uniform float uFogDensity;
uniform float uFogFalloff;
out vec4 FragColor;
float heightFog(vec3 c, vec3 w, float D, float k) {
    vec3 v = w - c;
    float dist = length(v);
    if (dist < 1e-4) return 0.0;
    float ry = v.y / dist;
    float baseD = D * exp(-k * c.y);
    float kry = k * ry;
    float od = (abs(kry) > 1e-5) ? baseD * (1.0 - exp(-kry * dist)) / kry
                                 : baseD * dist;
    return 1.0 - exp(-max(od, 0.0));
}
void main() {
    vec4 w = texture(uSplat, vUv);
    vec2 tuv = vWorld.xz * uTileScale;
    vec3 col = texture(uTex0, tuv).rgb * w.r
             + texture(uTex1, tuv).rgb * w.g
             + texture(uTex2, tuv).rgb * w.b
             + texture(uTex3, tuv).rgb * w.a;
    col /= max(w.r + w.g + w.b + w.a, 0.001);    // normalise so weights<1 don't darken
    vec3 n = normalize(vNormal);
    float diff = max(dot(n, normalize(uLightDir)), 0.0);
    vec3 lit = col * (uAmbient + uLightColor * diff);
    float fog = heightFog(uCamPos, vWorld, uFogDensity, uFogFalloff);
    FragColor = vec4(mix(lit, uFogColor, fog), 1.0);
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
uniform vec3  uHazeColor;    // weather haze tint (matches the ground fog colour)
uniform float uHaze;         // 0..1 horizon haze strength (fog/storm murk)
uniform float uLightning;    // current flash brightness (0 = none)
uniform float uBoltAz;       // world azimuth of the lightning bolt
uniform float uBoltSeed;     // jagged-shape seed for the bolt
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
    float thr   = mix(0.66, 0.04, cover);   // high at low cover -> clear skies clear
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
    float thr   = mix(0.66, 0.04, cover);   // high at low cover -> clear skies clear
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

// ---- lightning bolt ------------------------------------------------------
float boltHash(float x) { return fract(sin(x * 127.1) * 43758.5453); }
// Piecewise-smooth zigzag: random value per integer step, interpolated.
float boltZig(float t, float seed) {
    float i = floor(t), f = fract(t);
    f = f * f * (3.0 - 2.0 * f);
    return mix(boltHash(i + seed) * 2.0 - 1.0, boltHash(i + 1.0 + seed) * 2.0 - 1.0, f);
}
// A jagged, forked bolt at world azimuth az0, running from the horizon up. Works
// in azimuth/elevation so it's anchored in the world (stays put as you turn).
float lightningBolt(vec3 dir, float az0, float seed) {
    float el = asin(clamp(dir.y, -1.0, 1.0));
    if (el < 0.0) return 0.0;
    float vert = smoothstep(0.0, 0.05, el) * smoothstep(1.15, 0.65, el);
    if (vert <= 0.0) return 0.0;
    float az = atan(dir.x, dir.z);
    // Main channel: azimuth zigzags down the bolt, with a slight lean. Widths are
    // in radians of azimuth -- kept small so the bolt is a thin filament + a faint
    // narrow glow, not a fat luminous column.
    float mAz = az0 + boltZig(el * 7.0 + seed, seed) * 0.035 + el * 0.05;
    float dm  = abs(az - mAz); dm = min(dm, 6.2831853 - dm);
    // Squared-Lorentzian falloff: a thin hot core that fades smoothly with no hard
    // edge (a soft glowing filament, not a solid cut-out strip).
    float w   = 0.0026;
    float chan = (w * w) / (dm * dm + w * w);
    // One fork peeling off the lower half.
    float fEl  = 0.5;
    float fAz  = az0 + boltZig(el * 6.0 + seed + 9.0, seed + 9.0) * 0.06 + (fEl - el) * 0.30;
    float df   = abs(az - fAz); df = min(df, 6.2831853 - df);
    float wf   = 0.0020;
    float fork = (wf * wf) / (df * df + wf * wf) * smoothstep(fEl + 0.05, fEl - 0.35, el);
    return (chan + fork) * vert;
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

    // Lightning bolt: a brief, bright jagged fork during the flash. Added in HDR
    // so the core tonemaps to white with a soft glow around it.
    if (uLightning > 0.01) {
        float bv = smoothstep(0.35, 0.7, uLightning);   // only on the bright peak
        col += lightningBolt(dir, uBoltAz, uBoltSeed) * bv * vec3(0.90, 0.95, 1.0) * 5.0;
    }

    // Tonemap + gamma.
    col = vec3(1.0) - exp(-col * uExposure);
    col = pow(col, vec3(1.0 / 2.2));

    // Horizon haze: blend the sky toward the weather haze colour near the horizon
    // so distance fog is unified with the ground (storms read as murky top to
    // bottom, and the ground edge always melts into the sky). Applied in display
    // space so it matches the (un-tonemapped) object/ground fog colour.
    float hz = uHaze * (1.0 - smoothstep(0.0, 0.32, max(dir.y, 0.0)));
    col = mix(col, uHazeColor, hz);
    FragColor = vec4(col, 1.0);
}
)glsl";

// World-space rain particles. A static instance buffer holds random drop
// positions inside a local box; the vertex shader animates the fall (and wraps
// it) entirely on the GPU, so there's no per-frame CPU work. The box follows the
// camera, so rain always surrounds the viewer; because the streaks live at real
// world positions they parallax correctly as you look around (unlike a
// screen-space overlay). Each instance is a 4-vertex quad expanded in clip space
// into a thin, camera-facing ribbon along the drop's fall direction.
const char* kRainVS = R"glsl(
#version 330 core
layout(location = 0) in vec2 aCorner;   // x: side -1..1, y: along 0..1
layout(location = 1) in vec3 aBase;     // per-instance base position in the local box
uniform mat4  uViewProj;
uniform vec3  uCamPos;
uniform vec3  uRainDisp;     // accumulated world displacement (integrated on the CPU)
uniform vec3  uRainDir;      // current normalized rain velocity (streak orientation)
uniform vec3  uBox;          // halfX, height, halfZ
uniform float uStreakLen;    // world length of a streak
uniform float uStreakWidth;  // world half-width
out vec2 vUv;
out float vFade;
void main() {
    float H = uBox.y;
    // Per-drop pseudo-random: vary speed / length / angle so drops don't fall in
    // rigid lockstep (which reads as a sliding sheet of "bands") and aren't all
    // perfectly parallel.
    float r1 = fract(sin(dot(aBase.xz, vec2(12.9898, 78.233))) * 43758.5453);
    float r2 = fract(sin(dot(aBase.zx, vec2(39.346, 11.135))) * 24634.6334);
    float dropMul = 0.70 + 0.60 * r1;                        // per-drop speed
    float len     = uStreakLen * (0.55 + 0.90 * r2);
    // World-anchored, wind-drifting columns. The drop's xz drifts with the wind
    // (uRainDisp.xz) in WORLD space, then snaps to the tile nearest the camera, so
    // it stays fixed in the world as you move -- you walk THROUGH the rain instead
    // of dragging it along. It only jumps a whole tile when you cross a half-cell,
    // which happens out at the faded box edge, so the recycle is invisible. The
    // integrated displacement keeps the motion smooth through weather transitions.
    float R = uBox.x;
    float cell = 2.0 * R;
    vec2 base  = aBase.xz + uRainDisp.xz * dropMul;
    vec2 colXZ = base + cell * vec2(round((uCamPos.x - base.x) / cell),
                                    round((uCamPos.z - base.y) / cell));
    // Vertical fall (smooth, wrapped) in a band that follows the camera height so
    // rain always surrounds you vertically.
    float fall = mod(aBase.y + uRainDisp.y * dropMul, H);
    vec3 center = vec3(colXZ.x, uCamPos.y + fall - H * 0.5, colXZ.y);
    vec3 dir = uRainDir;                                     // streak orientation = travel dir
    // Distance opacity: fade far drops out before the box edge (depth + hides the
    // boundary, including the wrap) and gently fade ones right on top of the camera.
    float dist = length(center - uCamPos);
    // Cull drops right on top of the camera: their streak would straddle the near
    // clip plane, where perspective interpolation goes NaN and smears a bright line
    // across the screen. They're faded to nothing anyway, so just drop them.
    if (dist < 1.5) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vUv = vec2(0.0); vFade = 0.0; return; }
    vFade = (1.0 - smoothstep(uBox.x * 0.45, uBox.x * 0.97, dist)) * smoothstep(0.4, 2.5, dist);
    // World-space velocity-stretched billboard. The streak is a real line segment
    // in the world, lying ALONG the drop's travel direction -- like a laser moving
    // in one fixed world direction. Its orientation is fixed in the world, so it
    // does NOT change when the camera rotates: you just view that same 3D streak
    // from a different angle. The width faces the camera (perpendicular to the
    // streak and the view ray), and the segment is centred on the drop.
    vec3 toCam = uCamPos - center;
    vec3 c = cross(dir, toCam);
    vec3 side = (length(c) > 1e-4) ? normalize(c) : normalize(cross(dir, vec3(1.0, 0.0, 0.0)));
    vec3 wpos = center + dir * (len * (aCorner.y - 0.5)) + side * (uStreakWidth * aCorner.x);
    gl_Position = uViewProj * vec4(wpos, 1.0);
    vUv = aCorner;
}
)glsl";

const char* kRainFS = R"glsl(
#version 330 core
in vec2 vUv;             // x: -1..1 across width, y: 0..1 along streak
in float vFade;          // distance opacity (far/near drops fade)
uniform vec3  uColor;
uniform float uIntensity;
out vec4 FragColor;
void main() {
    float across = 1.0 - abs(vUv.x);                                   // soft edges
    float along  = smoothstep(0.0, 0.25, vUv.y) * smoothstep(1.0, 0.55, vUv.y);
    // Drops stay crisp; the drawn count conveys how heavy it is. Only fade out
    // when the spell is nearly dry so easing in/out doesn't pop.
    float fade = clamp(uIntensity * 4.0, 0.0, 1.0);
    float a = across * along * 0.6 * fade * vFade;
    FragColor = vec4(uColor, a);
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
