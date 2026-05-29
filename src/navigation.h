#pragma once
#include <glm/glm.hpp>

#include <vector>

// ---- Grid A* navigation (XZ ground plane) --------------------------------
// Geometry-only: knows nothing about the scene. The npc_sim layer translates
// scene objects into obstacle rectangles and feeds them in here.

// Axis-aligned obstacle footprint in the world XZ plane.
struct NavRect {
    glm::vec2 min{0.0f};
    glm::vec2 max{0.0f};
};

// A uniform walkability grid over the XZ plane. Cell (cx,cz) is free when
// walkable[cz*cols + cx] != 0. World<->cell mapping uses `origin` (the world XZ
// of cell (0,0)'s min corner) and `cell` (the cell size in world units).
struct NavGrid {
    glm::vec2 origin{0.0f};
    float     cell = 0.5f;
    int       cols = 0;
    int       rows = 0;
    std::vector<unsigned char> walkable;   // cols*rows, 1 = free, 0 = blocked

    bool inBounds(int cx, int cz) const {
        return cx >= 0 && cz >= 0 && cx < cols && cz < rows;
    }
    bool isWalkable(int cx, int cz) const {
        return inBounds(cx, cz) && walkable[(size_t)cz * cols + cx] != 0;
    }
    // Cell index containing world point p (clamped to grid bounds).
    void worldToCell(glm::vec2 p, int& cx, int& cz) const;
    // World XZ of a cell's center.
    glm::vec2 cellCenter(int cx, int cz) const;
};

// Build a grid spanning [worldMin, worldMax] (world XZ) at `cellSize`. Any cell
// overlapping an obstacle rect (inflated by `agentRadius`) is marked blocked.
void buildNavGrid(NavGrid& grid, glm::vec2 worldMin, glm::vec2 worldMax,
                  float cellSize, const std::vector<NavRect>& obstacles,
                  float agentRadius = 0.0f);

// A* from startXZ to goalXZ (world coords). On success fills `outPath` with
// smoothed world-XZ waypoints (start first, goal last) and returns true. If the
// start or goal cell is blocked, snaps to the nearest free cell first. Returns
// false if no path exists or the grid is empty.
bool findPath(const NavGrid& grid, glm::vec2 startXZ, glm::vec2 goalXZ,
              std::vector<glm::vec2>& outPath);
