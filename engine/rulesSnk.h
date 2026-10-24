// rulesSnk.h — snooker (12-ft table) rules, simplified:
//  * Balls: 0 cue, 1..15 reds, 16 yellow(2) 17 green(3) 18 brown(4) 19 blue(5) 20 pink(6) 21 black(7).
//  * Break-off and after an in-off: cue ball in hand inside the D.
//  * Red on: first contact must be a red; every red potted scores 1.
//    Colour on (after a potted red): the striker nominates a colour (nomRequired);
//    without the option, or without a nomination, the first colour struck counts as
//    colour (automatic nomination); only it may be potted, it scores its value.
//    Colours are re-spotted while reds remain (and the colour after the last red).
//  * Then the colours yellow..black in order; they stay down.
//  * Fouls: in-off, miss everything, wrong ball first, potting a ball not on.
//    Penalty max(4, ball on, ball(s) involved) to the opponent, who plays next.
//  * Frame ends when the final black is potted (or fouled away). Tie -> black re-spotted.
//  * After a foul the incoming player chooses: play from where the balls lie
//    (with a free ball if snookered), or make the offender play again; after a
//    called miss also: balls replaced and the offender plays again. The miss
//    itself is judged by the referee in bot.h (it needs simulations).
//  * Free ball: the nominated ball acts as the ball on (red value while reds are
//    on, else the value of the colour on) and is re-spotted when potted. Leaving
//    the opponent snookered behind the free ball is a foul (unless only pink and
//    black remain).
#pragma once
#include "rules8.h"

