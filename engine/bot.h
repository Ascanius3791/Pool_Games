// bot.h — 8-ball bot: geometric shot generation + physics-simulation search,
// with a learned position evaluator (small MLP on position features).
//
// Per turn:
//  1. Candidate pots: for every legal ball x pocket with clear lanes, the
//     ghost-ball aim is refined by simulation (corrects throw / squirt), then
//     expanded into power x spin variants.
//  2. Safety candidates: soft/medium hits on legal balls at several thicknesses.
//  3. Ball in hand: candidate cue placements behind good pots.
//  4. Every candidate is simulated with the real engine and the rules applied;
//     the resulting position is scored by the value function V
//     (= estimated win probability of the player to move).
//  5. Precision variant: the best few are re-evaluated under K noisy executions
//     (same noise model the game uses) and the best mean wins -> the bot
//     prefers robust, lower-power shots automatically.
//  6. Human-like judgement (posSigma > 0): every noisy sample also sees the balls a few mm
//     from where they are, so the bot cannot foresee kisses, cannons and cue-ball paths
//     better than a player who judges the table by eye.
#pragma once
#include "rulesSnk.h"

namespace pool {

// --------------------------------------------------------------- shot execution
// Shared by game and training: noise + miscue + strike.
template <class G>
inline int executeShot(G& g, ShotParams sp, bool noisy, const NoiseModel& nm, Rng& rng, ShotParams* executed = 0) {
    if (noisy) applyNoise(nm, g.sim.P, rng, sp);
    real off = psqrt(sp.a * sp.a + sp.b * sp.b);
    int miscue = off > g.sim.P.maxOffset ? 1 : 0;
    if (miscue) {
        sp.phi += rng.gauss() * 0.15;
        sp.V *= 0.25 + 0.2 * rng.uniform();
        sp.a *= 0.3; sp.b *= 0.3;
    }
    strike(g.sim, sp.phi, sp.V, sp.a, sp.b);
    g.ballInHand = 0;
    if (executed) *executed = sp;
    return miscue;
}

// --------------------------------------------------------------- MLP
const int NFEAT = 16;
const int H1 = 24, H2 = 12;
const int NWEIGHTS = H1 * NFEAT + H1 + H2 * H1 + H2 + H2 + 1;
inline real ptanh(real x) { if (x > 15) return 1; if (x < -15) return -1; real e = pexp(2 * x); return (e - 1) / (e + 1); }
inline real psigmoid(real x) { if (x < -30) return 0; if (x > 30) return 1; return 1 / (1 + pexp(-x)); }
inline real mlpForward(const float* w, const real* x, real* h1out = 0, real* h2out = 0) {
    const float* W1 = w; const float* b1 = W1 + H1 * NFEAT;
    const float* W2 = b1 + H1; const float* b2 = W2 + H2 * H1;
    const float* W3 = b2 + H2; const float* b3 = W3 + H2;
    real h1[H1], h2[H2];
    for (int i = 0; i < H1; ++i) { real s = b1[i]; for (int j = 0; j < NFEAT; ++j) s += W1[i * NFEAT + j] * x[j]; h1[i] = ptanh(s); }
    for (int i = 0; i < H2; ++i) { real s = b2[i]; for (int j = 0; j < H1; ++j) s += W2[i * H1 + j] * h1[j]; h2[i] = ptanh(s); }
    real z = b3[0]; for (int j = 0; j < H2; ++j) z += W3[j] * h2[j];
    if (h1out) for (int i = 0; i < H1; ++i) h1out[i] = h1[i];
    if (h2out) for (int i = 0; i < H2; ++i) h2out[i] = h2[i];
    return z;   // logit
}

// --------------------------------------------------------------- geometry
inline bool legalTarget(const Game8& g, int p, int id) {
    if (!g.sim.b[id].on || id == 0) return false;
    int grp = g.group[p];
    if (grp == GROUP_NONE) return id != 8;
    if (groupRemaining(g, grp) == 0) return id == 8;
    return ballGroup(id) == grp;
}

inline bool laneClear(const Sim& s, real x0, real y0, real x1, real y1, real clr, int skipA, int skipB) {
    real dx = x1 - x0, dy = y1 - y0, L2 = dx * dx + dy * dy;
    // balls outside the lane's bounding box (grown by clr) cannot block it
    const real bx0 = pmin(x0, x1) - clr, bx1 = pmax(x0, x1) + clr, by0 = pmin(y0, y1) - clr, by1 = pmax(y0, y1) + clr;
    const real clr2 = clr * clr;
    for (int i = 0; i < s.n; ++i) {
        const Ball& b = s.b[i];
        if (b.x < bx0 || b.x > bx1 || b.y < by0 || b.y > by1) continue;
        if (i == skipA || i == skipB || !b.on) continue;
        real px = b.x - x0, py = b.y - y0;
        real dot = px * dx + py * dy, t;
        if (!(L2 > 0) || dot <= 0) t = 0; else if (dot >= L2) t = 1; else t = dot / L2;
        real ex = px - t * dx, ey = py - t * dy;
        if (ex * ex + ey * ey < clr2) return false;
    }
    return true;
}

inline void pocketTarget(const Sim& s, int k, real& px, real& py) {
    const Pocket& p = s.T.pk[k];
    px = p.mx + p.nx * 0.5 * s.P.R; py = p.my + p.ny * 0.5 * s.P.R;
}

// Can ball t be potted in pocket k (object lane clear, entry angle ok)? Returns
// unit direction u and distance d2.
inline bool objLane(const Sim& s, int t, int k, real& ux, real& uy, real& d2) {
    real px, py; pocketTarget(s, k, px, py);
    const Ball& B = s.b[t];
    ux = px - B.x; uy = py - B.y; d2 = psqrt(ux * ux + uy * uy);
    if (d2 < 1e-6) return false;
    ux /= d2; uy /= d2;
    const Pocket& P = s.T.pk[k];
    real cosEntry = ux * P.nx + uy * P.ny;
    bool side = (k >= 4);
    if (cosEntry < (side ? 0.55 : 0.40)) return false;
    return laneClear(s, B.x, B.y, px, py, 2 * s.P.R * 0.98, t, -1);
}

// Geometric ease in [0,1] of potting t in k from cue position (cx,cy). 0 = impossible.
// threshold variant: if the geometric ease cannot exceed `need`, return it without the two
// lane checks (callers that only keep values above `need` get identical results, cheaper)
inline real shotEaseAbove(const Sim& s, real cx, real cy, int t, int k, real need, real* phiOut = 0);
inline real shotEase(const Sim& s, real cx, real cy, int t, int k, real* phiOut = 0) { return shotEaseAbove(s, cx, cy, t, k, -1, phiOut); }
inline real shotEaseAbove(const Sim& s, real cx, real cy, int t, int k, real need, real* phiOut) {
    // cheap geometry first (entry angle, cut angle), the two lane checks last
    real px, py; pocketTarget(s, k, px, py);
    const Ball& B = s.b[t];
    real ux = px - B.x, uy = py - B.y, d2 = psqrt(ux * ux + uy * uy);
    if (d2 < 1e-6) return 0;
    ux /= d2; uy /= d2;
    const Pocket& PK = s.T.pk[k];
    if (ux * PK.nx + uy * PK.ny < (k >= 4 ? 0.55 : 0.40)) return 0;
    const real R = s.P.R;
    real gx = B.x - 2 * R * ux, gy = B.y - 2 * R * uy;
    real vx = gx - cx, vy = gy - cy, d1 = psqrt(vx * vx + vy * vy);
    if (d1 < 1e-6) return 0;
    vx /= d1; vy /= d1;
    real cth = vx * ux + vy * uy;
    if (cth < 0.20) return 0;                                   // > ~78 deg cut
    real e = cth * cth * pexp(-0.45 * d1 - 0.75 * d2);
    if (k >= 4) e *= 0.8;
    if (e <= need) return e;                                    // caller ignores it anyway
    if (!laneClear(s, B.x, B.y, px, py, 2 * R * 0.98, t, -1)) return 0;
    if (!laneClear(s, cx, cy, gx, gy, 2 * R * 0.98, 0, t)) return 0;
    if (phiOut) *phiOut = patan2(vy, vx);
    return e;
}

// --------------------------------------------------------------- features
inline void features(const Game8& g, int p, int bih, real* f) {
    const Sim& s = g.sim;
    int op = 1 - p;
    int remS = groupRemaining(g, GROUP_SOLIDS), remT = groupRemaining(g, GROUP_STRIPES);
    bool open = g.group[p] == GROUP_NONE;
    real myRem = open ? 0.5 * (remS + remT) : groupRemaining(g, g.group[p]);
    real opRem = open ? 0.5 * (remS + remT) : groupRemaining(g, g.group[op]);
    bool onMe = !open && myRem == 0, onOp = !open && opRem == 0;
    real e[3] = {0, 0, 0}; int nEasy = 0;
    int myPot = 0, opPot = 0;
    bool counted[16] = {false};
    for (int t = 1; t < 16; ++t) {
        if (!s.b[t].on) continue;
        bool mine = legalTarget(g, p, t);
        bool theirs = legalTarget(g, op, t);
        if (!mine && !theirs) continue;
        for (int k = 0; k < s.T.npk; ++k) {
            real ux, uy, d2;
            if (!objLane(s, t, k, ux, uy, d2)) continue;
            if (mine && !counted[t]) { myPot++; counted[t] = true; }
            if (theirs && !(mine)) { /* counted below */ }
            real ease;
            if (mine) {
                if (bih) ease = pexp(-0.15 - 0.75 * d2) * (k >= 4 ? 0.8 : 1);
                else ease = shotEase(s, s.b[0].x, s.b[0].y, t, k);
                if (ease > 0.25) nEasy++;
                if (ease > e[2]) {
                    if (ease > e[0]) { e[2] = e[1]; e[1] = e[0]; e[0] = ease; }
                    else if (ease > e[1]) { e[2] = e[1]; e[1] = ease; }
                    else e[2] = ease;
                }
            }
        }
    }
    for (int t = 1; t < 16; ++t) {
        if (!legalTarget(g, op, t)) continue;
        for (int k = 0; k < s.T.npk; ++k) { real ux, uy, d2; if (objLane(s, t, k, ux, uy, d2)) { opPot++; break; } }
    }
    bool eightClear = false;
    if (s.b[8].on) for (int k = 0; k < s.T.npk; ++k) { real ux, uy, d2; if (objLane(s, 8, k, ux, uy, d2)) { eightClear = true; break; } }
    f[0] = myRem / 7; f[1] = opRem / 7; f[2] = open; f[3] = onMe; f[4] = onOp; f[5] = bih;
    f[6] = e[0]; f[7] = e[1]; f[8] = e[2]; f[9] = nEasy / 5.0;
    f[10] = myPot / 7.0; f[11] = opPot / 7.0; f[12] = eightClear; f[13] = (opRem - myRem) / 7;
    f[14] = (myRem > 0 ? (real)myPot / myRem : 1); f[15] = (opRem > 0 ? (real)opPot / opRem : 1);
}

inline real heuristicLogit(const real* f) {
    return 2.6 * f[13] + 2.2 * f[6] + 0.8 * f[7] + 0.4 * f[8] + 0.7 * f[5] + 1.0 * f[3] - 1.0 * f[4]
         + 0.6 * f[14] - 0.4 * f[15] - 0.25;
}

// --------------------------------------------------------------- bot
// Two-phase search, generic over the game (Game8 / GameSnk).
//
// Budget: a time-independent number of shot simulations per decision. A full
// simulation (until everything stops) costs 1; a root-finding probe, which only
// simulates until the cue ball's first contact, costs 1/4.
//
// Phase 1 (breadth): every legal ball x pocket gets its aim from a 1-D
// root-finding problem: f(phi) = launch direction of the object ball minus the
// direction to the pocket target, solved by Newton/secant on short partial
// simulations (this absorbs throw and squirt). One default variant per pot, a
// set of safeties, then power/spin variants of the most promising pots.
//
// Phase 2 (depth, best-first): repeatedly take the candidate with the highest
// stored value and refine it: re-centre its aim for its own power/spin (secant,
// warm-started with the stored slope), then pattern search over power, spin and
// side (safeties: direction and power), aim re-centred at every trial point.
// In the precision variant every evaluation is the mean over K noisy executions
// with common random numbers. When a refined candidate's value falls below the
// stored value of another one, the next iteration automatically switches to it.

// Snooker evaluator. Positional terms give E = expected points of the coming visit for
// the player to move; E shifts the score difference, and a win table
// W(D, R) = P(player to move wins | score difference D, points remaining R)
// (from the abstract frame simulator, train/abstract_snk.cpp) turns it into a win chance.
// Without a table the bot falls back to the old points-based evaluation.
const int SNK_NF = 8;   // record features: D, R, e1, v1, cont, e2, snookered, bih
struct SnkEval {
    real a0, a1, a2, a3, a4;      // E = a0 + a1*e1*(v1+cont) + a2*e2 + a3*snookered + a4*bih
    real eScale;                  // effective D = D + eScale * E
    const unsigned char* table;   // [tNR][tND] uint8 win probabilities * 255
    int tD0, tND, tNR;            // D index = D + tD0
    const float* net;             // neural evaluator weights (SNK_NET_NW floats, 0 = linear E)
    real skill;                   // human-error level the net judges for (0 = none, 1 = club)
};
inline void snkEvalDefaults(SnkEval& e) {
    e.a0 = 0; e.a1 = 1; e.a2 = 0.5; e.a3 = -3; e.a4 = 0; e.eScale = 1;
    e.table = 0; e.tD0 = 160; e.tND = 321; e.tNR = 148;
    e.net = 0; e.skill = 1;
}
inline real snkWin(const SnkEval& ev, real D, int R) {
    if (R < 0) R = 0; if (R >= ev.tNR) R = ev.tNR - 1;
    real x = D + ev.tD0;
    if (x <= 0) return ev.table[R * ev.tND] / 255.0;
    if (x >= ev.tND - 1) return ev.table[R * ev.tND + ev.tND - 1] / 255.0;
    int i = (int)pfloor(x); real f = x - i;
    const unsigned char* row = ev.table + R * ev.tND;
    return ((1 - f) * row[i] + f * row[i + 1]) / 255.0;
}

// an escape from a snooker: aim phi (best centre of the hitting window, step dphi), chance p
struct SnkEscFam { real p, phi, dphi; int type, a, b, tgt; };
struct BotCtx {
    const float* nn;     // NN weights for the 8-ball evaluator (0 = heuristic)
    const SnkEval* snk;  // snooker evaluator (0 = legacy points evaluation)
    bool noisy;
    NoiseModel nm;
    Rng rng;
    u64 crnSeed;
    real budget, used;
    int K;
    bool phase2;
    int nPow; real pows[6];
    int nSpin; real spins[4];
    int nSafe;
    // snooker miss rule as the referee applies it (mode 0 judged by profile with threshold tau, 1 pro rules)
    int refMode; real refTau;
    int mxDirect; real mxPStar;   // per decision: clear sight of a ball on (-1 unknown), escape chance p* (-1 not judged)
    real rootW;                   // per decision: win chance of the position as it stands (-1: points evaluator)
    SnkEscFam esc[6]; int nEsc;   // escape windows when snookered
    bool foulGuard;               // a deliberate foul into a miss is never preferred to a real attempt (off: the evaluator alone decides)
    real posSigma;                // human-like judgement: sd [m] of the ball positions in each noisy sample (0 = exact)
    // statistics of the last decision
    int nFull, nPartial, nSecant, nRefine, nSwitch, nCand;
    real phase1Used;
};

inline void botDefaults(BotCtx& c, bool noisy, const float* nn, u64 seed) {
    c.nn = nn; c.snk = 0; c.noisy = noisy; defaultNoise(c.nm); c.rng.seed(seed); c.crnSeed = seed * 2654435761ULL + 1;
    c.budget = noisy ? 900 : 400; c.used = 0;
    c.K = 8; c.phase2 = true;
    c.nPow = 4; c.pows[0] = 0.22; c.pows[1] = 0.34; c.pows[2] = 0.48; c.pows[3] = 0.66;
    c.nSpin = 3; c.spins[0] = -0.35; c.spins[1] = 0.0; c.spins[2] = 0.3;
    c.nSafe = 20;
    c.refMode = 0; c.refTau = 0.5; c.mxDirect = -1; c.mxPStar = -1; c.nEsc = 0; c.rootW = -1; c.foulGuard = true;
    c.posSigma = 0;
}
inline void botLight(BotCtx& c) { c.budget = c.noisy ? 300 : 120; c.K = 5; c.nSafe = 8; }

// ------------------------------------------------ game adapters: 8-ball
inline real valueOf(const BotCtx& c, const Game8& g, int p, int bih) {
    real f[NFEAT]; features(g, p, bih, f);
    real z = c.nn ? mlpForward(c.nn, f) : heuristicLogit(f);
    return psigmoid(z);
}
inline real valueAfter(BotCtx& c, const Game8& before, Game8& a, const ShotParams*) {
    const int me = before.turn;
    endShot8(a);
    if (a.winner == me) return 1;
    if (a.winner >= 0) return 0;
    real v = valueOf(c, a, a.turn, a.ballInHand);
    return a.turn == me ? v : 1 - v;
}
inline real outcomeValue(const BotCtx& c, Game8& a, int me) {   // kept for compatibility
    endShot8(a);
    if (a.winner == me) return 1;
    if (a.winner >= 0) return 0;
    real v = valueOf(c, a, a.turn, a.ballInHand);
    return a.turn == me ? v : 1 - v;
}
inline bool legalT(const Game8& g, int id) { return legalTarget(g, g.turn, id); }
inline bool placeOk(const Game8& g, real x, real y) { return placeValid(g, x, y, 0, g.kitchenOnly != 0); }
inline bool regionLimited(const Game8& g) { return g.kitchenOnly != 0; }
inline void sampleRegion(const Game8& g, Rng& r, real& x, real& y) {
    const Table& T = g.sim.T; const real R = g.sim.P.R;
    real x0 = -T.L / 2 + R, x1 = g.kitchenOnly ? headStringX(T) : T.L / 2 - R;
    x = r.uniform(x0, x1); y = r.uniform(-T.W / 2 + R, T.W / 2 - R);
}

// ------------------------------------------------ game adapters: snooker
inline bool legalT(const GameSnk& g, int id) { return snkOn(g, id); }
inline bool placeOk(const GameSnk& g, real x, real y) { return placeValid(g, x, y, 0, true); }
inline bool regionLimited(const GameSnk&) { return true; }
inline void sampleRegion(const GameSnk& g, Rng& r, real& x, real& y) {
    SnkGeo G; snkGeo(g.sim.T, G);
    real a = r.uniform(PI / 2, 3 * PI / 2), rr = G.dR * psqrt(r.uniform());
    x = G.baulkX + rr * pcos(a); y = rr * psin(a);
}
// can the player to move hit some ball on directly (centre or either edge line)?
inline bool snkCanHit(const GameSnk& g) {
    const Sim& s = g.sim; const real R = s.P.R;
    const Ball& c = s.b[0];
    for (int id = 1; id < s.n; ++id) {
        if (!snkOn(g, id)) continue;
        real dx = s.b[id].x - c.x, dy = s.b[id].y - c.y, d = psqrt(dx * dx + dy * dy);
        if (d < 1e-9) return true;
        real nx = -dy / d, ny = dx / d;
        for (int k = -1; k <= 1; ++k) {
            real ox = nx * k * 1.8 * R, oy = ny * k * 1.8 * R;
            if (laneClear(s, c.x, c.y, s.b[id].x + ox, s.b[id].y + oy, 2 * R * 0.98, 0, id)) return true;
        }
    }
    return false;
}
// rough expected points of the coming visit for the player to move
// position features for the player to move: f = {D, R, e1, v1, cont, e2, snookered, bih}
inline void snkFeatures(const GameSnk& g, int bih, real* f) {
    const Sim& s = g.sim;
    real e1 = 0, e2 = 0; int v1 = 1;
    for (int t = 1; t < s.n; ++t) {
        if (!snkOn(g, t)) continue;
        for (int k = 0; k < s.T.npk; ++k) {
            real e;
            if (bih) { real ux, uy, d2; e = objLane(s, t, k, ux, uy, d2) ? pexp(-0.15 - 0.6 * d2) : 0; }
            else {   // only values that change the top two matter: skip lane checks otherwise
                int vv = snkValue(t);
                real need = pmin(e2, e1 * (v1 + 3) / (vv + 3));
                e = shotEaseAbove(s, s.b[0].x, s.b[0].y, t, k, need);
                if (e <= need) e = 0;
            }
            int v = snkValue(t);
            if (e * (v + 3) > e1 * (v1 + 3)) { e2 = e1; e1 = e; v1 = v; } else if (e > e2) e2 = e;
        }
    }
    int rem = snkRemaining(g);
    f[0] = g.score[g.turn] - g.score[1 - g.turn]; f[1] = rem;
    f[2] = e1; f[3] = v1; f[4] = rem > 12 ? 6.0 : rem * 0.5; f[5] = e2;
    f[6] = (!bih && !snkCanHit(g)) ? 1 : 0; f[7] = bih ? 1 : 0;
}
inline real snkVisitP(const SnkEval& ev, const real* f) {
    return ev.a0 + ev.a1 * f[2] * (f[3] + f[4]) + ev.a2 * f[5] + ev.a3 * f[6] + ev.a4 * f[7];
}

// Neural snooker evaluator (trained by train/snknet.py on the self-play positions).
//   P(win) = sigmoid(gamma * logit W(D + dOff + E, R)),  E = E_lin + e,  gamma = exp(g)
// E_lin is the linear evaluator above (its 6 parameters are stored with the net), (e, g) come
// from a small network: every object ball is encoded by a shared layer, the codes are pooled
// (sum and max, reds and colours apart) and a head combines them with the global inputs.
// The table keeps the score context; the net learns the positional points and, through
// gamma, how decisive a lead is at the given human-error level. With e = g = 0 it is exactly
// the linear evaluator, which is how training starts.
// Inputs are mirrored so the cue ball is always at y >= 0 (the table is symmetric in y).
const int SNK_NB = 12;            // per-ball inputs
const int SNK_NG = 23;            // global inputs
const int SNK_HE = 24, SNK_H1 = 48, SNK_H2 = 24;
const int SNK_NP = 4 * SNK_HE + SNK_NG;   // head input: red sum, red max, colour sum, colour max, globals
// weight layout (input-major, so the inner loops run over outputs and vectorise without
// reordering any sum): We[NB][HE] be[HE] W1[NP][H1] b1[H1] W2[H1][H2] b2[H2] W3[H2][2] b3[2] lin[6]
const int SNK_NET_NW = SNK_HE * SNK_NB + SNK_HE + SNK_H1 * SNK_NP + SNK_H1 + SNK_H2 * SNK_H1 + SNK_H2 + 2 * SNK_H2 + 2 + 6;

// max over the pockets with an open object lane (objLane) of exp(-0.75 d) (x 0.8 for middle
// pockets): pockets are tried from the best value down, the first open one is the answer
inline real snkPocketLane(const Sim& s, int t) {
    const Ball& B = s.b[t];
    real key[6], dd[6]; int ord[6], n = 0;
    for (int k = 0; k < s.T.npk; ++k) {
        real px, py; pocketTarget(s, k, px, py);
        real ux = px - B.x, uy = py - B.y, d = psqrt(ux * ux + uy * uy);
        if (d < 1e-6) continue;
        const Pocket& P = s.T.pk[k];
        if ((ux * P.nx + uy * P.ny) / d < (k >= 4 ? 0.55 : 0.40)) continue;
        real kk = d + (k >= 4 ? 0.29752 : 0);   // -ln(0.8) / 0.75
        int i = n++;
        while (i > 0 && key[i - 1] > kk) { key[i] = key[i - 1]; ord[i] = ord[i - 1]; dd[i] = dd[i - 1]; --i; }
        key[i] = kk; ord[i] = k; dd[i] = d;
    }
    for (int i = 0; i < n; ++i) {
        real px, py; pocketTarget(s, ord[i], px, py);
        if (laneClear(s, B.x, B.y, px, py, 2 * s.P.R * 0.98, t, -1)) return pexp(-0.75 * dd[i]) * (ord[i] >= 4 ? 0.8 : 1);
    }
    return 0;
}
// Inputs of position g for the player to move: ball rows bf[21][NB] (ball id 1..21, zeros if
// off the table), globals gf[NG] and the 8 record features f (snkFeatures, same values).
inline void snkNetInputs(const GameSnk& g, int bih, real skill, float* bf, float* gf, real* f) {
    const Sim& s = g.sim; const real R = s.P.R;
    const real hx = s.T.L / 2, hy = s.T.W / 2;
    const Ball& c = s.b[0];
    const real ys = c.y < 0 ? -1 : 1;
    // shot eases from the cue ball: the same top-two bookkeeping as snkFeatures, and the best
    // ease per ball. Lane checks only for eases that could change one of them (identical results)
    real e1 = 0, e2 = 0; int v1 = 1;
    for (int t = 1; t < 22; ++t) {
        float* b = bf + (t - 1) * SNK_NB;
        for (int j = 0; j < SNK_NB; ++j) b[j] = 0;
        if (t >= s.n || !s.b[t].on) continue;
        const Ball& B = s.b[t];
        const bool on = snkOn(g, t);
        real best = 0, lane = 0;
        if (!(on && bih)) lane = snkPocketLane(s, t);
        for (int k = 0; on && k < s.T.npk; ++k) {
            int v = snkValue(t);
            real e;
            if (bih) {
                real ux, uy, d2;
                const bool open = objLane(s, t, k, ux, uy, d2);
                if (open) lane = pmax(lane, pexp(-0.75 * d2) * (k >= 4 ? 0.8 : 1));
                e = open ? pexp(-0.15 - 0.6 * d2) : 0;
            } else {
                real need = pmin(best, pmin(e2, e1 * (v1 + 3) / (v + 3)));
                e = shotEaseAbove(s, c.x, c.y, t, k, need);
                if (e <= need) e = 0;
            }
            best = pmax(best, e);
            if (e * (v + 3) > e1 * (v1 + 3)) { e2 = e1; e1 = e; v1 = v; } else if (e > e2) e2 = e;
        }
        real dx = B.x - c.x, dy = B.y - c.y;
        b[0] = 1; b[1] = B.x / hx; b[2] = ys * B.y / hy;
        b[3] = psqrt(dx * dx + dy * dy) / s.T.L;
        b[4] = on ? 1 : 0; b[5] = snkIsRed(t) ? 1 : 0; b[6] = snkIsRed(t) ? 0 : snkValue(t) / 7.0;
        b[7] = best; b[8] = lane;
        b[9] = laneClear(s, c.x, c.y, B.x, B.y, 2 * R * 0.98, 0, t) ? 1 : 0;
        b[10] = pmin(1, pmin(hx - pabs(B.x), hy - pabs(B.y)) / 0.2);
        int near = 0;
        for (int j = 1; j < s.n; ++j) if (j != t && s.b[j].on) { real ex = s.b[j].x - B.x, ey = s.b[j].y - B.y; if (ex * ex + ey * ey < 16 * R * R) near++; }
        b[11] = near / 4.0;
    }
    const int rem = snkRemaining(g);
    f[0] = g.score[g.turn] - g.score[1 - g.turn]; f[1] = rem;
    f[2] = e1; f[3] = v1; f[4] = rem > 12 ? 6.0 : rem * 0.5; f[5] = e2;
    f[6] = (!bih && !snkCanHit(g)) ? 1 : 0; f[7] = bih ? 1 : 0;
    const real D = f[0];
    gf[0] = D / 40; gf[1] = rem / 147.0; gf[2] = pclamp(D / (rem + 1), -2, 2) / 2;
    gf[3] = -D > rem ? 1 : 0; gf[4] = D > rem ? 1 : 0;
    gf[5] = g.phase == SNK_RED_ON; gf[6] = g.phase == SNK_COLOUR_ON; gf[7] = g.phase == SNK_SEQUENCE;
    gf[8] = g.phase == SNK_SEQUENCE ? snkValue(g.nextColour) / 7.0 : 0;
    gf[9] = bih ? 1 : 0; gf[10] = g.freeBall ? 1 : 0;
    gf[11] = skill / 2.5; gf[12] = skill <= 0 ? 1 : 0;
    gf[13] = snkRedsOn(g) / 15.0;
    gf[14] = c.x / hx; gf[15] = ys * c.y / hy; gf[16] = pmin(1, pmin(hx - pabs(c.x), hy - pabs(c.y)) / 0.2);
    gf[17] = f[2]; gf[18] = f[3] / 7; gf[19] = f[4] / 6; gf[20] = f[5]; gf[21] = f[6];
    gf[22] = f[2] * (f[3] + f[4]) / 13;
}
// dense layer y = relu?(b + x W), W input-major [n][m]
inline void snkDense(const float* W, const float* b, const float* x, int n, int m, float* y, bool relu) {
    for (int i = 0; i < m; ++i) y[i] = b[i];
    for (int j = 0; j < n; ++j) { const float xj = x[j]; if (xj == 0) continue; const float* Wj = W + j * m; for (int i = 0; i < m; ++i) y[i] += xj * Wj[i]; }
    if (relu) for (int i = 0; i < m; ++i) y[i] = y[i] > 0 ? y[i] : 0;
}
// network part: (e, g) from the inputs
inline void snkNetForward(const float* w, const float* bf, const float* gf, real& eOut, real& gOut) {
    const float* We = w; const float* be = We + SNK_HE * SNK_NB;
    const float* W1 = be + SNK_HE; const float* b1 = W1 + SNK_H1 * SNK_NP;
    const float* W2 = b1 + SNK_H1; const float* b2 = W2 + SNK_H2 * SNK_H1;
    const float* W3 = b2 + SNK_H2; const float* b3 = W3 + 2 * SNK_H2;
    float x[SNK_NP], h[SNK_HE];
    for (int i = 0; i < 4 * SNK_HE; ++i) x[i] = 0;
    for (int t = 0; t < 21; ++t) {
        const float* b = bf + t * SNK_NB;
        if (b[0] == 0) continue;
        snkDense(We, be, b, SNK_NB, SNK_HE, h, true);
        float* pool = x + (t < 15 ? 0 : 2 * SNK_HE);
        const float sc = t < 15 ? 0.125f : 0.25f;
        for (int i = 0; i < SNK_HE; ++i) { pool[i] += h[i] * sc; pool[SNK_HE + i] = h[i] > pool[SNK_HE + i] ? h[i] : pool[SNK_HE + i]; }
    }
    for (int j = 0; j < SNK_NG; ++j) x[4 * SNK_HE + j] = gf[j];
    float h1[SNK_H1], h2[SNK_H2], o[2];
    snkDense(W1, b1, x, SNK_NP, SNK_H1, h1, true);
    snkDense(W2, b2, h1, SNK_H1, SNK_H2, h2, true);
    snkDense(W3, b3, h2, SNK_H2, 2, o, false);
    eOut = o[0]; gOut = o[1];
}
// win chance from the table value p and the net outputs
inline real snkNetCombine(real p, real gOut) {
    p = pclamp(p, 0.5 / 255, 254.5 / 255);
    real z = plog(p / (1 - p)) * pexp(pclamp(gOut, -3, 3));
    return psigmoid(z);
}
inline real snkNetWin(const SnkEval& ev, const GameSnk& g, int bih, real dOff) {
    float bf[21 * SNK_NB], gf[SNK_NG]; real f[SNK_NF], e, gg;
    snkNetInputs(g, bih, ev.skill, bf, gf, f);
    snkNetForward(ev.net, bf, gf, e, gg);
    const float* lin = ev.net + SNK_NET_NW - 6;
    real E = lin[5] * (lin[0] + lin[1] * f[2] * (f[3] + f[4]) + lin[2] * f[5] + lin[3] * f[6] + lin[4] * f[7]) + e;
    return snkNetCombine(snkWin(ev, f[0] + dOff + E, (int)f[1]), gg);
}
// human-error level of a noise profile for the net: geometric mean of the channel sigmas at a
// medium stroke relative to the default profile (a default profile scaled by k gives k)
inline real snkSkillOf(const NoiseModel& nm, bool noisy) {
    if (!noisy) return 0;
    NoiseModel d; defaultNoise(d);
    real l = 0;
    for (int ch = 0; ch < 4; ++ch) l += plog(pmax(1e-6, noiseSigma(nm, ch, 0.35)) / noiseSigma(d, ch, 0.35));
    real k = pexp(l / 4) * (1 + 0.1 * nm.missProb * (nm.missMul - 1));
    return pclamp(k, 0, 3);
}

// win chance of the player to move in g (dOff: extra points credited to the player to move)
inline real snkWinNow(const SnkEval& ev, const GameSnk& g, int bih, real dOff = 0) {
    if (ev.net) return snkNetWin(ev, g, bih, dOff);
    real f[SNK_NF]; snkFeatures(g, bih, f);
    return snkWin(ev, f[0] + dOff + ev.eScale * snkVisitP(ev, f), (int)f[1]);
}
inline real snkVisit(const GameSnk& g, int bih) {
    const Sim& s = g.sim;
    real e1 = 0, e2 = 0; int v1 = 1;
    for (int t = 1; t < s.n; ++t) {
        if (!snkOn(g, t)) continue;
        for (int k = 0; k < s.T.npk; ++k) {
            real e;
            if (bih) { real ux, uy, d2; e = objLane(s, t, k, ux, uy, d2) ? pexp(-0.15 - 0.6 * d2) : 0; }
            else {   // only values that change the top two matter: skip lane checks otherwise
                int vv = snkValue(t);
                real need = pmin(e2, e1 * (v1 + 3) / (vv + 3));
                e = shotEaseAbove(s, s.b[0].x, s.b[0].y, t, k, need);
                if (e <= need) e = 0;
            }
            int v = snkValue(t);
            if (e * (v + 3) > e1 * (v1 + 3)) { e2 = e1; e1 = e; v1 = v; } else if (e > e2) e2 = e;
        }
    }
    int rem = snkRemaining(g);
    real cont = rem > 12 ? 6.0 : rem * 0.5;
    real E = e1 * (v1 + cont) + 0.5 * e2;
    if (!bih && !snkCanHit(g)) E -= 3.0;       // snookered: a foul (4+) is likely
    return E;
}
// ================================================================ snooker referee (miss rule)
// p* = chance that a player with this noise profile hits a ball on with the best
// available escape (direct, one cushion, two cushions), found by simulation.
enum { MISS_NONE = 0, MISS_DIRECT = 1, MISS_EXPECTED = 2, MISS_EFFORT = 3, MISS_PRO = 4, NOMISS_SNOOKERS = 5, NOMISS_HARD = 6 };
struct RefInfo { int checked, miss, reason, direct, forfeit, escType, escR1, escR2; real pStar, pPlayed; };

// does this stroke make first contact with a ball on? (simulates only until first contact)
inline int snkFirstOn(const GameSnk& pre, const ShotParams& sp, const NoiseModel* nm, Rng* rng) {
    Sim s = pre.sim; s.trackOrient = 0;
    ShotParams q = sp;
    if (nm) applyNoise(*nm, s.P, *rng, q);
    strike(s, q.phi, q.V, q.a, q.b);
    simulateFirstHit(s, 12);
    int fh = s.ev.firstHit;
    return (fh > 0 && snkOn(pre, fh)) ? 1 : 0;
}
inline real snkHitChance(const GameSnk& pre, const ShotParams& sp, const NoiseModel& nm, u64 seed, int K) {
    int hits = 0;
    for (int k = 0; k < K; ++k) { Rng r; r.seed(seed + 104729ULL * (u64)(k + 1)); hits += snkFirstOn(pre, sp, &nm, &r); }
    return (real)hits / K;
}
// Escape families from a snooker: straight, one cushion, two cushions (aimed at mirror images
// of the ball on). Each family's hitting window is scanned with partial simulations at speed V.
// p = chance that a Gaussian aim error (the profile's sigma at that speed, at least minStep scan
// steps, with the profile's aim tendency) lands in the window when aiming at phi. A floor on the
// sigma centres the aim in the window for steady cueing.
inline void snkMirror(const Sim& s, real& x, real& y, int r) {
    const real hx = s.T.L / 2 - s.P.R, hy = s.T.W / 2 - s.P.R;
    if (r == 0) x = 2 * hx - x; else if (r == 1) x = -2 * hx - x; else if (r == 2) y = 2 * hy - y; else y = -2 * hy - y;
}
// one family (type 0 straight, 1 cushion ra, 2 cushions ra then rb); returns 0 if nothing hits
inline int snkEscWindow(const GameSnk& pre, const NoiseModel& nm, real V, real minStep, int tgt, int type, int ra, int rb, SnkEscFam& out, int& nPartial) {
    const Sim& s = pre.sim; const real R = s.P.R;
    const Ball& c = s.b[0]; const Ball& T = s.b[tgt];
    real ix = T.x, iy = T.y;
    if (type == 1) snkMirror(s, ix, iy, ra);
    else if (type == 2) { snkMirror(s, ix, iy, rb); snkMirror(s, ix, iy, ra); }
    real dx = ix - c.x, dy = iy - c.y, dist = psqrt(dx * dx + dy * dy);
    if (dist < 1e-6) return 0;
    real phiC = patan2(dy, dx), w = 2 * R / dist;
    const int G = 9;
    real phis[G]; int hit[G]; int any = 0;
    ShotParams sp; sp.V = V; sp.a = 0; sp.b = 0;
    for (int k = 0; k < G; ++k) {
        phis[k] = phiC + (k - (G - 1) / 2) * w * 1.6 / ((G - 1) / 2);
        sp.phi = phis[k];
        hit[k] = snkFirstOn(pre, sp, 0, 0); any += hit[k];
    }
    nPartial += G;
    if (!any) return 0;
    const real dphi = phis[1] - phis[0];
    real sig = noiseSigma(nm, 0, V / s.P.vmax);
    if (sig < minStep * dphi) sig = minStep * dphi;
    if (sig < 1e-9) sig = 1e-9;
    real bp = 0, bphi = phis[0];
    for (int k0 = 0; k0 < G; ++k0) {
        real P = 0;
        for (int k = 0; k < G; ++k) if (hit[k]) {
            real z = (phis[k] - (phis[k0] - nm.mu[0])) / sig;
            P += pexp(-0.5 * z * z);
        }
        P *= dphi / (2.5066282746310002 * sig);
        if (P > 1) P = 1;
        if (P > bp) { bp = P; bphi = phis[k0]; }
    }
    out.p = bp; out.phi = bphi; out.dphi = dphi; out.type = type; out.a = ra; out.b = rb; out.tgt = tgt;
    return 1;
}
// the best maxF families over the nearest nT balls on (17 families each)
inline int snkEscapeScan(const GameSnk& pre, const NoiseModel& nm, real V, real minStep, int nT, SnkEscFam* best, int maxF, int& nPartial) {
    const Sim& s = pre.sim; const Ball& c = s.b[0];
    int ids[22], n = 0;
    for (int t = 1; t < s.n; ++t) if (snkOn(pre, t)) ids[n++] = t;
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) {
        real di = (s.b[ids[i]].x - c.x) * (s.b[ids[i]].x - c.x) + (s.b[ids[i]].y - c.y) * (s.b[ids[i]].y - c.y);
        real dj = (s.b[ids[j]].x - c.x) * (s.b[ids[j]].x - c.x) + (s.b[ids[j]].y - c.y) * (s.b[ids[j]].y - c.y);
        if (dj < di) { int t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
    }
    if (n > nT) n = nT;
    int nb = 0;
    for (int i = 0; i < n; ++i)
        for (int f = 0; f < 17; ++f) {
            int type, ra = -1, rb = -1;
            if (f == 0) type = 0;
            else if (f < 5) { type = 1; ra = f - 1; }
            else { type = 2; int k = f - 5; ra = k / 3; rb = k % 3; if (rb >= ra) rb++; }
            SnkEscFam fm;
            if (!snkEscWindow(pre, nm, V, minStep, ids[i], type, ra, rb, fm, nPartial)) continue;
            int slot = nb < maxF ? nb++ : -1;
            if (slot < 0) { int lo = 0; for (int q = 1; q < maxF; ++q) if (best[q].p < best[lo].p) lo = q; if (best[lo].p < fm.p) slot = lo; }
            if (slot >= 0) best[slot] = fm;
        }
    return nb;
}
// p* = chance of hitting a ball on with the best of the 4 best families (24 noisy partial sims each)
inline real snkEscapeFrom(const GameSnk& pre, const NoiseModel& nm, u64 seed, const SnkEscFam* best, int nb, int& escType, int& r1, int& r2) {
    escType = -1; r1 = r2 = -1;
    real pStar = 0;
    ShotParams sp; sp.V = 2.4; sp.a = 0; sp.b = 0;
    for (int q = 0; q < nb; ++q) {
        sp.phi = best[q].phi;
        real p = snkHitChance(pre, sp, nm, seed + q * 31, 24);   // full noise: aim, strength, tip
        if (p > pStar) { pStar = p; escType = best[q].type; r1 = best[q].a; r2 = best[q].b; }
    }
    return pStar;
}
inline real snkEscapeChance(const GameSnk& pre, const NoiseModel& nm, u64 seed, int& escType, int& r1, int& r2) {
    SnkEscFam best[4]; int np = 0;
    int nb = snkEscapeScan(pre, nm, 2.4, 0, 6, best, 4, np);
    return snkEscapeFrom(pre, nm, seed, best, nb, escType, r1, r2);
}

// ---- the referee as the bot sees it (mirrors snkReferee), prepared once per decision
inline void snkMissPrep(BotCtx& c, const Game8&) { c.mxDirect = 0; c.mxPStar = -1; c.nEsc = 0; c.rootW = -1; }
inline void snkMissPrep(BotCtx& c, const GameSnk& g) {
    c.mxDirect = -1; c.mxPStar = -1; c.nEsc = 0;
    c.rootW = c.snk && c.snk->table ? snkWinNow(*c.snk, g, g.ballInHand) : -1;
    if (g.ballInHand || g.isBreak) return;            // ball in hand: sight depends on the placement
    c.mxDirect = snkDirectSight(g) ? 1 : 0;
    const bool needP = !c.mxDirect && c.refMode == 0 && !snkNeedsSnookers(g, g.turn);
    if (!needP && snkCanHit(g)) return;
    // escape windows: shared by the escape candidates and p*
    int np = 0;
    c.nEsc = snkEscapeScan(g, c.nm, 2.4, 0.7, c.budget > 1500 ? 6 : 4, c.esc, 6, np);
    c.used += 0.25 * np; c.nPartial += np;
    if (needP) {
        SnkEscFam b4[6]; int nb = c.nEsc;
        for (int i = 0; i < nb; ++i) b4[i] = c.esc[i];
        for (int i = 0; i < nb; ++i) for (int j = i + 1; j < nb; ++j) if (b4[j].p > b4[i].p) { SnkEscFam t = b4[i]; b4[i] = b4[j]; b4[j] = t; }
        if (nb > 4) nb = 4;
        int t, r1, r2;
        c.mxPStar = snkEscapeFrom(g, c.nm, c.crnSeed ^ 0x2545F4914F6CDD1DULL, b4, nb, t, r1, r2);
        c.used += 0.25 * 24 * nb; c.nPartial += 24 * nb;
    }
}
// A miss puts the balls back: the foul is worth at most the position we started from, less a
// fee for the penalty, which grows with consecutive misses (the third with a clear sight
// loses the frame, see snkMissCalled).
inline real snkMissFee(const GameSnk& pre) { return 0.02 * (1 + 2 * pre.missCount[pre.turn]); }
// Would the referee call this failure to hit a ball on a miss? 0 no, 1 miss, 2 miss that
// forfeits the frame (third in a row with a clear sight). `pre` = position as struck.
inline int snkMissCalled(BotCtx& c, const GameSnk& pre, const GameSnk& a, const ShotParams* sp) {
    if (!a.foulNoHit || a.practice) return 0;
    const int me = pre.turn;
    if (snkNeedsSnookers(pre, me) || snkNeedsSnookers(a, me)) return 0;
    int direct = c.mxDirect;
    if (direct < 0) direct = snkDirectSight(pre) ? 1 : 0;
    if (direct) return pre.missCount[me] >= 2 ? 2 : 1;
    if (c.refMode == 1) return 1;
    if (pre.ballInHand) return 1;                      // we placed the cue ball there ourselves: escapes are easy to find
    if (c.mxPStar < 0) return 0;
    if (c.mxPStar >= c.refTau) return 1;
    if (c.mxPStar >= 0.15 && sp) {                     // effort test: was it a real attempt?
        c.used += 0.25; c.nPartial++;
        if (!snkFirstOn(pre, *sp, 0, 0)) return 1;
    }
    return 0;
}
// After a foul the opponent picks their best option: play on (maybe with a free ball), put us
// back in, or, after a miss, have the balls replaced so we face the same position again with
// the penalty paid. `before` = position as struck, sp = the stroke as intended (effort test).
inline real valueAfterWin(BotCtx& c, const GameSnk& before, GameSnk& a, const ShotParams* sp) {
    const SnkEval& ev = *c.snk;
    const int me = before.turn;
    endShotSnk(a);
    if (a.winner >= 0) return a.winner == me ? 1 : 0;
    if (a.pendingChoice) {
        GameSnk t = a; t.freeBall = a.freeAvail;
        real pPlay = snkWinNow(ev, t, t.ballInHand);
        GameSnk u = a; u.turn = me; u.freeBall = 0;
        real pBack = 1 - snkWinNow(ev, u, u.ballInHand);
        real pOpp = pmax(pPlay, pBack);
        int m = snkMissCalled(c, before, a, sp);
        if (m == 2) return 0;
        if (m) {
            GameSnk r = before; r.score[0] = a.score[0]; r.score[1] = a.score[1];
            pOpp = pmax(pOpp, 1 - snkWinNow(ev, r, r.ballInHand));
            if (c.rootW >= 0) pOpp = pmax(pOpp, 1 - pmax(0, c.rootW - snkMissFee(before)));   // a win chance: never below 0
        }
        return 1 - pOpp;
    }
    real w = snkWinNow(ev, a, a.ballInHand);
    return a.turn == me ? w : 1 - w;
}
inline real valueAfter(BotCtx& c, const GameSnk& before, GameSnk& a, const ShotParams* sp) {
    if (c.snk && c.snk->table) return valueAfterWin(c, before, a, sp);
    const int me = before.turn, op = 1 - me;
    endShotSnk(a);
    real d = (a.score[me] - before.score[me]) - (a.score[op] - before.score[op]);
    if (a.winner >= 0) return d + (a.winner == me ? 40 : -40);
    if (a.pendingChoice) {
        // after our foul the opponent picks the best of: play on (maybe with a free ball) / put us back in / replace
        GameSnk t = a; t.freeBall = a.freeAvail;
        real ePlay = snkVisit(t, t.ballInHand);
        GameSnk u = a; u.turn = me; u.freeBall = 0;
        real eBack = snkVisit(u, u.ballInHand);
        real vOpp = pmax(ePlay, -eBack);
        int m = snkMissCalled(c, before, a, sp);
        if (m == 2) return d - 40;
        if (m) vOpp = pmax(vOpp, -snkVisit(before, before.ballInHand));
        return d - vOpp;
    }
    real E = snkVisit(a, a.ballInHand);
    return d + (a.turn == me ? E : -E);
}
// nominate the target of a candidate as the free ball
inline void prepShot(Game8&, int) {}
inline void prepShot(GameSnk& a, int tgt) {
    if (a.phase == SNK_COLOUR_ON) a.nominated = snkIsColour(tgt) ? tgt : -1;   // bots always nominate
    if (!a.freeBall) return;
    a.freeNom = (tgt > 0 && a.sim.b[tgt].on && !snkPhaseOn(a, tgt)) ? tgt : -1;
}

// ------------------------------------------------ search
struct Cand {
    int place; real px, py; ShotParams sp;
    int tgt, pk, kind;            // kind: 0 pot, 1 safety, 2 break, 3 escape from a snooker
    real val; int level, done;
    real sV, sB, sA, sPhi;        // pattern-search step sizes
    real slope;                   // d(launch error)/d(phi) for warm-started secant
};
const int MAXC = 700;
struct CandList { int n; Cand c[MAXC]; };
inline void pushCand(CandList& L, const Cand& c) { if (L.n < MAXC) L.c[L.n++] = c; }
inline real wrapPi(real a) { while (a > PI) a -= TWO_PI; while (a < -PI) a += TWO_PI; return a; }

struct BotDecision {
    int place; real px, py; ShotParams sp; real value; int tgt, kind, pk;
    real sims; int nFull, nPartial, nSecant, nRefine, nSwitch, nCand; real phase1;
};

// Every ball moved by a gaussian of sd sigma (at most 3 sigma), staying on the bed and clear
// of the others (a ball that finds no legal spot in a few tries stays where it is), the same
// kind of jitter the rack gets, only larger.
inline void perturbBalls(Sim& s, real sigma, Rng& r) {
    const real R = s.P.R, xm = s.T.L / 2 - R, ym = s.T.W / 2 - R, cap = 3 * sigma;
    for (int i = 0; i < s.n; ++i) {
        Ball& b = s.b[i];
        if (!b.on) continue;
        for (int tries = 0; tries < 6; ++tries) {
            real dx = sigma * r.gauss(), dy = sigma * r.gauss(), d = psqrt(dx * dx + dy * dy);
            if (d > cap) { dx *= cap / d; dy *= cap / d; }
            real x = b.x + dx, y = b.y + dy;
            if (pabs(x) > xm || pabs(y) > ym) continue;
            bool ok = true;
            for (int j = 0; j < s.n && ok; ++j) {
                if (j == i || !s.b[j].on) continue;
                real ex = s.b[j].x - x, ey = s.b[j].y - y;
                ok = ex * ex + ey * ey >= 4 * R * R * 1.0001;
            }
            if (ok) { b.x = x; b.y = y; break; }
        }
    }
}
// The stroke as meant for the perturbed balls: the same impact parameter on the target
// (same thickness, so a pot is still aimed at its pocket). Escapes off cushions are aimed at
// the table, not at a ball, and keep their angle.
inline void reaimPerturbed(const Sim& was, const Sim& now, int tgt, int kind, ShotParams& sp) {
    if (tgt <= 0 || kind == 3) return;
    real ux = pcos(sp.phi), uy = psin(sp.phi);
    real dx = was.b[tgt].x - was.b[0].x, dy = was.b[tgt].y - was.b[0].y;
    real bimp = ux * dy - uy * dx;
    real ex = now.b[tgt].x - now.b[0].x, ey = now.b[tgt].y - now.b[0].y, L = psqrt(ex * ex + ey * ey);
    real dxL = psqrt(dx * dx + dy * dy);
    if (L < 1e-9 || dxL < 1e-9) return;
    real q0 = pclamp(bimp / dxL, -1, 1), q1 = pclamp(bimp / L, -1, 1);
    // phi = theta - asin(b / L); asin via atan2 keeps us inside pmath
    real th0 = patan2(dy, dx), th1 = patan2(ey, ex);
    real as0 = patan2(q0, psqrt(1 - q0 * q0)), as1 = patan2(q1, psqrt(1 - q1 * q1));
    sp.phi += (th1 - as1) - (th0 - as0);
}
// one sampled play of a candidate: pre = position as struck (perturbed in a human-like noisy
// sample), a = after the balls stopped (rules not applied yet)
template <class G> void simSample(BotCtx& c, const G& g, const Cand& cd, bool noisy, int k, G& pre, G& a) {
    pre = g;
    pre.sim.trackOrient = 0;
    if (cd.place) resetBall(pre.sim.b[0], cd.px, cd.py);
    prepShot(pre, cd.tgt);
    ShotParams sp = cd.sp;
    if (noisy && c.posSigma > 0) {
        Sim was = pre.sim;
        Rng pr; pr.seed((c.crnSeed ^ 0x9e3779b97f4a7c15ULL) + 104729ULL * (u64)(k + 1));
        perturbBalls(pre.sim, c.posSigma, pr);
        reaimPerturbed(was, pre.sim, cd.tgt, cd.kind, sp);
    }
    a = pre;
    if (noisy) { Rng r; r.seed(c.crnSeed + 7919ULL * (u64)(k + 1)); executeShot(a, sp, true, c.nm, r); }
    else { strike(a.sim, sp.phi, sp.V, sp.a, sp.b); a.ballInHand = 0; }
    simulate(a.sim, SIM_DT, 25);
}
template <class G> real simFull(BotCtx& c, const G& g, const Cand& cd, bool noisy, int k) {
    G pre, a;
    simSample(c, g, cd, noisy, k, pre, a);
    c.used += 1; c.nFull++;
    return valueAfter(c, pre, a, &cd.sp);
}
template <class G> real evaluate(BotCtx& c, const G& g, const Cand& cd, bool noisy) {
    if (!noisy) return simFull(c, g, cd, false, 0);
    real s = 0;
    for (int k = 0; k < c.K; ++k) s += simFull(c, g, cd, true, k);
    return s / c.K;
}
// partial simulation up to the first contact: object-ball launch error (rad)
template <class G> int launchErr(BotCtx& c, const G& g, const Cand& cd, real phi, real& err) {
    Sim s = g.sim;
    s.trackOrient = 0;
    if (cd.place) resetBall(s.b[0], cd.px, cd.py);
    strike(s, phi, cd.sp.V, cd.sp.a, cd.sp.b);
    simulateFirstHit(s, 8);
    c.used += 0.25; c.nPartial++;
    if (s.ev.firstHit != cd.tgt) return 0;
    const Ball& B = s.b[cd.tgt];
    if (B.vx == 0 && B.vy == 0) return 0;
    real px, py; pocketTarget(g.sim, cd.pk, px, py);
    real req = patan2(py - g.sim.b[cd.tgt].y, px - g.sim.b[cd.tgt].x);
    err = wrapPi(patan2(B.vy, B.vx) - req);
    return 1;
}
// Newton / secant root finding on phi so the object ball launches at the pocket target
template <class G> int centreAim(BotCtx& c, const G& g, Cand& cd, int maxIt) {
    real phi0 = cd.sp.phi, e0;
    if (!launchErr(c, g, cd, phi0, e0)) return 0;
    real slope = cd.slope;
    if (slope == 0) {
        real h = 3e-4, e1;
        if (!launchErr(c, g, cd, phi0 + h, e1)) { h = -h; if (!launchErr(c, g, cd, phi0 + h, e1)) return 0; }
        slope = (e1 - e0) / h;
        if (slope == 0) return 0;
    }
    for (int it = 0; it < maxIt && pabs(e0) > 2e-5; ++it) {
        real dphi = -e0 / slope;
        if (dphi > 0.05) dphi = 0.05; else if (dphi < -0.05) dphi = -0.05;
        real e1, phi1 = phi0 + dphi;
        if (!launchErr(c, g, cd, phi1, e1)) { phi1 = phi0 + 0.5 * dphi; if (!launchErr(c, g, cd, phi1, e1)) break; }
        if (e1 != e0 && phi1 != phi0) { real sl = (e1 - e0) / (phi1 - phi0); if (sl != 0) slope = sl; }
        phi0 = phi1; e0 = e1; c.nSecant++;
    }
    cd.sp.phi = phi0; cd.slope = slope;
    return pabs(e0) < 2e-3;
}

template <class G> void addPotPair(BotCtx& c, const G& g, CandList& L, real cx, real cy, int place, int t, int k, real ease, real phi) {
    Cand cd; cd.place = place; cd.px = cx; cd.py = cy; cd.tgt = t; cd.pk = k; cd.kind = 0;
    cd.sp.phi = phi; cd.sp.V = powerToSpeed(g.sim.P, 0.38); cd.sp.a = 0; cd.sp.b = 0;
    cd.val = -1e9; cd.level = 0; cd.done = 0; cd.sV = 0.25; cd.sB = 0.2; cd.sA = 0.15; cd.sPhi = 0; cd.slope = 0;
    (void)ease;
    if (!centreAim(c, g, cd, 4)) return;
    cd.val = evaluate(c, g, cd, false);
    pushCand(L, cd);
}

// A stroke that, played as intended, fails to hit a ball on while the referee would call a miss
// for it. Never the best choice: the balls are replaced and we pay the penalty for the same
// position. (An effort-test miss needs p* >= 0.15; a cue ball placed in a snooker counts as a
// miss; the striker must not need snookers after the minimum penalty either.)
inline bool snkDeliberateFoul(BotCtx&, const Game8&, const Cand&) { return false; }
inline bool snkDeliberateFoul(BotCtx& c, const GameSnk& g, const Cand& cd) {
    if (cd.done == 2) return true;
    if (g.practice) return false;
    const int me = g.turn;
    if (g.score[1 - me] + 4 - g.score[me] > snkRemaining(g)) return false;
    GameSnk pre = g;
    if (cd.place) resetBall(pre.sim.b[0], cd.px, cd.py);
    prepShot(pre, cd.tgt);
    int direct = c.mxDirect;                       // ball in hand: sight from this placement
    if (cd.place || direct < 0) direct = snkDirectSight(pre) ? 1 : 0;
    if (!(direct == 1 || c.refMode == 1 || cd.place || c.mxPStar >= 0.15)) return false;
    c.used += 0.25; c.nPartial++;
    return !snkFirstOn(pre, cd.sp, 0, 0);
}

inline real missFee(const Game8&) { return 0; }
inline real missFee(const GameSnk& g) { return snkMissFee(g); }

// does a random safety make first contact with a ball on, played as intended? (snooker only)
inline bool safetyHitsOn(BotCtx&, const Game8&, const Cand&) { return true; }
inline bool safetyHitsOn(BotCtx& c, const GameSnk& g, const Cand& cd) {
    GameSnk pre = g;
    if (cd.place) resetBall(pre.sim.b[0], cd.px, cd.py);
    prepShot(pre, cd.tgt);
    c.used += 0.25; c.nPartial++;
    return snkFirstOn(pre, cd.sp, 0, 0) != 0;
}

// escapes from a snooker: the best hitting windows at three speeds, refined like safeties
inline void addEscapes(BotCtx&, const Game8&, CandList&) {}
inline void addEscapes(BotCtx& c, const GameSnk& g, CandList& L) {
    if (!c.nEsc || g.ballInHand || snkCanHit(g)) return;
    const Sim& s = g.sim;
    const real Vs[3] = {2.4, 1.5, 3.6};
    for (int iv = 0; iv < 3; ++iv)
        for (int q = 0; q < c.nEsc; ++q) {
            SnkEscFam f = c.esc[q];
            if (iv > 0) {        // re-centre the window at this speed (cushions throw differently)
                int np = 0;
                int ok = snkEscWindow(g, c.nm, Vs[iv], 0.7, f.tgt, f.type, f.a, f.b, f, np);
                c.used += 0.25 * np; c.nPartial += np;
                if (!ok) continue;
            }
            Cand cd; cd.place = 0; cd.px = s.b[0].x; cd.py = s.b[0].y; cd.tgt = f.tgt; cd.pk = -1; cd.kind = 3;
            cd.sp.phi = f.phi; cd.sp.V = Vs[iv]; cd.sp.a = 0; cd.sp.b = 0;
            cd.level = 0; cd.done = 0; cd.sV = 0.15; cd.sB = 0.2; cd.sA = 0; cd.sPhi = 0.5 * f.dphi; cd.slope = 0;
            cd.val = evaluate(c, g, cd, false);
            pushCand(L, cd);
        }
}

template <class G> void botThink(BotCtx& c, const G& g0, BotDecision& d) {
    static CandList L;
    L.n = 0;
    c.used = 0; c.nFull = c.nPartial = c.nSecant = c.nRefine = c.nSwitch = 0;
    G g = g0;
    const Sim& s = g.sim;
    const real R = s.P.R;
    const bool bih = g.ballInHand != 0;
    // deep thinking (more than 1500 simulations): a wider search (more safeties, more
    // power/spin variants) and, with human error, more noisy samples per shot. Budgets up
    // to 1500 behave exactly as before.
    if (c.budget > 1500) {
        const real sc = c.budget / 1500;
        c.nSafe = (int)pmin(80, 20 * psqrt(sc));
        c.nPow = 6; const real pw[6] = {0.16, 0.22, 0.30, 0.40, 0.52, 0.68};
        for (int i = 0; i < 6; ++i) c.pows[i] = pw[i];
        c.nSpin = 4; const real sp4[4] = {-0.40, -0.15, 0.10, 0.35};
        for (int i = 0; i < 4; ++i) c.spins[i] = sp4[i];
        if (c.noisy) c.K = (int)pmin(24, 8 * pexp(plog(sc) / 3));
    }
    const real p1Budget = c.phase2 ? c.budget * 0.5 : c.budget;
    snkMissPrep(c, g);

    // ---- 8-ball break: hit the apex at full power from a few spots
    bool did = false;
    if (breakShot(g)) {
        real ys[5] = {0, 0.12, -0.12, 0.24, -0.24};
        int apex = 1; real best = 1e9;
        for (int t = 1; t < s.n; ++t) if (s.b[t].on && s.b[t].x < best) { best = s.b[t].x; apex = t; }
        for (int i = 0; i < 5; ++i) {
            real px = bih ? headStringX(s.T) - 0.12 : s.b[0].x, py = bih ? ys[i] : s.b[0].y;
            if (bih && !placeOk(g, px, py)) continue;
            Cand cd; cd.place = bih; cd.px = px; cd.py = py; cd.tgt = apex; cd.pk = -1; cd.kind = 2;
            cd.sp.phi = patan2(s.b[apex].y - py, s.b[apex].x - px); cd.sp.V = powerToSpeed(s.P, 1.0);
            cd.sp.a = 0; cd.sp.b = -0.15; cd.level = 0; cd.done = 0; cd.sV = 0; cd.sB = 0.1; cd.sA = 0; cd.sPhi = 0.004; cd.slope = 0;
            cd.val = evaluate(c, g, cd, false);
            pushCand(L, cd);
            if (!bih) break;
        }
        did = true;
    }
    if (!did) {
        // ---- cue positions: current, or ball-in-hand placements
        struct Pos { real x, y; int place; } pos[48]; int np = 0;
        if (!bih) { pos[np].x = s.b[0].x; pos[np].y = s.b[0].y; pos[np].place = 0; np++; }
        else {
            for (int t = 1; t < s.n && np < 36; ++t) {
                if (!legalT(g, t)) continue;
                for (int k = 0; k < s.T.npk && np < 36; ++k) {
                    real ux, uy, d2; if (!objLane(s, t, k, ux, uy, d2)) continue;
                    const real angs[3] = {0, 0.3, -0.3};
                    for (int ia = 0; ia < 3 && np < 36; ++ia) {
                        real ca = pcos(angs[ia]), sa = psin(angs[ia]);
                        real vx = ux * ca - uy * sa, vy = ux * sa + uy * ca;
                        real x = s.b[t].x - 2 * R * ux - vx * 0.3, y = s.b[t].y - 2 * R * uy - vy * 0.3;
                        if (placeOk(g, x, y)) { pos[np].x = x; pos[np].y = y; pos[np].place = 1; np++; }
                    }
                }
            }
            for (int i = 0; i < 40 && np < 48; ++i) {
                real x, y; sampleRegion(g, c.rng, x, y);
                if (placeOk(g, x, y)) { pos[np].x = x; pos[np].y = y; pos[np].place = 1; np++; }
            }
        }
        // ---- pot pairs ordered by geometric ease
        struct Pair { int pi, t, k; real e, phi; } pr[400]; int npr = 0;
        for (int pi = 0; pi < np; ++pi) {
            G tmp = g; if (pos[pi].place) resetBall(tmp.sim.b[0], pos[pi].x, pos[pi].y);
            for (int t = 1; t < s.n; ++t) {
                if (!legalT(g, t)) continue;
                for (int k = 0; k < s.T.npk; ++k) {
                    real phi, e = shotEase(tmp.sim, pos[pi].x, pos[pi].y, t, k, &phi);
                    if (e > 0 && npr < 400) { pr[npr].pi = pi; pr[npr].t = t; pr[npr].k = k; pr[npr].e = e; pr[npr].phi = phi; npr++; }
                }
            }
        }
        for (int i = 0; i < npr; ++i) { int bi = i; for (int j = i + 1; j < npr; ++j) if (pr[j].e > pr[bi].e) bi = j; Pair t = pr[i]; pr[i] = pr[bi]; pr[bi] = t; }
        // pass A: one default variant per pot (keeps ~40% of phase 1 for safeties/variants)
        const real passA = p1Budget * 0.6;
        for (int i = 0; i < npr && c.used < passA; ++i) {
            const Pos& P = pos[pr[i].pi];
            if (P.place) { G tmp = g; resetBall(tmp.sim.b[0], P.x, P.y); addPotPair(c, tmp, L, P.x, P.y, 1, pr[i].t, pr[i].k, pr[i].e, pr[i].phi); }
            else addPotPair(c, g, L, P.x, P.y, 0, pr[i].t, pr[i].k, pr[i].e, pr[i].phi);
        }
        // safeties
        int nPots = L.n;
        int nSafe = c.nSafe; if (nSafe > (int)(c.budget * 0.15) + 2) nSafe = (int)(c.budget * 0.15) + 2;
        int legal[22], nl = 0;
        for (int t = 1; t < s.n; ++t) if (legalT(g, t)) legal[nl++] = t;
        for (int i = 0; i < nSafe && nl; ++i) {
            Cand cd; bool ok = false;
            for (int tries = 0; tries < 4 && !ok; ++tries) {   // snooker: redraw safeties that would foul first contact
                const Pos& P = pos[c.rng.below(np)];
                int t = legal[c.rng.below(nl)];
                real dx = s.b[t].x - P.x, dy = s.b[t].y - P.y, dd = psqrt(dx * dx + dy * dy);
                if (dd < 1e-6) break;
                real mo = 2 * R / dd; if (mo > 1) mo = 1;
                cd.place = P.place; cd.px = P.x; cd.py = P.y; cd.tgt = t; cd.pk = -1; cd.kind = 1;
                cd.sp.phi = patan2(dy, dx) + c.rng.uniform(-0.95, 0.95) * patan(mo);
                cd.sp.V = powerToSpeed(s.P, c.rng.uniform(0.15, 0.5)); cd.sp.a = 0; cd.sp.b = c.rng.uniform(-0.3, 0.2);
                cd.level = 0; cd.done = 0; cd.sV = 0.25; cd.sB = 0.2; cd.sA = 0; cd.sPhi = 0.25 * patan(mo); cd.slope = 0;
                ok = safetyHitsOn(c, g, cd) || tries == 3;
            }
            if (!ok) continue;
            cd.val = evaluate(c, g, cd, false);
            pushCand(L, cd);
        }
        addEscapes(c, g, L);
        // pass C: power / spin variants of the best pots
        for (int i = 0; i < nPots; ++i) {   // sort pots by value
            int bi = i; for (int j = i + 1; j < nPots; ++j) if (L.c[j].val > L.c[bi].val) bi = j;
            Cand t = L.c[i]; L.c[i] = L.c[bi]; L.c[bi] = t;
        }
        for (int i = 0; i < nPots && c.used < p1Budget; ++i) {
            for (int ip = 0; ip < c.nPow && c.used < p1Budget; ++ip)
                for (int is = 0; is < c.nSpin && c.used < p1Budget; ++is) {
                    Cand cd = L.c[i];
                    cd.sp.V = powerToSpeed(s.P, c.pows[ip]); cd.sp.b = c.spins[is];
                    if (pabs(cd.sp.V - L.c[i].sp.V) < 1e-9 && cd.sp.b == L.c[i].sp.b) continue;
                    G tmp = g; if (cd.place) resetBall(tmp.sim.b[0], cd.px, cd.py);
                    cd.val = evaluate(c, tmp, cd, false);
                    pushCand(L, cd);
                }
        }
    }
    if (L.n == 0) {   // nothing found: any shot
        Cand cd; cd.place = 0; cd.px = s.b[0].x; cd.py = s.b[0].y; cd.tgt = -1; cd.pk = -1; cd.kind = 1;
        cd.sp.phi = c.rng.uniform(-PI, PI); cd.sp.V = 2.0; cd.sp.a = 0; cd.sp.b = 0;
        cd.level = 1; cd.done = 1; cd.sV = cd.sB = cd.sA = cd.sPhi = 0; cd.slope = 0;
        if (bih) { real x, y; for (int i = 0; i < 50; ++i) { sampleRegion(g, c.rng, x, y); if (placeOk(g, x, y)) { cd.place = 1; cd.px = x; cd.py = y; break; } } }
        cd.val = evaluate(c, g, cd, false);
        pushCand(L, cd);
    }
    c.phase1Used = c.used;

    // ---- phase 2: best-first refinement
    int last = -1;
    while (c.phase2 && c.used < c.budget) {
        int bi = -1;
        for (int i = 0; i < L.n; ++i) if (!L.c[i].done && (bi < 0 || L.c[i].val > L.c[bi].val)) bi = i;
        if (bi < 0) break;
        if (last >= 0 && bi != last) c.nSwitch++;
        last = bi;
        Cand& cd = L.c[bi];
        G gp = g; if (cd.place) resetBall(gp.sim.b[0], cd.px, cd.py);
        c.nRefine++;
        if (cd.level == 0) {
            if (cd.kind == 0 && !centreAim(c, gp, cd, 3)) { cd.done = 1; cd.val = -1e9; continue; }
            if (c.foulGuard && c.nEsc && snkDeliberateFoul(c, g, cd)) { cd.done = 2; continue; }   // snookered: spend the budget on real attempts
            cd.val = evaluate(c, gp, cd, c.noisy);
            cd.level = 1;
            continue;
        }
        // pattern poll around the current point
        Cand bestT = cd; bool improved = false;
        for (int dim = 0; dim < 4 && !improved; ++dim)
            for (int sg = -1; sg <= 1 && !improved; sg += 2) {
                Cand t = cd;
                if (dim == 0) { if (cd.sV <= 0) continue; t.sp.V = pclamp(cd.sp.V * (1 + sg * cd.sV), s.P.vmin, s.P.vmax); }
                else if (dim == 1) { if (cd.sB <= 0) continue; t.sp.b = cd.sp.b + sg * cd.sB; }
                else if (dim == 2) { if (cd.kind != 0 || cd.sA <= 0) continue; t.sp.a = cd.sp.a + sg * cd.sA; }
                else { if (cd.kind == 0 || cd.sPhi <= 0) continue; t.sp.phi = cd.sp.phi + sg * cd.sPhi; }
                real off = psqrt(t.sp.a * t.sp.a + t.sp.b * t.sp.b);
                if (off > 0.5) { t.sp.a *= 0.5 / off; t.sp.b *= 0.5 / off; }
                if (t.kind == 0 && !centreAim(c, gp, t, 2)) continue;
                t.val = evaluate(c, gp, t, c.noisy);
                if (t.val > cd.val + 1e-9) { bestT = t; improved = true; }
                if (c.used >= c.budget) break;
            }
        if (improved) { cd = bestT; }
        else {
            cd.sV *= 0.5; cd.sB *= 0.5; cd.sA *= 0.5; cd.sPhi *= 0.5;
            if (cd.sV < 0.02 && cd.sB < 0.02 && cd.sA < 0.02 && cd.sPhi < 2e-4) cd.done = 1;
        }
    }
    // ---- budget left after the search converged (human error only): more noisy samples
    // for the leading candidates, so the final choice is less a matter of luck
    if (c.noisy && c.phase2 && c.used < c.budget) {
        int idx[6], m = 0;
        for (int r = 0; r < 6; ++r) {
            int bi = -1;
            for (int i = 0; i < L.n; ++i) {
                if (L.c[i].level == 0 || L.c[i].val < -1e8) continue;
                bool taken = false; for (int q = 0; q < m; ++q) if (idx[q] == i) taken = true;
                if (!taken && (bi < 0 || L.c[i].val > L.c[bi].val)) bi = i;
            }
            if (bi < 0) break;
            idx[m++] = bi;
        }
        real sum[6]; int ns[6];
        for (int q = 0; q < m; ++q) { sum[q] = L.c[idx[q]].val * c.K; ns[q] = c.K; }
        for (int k = c.K; m > 0 && k < 6 * c.K && c.used < c.budget; ++k)
            for (int q = 0; q < m && c.used < c.budget; ++q) { sum[q] += simFull(c, g, L.c[idx[q]], true, k); ns[q]++; }
        for (int q = 0; q < m; ++q) L.c[idx[q]].val = sum[q] / ns[q];
    }
    // ---- choose: noisy -> best among noise-evaluated candidates. A deliberate foul into a miss
    // (done == 2) is worth at most the best real attempt less the miss fee: replaying the same
    // position cannot be better than the best way to play it. So one is chosen only if there is
    // no real attempt at all. This is the searched form of the root cap in valueAfterWin (rootW is
    // the evaluator's guess of the same value); with an accurate evaluator the two agree and this
    // never binds. Training labels are frame results, so it changes only which strokes are played.
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass)
        for (int tries = 0; tries < L.n; ++tries) {
            best = -1;
            for (int i = 0; i < L.n; ++i) {
                if (L.c[i].done == 2 || (pass == 0 && c.noisy && c.phase2 && L.c[i].level == 0)) continue;
                if (best < 0 || L.c[i].val > L.c[best].val) best = i;
            }
            if (best < 0 || !c.foulGuard || !snkDeliberateFoul(c, g, L.c[best])) break;
            L.c[best].done = 2;
        }
    if (best >= 0 && c.foulGuard) { const real cap = c.snk && c.snk->table ? pmax(0, L.c[best].val - missFee(g)) : L.c[best].val - missFee(g); for (int i = 0; i < L.n; ++i) if (L.c[i].done == 2 && L.c[i].val > cap) L.c[i].val = cap; }
    if (best < 0) { best = 0; for (int i = 1; i < L.n; ++i) if (L.c[i].val > L.c[best].val) best = i; }
    const Cand& b = L.c[best];
    d.place = b.place; d.px = b.px; d.py = b.py; d.sp = b.sp; d.value = b.val; d.tgt = b.tgt; d.kind = b.kind; d.pk = b.pk;
    d.sims = c.used; d.nFull = c.nFull; d.nPartial = c.nPartial; d.nSecant = c.nSecant;
    d.nRefine = c.nRefine; d.nSwitch = c.nSwitch; d.nCand = L.n; d.phase1 = c.phase1Used;
}

