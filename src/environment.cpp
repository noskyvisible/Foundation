#include "environment.h"

#include <cmath>

void updateEnvironment(Environment& e, float dt, bool advance) {
    if (advance && e.running && e.dayLengthSec > 0.0f) {
        e.timeOfDay += dt * (24.0f / e.dayLengthSec);
        while (e.timeOfDay >= 24.0f) { e.timeOfDay -= 24.0f; ++e.day; }
        while (e.timeOfDay < 0.0f)   { e.timeOfDay += 24.0f; --e.day; }
    }
    // Wind/cloud drift runs even while the clock is frozen so the editor still
    // shows a living sky. Stormier weather (higher wind) drifts the clouds faster.
    e.windTime += dt * (0.5f + 1.5f * e.weather.wind);

    const float PI = 3.14159265f;
    float a = (e.timeOfDay - 6.0f) / 12.0f * PI;   // 0 at 06:00, PI at 18:00
    float elev = std::sin(a);                       // sun elevation, <0 at night
    e.sunDir = glm::normalize(glm::vec3(std::cos(a), elev, 0.35f));
    e.moonDir = -e.sunDir;                           // moon rides opposite the sun

    float dayAmt = glm::clamp((elev + 0.05f) / 0.30f, 0.0f, 1.0f);  // 0 night -> 1 day

    e.sunColor     = glm::mix(glm::vec3(1.0f, 0.55f, 0.25f), glm::vec3(1.0f, 0.97f, 0.9f),
                              glm::clamp(elev / 0.4f, 0.0f, 1.0f));
    e.lightColor   = e.sunColor * dayAmt;
    // Night keeps a dim blue ambient so the world stays visible (Skyrim-style),
    // rising to a neutral grey during the day.
    glm::vec3 nightAmbient(0.16f, 0.18f, 0.27f);
    glm::vec3 dayAmbient(0.30f, 0.30f, 0.30f);
    e.ambient      = glm::mix(nightAmbient, dayAmbient, dayAmt);

    // Weather drives the cloud cover now (eased between presets in weather.cpp).
    e.cloudCover = glm::clamp(e.weather.cloudiness, 0.0f, 1.0f);

    // Cloud coherence: overcast skies block direct sun (dimmer, flatter light)
    // and scatter more skylight (lifted ambient), so the ground matches the dome.
    float oc = e.cloudCover;
    e.lightColor *= glm::mix(1.0f, 0.35f, oc);
    e.ambient    += glm::vec3(0.12f) * dayAmt * oc;

    // Gloom: overcast/stormy presets dim the sun further and pull the ambient
    // toward the haze colour, so the ground reads as a heavy, overcast day.
    float gloom = glm::clamp(e.weather.skyDarken, 0.0f, 1.0f);
    e.lightColor *= glm::mix(1.0f, 0.45f, gloom);
    e.ambient     = glm::mix(e.ambient, e.weather.fogColor * (0.25f + 0.45f * dayAmt), gloom * 0.5f);

    // Overcast/stormy weather darkens the sky via an exposure multiplier on top
    // of the user's base exposure.
    e.skyExposure = e.exposure * e.weather.exposureMul;

    // Fog/haze colour: the user-picked tint, but darkened toward night and warmed
    // at dawn/dusk so it matches the sky at the horizon (no bright fog at night).
    float dayB = glm::clamp((elev + 0.10f) / 0.35f, 0.0f, 1.0f);   // 0 night .. 1 day
    glm::vec3 fc = e.fogTint;
    float low = glm::clamp(1.0f - elev / 0.30f, 0.0f, 1.0f) * dayB; // sun low but still up
    fc = glm::mix(fc, glm::vec3(0.95f, 0.62f, 0.40f), low * 0.45f); // warm dawn/dusk tint
    fc *= (0.10f + 0.90f * dayB);                                   // dim toward night
    e.fogColor = fc;

    // Lightning flash: a brief cool-white flood that lights the scene (ambient +
    // sun) and momentarily brightens the sky. Most dramatic against a dark storm.
    float fl = glm::clamp(e.weather.lightning, 0.0f, 1.0f);
    if (fl > 0.0f) {
        e.ambient     += glm::vec3(0.55f, 0.60f, 0.75f) * fl;
        e.lightColor  += glm::vec3(0.50f, 0.55f, 0.70f) * fl;
        e.skyExposure *= 1.0f + 1.3f * fl;
    }
}
