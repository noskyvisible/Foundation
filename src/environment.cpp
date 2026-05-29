#include "environment.h"

#include <cmath>

void updateEnvironment(Environment& e, float dt, bool advance) {
    if (advance && e.running && e.dayLengthSec > 0.0f) {
        e.timeOfDay += dt * (24.0f / e.dayLengthSec);
        while (e.timeOfDay >= 24.0f) { e.timeOfDay -= 24.0f; ++e.day; }
        while (e.timeOfDay < 0.0f)   { e.timeOfDay += 24.0f; --e.day; }
    }
    // Wind/cloud drift runs even while the clock is frozen so the editor still
    // shows a living sky.
    e.windTime += dt;

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

    // Cloud coherence: overcast skies block direct sun (dimmer, flatter light)
    // and scatter more skylight (lifted ambient), so the ground matches the dome.
    float oc = glm::clamp(e.cloudCover, 0.0f, 1.0f);
    e.lightColor *= glm::mix(1.0f, 0.35f, oc);
    e.ambient    += glm::vec3(0.12f) * dayAmt * oc;
}
