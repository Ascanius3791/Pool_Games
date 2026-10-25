// positions.h — families of snooker starting positions for self-play and the arena.
//
// Self-play from the break rarely reaches some situations that decide frames: needing
// snookers, escaping a laid snooker, a free ball, a long safety exchange, the last colours,
// a black-ball finish. Each family below builds such a position (legal: no overlaps, balls
// clear of cushions and jaws, phase/nextColour consistent with the balls on, scores
// consistent with the points that have left the table), so the evaluator sees it in
// training and the arena can grade bots on it. See TRAINING.md, "Starting positions".
//
//   break      frame from the break-off (the ordinary self-play frame)
//   random     the old randomStartSnk: random reds, scattered balls, score within +-40
//   snookers   mover needs snookers (or nearly): few reds or colours only, behind by
//              R-8 .. R+30 (one frame in five R+30 .. R+70, "comically" far behind),
//              balls not on near the ball on, so snookers can be laid
//   lead       the mirror: mover ahead by R-25 .. R+15 with up to 8 reds (tighten up)
//   snookered  mover is snookered (engine definition: no ball on with both edges in
//              sight): cue ball behind a ball not on, half of them tight on a cushion
//   freeball   the same after the opponent's foul: the mover has a free ball
//   safety     8-15 reds in a (partly broken) pack, colours on their spots, cue ball on or
//              near the baulk cushion (or behind the pack), close score
//   clearance  colours only (yellow..pink on) or 1-3 reds left, score in balance (the
//              mover can win by clearing), cue ball often in position on a ball on
//   black      black-ball finishes: re-spotted black (tie, ball in hand), last black or
//              pink + black with the score within a few points
//   perturbed  a real self-play position (from *.posrec) with the score changed and
//              sometimes a ball moved or the mover swapped
#pragma once
#include "common.h"
#include <dirent.h>
#include <sstream>

