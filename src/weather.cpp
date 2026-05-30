#include "weather.h"

#include <cmath>

const char* weatherName(WeatherType t) {
    switch (t) {
        case WeatherType::Clear:        return "Clear";
        case WeatherType::Fair:         return "Fair";
        case WeatherType::Overcast:     return "Overcast";
        case WeatherType::Rain:         return "Rain";
        case WeatherType::Thunderstorm: return "Storm";
    }
    return "?";
}

namespace {

// Target values each weather eases toward. cloudiness drives the cloud cover;
// wind sets drift speed; skyDarken adds gloom (dimmer sun, greyer ambient);
// fogColor is the haze tint the ambient leans toward under heavy sky.
struct Preset {
    float cloudiness;
    float wind;
    float skyDarken;
    float exposureMul;
    float rain;
    float fogDensity;
    glm::vec3 fogColor;
};

Preset presetFor(WeatherType t) {
    switch (t) {
        //                              cloud  wind  dark  expo  rain  fog    fogColor
        // fog here is the EXTRA density weather adds on top of the clear-day base
        // (Environment.fogDensity ~0.0225). Storm tops out so total reaches ~0.40.
        case WeatherType::Clear:        return { 0.00f, 0.15f, 0.00f, 1.05f, 0.0f, 0.000f, glm::vec3(0.76f, 0.82f, 0.92f) };
        case WeatherType::Fair:         return { 0.50f, 0.30f, 0.05f, 1.00f, 0.0f, 0.020f, glm::vec3(0.74f, 0.78f, 0.86f) };
        case WeatherType::Overcast:     return { 0.92f, 0.55f, 0.45f, 0.62f, 0.0f, 0.090f, glm::vec3(0.60f, 0.63f, 0.69f) };
        case WeatherType::Rain:         return { 0.97f, 0.70f, 0.60f, 0.52f, 0.7f, 0.200f, glm::vec3(0.55f, 0.58f, 0.63f) };
        case WeatherType::Thunderstorm: return { 1.00f, 0.95f, 0.78f, 0.42f, 1.0f, 0.380f, glm::vec3(0.46f, 0.49f, 0.55f) };
    }
    return { 0.50f, 0.30f, 0.05f, 1.00f, 0.0f, 0.020f, glm::vec3(0.74f, 0.78f, 0.86f) };
}

// Cheap xorshift32 -> [0,1). Keeps weather self-contained (no <random>/global rand).
float frand(unsigned& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return float(s & 0x00FFFFFFu) / float(0x01000000);
}

} // namespace

void updateWeather(Weather& w, float dt, bool advanceCycle) {
    if (dt < 0.0f) dt = 0.0f;

    // Auto-cycle: when the dwell timer runs out, pick a *different* weather and
    // schedule the next change somewhere in [minDwell, maxDwell].
    if (advanceCycle && w.autoCycle) {
        w.dwellTimer -= dt;
        if (w.dwellTimer <= 0.0f) {
            // Natural progression: a mean-reverting random walk along the severity
            // scale (Clear < Fair < Overcast < Rain < Storm). Weather drifts one
            // step at a time and mostly sits in the calmer states, so storms build
            // up through overcast/rain and pass back down instead of popping in.
            int cur = int(w.current);
            float downBias = 0.30f + 0.14f * float(cur);     // harsher -> more likely to ease off
            int step = (frand(w.rng) < downBias) ? -1 : 1;
            if (frand(w.rng) < 0.12f) step *= 2;             // occasional bigger jump
            int next = cur + step;
            if (next < 0) next = 0;
            if (next > kWeatherCount - 1) next = kWeatherCount - 1;
            if (next == cur) next = (cur == 0) ? 1 : cur - 1;  // guarantee a change
            w.current = WeatherType(next);
            w.dwellTimer = w.minDwellSec + frand(w.rng) * (w.maxDwellSec - w.minDwellSec);
        }
    }

    // On any state change (auto or manual), roll the rain intensity for the spell
    // so rainy weather ranges from a light drizzle to a heavy downpour.
    if (w.current != w.prevType) {
        w.prevType = w.current;
        if (w.current == WeatherType::Rain)
            w.rainTarget = 0.25f + frand(w.rng) * 0.55f;   // 0.25 .. 0.80
        else if (w.current == WeatherType::Thunderstorm)
            w.rainTarget = 0.75f + frand(w.rng) * 0.25f;   // 0.75 .. 1.00
        else
            w.rainTarget = 0.0f;
    }

    // Build the target params. For rainy states the gloom scales with the rolled
    // intensity, so a drizzle keeps a brighter sky and a downpour goes dark.
    Preset p = presetFor(w.current);
    float tCloud = p.cloudiness, tWind = p.wind, tDark = p.skyDarken,
          tExpo = p.exposureMul, tRain = p.rain, tFogDen = p.fogDensity;
    glm::vec3 tFog = p.fogColor;
    if (w.current == WeatherType::Rain || w.current == WeatherType::Thunderstorm) {
        float wet = w.rainTarget;
        tRain   = wet;
        tCloud  = glm::mix(0.55f, p.cloudiness, wet);
        tWind   = glm::mix(0.35f, p.wind,       wet);
        tDark   = p.skyDarken * wet;
        tExpo   = glm::mix(1.0f, p.exposureMul, wet);
        tFogDen = p.fogDensity * wet;                    // lighter rain adds less extra fog
        tFog    = glm::mix(glm::vec3(0.70f, 0.74f, 0.82f), p.fogColor, wet);
    }

    // Frame-rate-independent exponential approach toward the target.
    float k = 1.0f - std::exp(-w.blendSpeed * dt);
    w.cloudiness  = glm::mix(w.cloudiness,  tCloud,  k);
    w.wind        = glm::mix(w.wind,        tWind,   k);
    w.skyDarken   = glm::mix(w.skyDarken,   tDark,   k);
    w.exposureMul = glm::mix(w.exposureMul, tExpo,   k);
    w.rain        = glm::mix(w.rain,        tRain,   k);
    w.fogDensity  = glm::mix(w.fogDensity,  tFogDen, k);
    w.fogColor    = glm::mix(w.fogColor,    tFog,    k);

    // Integrate the rain displacement from the *current* wind-driven velocity.
    // Accumulating an offset (instead of dir*speed*time) keeps the drops moving
    // smoothly when wind/speed shift mid-transition -- recomputing dir*speed*time
    // would retroactively rescale all past motion and the drops would lurch.
    float rainSpeed = 17.0f + 8.0f * w.wind;
    w.rainDir   = glm::normalize(glm::vec3(w.wind * 0.6f, -1.0f, 0.0f));
    w.rainDisp += w.rainDir * (rainSpeed * dt);

    // Lightning: random flashes during a thunderstorm. The flash spikes then
    // decays fast; environment.cpp turns w.lightning into a brief light/sky boost.
    w.lightning *= std::exp(-6.0f * dt);
    if (w.current == WeatherType::Thunderstorm && frand(w.rng) < 0.30f * dt) {
        w.lightning = 0.8f + 0.2f * frand(w.rng);
        w.boltAz    = frand(w.rng) * 6.2831853f;   // new bolt: random sky direction
        w.boltSeed  = frand(w.rng) * 100.0f;       //           + random jagged shape
    }
    if (w.lightning < 0.003f) w.lightning = 0.0f;
}
