// SPDX-License-Identifier: GPL-3.0-or-later
#include "../um_cursor_map.hpp"
#include <cassert>
#include <cstdio>

int main() {
    CursorMap map;
    long x, y;
    map.SetGeometry(240, 0, 1024, 768, 1.40625);
    map.ToScreen(400, 300, x, y);
    assert(x == 803 && y == 422);
    map.ToGame(x + 3, y, x, y);
    assert(x == 402 && y == 300);

    // A caption drag to another monitor discards the old warp's origin.
    map.SetGeometry(-1500, 90, 1024, 768, 1.0);
    map.ToGame(-1300, 390, x, y);
    assert(x == 200 && y == 300);
    map.ToScreen(200, 300, x, y);
    assert(x == -1300 && y == 390);

    // Slow camera travel retains fractions across warps, but not across window moves.
    map.SetGeometry(0, 0, 1024, 768, 1.5);
    map.ToScreen(0, 0, x, y);
    map.ToGame(1, 0, x, y);
    assert(x == 0 && y == 0);
    map.ToScreen(0, 0, x, y);
    map.ToGame(1, 0, x, y);
    assert(x == 1 && y == 0);
    map.SetGeometry(200, 150, 1024, 768, 1.5);
    map.ToScreen(0, 0, x, y);
    map.ToGame(201, 150, x, y);
    assert(x == 0 && y == 0);

    map.SetGeometry(-1920, 30, 1024, 768, 1.0);
    map.ToGame(-2000, 0, x, y);
    assert(x == 0 && y == 0);
    map.ToGame(0, 1200, x, y);
    assert(x == 1023 && y == 767);
    std::puts("cursor mapping: move, negative origins, warp fractions and boundaries passed");
}