namespace pool {

enum { SNK_RED_ON = 0, SNK_COLOUR_ON = 1, SNK_SEQUENCE = 2 };
enum { SFOUL_NONE = 0, SFOUL_INOFF = 1, SFOUL_MISS = 2, SFOUL_WRONG_FIRST = 3, SFOUL_WRONG_POT = 4, SFOUL_FB_SNOOKER = 5 };
enum { CH_PLAY = 1, CH_PUTBACK = 2, CH_REPLACE = 4 };

inline int snkValue(int id) { return id == 0 ? 0 : (id <= 15 ? 1 : id - 14); }
inline bool snkIsRed(int id) { return id >= 1 && id <= 15; }
inline bool snkIsColour(int id) { return id >= 16 && id <= 21; }

struct GameSnk {
    Sim sim;
    Rng rng;
    int turn;
    int score[2];
    int brk;              // current break
    int highBreak[2];
    int phase;            // SNK_RED_ON / SNK_COLOUR_ON / SNK_SEQUENCE
    int nextColour;       // in sequence phase: 16..21
    int isBreak;
    int ballInHand;       // in the D
    int winner;           // -1 running
    int lastFoul, foulPts;
    int keptTurn;
    int lastPts;          // points scored by the shooter on the last shot
    int nominated;        // nominated colour when a colour is on (-1 = none)
    int nomRequired;      // 1: a nominated colour must be hit first and only it may be potted
    int shots;
    int practice;
    int blackGame;        // re-spotted black after a tie
    // free ball / foul options / miss
    int freeBall;         // striker has a free ball for this shot
    int freeNom;          // nominated free ball (-1 = first ball struck)
    int freeAvail;        // incoming player is snookered after the foul -> free ball if they play on
    int pendingChoice;    // CH_* mask the incoming player must choose from (0 = none)
    int offender;         // player who committed the last foul
    int foulNoHit;        // last foul was a failure to hit a ball on (miss-eligible)
    int lastMiss;         // referee called a miss
    int missCount[2];     // consecutive misses with a clear sight of a ball on
};

struct SnkGeo { real baulkX, dR, spotX[22], spotY[22]; };
inline void snkGeo(const Table& T, SnkGeo& g) {
    g.baulkX = -T.L / 2 + 0.737; g.dR = 0.292;
    for (int i = 0; i < 22; ++i) { g.spotX[i] = 0; g.spotY[i] = 0; }
    // seen from the baulk end (looking toward +x) yellow is on the right (-y)
    g.spotX[16] = g.baulkX; g.spotY[16] = -g.dR;
    g.spotX[17] = g.baulkX; g.spotY[17] = g.dR;
    g.spotX[18] = g.baulkX; g.spotY[18] = 0;
    g.spotX[19] = 0;        g.spotY[19] = 0;
    g.spotX[20] = T.L / 4;  g.spotY[20] = 0;
    g.spotX[21] = T.L / 2 - 0.324; g.spotY[21] = 0;
}

inline bool snkFree(const GameSnk& g, real x, real y, int self) {
    const Sim& s = g.sim; const real R = s.P.R;
    if (pabs(x) > s.T.L / 2 - R || pabs(y) > s.T.W / 2 - R) return false;
    for (int i = 0; i < s.n; ++i) {
        if (i == self || !s.b[i].on) continue;
        real dx = s.b[i].x - x, dy = s.b[i].y - y;
        if (dx * dx + dy * dy < 4 * R * R * 1.0001) return false;
    }
    return true;
}
inline bool snkInD(const Table& T, real x, real y) {
    SnkGeo G; snkGeo(T, G);
    real dx = x - G.baulkX;
    return x <= G.baulkX && dx * dx + y * y <= G.dR * G.dR;
}
inline bool placeValid(const GameSnk& g, real x, real y, int self, bool inD) {
    if (inD && !snkInD(g.sim.T, x, y)) return false;
    return snkFree(g, x, y, self);
}

// ---- sight lines (balls only, as in the rules: cushions never snooker you)
inline bool snkLaneClear(const Sim& s, real x0, real y0, real x1, real y1, int skipA, int onlyId, unsigned onMask = 0) {
    const real R = s.P.R, clr = 2 * R * 0.999;
    real dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    for (int i = 1; i < s.n; ++i) {
        if (i == skipA || !s.b[i].on) continue;
        if (onlyId >= 0 && i != onlyId) continue;
        if (onMask & (1u << i)) continue;             // balls on never obstruct: you would hit a ball on
        real px = s.b[i].x - x0, py = s.b[i].y - y0;
        real t = L2 > 0 ? (px * dx + py * dy) / L2 : 0;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        real ex = px - t * dx, ey = py - t * dy;
        if (ex * ex + ey * ey < clr * clr) return false;
    }
    return true;
}
// straight line from the cue ball that contacts ball t with lateral offset e (|e| < 2R)
inline bool snkLineTo(const Sim& s, int t, real e, int onlyId, unsigned onMask = 0) {
    const Ball& c = s.b[0]; const Ball& B = s.b[t]; const real R = s.P.R;
    real dx = B.x - c.x, dy = B.y - c.y, d = psqrt(dx * dx + dy * dy);
    if (d < 2 * R) return true;
    real nx = -dy / d, ny = dx / d;
    real px = B.x + e * nx, py = B.y + e * ny;              // point the cue-ball centre passes
    real ux = px - c.x, uy = py - c.y, ul = psqrt(ux * ux + uy * uy); ux /= ul; uy /= ul;
    // cue-ball centre at contact: back off along the line until |G - B| = 2R
    real bx = B.x - c.x, by = B.y - c.y, proj = bx * ux + by * uy, perp2 = bx * bx + by * by - proj * proj;
    real back = psqrt(pmax(0, 4 * R * R - perp2));
    real gx = c.x + ux * (proj - back), gy = c.y + uy * (proj - back);
    return snkLaneClear(s, c.x, c.y, gx, gy, t, onlyId, onMask);
}
// balls that are "on" for the player to move, ignoring any free ball
inline bool snkPhaseOn(const GameSnk& g, int id) {
    if (id <= 0 || !g.sim.b[id].on) return false;
    if (g.phase == SNK_RED_ON) return snkIsRed(id);
    if (g.phase == SNK_COLOUR_ON) return snkIsColour(id);
    return id == g.nextColour;
}
// snookered: no ball on can be hit on both extreme edges in a straight line
inline bool snkSnookered(const GameSnk& g, int onlyId) {
    const real ee = 2 * g.sim.P.R - 0.0004;
    bool any = false;
    unsigned mask = 0;
    for (int t = 1; t < g.sim.n; ++t) if (snkPhaseOn(g, t)) mask |= 1u << t;
    for (int t = 1; t < g.sim.n; ++t) {
        if (!snkPhaseOn(g, t)) continue;
        any = true;
        if (snkLineTo(g.sim, t, ee, onlyId, mask) && snkLineTo(g.sim, t, -ee, onlyId, mask)) return false;
    }
    return any;
}
// clear straight sight of any part of any ball on (miss rule: always a miss)
inline bool snkDirectSight(const GameSnk& g) {
    const real R = g.sim.P.R;
    unsigned mask = 0;
    for (int t = 1; t < g.sim.n; ++t) if (snkPhaseOn(g, t) || (g.freeBall && g.sim.b[t].on)) mask |= 1u << t;
    for (int t = 1; t < g.sim.n; ++t) {
        if (!(mask & (1u << t))) continue;
        for (int k = -8; k <= 8; ++k) if (snkLineTo(g.sim, t, k * (2 * R - 0.0004) / 8, -1, mask)) return true;
    }
    return false;
}
inline bool snkNeedsSnookers(const GameSnk& g, int p);

// Re-spot a colour: own spot, else highest free spot, else nearest free point
// behind its own spot toward the top cushion (then toward baulk).
inline void snkRespot(GameSnk& g, int id) {
    SnkGeo G; snkGeo(g.sim.T, G);
    Ball& b = g.sim.b[id];
    if (snkFree(g, G.spotX[id], G.spotY[id], id)) { resetBall(b, G.spotX[id], G.spotY[id]); return; }
    for (int k = 21; k >= 16; --k) if (snkFree(g, G.spotX[k], G.spotY[k], id)) { resetBall(b, G.spotX[k], G.spotY[k]); return; }
    for (int k = 1; k < 2000; ++k) {
        real x = G.spotX[id] + k * 0.001;
        if (x < g.sim.T.L / 2 - g.sim.P.R && snkFree(g, x, G.spotY[id], id)) { resetBall(b, x, G.spotY[id]); return; }
        x = G.spotX[id] - k * 0.001;
        if (snkFree(g, x, G.spotY[id], id)) { resetBall(b, x, G.spotY[id]); return; }
    }
}

inline int snkRedsOn(const GameSnk& g) { int n = 0; for (int i = 1; i <= 15; ++i) n += g.sim.b[i].on; return n; }

inline void rackSnooker(GameSnk& g) {
    Sim& s = g.sim; const real R = s.P.R;
    s.n = 22;
    SnkGeo G; snkGeo(s.T, G);
    for (int id = 16; id <= 21; ++id) resetBall(s.b[id], G.spotX[id], G.spotY[id]);
    const real gap = 0.00005, d = 2 * R + gap, rowDx = d * 0.8660254037844386;
    real ax = G.spotX[20] + 2 * R + 0.001;
    int id = 1;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c <= r; ++c) {
            real x = ax + r * rowDx + g.rng.uniform(-1, 1) * 0.00003;
            real y = (c - r * 0.5) * d + g.rng.uniform(-1, 1) * 0.00003;
            resetBall(s.b[id++], x, y);
        }
    resetBall(s.b[0], G.baulkX - 0.10, -0.18);   // in the D, brown/yellow side
}

