#include "terrain.h"

#include <algorithm>
#include <cmath>

namespace {

// Cheap integer-hash value noise + fBm for the initial dune field.
float hash2(int x, int y) {
    int n = x * 374761393 + y * 668265263;
    n = (n ^ (n >> 13)) * 1274126177;
    return float((n ^ (n >> 16)) & 0x7fffffff) / float(0x7fffffff);
}
float vnoise(float x, float y) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float xf = x - xi, yf = y - yi;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float a = hash2(xi, yi),     b = hash2(xi + 1, yi);
    float c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
float fbm(float x, float y) {
    float v = 0.0f, amp = 0.5f, f = 1.0f;
    for (int i = 0; i < 5; ++i) { v += amp * vnoise(x * f, y * f); f *= 2.0f; amp *= 0.5f; }
    return v;
}
inline float smooth01(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

} // namespace

float Terrain::heightAt(float wx, float wz) const {
    float gx = (wx - originX()) / spacing();
    float gz = (wz - originZ()) / spacing();
    if (gx < 0.0f || gz < 0.0f || gx > res - 1 || gz > res - 1) return 0.0f;
    int i = std::min((int)gx, res - 2);
    int j = std::min((int)gz, res - 2);
    float fx = gx - i, fz = gz - j;
    float h00 = height[j * res + i],       h10 = height[j * res + i + 1];
    float h01 = height[(j + 1) * res + i], h11 = height[(j + 1) * res + i + 1];
    return (h00 * (1 - fx) + h10 * fx) * (1 - fz) + (h01 * (1 - fx) + h11 * fx) * fz;
}

void initTerrain(Terrain& t) {
    // Start FLAT at y=0 so the camera/objects sit on top of it (you sculpt up from
    // here, like every terrain tool). Brushes raise/lower from this baseline.
    t.height.assign(t.res * t.res, 0.0f);
    // Splatmap starts as all sand (layer 0); painting adds the other layers.
    t.splat.assign(t.splatRes * t.splatRes * 4, 0);
    for (int k = 0; k < t.splatRes * t.splatRes; ++k)
        t.splat[k * 4 + 0] = 255;   // R = sand = 1
    t.version++;
}

// Recompute the splat for a flat-vs-slope auto-look (used by a "reseed dunes"
// helper; kept here so the fbm/noise above stays referenced for later tools).
void seedDunes(Terrain& t, float amplitude) {
    for (int j = 0; j < t.res; ++j)
        for (int i = 0; i < t.res; ++i) {
            float u = i / float(t.res - 1), v = j / float(t.res - 1);
            float h = fbm(u * 6.0f, v * 6.0f); h = h * h;
            float edge = std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v));
            t.height[j * t.res + i] = h * amplitude * smooth01(edge / 0.12f);
        }
    t.version++;
}

void buildTerrainMesh(const Terrain& t, std::vector<float>& verts,
                      std::vector<unsigned int>& indices) {
    verts.clear();
    indices.clear();
    verts.reserve(t.res * t.res * 8);
    indices.reserve((t.res - 1) * (t.res - 1) * 6);

    const float sp = t.spacing(), ox = t.originX(), oz = t.originZ();
    for (int j = 0; j < t.res; ++j)
        for (int i = 0; i < t.res; ++i) {
            float x = ox + i * sp, z = oz + j * sp, y = t.height[j * t.res + i];
            int il = std::max(i - 1, 0),         ir = std::min(i + 1, t.res - 1);
            int jd = std::max(j - 1, 0),         ju = std::min(j + 1, t.res - 1);
            float hl = t.height[j * t.res + il], hr = t.height[j * t.res + ir];
            float hd = t.height[jd * t.res + i], hu = t.height[ju * t.res + i];
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.0f * sp, hd - hu));
            float u = i / float(t.res - 1), v = j / float(t.res - 1);
            verts.insert(verts.end(), { x, y, z, n.x, n.y, n.z, u, v });
        }

    for (int j = 0; j < t.res - 1; ++j)
        for (int i = 0; i < t.res - 1; ++i) {
            unsigned int a = j * t.res + i, b = a + 1;
            unsigned int c = a + t.res,     d = c + 1;
            indices.insert(indices.end(), { a, c, b, b, c, d });
        }
}

bool raycastTerrain(const Terrain& t, glm::vec3 ro, glm::vec3 rd, glm::vec3& outHit) {
    // March along the ray and find where it first drops below the heightfield,
    // then binary-refine. Good enough for brush placement.
    float maxT = t.worldSize * 2.5f;
    float step = std::max(t.spacing(), 0.5f);
    float prevT = 0.0f;
    float prevDiff = ro.y - t.heightAt(ro.x, ro.z);
    for (float tt = step; tt < maxT; tt += step) {
        glm::vec3 p = ro + rd * tt;
        float diff = p.y - t.heightAt(p.x, p.z);
        if (diff <= 0.0f && prevDiff > 0.0f) {
            float lo = prevT, hi = tt;
            for (int it = 0; it < 14; ++it) {
                float mid = 0.5f * (lo + hi);
                glm::vec3 pm = ro + rd * mid;
                if (pm.y - t.heightAt(pm.x, pm.z) <= 0.0f) hi = mid; else lo = mid;
            }
            outHit = ro + rd * hi;
            return true;
        }
        prevT = tt;
        prevDiff = diff;
    }
    return false;
}

void applyTerrainBrush(Terrain& t, glm::vec3 center, const TerrainBrush& b, float dt) {
    const float sp = t.spacing(), ox = t.originX(), oz = t.originZ();
    int ci  = (int)std::lround((center.x - ox) / sp);
    int cj  = (int)std::lround((center.z - oz) / sp);
    int rad = (int)std::ceil(b.radius / sp) + 1;
    float amt = b.strength * dt;

    for (int j = std::max(cj - rad, 0); j <= std::min(cj + rad, t.res - 1); ++j)
        for (int i = std::max(ci - rad, 0); i <= std::min(ci + rad, t.res - 1); ++i) {
            float wx = ox + i * sp, wz = oz + j * sp;
            float d = std::sqrt((wx - center.x) * (wx - center.x) + (wz - center.z) * (wz - center.z));
            if (d > b.radius) continue;
            float fall = 1.0f - d / b.radius;
            fall = fall * fall * (3.0f - 2.0f * fall);     // smooth falloff
            float& h = t.height[j * t.res + i];
            switch (b.tool) {
                case TerrainTool::Raise:   h += amt * fall; break;
                case TerrainTool::Lower:   h -= amt * fall; break;
                case TerrainTool::Flatten: h = glm::mix(h, center.y, std::min(1.0f, amt * 0.4f * fall)); break;
                case TerrainTool::Smooth: {
                    float sum = 0.0f; int n = 0;
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di) {
                            int ii = i + di, jj = j + dj;
                            if (ii < 0 || ii >= t.res || jj < 0 || jj >= t.res) continue;
                            sum += t.height[jj * t.res + ii]; ++n;
                        }
                    h = glm::mix(h, sum / n, std::min(1.0f, amt * 0.5f * fall));
                    break;
                }
            }
        }
    t.version++;
}
