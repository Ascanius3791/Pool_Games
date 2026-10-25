// rules8.h — 8-ball rules (simplified WPA, no called pockets)
//  * Break: cue ball behind the head string. Table is open after the break.
//    Illegal break (nothing potted and < 4 object balls to a rail) = foul.
//    8 potted on the break is re-spotted. Scratch on break = foul.
//  * Open table: first ball potted on a legal shot decides the groups.
//    Hitting the 8 first on an open table is a foul.
//  * Fouls: scratch, no ball hit, wrong ball first, no rail after contact
//    (when nothing is potted). Foul -> opponent has ball in hand anywhere.
//  * Potting the 8: wins if the shooter's group was already cleared and the
//    shot is not a foul; otherwise the shooter loses.
#pragma once
#include "physics.h"

namespace pool {

enum { GROUP_NONE = 0, GROUP_SOLIDS = 1, GROUP_STRIPES = 2 };
enum {
    FOUL_NONE = 0, FOUL_SCRATCH = 1, FOUL_NO_HIT = 2, FOUL_WRONG_FIRST = 3,
    FOUL_NO_RAIL = 4, FOUL_BAD_BREAK = 5
};

inline int ballGroup(int id) { return id == 0 ? -1 : (id == 8 ? 0 : (id < 8 ? GROUP_SOLIDS : GROUP_STRIPES)); }

struct Game8 {
    Sim sim;
    Rng rng;
    int turn;            // player to shoot: 0 or 1
    int group[2];
    int isBreak;
    int ballInHand;      // 1 = current player may place the cue ball
    int kitchenOnly;     // placement restricted behind the head string
    int winner;          // -1 running, 0/1 winner
    int lastFoul;
    int lastPotted;      // number of balls potted last shot
    int keptTurn;        // shooter continues
    int shots;
    int practice;        // 1 = no rules, always same player, ball in hand on scratch only
};

inline real headStringX(const Table& T) { return -T.L / 4; }
inline real footSpotX(const Table& T) { return T.L / 4; }

inline void resetBall(Ball& b, real x, real y) {
    b.x = x; b.y = y; b.vx = b.vy = b.wx = b.wy = b.wz = 0;
    b.qw = 1; b.qx = b.qy = b.qz = 0; b.on = 1; b.pocket = -1;
}

inline bool placeValid(const Game8& g, real x, real y, int self, bool kitchen) {
    const Sim& s = g.sim;
    const real R = s.P.R;
    if (pabs(x) > s.T.L / 2 - R || pabs(y) > s.T.W / 2 - R) return false;
    if (kitchen && x > headStringX(s.T)) return false;
    for (int i = 0; i < s.n; ++i) {
        if (i == self || !s.b[i].on) continue;
        real dx = s.b[i].x - x, dy = s.b[i].y - y;
        if (dx * dx + dy * dy < 4 * R * R * 1.0001) return false;
    }
    return true;
}

// Find a free spot for ball `id` near (x,y) along the long axis toward the foot rail.
inline void spotBall(Game8& g, int id, real x, real y) {
    Sim& s = g.sim;
    for (int k = 0; k < 400; ++k) {
        real tx = x + (k % 2 ? 1 : -1) * (k / 2) * 0.004;
        if (placeValid(g, tx, y, id, false)) { resetBall(s.b[id], tx, y); return; }
    }
    resetBall(s.b[id], x, y);
}

inline void rack8(Game8& g) {
    Sim& s = g.sim;
    const real R = s.P.R;
    s.n = 16;
    // order: apex = 1 ball (solid), 8 in the middle of row 3, back corners: one solid one stripe
    int ids[15];
    int solids[7] = {1, 2, 3, 4, 5, 6, 7}, stripes[7] = {9, 10, 11, 12, 13, 14, 15};
    // shuffle
    for (int i = 6; i > 0; --i) { int j = g.rng.below(i + 1); int t = solids[i]; solids[i] = solids[j]; solids[j] = t; }
    for (int i = 6; i > 0; --i) { int j = g.rng.below(i + 1); int t = stripes[i]; stripes[i] = stripes[j]; stripes[j] = t; }
    // positions 0..14 (rows 0..4). 8 at pos 4 (row 2 centre), back corners 10 & 14
    int rest[12]; int nr = 0;
    for (int i = 1; i < 7; ++i) rest[nr++] = solids[i];
    for (int i = 1; i < 7; ++i) rest[nr++] = stripes[i];
    for (int i = 11; i > 0; --i) { int j = g.rng.below(i + 1); int t = rest[i]; rest[i] = rest[j]; rest[j] = t; }
    bool flip = g.rng.below(2);
    int ri = 0;
    for (int p = 0; p < 15; ++p) {
        if (p == 4) ids[p] = 8;
        else if (p == 10) ids[p] = flip ? stripes[0] : solids[0];
        else if (p == 14) ids[p] = flip ? solids[0] : stripes[0];
        else ids[p] = rest[ri++];
    }
    int pos = 0;
    // ensure back corners have one solid & one stripe: positions 10 and 14 already fixed
    const real gap = 0.00005;     // tiny gap so the collision order is well defined
    const real d = 2 * R + gap;
    const real rowDx = d * 0.8660254037844386;
    real fx = footSpotX(s.T);
    pos = 0;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c <= r; ++c, ++pos) {
            real x = fx + r * rowDx + g.rng.uniform(-1, 1) * 0.000015;
            real y = (c - r * 0.5) * d + g.rng.uniform(-1, 1) * 0.000015;
            resetBall(s.b[ids[pos]], x, y);
        }
    resetBall(s.b[0], headStringX(s.T) - 0.15, 0);
}