namespace train {

enum { FAM_BREAK, FAM_RANDOM, FAM_SNOOKERS, FAM_LEAD, FAM_SNOOKERED, FAM_FREEBALL, FAM_SAFETY,
       FAM_CLEARANCE, FAM_BLACK, FAM_PERTURBED, FAM_N };
static const char* const FAM_NAMES[FAM_N] = {"break", "random", "snookers", "lead", "snookered", "freeball",
                                             "safety", "clearance", "black", "perturbed"};
const int FAM_UNKNOWN = SNK_FAM_UNKNOWN;   // frames written before the families existed (posrec version 2)
inline int famIndex(const std::string& s) {
    for (int i = 0; i < FAM_N; ++i) if (s == FAM_NAMES[i]) return i;
    return -1;
}
inline const char* famName(int f) { return f >= 0 && f < FAM_N ? FAM_NAMES[f] : "unknown"; }
// families whose shot statistics may calibrate the abstract simulator (natural play)
inline bool famNatural(int f) { return f == FAM_BREAK || f == FAM_RANDOM; }

// ------------------------------------------------------------------ family weights
// spec: "break:3 snookers:1,lead:0.5 safety" (space or comma separated, a bare name = 1,
// "all" = every family with weight 1)
struct FamMix {
    double w[FAM_N] = {0};
    double cur[FAM_N] = {0};
    bool parse(const std::string& spec, std::string& err) {
        for (double& x : w) x = 0;
        for (double& x : cur) x = 0;
        std::string s = spec; for (char& ch : s) if (ch == ',') ch = ' ';
        std::istringstream in(s); std::string tok;
        while (in >> tok) {
            size_t c = tok.find(':');
            std::string name = c == std::string::npos ? tok : tok.substr(0, c);
            double v = c == std::string::npos ? 1 : atof(tok.c_str() + c + 1);
            if (name == "all") { for (double& x : w) x = v; continue; }
            int f = famIndex(name);
            if (f < 0) { err = "unknown family '" + name + "'"; return false; }
            if (v < 0) { err = "negative weight for " + name; return false; }
            w[f] = v;
        }
        if (total() <= 0) { err = "no family with a positive weight"; return false; }
        return true;
    }
    double total() const { double t = 0; for (double x : w) t += x; return t; }
    int sample(Rng& r) const {                 // random, by weight
        double u = r.uniform() * total();
        for (int i = 0; i < FAM_N; ++i) { if (w[i] <= 0) continue; if (u < w[i]) return i; u -= w[i]; }
        for (int i = FAM_N - 1; i >= 0; --i) if (w[i] > 0) return i;
        return FAM_BREAK;
    }
    int next() {                               // deterministic interleaving (smooth weighted round robin)
        int bi = -1; const double T = total();
        for (int i = 0; i < FAM_N; ++i) { if (w[i] <= 0) continue; cur[i] += w[i]; if (bi < 0 || cur[i] > cur[bi]) bi = i; }
        if (bi < 0) return FAM_BREAK;
        cur[bi] -= T; return bi;
    }
    std::string str() const {
        std::string o; char b[64];
        for (int i = 0; i < FAM_N; ++i) if (w[i] > 0) { snprintf(b, sizeof b, "%s%s:%g", o.empty() ? "" : " ", FAM_NAMES[i], w[i]); o += b; }
        return o;
    }
};

// ------------------------------------------------------------------ geometry
// ball centre (x, y) clear of the cushion noses, jaw facings and jaw noses (margin m), on the
// table side of every pocket mouth, and at least pocketClear from every mouth midpoint.
// inPlay: a ball at rest after a shot may hang in a pocket opening; only penetration counts.
inline bool snkFitsTable(const Sim& s, real x, real y, real m = 1e-6, real pocketClear = 0, bool inPlay = false) {
    const Table& T = s.T; const real R = s.P.R;
    if (inPlay) { if (pabs(x) > T.L / 2 + 0.1 || pabs(y) > T.W / 2 + 0.1) return false; }
    else if (pabs(x) > T.L / 2 - R - m || pabs(y) > T.W / 2 - R - m) return false;   // beyond a rail line
    for (int k = 0; k < T.nseg; ++k) {
        const Seg& g = T.seg[k];
        real d = (x - g.ax) * g.nx + (y - g.ay) * g.ny, sp = (x - g.ax) * g.dx + (y - g.ay) * g.dy;
        if (sp < 0 || sp > g.len) continue;
        if (d > -R && d < R + m) return false;
    }
    for (int k = 0; k < T.npt; ++k) {
        real dx = x - T.ptx[k], dy = y - T.pty[k];
        if (dx * dx + dy * dy < (R + T.ptr[k] + m) * (R + T.ptr[k] + m)) return false;
    }
    for (int k = 0; k < T.npk && !inPlay; ++k) {
        const Pocket& P = T.pk[k];
        real dx = x - P.mx, dy = y - P.my;
        if (dx * P.nx + dy * P.ny > -0.25 * R) return false;          // at or past the mouth line
        if (pocketClear > 0 && dx * dx + dy * dy < pocketClear * pocketClear) return false;
    }
    return true;
}
// no other ball on within 2R + gap
inline bool snkClearOfBalls(const Sim& s, int self, real x, real y, real gap) {
    const real dmin = 2 * s.P.R + gap;
    for (int i = 0; i < s.n; ++i) {
        if (i == self || !s.b[i].on) continue;
        real dx = s.b[i].x - x, dy = s.b[i].y - y;
        if (dx * dx + dy * dy < dmin * dmin) return false;
    }
    return true;
}
const real POS_GAP = 0.0003;          // minimum gap between generated balls (0.3 mm)
const real POS_POCKET = 0.075;        // generated balls stay this far from the pocket mouths
inline bool tryPlace(GameSnk& g, int id, real x, real y, real gap = POS_GAP, real pocketClear = POS_POCKET) {
    if (!snkFitsTable(g.sim, x, y, 2e-4, pocketClear) || !snkClearOfBalls(g.sim, id, x, y, gap)) return false;
    resetBall(g.sim.b[id], x, y); return true;
}
inline bool placeIn(GameSnk& g, int id, Rng& r, real x0, real x1, real y0, real y1, int tries = 300) {
    for (int k = 0; k < tries; ++k) if (tryPlace(g, id, r.uniform(x0, x1), r.uniform(y0, y1))) return true;
    return false;
}
inline bool placeAnywhere(GameSnk& g, int id, Rng& r) {
    const real hx = g.sim.T.L / 2, hy = g.sim.T.W / 2;
    return placeIn(g, id, r, -hx, hx, -hy, hy);
}
// within `band` of a random cushion (frozen to it with probability pFrozen)
inline bool placeNearCushion(GameSnk& g, int id, Rng& r, real band, real pFrozen) {
    const real hx = g.sim.T.L / 2, hy = g.sim.T.W / 2, R = g.sim.P.R;
    for (int k = 0; k < 300; ++k) {
        int side = r.below(4);
        real d = r.uniform() < pFrozen ? r.uniform(2.5e-4, 0.002) : r.uniform(0.002, band);
        real x, y;
        if (side < 2) { y = (side ? 1 : -1) * (hy - R - d); x = r.uniform(-hx, hx); }
        else { x = (side == 2 ? 1 : -1) * (hx - R - d); y = r.uniform(-hy, hy); }
        if (tryPlace(g, id, x, y)) return true;
    }
    return false;
}
inline bool placeSpotOr(GameSnk& g, int id, Rng& r, real pSpot) {
    SnkGeo G; snkGeo(g.sim.T, G);
    if (r.uniform() < pSpot && tryPlace(g, id, G.spotX[id], G.spotY[id])) return true;
    return placeAnywhere(g, id, r);
}

// ------------------------------------------------------------------ rules state
inline void snkClearBalls(GameSnk& g) { for (int i = 0; i < g.sim.n; ++i) g.sim.b[i].on = 0; }
// common state of a mid-frame start (no break-off, nothing pending)
inline void snkMidFrame(GameSnk& g, int phase, int nextColour) {
    g.phase = phase; g.nextColour = nextColour; g.isBreak = 0; g.ballInHand = 0; g.brk = 0;
    g.freeBall = 0; g.freeNom = -1; g.freeAvail = 0; g.pendingChoice = 0; g.offender = -1;
    g.foulNoHit = 0; g.lastMiss = 0; g.missCount[0] = g.missCount[1] = 0; g.lastFoul = 0; g.foulPts = 0;
    g.nominated = -1; g.blackGame = 0; g.winner = -1; g.keptTurn = 0; g.lastPts = 0;
}
// scores with the mover ahead by D: the total is what has left the table plus a few foul points
inline void snkSetScores(GameSnk& g, int D, Rng& r) {
    if (D > 155) D = 155; if (D < -155) D = -155;
    const int taken = 147 - snkRemaining(g);
    int S = taken + r.below(12);
    if (S < (D < 0 ? -D : D)) S = (D < 0 ? -D : D) + r.below(6);
    if ((S + D) & 1) S++;
    g.score[g.turn] = (S + D) / 2; g.score[1 - g.turn] = (S - D) / 2;
}
inline int lead(const GameSnk& g) { return g.score[g.turn] - g.score[1 - g.turn]; }
inline int randInt(Rng& r, int lo, int hi) { return lo + r.below(hi - lo + 1); }   // [lo, hi]

// Is g a legal, consistent position to start a frame (or, with inPlay, a position reached in
// play: balls may hang in the jaws, a foul choice may be pending, the re-spotted black may have
// been fouled)? Returns "" if so, else the first problem found.
inline std::string snkCheck(const GameSnk& g, bool inPlay = false) {
    const Sim& s = g.sim; const real R = s.P.R;
    char b[160];
    const real tol = inPlay ? 1e-6 : 0;   // contacts at rest after a simulated shot
    if (s.n != 22) return "ball count";
    if (!s.b[0].on) return "cue ball off the table";
    for (int i = 0; i < s.n; ++i) {
        const Ball& A = s.b[i]; if (!A.on) continue;
        if (!(A.x == A.x) || !(A.y == A.y)) return "NaN position";
        if (A.vx != 0 || A.vy != 0 || A.wx != 0 || A.wy != 0 || A.wz != 0) { snprintf(b, sizeof b, "ball %d moving", i); return b; }
        if (!snkFitsTable(s, A.x, A.y, -tol, 0, inPlay)) { snprintf(b, sizeof b, "ball %d at (%.4f, %.4f) not clear of the cushions/pockets", i, A.x, A.y); return b; }
        for (int j = i + 1; j < s.n; ++j) {
            const Ball& B = s.b[j]; if (!B.on) continue;
            real dx = A.x - B.x, dy = A.y - B.y;
            if (dx * dx + dy * dy < (2 * R - tol) * (2 * R - tol)) { snprintf(b, sizeof b, "balls %d and %d overlap by %.2e m", i, j, 2 * R - psqrt(dx * dx + dy * dy)); return b; }
        }
    }
    const int reds = snkRedsOn(g);
    int cols = 0; for (int c = 16; c <= 21; ++c) cols += s.b[c].on;
    if (g.phase == SNK_RED_ON) { if (reds == 0) return "red on without reds"; if (cols != 6) return "a colour is missing while reds are on"; }
    else if (g.phase == SNK_COLOUR_ON) { if (cols != 6) return "a colour is missing with a colour on"; }
    else if (g.phase == SNK_SEQUENCE) {
        if (reds) return "sequence with reds on";
        if (g.nextColour < 16 || g.nextColour > 21) return "nextColour out of range";
        for (int c = 16; c <= 21; ++c) if ((c >= g.nextColour) != (s.b[c].on != 0)) { snprintf(b, sizeof b, "colour %d inconsistent with nextColour %d", c, g.nextColour); return b; }
    } else return "bad phase";
    if (g.turn != 0 && g.turn != 1) return "bad turn";
    if (g.winner != -1) return "frame already over";
    if (!inPlay && g.pendingChoice) return "pending foul choice";
    if (g.score[0] < 0 || g.score[1] < 0) return "negative score";
    const int D = g.score[0] - g.score[1];
    if (D > 160 || D < -160) return "score difference beyond the win table";
    if (g.score[0] + g.score[1] > 147 - snkRemaining(g) + 120) return "more points than taken plus fouls";
    if (g.blackGame && !(g.phase == SNK_SEQUENCE && g.nextColour == 21 && (inPlay || g.score[0] == g.score[1]))) return "blackGame without a tie on the black";
    if (g.ballInHand && !snkInD(s.T, s.b[0].x, s.b[0].y)) return "ball in hand outside the D";
    if (g.freeBall && (g.ballInHand || !snkSnookered(g, -1))) return "free ball without being snookered";
    if (g.isBreak && (reds != 15 || !g.ballInHand)) return "break-off without the full rack";
    return "";
}

// ------------------------------------------------------------------ the families
// helpers shared by several families: colours yellow..black from `first` (16 = all), reds 1..k
inline bool layColours(GameSnk& g, Rng& r, int first, real pSpot) {
    for (int c = 21; c >= first; --c) if (!placeSpotOr(g, c, r, pSpot)) return false;   // black first: its spot is the busiest
    return true;
}
inline bool layReds(GameSnk& g, Rng& r, int k, real pCushion) {
    for (int i = 1; i <= k; ++i) {
        bool ok = r.uniform() < pCushion ? placeNearCushion(g, i, r, 0.15, 0.3) : placeAnywhere(g, i, r);
        if (!ok) return false;
    }
    return true;
}
// is some ball not on within `dist` of a ball on? (something to hide the ball on behind)
inline bool snookerPotential(const GameSnk& g, real dist) {
    const Sim& s = g.sim;
    for (int t = 1; t < s.n; ++t) {
        if (!snkPhaseOn(g, t)) continue;
        for (int j = 1; j < s.n; ++j) {
            if (j == t || !s.b[j].on || snkPhaseOn(g, j)) continue;
            real dx = s.b[j].x - s.b[t].x, dy = s.b[j].y - s.b[t].y;
            if (dx * dx + dy * dy < dist * dist) return true;
        }
    }
    return false;
}
inline int pickSequenceStart(Rng& r, const double* w6) {   // weights for yellow..black
    double t = 0; for (int i = 0; i < 6; ++i) t += w6[i];
    double u = r.uniform() * t;
    for (int i = 0; i < 6; ++i) { if (u < w6[i]) return 16 + i; u -= w6[i]; }
    return 16;
}

// needs snookers (lead = false) or the leader's mirror image (lead = true)
inline bool genScoreContext(GameSnk& g, Rng& r, bool leadFam) {
    snkClearBalls(g);
    const bool seq = r.uniform() < (leadFam ? 0.35 : 0.5);
    int reds = 0, first = 16;
    if (seq) { const double w6[6] = {2, 2, 2, 1.5, 1, 0}; first = pickSequenceStart(r, w6); }
    else reds = leadFam ? randInt(r, 1, 8) : randInt(r, 1, 4);
    if (!layColours(g, r, first, 0.7) || !layReds(g, r, reds, 0.5)) return false;
    snkMidFrame(g, seq ? SNK_SEQUENCE : SNK_RED_ON, first);
    if (!snookerPotential(g, 0.45)) return false;
    // cue ball anywhere, preferably with a ball on in sight (the mover has the choice)
    bool placed = false;
    for (int k = 0; k < 30 && !placed; ++k) { if (!placeAnywhere(g, 0, r)) return false; placed = !snkSnookered(g, -1); }
    g.turn = r.below(2);
    const int R = snkRemaining(g);
    int D;
    if (!leadFam) D = -(R + (r.uniform() < 0.2 ? randInt(r, 30, 70) : randInt(r, -8, 30)));
    else D = R + randInt(r, -25, 15);
    if (leadFam && D < 1) D = 1;
    snkSetScores(g, D, r);
    return true;
}

// cue ball snookered behind a ball not on; freeBall: after the opponent's foul
inline bool genSnookered(GameSnk& g, Rng& r, bool freeBall) {
    snkClearBalls(g);
    const bool seq = r.uniform() < 0.45;
    int reds = 0, first = 16;
    if (seq) { const double w6[6] = {2, 2, 2, 1.5, 1, 0}; first = pickSequenceStart(r, w6); }
    else reds = r.uniform() < 0.6 ? randInt(r, 1, 6) : randInt(r, 7, 15);
    if (!layColours(g, r, first, 0.6) || !layReds(g, r, reds, 0.35)) return false;
    snkMidFrame(g, seq ? SNK_SEQUENCE : SNK_RED_ON, first);
    const Sim& s = g.sim; const real R = s.P.R;
    int blockers[22], nbk = 0, ons[22], non = 0;
    for (int i = 1; i < s.n; ++i) { if (!s.b[i].on) continue; if (snkPhaseOn(g, i)) ons[non++] = i; else blockers[nbk++] = i; }
    if (!nbk || !non) return false;
    for (int attempt = 0; attempt < 400; ++attempt) {
        const int B = blockers[r.below(nbk)];
        real cx, cy;
        if (r.uniform() < 0.45) {
            // tight on a cushion: blocker a little off the rail, cue ball frozen behind it
            if (!placeNearCushion(g, B, r, 0.10, 0.0)) continue;
            const real hx = s.T.L / 2, hy = s.T.W / 2, bx = s.b[B].x, by = s.b[B].y;
            const real dy = hy - pabs(by), dx = hx - pabs(bx);
            const real fr = r.uniform(2.5e-4, 0.0015);
            if (dy < dx) { cy = (by > 0 ? 1 : -1) * (hy - R - fr); cx = bx + r.uniform(-1.2, 1.2) * R; }
            else { cx = (bx > 0 ? 1 : -1) * (hx - R - fr); cy = by + r.uniform(-1.2, 1.2) * R; }
        } else {
            // behind the blocker, on the far side from a ball on
            const int T = ons[r.below(non)];
            real ux = s.b[B].x - s.b[T].x, uy = s.b[B].y - s.b[T].y, d = psqrt(ux * ux + uy * uy);
            if (d < 1e-6) continue;
            const real a = r.uniform(-0.35, 0.35), ca = pcos(a), sa = psin(a);
            real wx = (ux * ca - uy * sa) / d, wy = (ux * sa + uy * ca) / d;
            const real gap = r.uniform() < 0.4 ? r.uniform(POS_GAP, 0.01) : r.uniform(0.01, 0.12);
            cx = s.b[B].x + wx * (2 * R + gap); cy = s.b[B].y + wy * (2 * R + gap);
        }
        if (!tryPlace(g, 0, cx, cy, POS_GAP, 0.06)) continue;
        if (!snkSnookered(g, -1)) continue;
        g.turn = r.below(2);
        snkSetScores(g, randInt(r, -30, 30), r);
        if (freeBall) { g.freeBall = 1; g.freeNom = -1; }
        return true;
    }
    return false;
}

inline bool genSafety(GameSnk& g, Rng& r) {
    snkClearBalls(g);
    Sim& s = g.sim; const real R = s.P.R;
    SnkGeo G; snkGeo(s.T, G);
    if (!layColours(g, r, 16, 0.92)) return false;
    // the pack: rack slots around a jittered apex, spacing from touching to a few mm
    const real gap = r.uniform() < 0.5 ? r.uniform(0.00005, 0.0005) : r.uniform(0.0005, 0.004);
    const real d = 2 * R + gap, rowDx = d * 0.8660254037844386;
    const real ax = G.spotX[20] + 2 * R + 0.002 + r.uniform(0, 0.05), ay = r.uniform(-0.04, 0.04);
    real sx[15], sy[15]; int ord[15];
    for (int row = 0, k = 0; row < 5; ++row) for (int c = 0; c <= row; ++c, ++k) { sx[k] = ax + row * rowDx; sy[k] = ay + (c - row * 0.5) * d; ord[k] = k; }
    for (int i = 14; i > 0; --i) { int j = r.below(i + 1); int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    const int reds = randInt(r, 8, 15), loose = std::min(reds - 4, r.below(4));
    int id = 1;
    for (int k = 0; k < 15 && id <= reds - loose; ++k) if (tryPlace(g, id, sx[ord[k]], sy[ord[k]], 0, POS_POCKET)) id++;
    for (; id <= reds; ++id)
        if (!(r.uniform() < 0.5 ? placeNearCushion(g, id, r, 0.2, 0.3) : placeIn(g, id, r, 0, s.T.L / 2, -s.T.W / 2, s.T.W / 2))) return false;
    if (snkRedsOn(g) != reds) return false;
    // cue ball: on or near the baulk cushion, or behind the pack on the top cushion
    const real hx = s.T.L / 2, hy = s.T.W / 2;
    bool ok = false;
    for (int k = 0; k < 200 && !ok; ++k) {
        const bool top = r.uniform() < 0.25;
        const real dd = r.uniform() < 0.35 ? r.uniform(2.5e-4, 0.002) : r.uniform(0.002, top ? 0.12 : 0.30);
        const real x = (top ? 1 : -1) * (hx - R - dd), y = r.uniform(-(hy - 0.12), hy - 0.12);
        ok = tryPlace(g, 0, x, y);
    }
    if (!ok) return false;
    snkMidFrame(g, SNK_RED_ON, 16);
    g.turn = r.below(2);
    snkSetScores(g, randInt(r, -20, 20), r);
    return true;
}

// cue ball in position on ball t (some open pocket, angle up to ~35 degrees)
inline bool placeInPosition(GameSnk& g, Rng& r, int t) {
    const Sim& s = g.sim; const real R = s.P.R;
    for (int k = 0; k < 60; ++k) {
        const int pk = r.below(s.T.npk);
        real ux, uy, d2;
        if (!objLane(s, t, pk, ux, uy, d2)) continue;
        const real a = r.uniform(-0.6, 0.6), ca = pcos(a), sa = psin(a), dist = r.uniform(0.12, 0.7);
        const real vx = ux * ca - uy * sa, vy = ux * sa + uy * ca;
        const real x = s.b[t].x - 2 * R * ux - vx * dist, y = s.b[t].y - 2 * R * uy - vy * dist;
        if (tryPlace(g, 0, x, y)) return true;
    }
    return false;
}

inline bool genClearance(GameSnk& g, Rng& r) {
    snkClearBalls(g);
    const bool seq = r.uniform() < 0.65;
    int reds = 0, first = 16;
    if (seq) { const double w6[6] = {3, 2, 2, 2, 1, 0}; first = pickSequenceStart(r, w6); }
    else reds = randInt(r, 1, 3);
    if (!layColours(g, r, first, seq ? 0.35 : 0.8) || !layReds(g, r, reds, 0.3)) return false;
    snkMidFrame(g, seq ? SNK_SEQUENCE : SNK_RED_ON, first);
    bool ok = false;
    if (r.uniform() < 0.55) ok = placeInPosition(g, r, seq ? first : randInt(r, 1, reds));
    if (!ok && !placeAnywhere(g, 0, r)) return false;
    g.turn = r.below(2);
    const int R = snkRemaining(g);
    snkSetScores(g, randInt(r, -(R - 1), R / 2), r);
    return true;
}

inline bool genBlack(GameSnk& g, Rng& r) {
    snkClearBalls(g);
    SnkGeo G; snkGeo(g.sim.T, G);
    const real u = r.uniform();
    if (u < 0.35) {   // re-spotted black after a tie: black on its spot, ball in hand in the D
        if (!tryPlace(g, 21, G.spotX[21], G.spotY[21])) return false;
        snkMidFrame(g, SNK_SEQUENCE, 21);
        real x, y; bool ok = false;
        for (int k = 0; k < 50 && !ok; ++k) { sampleRegion(g, r, x, y); ok = placeValid(g, x, y, 0, true) && tryPlace(g, 0, x, y, POS_GAP, 0); }
        if (!ok) return false;
        g.ballInHand = 1; g.blackGame = 1; g.turn = r.below(2);
        const int s = 40 + r.below(50); g.score[0] = g.score[1] = s;
        return true;
    }
    const int first = u < 0.75 ? 21 : 20;
    if (!layColours(g, r, first, 0.3)) return false;
    snkMidFrame(g, SNK_SEQUENCE, first);
    if (!(r.uniform() < 0.4 && placeInPosition(g, r, first)) && !placeAnywhere(g, 0, r)) return false;
    g.turn = r.below(2);
    snkSetScores(g, first == 21 ? randInt(r, -14, 14) : randInt(r, -20, 14), r);
    return true;
}

// ------------------------------------------------------------------ real positions, perturbed
struct PosPool { std::vector<SnkPosRec> rec; };
// up to maxN positions sampled uniformly (reservoir) from the *.posrec files under dirs
inline size_t loadPosPool(PosPool& P, const std::vector<std::string>& dirs, size_t maxN, u64 seed, int maxFiles = 12) {
    std::vector<std::string> files;
    for (auto& d : dirs) {
        DIR* dp = opendir(d.c_str());
        if (!dp) { if (d.size() > 7 && d.substr(d.size() - 7) == ".posrec") files.push_back(d); continue; }
        while (dirent* e = readdir(dp)) { std::string n = e->d_name; if (n.size() > 7 && n.substr(n.size() - 7) == ".posrec") files.push_back(d + "/" + n); }
        closedir(dp);
    }
    std::sort(files.begin(), files.end());
    Rng r; r.seed(seed * 2654435761ULL + 17);
    for (int i = (int)files.size() - 1; i > 0; --i) { int j = r.below(i + 1); std::swap(files[i], files[j]); }
    if ((int)files.size() > maxFiles) files.resize(maxFiles);
    P.rec.clear(); size_t seen = 0;
    for (auto& f : files) {
        std::vector<SnkRawFrame> fr; readRawFrames(f, fr);
        for (auto& F : fr) for (auto& p : F.pos) {
            ++seen;
            if (P.rec.size() < maxN) P.rec.push_back(p);
            else { size_t j = (size_t)(r.uniform() * seen); if (j < maxN) P.rec[j] = p; }
        }
    }
    return P.rec.size();
}
// push touching/overlapping balls apart and out of the cushions (positions in the records
// are rounded to 0.1 mm, so touching balls can overlap by a few hundredths of a millimetre)
inline void snkSeparate(GameSnk& g) {
    Sim& s = g.sim; const real R = s.P.R, dmin = 2 * R + 2e-5;
    for (int it = 0; it < 60; ++it) {
        bool moved = false;
        for (int i = 0; i < s.n; ++i) {
            Ball& A = s.b[i]; if (!A.on) continue;
            for (int k = 0; k < s.T.nseg; ++k) {
                const Seg& q = s.T.seg[k];
                real d = (A.x - q.ax) * q.nx + (A.y - q.ay) * q.ny, sp = (A.x - q.ax) * q.dx + (A.y - q.ay) * q.dy;
                if (sp < 0 || sp > q.len || d <= -R || d >= R + 1e-5) continue;
                A.x += (R + 1e-5 - d) * q.nx; A.y += (R + 1e-5 - d) * q.ny; moved = true;
            }
            for (int j = i + 1; j < s.n; ++j) {
                Ball& B = s.b[j]; if (!B.on) continue;
                real dx = B.x - A.x, dy = B.y - A.y, d2 = dx * dx + dy * dy;
                if (d2 >= dmin * dmin) continue;
                real d = psqrt(d2); if (d < 1e-9) { dx = 1; dy = 0; d = 1; }
                real push = 0.5 * (dmin - (d2 > 0 ? psqrt(d2) : 0));
                A.x -= dx / d * push; A.y -= dy / d * push; B.x += dx / d * push; B.y += dy / d * push; moved = true;
            }
        }
        if (!moved) break;
    }
}
inline bool genPerturbed(GameSnk& g, Rng& r, const PosPool* pool) {
    if (!pool || pool->rec.empty()) return false;
    const SnkPosRec& rc = pool->rec[(size_t)(r.uniform() * pool->rec.size())];
    snkClearBalls(g);
    for (int i = 0; i < 22; ++i) { resetBall(g.sim.b[i], rc.x[i] * 1e-4, rc.y[i] * 1e-4); g.sim.b[i].on = (rc.onMask >> i) & 1; }
    snkMidFrame(g, rc.phase, rc.nextColour);
    g.score[0] = rc.score0; g.score[1] = rc.score1; g.turn = rc.mover & 1;
    g.ballInHand = rc.flags & 1; g.freeBall = (rc.flags >> 1) & 1;
    if (snkRedsOn(g) == 15 && g.ballInHand && g.score[0] + g.score[1] == 0) return false;   // the break-off itself
    snkSeparate(g);
    const Sim& s = g.sim;
    // move one or two balls: a nudge of a few cm, or anywhere
    if (r.uniform() < 0.5) {
        const int nm = 1 + (r.uniform() < 0.3);
        for (int m = 0; m < nm; ++m) {
            int ids[22], n = 0; for (int i = 0; i < s.n; ++i) if (s.b[i].on) ids[n++] = i;
            const int id = ids[r.below(n)];
            if (id == 0 && g.ballInHand) continue;
            const real ox = s.b[id].x, oy = s.b[id].y;
            bool ok = false;
            if (r.uniform() < 0.3) ok = placeAnywhere(g, id, r);
            else for (int k = 0; k < 40 && !ok; ++k) { real a = r.uniform(-PI, PI), d = r.uniform(0.01, 0.12); ok = tryPlace(g, id, ox + d * pcos(a), oy + d * psin(a), POS_GAP, 0); }
            if (!ok) resetBall(g.sim.b[id], ox, oy);
        }
    }
    if (r.uniform() < 0.3) g.turn = 1 - g.turn;
    if (g.freeBall && (g.ballInHand || !snkSnookered(g, -1))) g.freeBall = 0;
    // new score difference: shifted, or drawn over the whole range that matters
    const int R = snkRemaining(g), D0 = lead(g);
    const int D = r.uniform() < 0.6 ? D0 + randInt(r, -(R / 2 + 25), R / 2 + 25) : randInt(r, -(R + 30), R + 30);
    snkSetScores(g, D, r);
    return true;
}

// ------------------------------------------------------------------ entry point
// Build a starting position of family fam in g (g = a fresh newGameSnk). Returns false (and
// leaves g as it was) if no legal position was found; the caller then plays from the break.
inline bool genSnkStart(GameSnk& g, int fam, Rng& r, const PosPool* pool = 0) {
    if (fam == FAM_BREAK) return true;
    const GameSnk base = g;
    for (int attempt = 0; attempt < 60; ++attempt) {
        g = base;
        bool ok = false;
        switch (fam) {
            case FAM_RANDOM:    randomStartSnk(g, r); ok = true; break;
            case FAM_SNOOKERS:  ok = genScoreContext(g, r, false); break;
            case FAM_LEAD:      ok = genScoreContext(g, r, true); break;
            case FAM_SNOOKERED: ok = genSnookered(g, r, false); break;
            case FAM_FREEBALL:  ok = genSnookered(g, r, true); break;
            case FAM_SAFETY:    ok = genSafety(g, r); break;
            case FAM_CLEARANCE: ok = genClearance(g, r); break;
            case FAM_BLACK:     ok = genBlack(g, r); break;
            case FAM_PERTURBED: ok = genPerturbed(g, r, pool); if (!ok && (!pool || pool->rec.empty())) { g = base; return false; } break;
        }
        if (ok && snkCheck(g).empty()) return true;
    }
    g = base;
    return false;
}

} // namespace train
