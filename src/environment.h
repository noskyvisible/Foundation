#pragma once
#include <glm/glm.hpp>

#include "weather.h"

// ---- Environment: game clock + day/night sky + sun-driven light ----------
struct Environment {
    // Authored / controllable state.
    float timeOfDay   = 9.0f;     // hours, 0-24
    int   day         = 1;
    float dayLengthSec = 60.0f;   // real seconds for a full 24h game day
                                  // (short for testing; UI slider goes to 3600)
    bool  running     = true;
    // Sky controls.
    float cloudCover  = 0.4f;     // 0 clear .. 1 overcast (driven by weather)
    float exposure    = 1.0f;     // user's base sky tonemap exposure
    float skyExposure = 1.0f;     // derived: exposure * weather dimming (sent to shader)
    // Exponential height fog (always on; weather adds to the density). Thickest at
    // ground level, thinning with altitude, integrated along the view ray.
    float fogDensity  = 0.0225f;  // clear-day fog thickness at ground level (weather adds)
    float fogFalloff  = 0.035f;   // how fast fog thins with height (bigger = hugs ground)
    glm::vec3 fogTint {0.74f, 0.78f, 0.86f};  // user-picked daytime fog colour
    glm::vec3 fogColor{0.74f, 0.78f, 0.86f};  // derived: fogTint darkened at night / warmed at dusk
    // Weather: drives cloudCover + sky mood; eases between states over time.
    Weather weather;
    // Derived each frame from timeOfDay.
    glm::vec3 sunDir     {0, 1, 0};   // direction TO the sun
    glm::vec3 moonDir    {0,-1, 0};   // direction TO the moon (opposite the sun)
    glm::vec3 sunColor   {1.0f};
    glm::vec3 lightColor {1.0f};      // directional light colour (0 at night)
    glm::vec3 ambient    {0.3f};      // sky ambient colour (dim blue at night)
    // Advanced unconditionally (even when the clock is paused) so clouds/stars
    // keep drifting while you scrub or preview in the editor.
    float windTime    = 0.0f;
};

// `advance` should be true only in play mode; the editor freezes the clock
// (you scrub the time slider to preview). The sky/light are recomputed from
// timeOfDay regardless, so scrubbing updates the view.
void updateEnvironment(Environment& e, float dt, bool advance);