inline void newGameSnk(GameSnk& g, unsigned long long seed, int practice) {
    g.rng.seed(seed);
    defaultSnookerParams(g.sim.P);
    TableSpec sp; snookerSpec(sp);
    buildTable(g.sim.T, g.sim.P, sp);
    g.sim.trackOrient = 1; g.sim.nSnd = 0;
    rackSnooker(g);
    clearEvents(g.sim);
    g.turn = 0; g.score[0] = g.score[1] = 0; g.brk = 0; g.highBreak[0] = g.highBreak[1] = 0;
    g.phase = SNK_RED_ON; g.nextColour = 16; g.isBreak = 1; g.ballInHand = 1;
    g.winner = -1; g.lastFoul = 0; g.foulPts = 0; g.keptTurn = 0; g.lastPts = 0; g.nominated = -1; g.nomRequired = 1;
    g.shots = 0; g.practice = practice; g.blackGame = 0;
    g.freeBall = 0; g.freeNom = -1; g.freeAvail = 0; g.pendingChoice = 0; g.offender = -1;
    g.foulNoHit = 0; g.lastMiss = 0; g.missCount[0] = g.missCount[1] = 0;
}
inline int snkRemaining(const GameSnk& g);
inline bool snkNeedsSnookers(const GameSnk& g, int p) {
    // after the scores and the points left on the table, p cannot win without penalty points
    return g.score[1 - p] - g.score[p] > snkRemaining(g);
}

// Is `id` a ball the striker may hit first (and pot)?
inline bool snkOn(const GameSnk& g, int id) {
    if (id <= 0 || !g.sim.b[id].on) return false;
    if (g.freeBall) return true;      // free ball: any ball may be nominated
    if (g.phase == SNK_RED_ON) return snkIsRed(id);
    if (g.phase == SNK_COLOUR_ON) return snkIsColour(id);
    return id == g.nextColour;
}
inline int snkValueOn(const GameSnk& g) {
    if (g.phase == SNK_RED_ON) return 1;
    if (g.phase == SNK_COLOUR_ON) return 7;
    return snkValue(g.nextColour);
}
inline int snkRemaining(const GameSnk& g) {
    int reds = snkRedsOn(g);
    if (g.phase == SNK_SEQUENCE) { int r = 0; for (int c = g.nextColour; c <= 21; ++c) r += snkValue(c); return r; }
    return reds * 8 + 27 + (g.phase == SNK_COLOUR_ON ? 7 : 0);
}