inline void newGame8(Game8& g, unsigned long long seed, int practice) {
    g.rng.seed(seed);
    defaultPoolParams(g.sim.P);
    buildPoolTable(g.sim.T, g.sim.P);
    g.sim.trackOrient = 1;
    g.sim.nSnd = 0;
    rack8(g);
    clearEvents(g.sim);
    g.turn = 0; g.group[0] = g.group[1] = GROUP_NONE;
    g.isBreak = 1; g.ballInHand = 1; g.kitchenOnly = 1;
    g.winner = -1; g.lastFoul = 0; g.lastPotted = 0; g.keptTurn = 0; g.shots = 0;
    g.practice = practice;
}

inline int groupRemaining(const Game8& g, int grp) {
    int n = 0;
    for (int i = 1; i < 16; ++i) if (i != 8 && g.sim.b[i].on && ballGroup(i) == grp) ++n;
    return n;
}

// Is the player "on the 8" (own group cleared)?
inline bool onEight(const Game8& g, int pl) {
    return g.group[pl] != GROUP_NONE && groupRemaining(g, g.group[pl]) == 0;
}

// Evaluate the finished shot (sim must be at rest). Updates game state.
inline void endShot8(Game8& g) {
    Sim& s = g.sim;
    Events& e = s.ev;
    const int me = g.turn, op = 1 - me;
    g.shots++;
    bool cuePotted = !s.b[0].on;
    bool eightPotted = !s.b[8].on;
    int foul = FOUL_NONE;
    g.lastPotted = e.nPot;

    if (g.practice) {
        if (cuePotted) { spotBall(g, 0, headStringX(s.T) - 0.15, 0); g.ballInHand = 1; g.kitchenOnly = 0; }
        else g.ballInHand = 0;
        if (eightPotted && groupRemaining(g, GROUP_SOLIDS) + groupRemaining(g, GROUP_STRIPES) > 0)
            ; // keep potted in practice
        g.lastFoul = cuePotted ? FOUL_SCRATCH : FOUL_NONE;
        g.isBreak = 0; g.keptTurn = 1;
        return;
    }

    bool wasOnEight = onEight(g, me);   // NB: evaluated after balls are gone, fix below
    // recompute: was the player on the 8 *before* the shot? -> group balls potted this shot count back
    if (g.group[me] != GROUP_NONE) {
        int rem = groupRemaining(g, g.group[me]);
        int potMine = 0;
        for (int k = 0; k < e.nPot; ++k) if (ballGroup(e.pot[k]) == g.group[me]) ++potMine;
        wasOnEight = (rem + potMine) == 0;
    } else wasOnEight = false;

    if (g.isBreak) {
        int railBalls = 0;
        for (int i = 1; i < 16; ++i) if (e.railAny & (1u << i)) ++railBalls;
        int objPot = 0;
        for (int k = 0; k < e.nPot; ++k) if (e.pot[k] != 0) ++objPot;
        if (cuePotted) foul = FOUL_SCRATCH;
        else if (e.firstHit < 0) foul = FOUL_NO_HIT;
        else if (objPot == 0 && railBalls < 4) foul = FOUL_BAD_BREAK;
        if (eightPotted) spotBall(g, 8, footSpotX(s.T), 0);
        g.isBreak = 0;
        bool potAny = false;
        for (int k = 0; k < e.nPot; ++k) if (e.pot[k] != 0 && e.pot[k] != 8) potAny = true;
        g.lastFoul = foul;
        if (foul) {
            if (cuePotted) spotBall(g, 0, headStringX(s.T) - 0.15, 0);
            g.turn = op; g.ballInHand = 1; g.kitchenOnly = 0; g.keptTurn = 0;
        } else {
            g.ballInHand = 0; g.kitchenOnly = 0;
            g.keptTurn = potAny ? 1 : 0;
            if (!potAny) g.turn = op;
        }
        return;
    }

    // ---- normal shot: foul detection
    if (cuePotted) foul = FOUL_SCRATCH;
    else if (e.firstHit < 0) foul = FOUL_NO_HIT;
    else {
        int fh = e.firstHit;
        if (g.group[me] == GROUP_NONE) { if (fh == 8) foul = FOUL_WRONG_FIRST; }
        else if (wasOnEight) { if (fh != 8) foul = FOUL_WRONG_FIRST; }
        else if (ballGroup(fh) != g.group[me]) foul = FOUL_WRONG_FIRST;
        if (!foul) {
            bool objPot = false;
            for (int k = 0; k < e.nPot; ++k) if (e.pot[k] != 0) objPot = true;
            unsigned rail = e.railAfter;
            if (!objPot && rail == 0) foul = FOUL_NO_RAIL;
        }
    }
    if (foul == FOUL_NO_HIT && cuePotted) foul = FOUL_SCRATCH;
    g.lastFoul = foul;

    // ---- 8-ball potted
    if (eightPotted) {
        g.winner = (wasOnEight && !foul) ? me : op;
        g.keptTurn = 0;
        return;
    }

    // ---- group assignment on an open table
    if (!foul && g.group[me] == GROUP_NONE) {
        for (int k = 0; k < e.nPot; ++k) {
            int gr = ballGroup(e.pot[k]);
            if (gr == GROUP_SOLIDS || gr == GROUP_STRIPES) {
                g.group[me] = gr; g.group[op] = 3 - gr; break;
            }
        }
    }

    bool potOwn = false;
    if (g.group[me] != GROUP_NONE)
        for (int k = 0; k < e.nPot; ++k) if (ballGroup(e.pot[k]) == g.group[me]) potOwn = true;

    if (foul) {
        if (cuePotted) spotBall(g, 0, headStringX(s.T) - 0.15, 0);
        g.turn = op; g.ballInHand = 1; g.kitchenOnly = 0; g.keptTurn = 0;
    } else {
        g.ballInHand = 0; g.kitchenOnly = 0;
        g.keptTurn = potOwn ? 1 : 0;
        if (!potOwn) g.turn = op;
    }
}

} // namespace pool