// Chance that the chosen stroke pots its target, as the bot sees it: N fresh samples with its
// own human error and judgement (independent of the search's samples, so not flattered by
// having picked the luckiest candidate). -1 if the stroke is not a pot.
template <class G> real botPotChance(BotCtx& c, const G& g, const BotDecision& d, int N) {
    if (d.kind != 0 || d.tgt <= 0) return -1;
    Cand cd; cd.place = d.place; cd.px = d.px; cd.py = d.py; cd.sp = d.sp; cd.tgt = d.tgt; cd.pk = d.pk; cd.kind = d.kind;
    if (!c.noisy) N = 1;
    int hit = 0;
    for (int k = 0; k < N; ++k) {
        G pre, a;
        simSample(c, g, cd, c.noisy, 100000 + k, pre, a);
        for (int i = 0; i < a.sim.ev.nPot; ++i) if (a.sim.ev.pot[i] == d.tgt) { hit++; break; }
    }
    return (real)hit / N;
}

inline bool breakShot(const Game8& g) { return g.isBreak != 0; }

// mode 0: judged by profile (expected-hit threshold tau + effort test), mode 1: pro rules
inline void snkReferee(const GameSnk& pre, GameSnk& g, const ShotParams& intended, const NoiseModel& nm, int mode, real tau, u64 seed, RefInfo& ri) {
    ri.checked = ri.miss = ri.reason = ri.direct = ri.forfeit = 0; ri.escType = ri.escR1 = ri.escR2 = -1; ri.pStar = ri.pPlayed = -1;
    if (!g.lastFoul || !g.foulNoHit || g.practice || g.winner >= 0 || !g.pendingChoice) return;
    ri.checked = 1;
    const int me = pre.turn;
    if (snkNeedsSnookers(pre, me) || snkNeedsSnookers(g, me)) { ri.reason = NOMISS_SNOOKERS; g.missCount[me] = 0; return; }
    ri.direct = snkDirectSight(pre) ? 1 : 0;
    if (mode == 1) { ri.miss = 1; ri.reason = MISS_PRO; }
    else if (ri.direct) { ri.miss = 1; ri.reason = MISS_DIRECT; }
    else {
        ri.pStar = snkEscapeChance(pre, nm, seed, ri.escType, ri.escR1, ri.escR2);
        ri.pPlayed = snkHitChance(pre, intended, nm, seed ^ 0x5bd1e995ULL, 24);
        if (ri.pStar >= tau) { ri.miss = 1; ri.reason = MISS_EXPECTED; }
        else if (ri.pStar >= 0.15 && ri.pPlayed < 0.5 * ri.pStar) { ri.miss = 1; ri.reason = MISS_EFFORT; }
        else ri.reason = NOMISS_HARD;
    }
    if (ri.miss) {
        g.lastMiss = 1; g.pendingChoice |= CH_REPLACE;
        if (ri.direct) {
            g.missCount[me]++;
            if (g.missCount[me] >= 3) { g.winner = 1 - me; ri.forfeit = 1; g.pendingChoice = 0; }
        } else g.missCount[me] = 0;
    } else g.missCount[me] = 0;
}
// The incoming player's decision after a foul (`pre` = position as struck). Each available
// option is searched from the chooser's view with a quarter of the context's budget (100-1500
// simulations), so a replaced snooker is judged by how well the offender can actually escape.
// vals[0..2] = play / put back / replace: win chance for the chooser (points with the legacy
// evaluator), -1e9 if not available.
inline int snkChooseOption(const BotCtx& proto, const GameSnk& g, const GameSnk& pre, real* vals) {
    const int opts[3] = {CH_PLAY, CH_PUTBACK, CH_REPLACE};
    const int chooser = g.turn;
    const bool win = proto.snk && proto.snk->table;
    int best = CH_PLAY; real bv = -1e18;
    for (int i = 0; i < 3; ++i) {
        vals[i] = -1e9;
        if (!(g.pendingChoice & opts[i])) continue;
        GameSnk x = g; snkApplyChoice(x, pre, opts[i]);
        BotCtx c = proto; c.budget = pclamp(proto.budget * 0.25, 100, 1500);
        BotDecision d; botThink(c, x, d);
        real v = x.turn == chooser ? d.value : (win ? 1 - d.value : -d.value);
        vals[i] = v;
        if (v > bv) { bv = v; best = opts[i]; }
    }
    return best;
}
inline bool breakShot(const GameSnk&) { return false; }

} // namespace pool
