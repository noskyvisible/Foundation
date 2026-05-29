#include "navigation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>

// ---- Cell <-> world helpers ----------------------------------------------
void NavGrid::worldToCell(glm::vec2 p, int& cx, int& cz) const {
    cx = (int)std::floor((p.x - origin.x) / cell);
    cz = (int)std::floor((p.y - origin.y) / cell);
    cx = std::max(0, std::min(cols - 1, cx));
    cz = std::max(0, std::min(rows - 1, cz));
}

glm::vec2 NavGrid::cellCenter(int cx, int cz) const {
    return glm::vec2(origin.x + (cx + 0.5f) * cell,
                     origin.y + (cz + 0.5f) * cell);
}

// ---- Grid construction ----------------------------------------------------
void buildNavGrid(NavGrid& grid, glm::vec2 worldMin, glm::vec2 worldMax,
                  float cellSize, const std::vector<NavRect>& obstacles,
                  float agentRadius) {
    if (cellSize <= 0.0f) cellSize = 0.5f;
    // Normalize bounds and guarantee at least one cell.
    glm::vec2 lo = glm::min(worldMin, worldMax);
    glm::vec2 hi = glm::max(worldMin, worldMax);
    grid.origin = lo;
    grid.cell   = cellSize;
    grid.cols   = std::max(1, (int)std::ceil((hi.x - lo.x) / cellSize));
    grid.rows   = std::max(1, (int)std::ceil((hi.y - lo.y) / cellSize));
    grid.walkable.assign((size_t)grid.cols * grid.rows, 1);

    // Mark a cell blocked if its center lies within any inflated obstacle.
    for (const NavRect& r : obstacles) {
        glm::vec2 rmin = glm::min(r.min, r.max) - glm::vec2(agentRadius);
        glm::vec2 rmax = glm::max(r.min, r.max) + glm::vec2(agentRadius);
        // Cell index range the rect can touch (inclusive).
        int x0 = (int)std::floor((rmin.x - lo.x) / cellSize);
        int x1 = (int)std::floor((rmax.x - lo.x) / cellSize);
        int z0 = (int)std::floor((rmin.y - lo.y) / cellSize);
        int z1 = (int)std::floor((rmax.y - lo.y) / cellSize);
        x0 = std::max(0, x0); z0 = std::max(0, z0);
        x1 = std::min(grid.cols - 1, x1); z1 = std::min(grid.rows - 1, z1);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                glm::vec2 c = grid.cellCenter(cx, cz);
                if (c.x >= rmin.x && c.x <= rmax.x && c.y >= rmin.y && c.y <= rmax.y)
                    grid.walkable[(size_t)cz * grid.cols + cx] = 0;
            }
    }
}

// Nearest free cell to (cx,cz) via an expanding ring search. Returns false if
// the whole grid is blocked.
static bool nearestFree(const NavGrid& g, int cx, int cz, int& ox, int& oz) {
    if (g.isWalkable(cx, cz)) { ox = cx; oz = cz; return true; }
    int maxR = std::max(g.cols, g.rows);
    for (int r = 1; r <= maxR; ++r) {
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) != r && std::abs(dz) != r) continue;  // ring only
                int nx = cx + dx, nz = cz + dz;
                if (g.isWalkable(nx, nz)) { ox = nx; oz = nz; return true; }
            }
    }
    return false;
}

// True if the straight segment a->b stays on walkable cells (used for string-
// pulling). Samples finely; obstacles are already inflated by agentRadius.
static bool lineClear(const NavGrid& g, glm::vec2 a, glm::vec2 b) {
    glm::vec2 d = b - a;
    float len = glm::length(d);
    if (len < 1e-5f) {
        int cx, cz; g.worldToCell(a, cx, cz);
        return g.isWalkable(cx, cz);
    }
    float step = g.cell * 0.25f;
    int n = (int)std::ceil(len / step);
    for (int i = 0; i <= n; ++i) {
        glm::vec2 p = a + d * ((float)i / (float)n);
        int cx, cz; g.worldToCell(p, cx, cz);
        if (!g.isWalkable(cx, cz)) return false;
    }
    return true;
}