inline void snkNextPhaseAfterTurnEnds(GameSnk& g) {
    g.phase = snkRedsOn(g) > 0 ? SNK_RED_ON : SNK_SEQUENCE;
    if (g.phase == SNK_SEQUENCE) {
        g.nextColour = 16;
        while (g.nextColour <= 21 && !g.sim.b[g.nextColour].on) g.nextColour++;
    }
}

inline void endShotSnk(GameSnk& g) {
    Sim& s = g.sim; Events& e = s.ev;
    const int me = g.turn, op = 1 - me;
    g.shots++;
    const bool inoff = !s.b[0].on;
    const int fh = e.firstHit;
    const int phase0 = g.phase, next0 = g.nextColour;
    const bool fb = g.freeBall != 0;
    const int nomCol = (phase0 == SNK_COLOUR_ON && g.nomRequired && g.nominated >= 16 && g.nominated <= 21) ? g.nominated : -1;
    g.nominated = -1;
    auto phaseLegal = [&](int id) {
        if (phase0 == SNK_COLOUR_ON) return nomCol > 0 ? id == nomCol : snkIsColour(id);
        return phase0 == SNK_RED_ON ? snkIsRed(id) : id == next0;
    };
    int F = -1;                                            // the free ball
    if (fb) { F = g.freeNom; if (F <= 0 && fh > 0 && !phaseLegal(fh)) F = fh; }
    g.freeBall = 0; g.freeNom = -1;
    int onVal = phase0 == SNK_RED_ON ? 1 : phase0 == SNK_COLOUR_ON ? (nomCol > 0 ? snkValue(nomCol) : fh >= 16 ? snkValue(fh) : 4) : snkValue(next0);
    int pen = onVal > 4 ? onVal : 4;
    int foul = SFOUL_NONE;
    const bool hitOn = fh > 0 && (phaseLegal(fh) || (fb && fh == F));
    if (fh < 0) foul = SFOUL_MISS;
    else if (!hitOn) { foul = SFOUL_WRONG_FIRST; int v = snkValue(fh); if (v > pen) pen = v; }
    int pts = 0, redsPot = 0; bool Fpot = false, nextPot = false, fhPot = false;
    for (int k = 0; k < e.nPot; ++k) {
        int id = e.pot[k];
        if (id == 0) continue;
        bool legal = phase0 == SNK_COLOUR_ON ? (nomCol > 0 ? id == nomCol : (id == fh && snkIsColour(id))) : phaseLegal(id);
        if (fb && id == F) legal = true;
        if (!legal) { if (!foul) foul = SFOUL_WRONG_POT; int v = snkValue(id); if (v > pen) pen = v; continue; }
        if (snkIsRed(id)) redsPot++;
        if (id == F) Fpot = true;
        if (id == next0) nextPot = true;
        if (id == fh) fhPot = true;
    }
    if (phase0 == SNK_RED_ON) pts = redsPot + ((Fpot && !snkIsRed(F)) ? 1 : 0);
    else if (phase0 == SNK_COLOUR_ON) pts = fhPot ? snkValue(fh) : 0;   // fh == nomCol whenever legal
    else pts = (nextPot || Fpot) ? snkValue(next0) : 0;
    if (inoff) foul = SFOUL_INOFF;
    // leaving the opponent snookered behind the free ball
    if (!foul && fb && pts == 0 && F > 0 && s.b[F].on) {
        int colsLeft = 0; for (int c = 16; c <= 21; ++c) colsLeft += s.b[c].on;
        bool onlyPB = snkRedsOn(g) == 0 && colsLeft == 2 && s.b[20].on && s.b[21].on;
        if (!onlyPB) {
            GameSnk t = g; t.turn = op; snkNextPhaseAfterTurnEnds(t);
            if (snkSnookered(t, F)) { foul = SFOUL_FB_SNOOKER; }
        }
    }
    if (g.practice) foul = inoff ? SFOUL_INOFF : SFOUL_NONE;
    g.lastFoul = foul; g.isBreak = 0; g.lastMiss = 0; g.pendingChoice = 0; g.freeAvail = 0;
    const bool blackFinal = (phase0 == SNK_SEQUENCE && next0 == 21);
    const bool blackDown = !s.b[21].on;

    if (foul) {
        g.foulPts = g.practice ? 0 : pen;
        g.score[op] += g.foulPts;
        g.lastPts = 0;
        g.offender = me;
        g.foulNoHit = (foul == SFOUL_MISS || foul == SFOUL_WRONG_FIRST || (foul == SFOUL_INOFF && !hitOn)) ? 1 : 0;
        for (int k = 0; k < e.nPot; ++k) { int id = e.pot[k]; if (snkIsColour(id) && !(blackFinal && id == 21)) snkRespot(g, id); }
        if (inoff) {
            SnkGeo G; snkGeo(s.T, G);
            for (int k = 0; k < 60; ++k) { real y = (k % 2 ? 1 : -1) * (k / 2) * 0.01; if (snkFree(g, G.baulkX - 0.10, y, 0)) { resetBall(s.b[0], G.baulkX - 0.10, y); break; } }
            g.ballInHand = 1;
        } else g.ballInHand = 0;
        if (g.brk > g.highBreak[me]) g.highBreak[me] = g.brk;
        g.brk = 0;
        g.keptTurn = 0;
        if (!g.practice) g.turn = op;
        if (blackFinal && blackDown) {
            if (g.score[0] != g.score[1]) { g.winner = g.score[0] > g.score[1] ? 0 : 1; return; }
            snkRespot(g, 21); g.blackGame = 1;
        }
        snkNextPhaseAfterTurnEnds(g);
        if (!g.practice) {
            g.pendingChoice = CH_PLAY | CH_PUTBACK;
            g.freeAvail = (!g.ballInHand && snkSnookered(g, -1)) ? 1 : 0;
        }
        return;
    }
    g.foulPts = 0;
    g.foulNoHit = 0;
    g.missCount[me] = 0;
    g.ballInHand = 0;
    g.lastPts = pts;
    g.score[me] += pts;
    if (Fpot && snkIsColour(F)) snkRespot(g, F);
    if (pts > 0) {
        g.brk += pts; g.keptTurn = 1;
        if (phase0 == SNK_RED_ON) { g.phase = SNK_COLOUR_ON; g.nominated = -1; return; }
        if (phase0 == SNK_COLOUR_ON) {
            snkRespot(g, fh);
            snkNextPhaseAfterTurnEnds(g);    // red on again, or the sequence starts
            return;
        }
        if (!nextPot) return;                // free ball potted in the sequence: same colour still on
        if (next0 == 21) {
            if (g.score[0] != g.score[1]) { g.winner = g.score[0] > g.score[1] ? 0 : 1; if (g.brk > g.highBreak[me]) g.highBreak[me] = g.brk; return; }
            snkRespot(g, 21); g.blackGame = 1; g.keptTurn = 0; g.turn = op;
            SnkGeo G; snkGeo(s.T, G); resetBall(s.b[0], G.baulkX - 0.1, 0.1); g.ballInHand = 1;
            if (g.brk > g.highBreak[me]) g.highBreak[me] = g.brk;
            g.brk = 0;
            return;
        }
        g.nextColour++;
        while (g.nextColour < 21 && !s.b[g.nextColour].on) g.nextColour++;
        return;
    }
    g.keptTurn = 0;
    if (g.brk > g.highBreak[me]) g.highBreak[me] = g.brk;
    g.brk = 0;
    if (!g.practice) g.turn = op;
    snkNextPhaseAfterTurnEnds(g);
}

// The incoming player's decision after a foul. `pre` is the position before the foul stroke.
inline void snkApplyChoice(GameSnk& g, const GameSnk& pre, int opt) {
    if (!(g.pendingChoice & opt)) opt = CH_PLAY;
    const int off = g.offender;
    if (opt == CH_PLAY) {
        g.freeBall = g.freeAvail; g.freeNom = -1;
        if (off >= 0) g.missCount[off] = 0;
    } else if (opt == CH_PUTBACK) {
        g.turn = off; g.freeBall = 0; g.freeNom = -1;
        if (off >= 0) g.missCount[off] = 0;
    } else {   // replace the balls, offender plays again from the original position
        for (int i = 0; i < g.sim.n; ++i) g.sim.b[i] = pre.sim.b[i];
        g.phase = pre.phase; g.nextColour = pre.nextColour; g.ballInHand = pre.ballInHand;
        g.freeBall = pre.freeBall; g.freeNom = pre.freeNom;
        g.turn = off;
    }
    g.pendingChoice = 0; g.freeAvail = 0;
}

} // namespace pool
