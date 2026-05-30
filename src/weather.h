#pragma once
#include <glm/glm.hpp>

// ---- Weather: drives the sky / clouds / lighting toward changing conditions --
// Each weather state is a *preset* of target parameters. The live values ease
// toward the active preset every frame, so switching weather crossfades instead
// of popping. The renderer and shaders read the LIVE values, never the preset.
//
// Only cloud-variation states exist for now (Clear/Fair/Overcast). The struct
// already carries rain/dust/fog fields so rain and dust storms drop in later
// without reworking the plumbing.
enum class WeatherType { Clear, Fair, Overcast, Rain, Thunderstorm };
inline constexpr int kWeatherCount = 5;

const char* weatherName(WeatherType t);

struct Weather {
    // ---- Control ----
    WeatherType current = WeatherType::Fair;  // active target preset
    bool   autoCycle    = true;     // drift between presets over time (play only)
    float  blendSpeed   = 0.4f;     // per-second easing rate toward the preset
    float  minDwellSec  = 30.0f;    // shortest time on one weather (auto-cycle)
    float  maxDwellSec  = 80.0f;    // longest time on one weather (auto-cycle)

    // ---- Live, interpolated parameters (what the rest of the engine reads) ----
    float cloudiness  = 0.35f;      // -> Environment.cloudCover
    float wind        = 0.30f;      // cloud drift speed (and rain slant, later)
    float skyDarken   = 0.05f;      // 0 bright .. 1 gloomy: extra light dimming
    float exposureMul = 1.0f;       // scales sky exposure (overcast = darker)
    float rain        = 0.0f;       // precipitation intensity -> rain overlay
    float lightning   = 0.0f;       // current flash brightness (decays fast)
    float boltAz      = 0.0f;       // world azimuth of the current bolt
    float boltSeed    = 0.0f;       // jagged-shape seed for the current bolt
    // Reserved for the dust-storm state; stays 0 for now.
    float dust       = 0.0f;
    float fogDensity = 0.0f;
    glm::vec3 fogColor{0.74f, 0.78f, 0.86f};

    // ---- Internals ----
    float       dwellTimer = 6.0f;  // counts down to the next auto change
    WeatherType prevType   = WeatherType::Fair;  // detects a state change
    float       rainTarget = 0.0f;  // intensity rolled for the current rainy spell
    unsigned    rng        = 0x9E3779B9u;
    // Rain motion, integrated on the CPU so wind/speed changes don't retroactively
    // rescale past motion (which made drops lurch on rain<->storm transitions).
    glm::vec3   rainDisp{0.0f};                  // accumulated world displacement
    glm::vec3   rainDir{0.0f, -1.0f, 0.0f};      // current rain velocity direction
};

// Advance timers and ease the live params toward the current preset's targets.
// `advanceCycle` should be true only in play mode: auto-cycling is paused in the
// editor, but easing always runs so a manual selection still previews live.
void updateWeather(Weather& w, float dt, bool advanceCycle);