bool findPath(const NavGrid& grid, glm::vec2 startXZ, glm::vec2 goalXZ,
              std::vector<glm::vec2>& outPath) {
    outPath.clear();
    if (grid.cols <= 0 || grid.rows <= 0 || grid.walkable.empty()) return false;

    int scx, scz, gcx, gcz;
    grid.worldToCell(startXZ, scx, scz);
    grid.worldToCell(goalXZ,  gcx, gcz);
    int sx, sz, gx, gz;
    if (!nearestFree(grid, scx, scz, sx, sz)) return false;
    if (!nearestFree(grid, gcx, gcz, gx, gz)) return false;

    const int   cols = grid.cols;
    const size_t N    = (size_t)cols * grid.rows;
    auto idx = [cols](int x, int z) { return (size_t)z * cols + x; };

    const float SQRT2 = 1.41421356f;
    auto heuristic = [&](int x, int z) {
        float dx = (float)std::abs(x - gx);
        float dz = (float)std::abs(z - gz);
        // Octile distance (scaled to world units).
        return (std::max(dx, dz) + (SQRT2 - 1.0f) * std::min(dx, dz)) * grid.cell;
    };

    std::vector<float> gScore(N, std::numeric_limits<float>::infinity());
    std::vector<int>   cameFrom(N, -1);
    std::vector<unsigned char> closed(N, 0);

    // Min-heap on f-score; entry = (f, cellIndex).
    using QEntry = std::pair<float, int>;
    std::priority_queue<QEntry, std::vector<QEntry>, std::greater<QEntry>> open;

    size_t startIdx = idx(sx, sz), goalIdx = idx(gx, gz);
    gScore[startIdx] = 0.0f;
    open.push({heuristic(sx, sz), (int)startIdx});

    const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    const int DZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

    bool found = false;
    while (!open.empty()) {
        int cur = open.top().second; open.pop();
        if ((size_t)cur == goalIdx) { found = true; break; }
        if (closed[cur]) continue;
        closed[cur] = 1;

        int cx = cur % cols, cz = cur / cols;
        for (int k = 0; k < 8; ++k) {
            int nx = cx + DX[k], nz = cz + DZ[k];
            if (!grid.isWalkable(nx, nz)) continue;
            bool diagonal = (DX[k] != 0 && DZ[k] != 0);
            // No corner cutting: both orthogonal neighbors must be free.
            if (diagonal && (!grid.isWalkable(cx + DX[k], cz) ||
                             !grid.isWalkable(cx, cz + DZ[k])))
                continue;
            size_t ni = idx(nx, nz);
            if (closed[ni]) continue;
            float stepCost = (diagonal ? SQRT2 : 1.0f) * grid.cell;
            float tentative = gScore[cur] + stepCost;
            if (tentative < gScore[ni]) {
                gScore[ni]  = tentative;
                cameFrom[ni] = cur;
                open.push({tentative + heuristic(nx, nz), (int)ni});
            }
        }
    }

    if (!found) return false;

    // Reconstruct cell-center path (goal -> start), then reverse.
    std::vector<glm::vec2> cells;
    for (int c = (int)goalIdx; c != -1; c = cameFrom[c])
        cells.push_back(grid.cellCenter(c % cols, c / cols));
    std::reverse(cells.begin(), cells.end());

    // Use the true start/goal positions as the path endpoints (nicer than cell
    // centers), keeping interior cell centers between them.
    cells.front() = startXZ;
    cells.back()  = goalXZ;

    // String-pull: keep a waypoint only when the line from the last kept point
    // to the next candidate would clip an obstacle.
    if (cells.size() <= 2) { outPath = cells; return true; }
    outPath.push_back(cells.front());
    size_t anchor = 0;
    for (size_t i = 1; i + 1 < cells.size(); ++i) {
        if (!lineClear(grid, cells[anchor], cells[i + 1])) {
            outPath.push_back(cells[i]);
            anchor = i;
        }
    }
    outPath.push_back(cells.back());
    return true;
}
