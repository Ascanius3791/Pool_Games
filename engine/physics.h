// physics.h — billiards physics engine (freestanding C++, native + WASM)
//
// Model (SI units, table plane = xy, z up):
//  * Ball state: position (x,y), velocity (vx,vy), angular velocity (wx,wy,wz).
//  * Cloth friction: contact-point slip u = v + w x (-R z).
//      sliding : friction mu_s*m*g opposes u   (du/dt = -7/2 mu_s g u_hat)
//      rolling : rolling resistance mu_r*g, rolling constraint kept
//      spinning: wz decays at a constant rate
//    Integrated with a half-kick / drift / half-kick scheme; the sliding->rolling
//    transition is snapped exactly.
//  * Collisions are found exactly (time of impact) within each substep.
//  * Ball-ball: restitution e_ball + Coulomb friction at the contact (cut- and
//    spin-induced throw, gear effect).
//  * Cushion: speed-dependent restitution; contact point above the ball's equator
//    (nose height), Coulomb friction there -> side spin changes rebound angle.
//  * Cue strike: tip offset (a,b) in units of R -> spin; squirt for side spin.
#pragma once
#include "pmath.h"

namespace pool {

const int MAXB = 22;   // enough for snooker later

struct Params {
    real R, m, g;
    real mu_s, mu_r, spinDecel;
    real e_ball, mu_ball;
    real e_c0, e_c1, e_cmin, mu_cush, cushSin, cushCos;
    real squirt;          // rad of squirt per unit side offset
    real massRatio;       // ball mass / cue mass
    real vmin, vmax, powExp;
    real maxOffset;       // max tip offset (units of R) before miscue
};

inline void defaultPoolParams(Params& p) {
    p.R = 0.028575; p.m = 0.17; p.g = 9.81;
    p.mu_s = 0.20; p.mu_r = 0.010; p.spinDecel = 10.0;
    p.e_ball = 0.95; p.mu_ball = 0.06;
    p.e_c0 = 0.92; p.e_c1 = 0.06; p.e_cmin = 0.60; p.mu_cush = 0.20;
    // cushion nose at 0.635 * ball diameter above slate -> sin(theta) = 0.27
    p.cushSin = 0.27; p.cushCos = psqrt(1 - 0.27 * 0.27);
    p.squirt = 0.035; p.massRatio = 0.17 / 0.54;
    p.vmin = 0.15; p.vmax = 9.0; p.powExp = 1.6;
    p.maxOffset = 0.6;
}

// Full-size snooker (12 ft): 52.5 mm balls, 142 g
inline void defaultSnookerParams(Params& p) {
    defaultPoolParams(p);
    p.R = 0.02625; p.m = 0.142;
    p.mu_r = 0.0085;                 // snooker cloth plays a little faster
    p.massRatio = 0.142 / 0.48;
}

// Simulation substep. Kick-drift-kick is exact for the constant-deceleration
// phases (sliding and rolling); collisions are found exactly inside each step,
// so the step only bounds when friction phases switch. 4 ms is plenty.
#ifndef POOL_SIM_DT
#define POOL_SIM_DT 0.004
#endif
const real SIM_DT = POOL_SIM_DT;

struct Seg { real ax, ay, bx, by, nx, ny, dx, dy, len; };
struct Pocket {
    real mx, my;       // mouth midpoint
    real nx, ny;       // unit direction into the pocket
    real tx, ty;       // unit along the mouth
    real halfw;        // half mouth width
    real depth;        // centre must pass mouth by this much to drop
    real cx, cy;       // visual pocket centre (for rendering / drop animation)
    real r;            // visual pocket radius
};
struct Table {
    real L, W;               // playing area (cushion nose to cushion nose)
    int nseg; Seg seg[24];                      // 6 rails + 12 jaw facings
    int npt; real ptx[16], pty[16], ptr[16];   // jaw noses: circles (radius 0 = sharp point), 12 used
    real safeX, safeY;                          // |x| < safeX && |y| < safeY: no cushion reachable
    int npk; Pocket pk[6];
};

inline void addSeg(Table& T, real ax, real ay, real bx, real by, real towardX, real towardY) {
    if (T.nseg >= 24) return;
    Seg& s = T.seg[T.nseg++];
    s.ax = ax; s.ay = ay; s.bx = bx; s.by = by;
    real dx = bx - ax, dy = by - ay, l = psqrt(dx * dx + dy * dy);
    s.dx = dx / l; s.dy = dy / l; s.len = l;
    s.nx = -s.dy; s.ny = s.dx;
    // orient the normal toward the playable side (point toward*)
    if ((towardX - ax) * s.nx + (towardY - ay) * s.ny < 0) { s.nx = -s.nx; s.ny = -s.ny; }
}
inline void addPt(Table& T, real x, real y, real r) { if (T.npt >= 16) return; T.ptx[T.npt] = x; T.pty[T.npt] = y; T.ptr[T.npt] = r; T.npt++; }

// Cushion nose outlines for rendering (filled by the builder)
struct TableVis { int n; int cnt[6]; real x[6][48], y[6][48]; };

struct TableSpec {
    real L, W, cornerMouth, sideMouth, jawC, jawS;
    real cornerFacingDeg, sideFacingDeg;     // angle between rail nose and jaw facing
    real filletC, filletS;                    // jaw nose rounding radius
    real depthC, depthS;                      // drop depth past the mouth line (units of R)
    real visOffC, visRC, visOffS, visRS;      // rendered pocket hole
};
inline void poolSpec(TableSpec& t) {
    t.L = 2.54; t.W = 1.27; t.cornerMouth = 0.118; t.sideMouth = 0.130; t.jawC = 0.07; t.jawS = 0.05;
    t.cornerFacingDeg = 142; t.sideFacingDeg = 104; t.filletC = 0; t.filletS = 0;
    t.depthC = 0.6; t.depthS = 0.9; t.visOffC = 0.004; t.visRC = 0.064; t.visOffS = 0.032; t.visRS = 0.062;
}
inline void snookerSpec(TableSpec& t) {
    t.L = 3.569; t.W = 1.778; t.cornerMouth = 0.092; t.sideMouth = 0.102; t.jawC = 0.06; t.jawS = 0.045;
    t.cornerFacingDeg = 140; t.sideFacingDeg = 102; t.filletC = 0.010; t.filletS = 0.014;
    t.depthC = 0.7; t.depthS = 1.0; t.visOffC = 0.0; t.visRC = 0.050; t.visOffS = 0.028; t.visRS = 0.050;
}

struct JawGeo { real Tr[2], Tf[2], E[2], C[2], rj; int narc; real ax[10], ay[10]; };
// J: jaw nose point (intersection of rail nose line and facing), dr: along the rail away
// from the pocket, df: along the facing into the pocket.
inline void makeJaw(JawGeo& g, real Jx, real Jy, real drx, real dry, real dfx, real dfy, real len, real rj) {
    g.rj = rj;
    g.E[0] = Jx + dfx * len; g.E[1] = Jy + dfy * len;
    if (rj <= 0) {
        g.Tr[0] = g.Tf[0] = g.C[0] = Jx; g.Tr[1] = g.Tf[1] = g.C[1] = Jy;
        g.narc = 1; g.ax[0] = Jx; g.ay[0] = Jy; return;
    }
    real ca = drx * dfx + dry * dfy;                 // cos(alpha)
    real sh = psqrt((1 - ca) / 2), ch = psqrt((1 + ca) / 2), th = sh / ch;
    real bx = drx + dfx, by = dry + dfy, bl = psqrt(bx * bx + by * by); bx /= bl; by /= bl;
    g.C[0] = Jx + bx * rj / sh; g.C[1] = Jy + by * rj / sh;
    g.Tr[0] = Jx + drx * rj / th; g.Tr[1] = Jy + dry * rj / th;
    g.Tf[0] = Jx + dfx * rj / th; g.Tf[1] = Jy + dfy * rj / th;
    // arc from Tf to Tr on the exposed side (shorter way)
    real a0 = patan2(g.Tf[1] - g.C[1], g.Tf[0] - g.C[0]), a1 = patan2(g.Tr[1] - g.C[1], g.Tr[0] - g.C[0]);
    real d = a1 - a0; while (d > PI) d -= TWO_PI; while (d < -PI) d += TWO_PI;
    g.narc = 10;
    for (int i = 0; i < 10; ++i) { real a = a0 + d * i / 9.0; g.ax[i] = g.C[0] + rj * pcos(a); g.ay[i] = g.C[1] + rj * psin(a); }
}

inline void buildTable(Table& T, const Params& P, const TableSpec& sp, TableVis* vis = 0) {
    T.L = sp.L; T.W = sp.W; T.nseg = 0; T.npt = 0; T.npk = 0;
    const real hx = T.L / 2, hy = T.W / 2;
    const real c = sp.cornerMouth / 1.4142135623730951;
    const real sh = sp.sideMouth / 2;
    const real be = (180 - sp.cornerFacingDeg) * PI / 180, cb = pcos(be), sb = psin(be);
    const real sa = sp.sideFacingDeg * PI / 180, sdx = pcos(sa), sdy = psin(sa);
    T.safeX = hx - P.R - 0.0005; T.safeY = hy - P.R - 0.0005;
    // jaws, indexed: corner (sx,sy) long-rail jaw / short-rail jaw, side jaws
    JawGeo cl[2][2], cs[2][2], sd[2][2];   // [sx][sy], side: [sy][+/-]
    for (int ix = 0; ix < 2; ++ix) for (int iy = 0; iy < 2; ++iy) {
        real sx = ix ? 1 : -1, sy = iy ? 1 : -1;
        makeJaw(cl[ix][iy], sx * (hx - c), sy * hy, -sx, 0, sx * cb, sy * sb, sp.jawC, sp.filletC);
        makeJaw(cs[ix][iy], sx * hx, sy * (hy - c), 0, -sy, sx * sb, sy * cb, sp.jawC, sp.filletC);
    }
    for (int iy = 0; iy < 2; ++iy) {
        real sy = iy ? 1 : -1;
        makeJaw(sd[iy][1], sh, sy * hy, 1, 0, sdx, sy * sdy, sp.jawS, sp.filletS);
        makeJaw(sd[iy][0], -sh, sy * hy, -1, 0, -sdx, sy * sdy, sp.jawS, sp.filletS);
    }
    // rails between jaw tangent points
    for (int iy = 0; iy < 2; ++iy) {
        addSeg(T, cl[0][iy].Tr[0], cl[0][iy].Tr[1], sd[iy][0].Tr[0], sd[iy][0].Tr[1], 0, 0);
        addSeg(T, sd[iy][1].Tr[0], sd[iy][1].Tr[1], cl[1][iy].Tr[0], cl[1][iy].Tr[1], 0, 0);
    }
    for (int ix = 0; ix < 2; ++ix) addSeg(T, cs[ix][0].Tr[0], cs[ix][0].Tr[1], cs[ix][1].Tr[0], cs[ix][1].Tr[1], 0, 0);
    // pockets + facings + noses
    for (int ix = 0; ix < 2; ++ix) for (int iy = 0; iy < 2; ++iy) {
        real sx = ix ? 1 : -1, sy = iy ? 1 : -1;
        real j1x = sx * (hx - c), j1y = sy * hy, j2x = sx * hx, j2y = sy * (hy - c);
        Pocket& pk = T.pk[T.npk++];
        pk.mx = (j1x + j2x) / 2; pk.my = (j1y + j2y) / 2;
        pk.nx = sx / 1.4142135623730951; pk.ny = sy / 1.4142135623730951;
        pk.tx = -pk.ny; pk.ty = pk.nx;
        pk.halfw = sp.cornerMouth / 2; pk.depth = sp.depthC * P.R;
        pk.cx = sx * (hx + sp.visOffC); pk.cy = sy * (hy + sp.visOffC); pk.r = sp.visRC;
        real thx = pk.mx + pk.nx * 0.03, thy = pk.my + pk.ny * 0.03;
        JawGeo* js[2] = {&cl[ix][iy], &cs[ix][iy]};
        for (int k = 0; k < 2; ++k) {
            addSeg(T, js[k]->Tf[0], js[k]->Tf[1], js[k]->E[0], js[k]->E[1], thx, thy);
            addPt(T, js[k]->C[0], js[k]->C[1], js[k]->rj);
        }
    }
    for (int iy = 0; iy < 2; ++iy) {
        real sy = iy ? 1 : -1;
        Pocket& pk = T.pk[T.npk++];
        pk.mx = 0; pk.my = sy * hy; pk.nx = 0; pk.ny = sy; pk.tx = 1; pk.ty = 0;
        pk.halfw = sp.sideMouth / 2; pk.depth = sp.depthS * P.R;
        pk.cx = 0; pk.cy = sy * (hy + sp.visOffS); pk.r = sp.visRS;
        for (int k = 0; k < 2; ++k) {
            JawGeo& j = sd[iy][k];
            addSeg(T, j.Tf[0], j.Tf[1], j.E[0], j.E[1], 0, sy * (hy + 0.03));
            addPt(T, j.C[0], j.C[1], j.rj);
        }
    }
    if (vis) {   // cushion outlines: facing end -> arc -> rail -> arc -> facing end
        vis->n = 6;
        auto emit = [&](int ci, JawGeo& A, JawGeo& B) {
            int& n = vis->cnt[ci]; n = 0;
            vis->x[ci][n] = A.E[0]; vis->y[ci][n++] = A.E[1];
            for (int i = 0; i < A.narc; ++i) { vis->x[ci][n] = A.ax[i]; vis->y[ci][n++] = A.ay[i]; }
            for (int i = B.narc - 1; i >= 0; --i) { vis->x[ci][n] = B.ax[i]; vis->y[ci][n++] = B.ay[i]; }
            vis->x[ci][n] = B.E[0]; vis->y[ci][n++] = B.E[1];
        };
        emit(0, cl[0][1], sd[1][0]); emit(1, sd[1][1], cl[1][1]);
        emit(2, cl[0][0], sd[0][0]); emit(3, sd[0][1], cl[1][0]);
        emit(4, cs[0][0], cs[0][1]); emit(5, cs[1][0], cs[1][1]);
    }
}
inline void buildPoolTable(Table& T, const Params& P) { TableSpec sp; poolSpec(sp); buildTable(T, P, sp); }

struct Ball {
    real x, y, vx, vy, wx, wy, wz;
    real qw, qx, qy, qz;     // orientation (rendering only)
    int on;                  // 1 = on table, 0 = pocketed / absent
    int pocket;              // pocket index if pocketed
};

struct Events {
    int firstHit;            // first ball contacted by the cue ball (-1 = none)
    int nPot; int pot[MAXB]; int potPocket[MAXB];
    unsigned railAfter;      // balls that touched a cushion after first contact
    unsigned railAny;        // balls that touched a cushion at all
    unsigned touched;        // balls that were hit by anything
    int nBallColl, nCushColl;
    real tEnd;
};

// Optional light-weight event recording (for sounds in the browser)
struct SoundEvent { int type; real strength; real x, y; };   // 1 ball-ball, 2 cushion, 3 pocket

struct Sim {
    Params P; Table T;
    int n; Ball b[MAXB];
    Events ev;
    real t;
    int trackOrient;
    int nSnd; SoundEvent snd[64];
};

inline void pushSound(Sim& s, int type, real str, real x, real y) {
    if (s.nSnd < 64) { SoundEvent& e = s.snd[s.nSnd++]; e.type = type; e.strength = str; e.x = x; e.y = y; }
}

inline void clearEvents(Sim& s) {
    s.ev.firstHit = -1; s.ev.nPot = 0; s.ev.railAfter = 0; s.ev.railAny = 0; s.ev.touched = 0;
    s.ev.nBallColl = 0; s.ev.nCushColl = 0; s.ev.tEnd = 0; s.t = 0;
}

inline bool ballMoving(const Ball& b) {
    return b.on && (b.vx != 0 || b.vy != 0 || b.wx != 0 || b.wy != 0);
}
inline bool anyMoving(const Sim& s) {
    for (int i = 0; i < s.n; ++i) if (ballMoving(s.b[i])) return true;
    return false;
}

// ---------------------------------------------------------------- friction
inline void applyFriction(Sim& s, Ball& b, real dt) {
    const Params& P = s.P;
    const real R = P.R;
    real ux = b.vx - R * b.wy, uy = b.vy + R * b.wx;
    real us = psqrt(ux * ux + uy * uy);
    if (us > 1e-9) {                                         // sliding
        real dec = 3.5 * P.mu_s * P.g * dt;
        real f = us > dec ? 1.0 : us / dec;
        real h = P.mu_s * P.g * dt * f;
        real ex = ux / us, ey = uy / us;
        b.vx -= h * ex; b.vy -= h * ey;
        b.wx -= 2.5 * h * ey / R; b.wy += 2.5 * h * ex / R;
        if (f < 1.0) { b.wx = -b.vy / R; b.wy = b.vx / R; }   // exactly rolling now
    } else {                                                  // rolling
        real sp = psqrt(b.vx * b.vx + b.vy * b.vy);
        if (sp > 0) {
            real dec = P.mu_r * P.g * dt;
            if (sp <= dec) { b.vx = 0; b.vy = 0; }
            else { real k = (sp - dec) / sp; b.vx *= k; b.vy *= k; }
        }
        b.wx = -b.vy / R; b.wy = b.vx / R;
    }
    real dw = P.spinDecel * dt;
    if (b.wz > dw) b.wz -= dw; else if (b.wz < -dw) b.wz += dw; else b.wz = 0;
}

inline void integrateOrientation(Ball& b, real dt) {
    real hw = 0.5 * dt;
    real ax = b.wx * hw, ay = b.wy * hw, az = b.wz * hw;
    // dq = (0,w) * q * dt/2
    real nw = b.qw - (ax * b.qx + ay * b.qy + az * b.qz);
    real nx = b.qx + (ax * b.qw + ay * b.qz - az * b.qy);
    real ny = b.qy + (ay * b.qw + az * b.qx - ax * b.qz);
    real nz = b.qz + (az * b.qw + ax * b.qy - ay * b.qx);
    real l = psqrt(nw * nw + nx * nx + ny * ny + nz * nz);
    b.qw = nw / l; b.qx = nx / l; b.qy = ny / l; b.qz = nz / l;
}

// ---------------------------------------------------------------- collisions
// earliest t in [0, tmax] at which |p + v t| == r, approaching; else -1
inline real toiCircle(real px, real py, real vx, real vy, real r, real tmax) {
    real b = px * vx + py * vy;
    if (b >= 0) return -1;                  // separating
    real a = vx * vx + vy * vy;
    real c = px * px + py * py - r * r;
    if (c <= 0) return 0;                    // already touching / overlapping, approaching
    real disc = b * b - a * c;
    if (disc < 0) return -1;
    real t = (-b - psqrt(disc)) / a;
    return (t <= tmax) ? t : -1;
}

inline void resolveBallBall(Sim& s, int i, int j) {
    const Params& P = s.P;
    Ball& A = s.b[i]; Ball& B = s.b[j];
    const real R = P.R;
    real nx = B.x - A.x, ny = B.y - A.y, d = psqrt(nx * nx + ny * ny);
    nx /= d; ny /= d;
    real vrel = (A.vx - B.vx) * nx + (A.vy - B.vy) * ny;
    if (vrel <= 0) return;
    real Jn = 0.5 * (1 + P.e_ball) * vrel;     // per unit mass
    A.vx -= Jn * nx; A.vy -= Jn * ny;
    B.vx += Jn * nx; B.vy += Jn * ny;
    // contact slip (3D). r_A = R n, r_B = -R n
    real uax = A.vx - A.wz * R * ny, uay = A.vy + A.wz * R * nx, uaz = R * (A.wx * ny - A.wy * nx);
    real ubx = B.vx + B.wz * R * ny, uby = B.vy - B.wz * R * nx, ubz = -R * (B.wx * ny - B.wy * nx);
    real ux = uax - ubx, uy = uay - uby, uz = uaz - ubz;
    real un = ux * nx + uy * ny;
    ux -= un * nx; uy -= un * ny;
    real ut = psqrt(ux * ux + uy * uy + uz * uz);
    if (ut > 1e-12) {
        real Jt = ut / 7.0;                   // impulse (per unit mass) to stop slip
        real Jmax = P.mu_ball * Jn;
        if (Jt > Jmax) Jt = Jmax;
        real jx = -Jt * ux / ut, jy = -Jt * uy / ut, jz = -Jt * uz / ut;   // on A
        // linear (horizontal part only; vertical absorbed by the slate)
        A.vx += jx; A.vy += jy; B.vx -= jx; B.vy -= jy;
        // angular: dw = (r x J) m / I = (r x j) * 2.5 / R^2
        real k = 2.5 / (R * R);
        // r_A x j  with r_A = (R nx, R ny, 0)
        real tax = R * ny * jz, tay = -R * nx * jz, taz = R * (nx * jy - ny * jx);
        A.wx += k * tax; A.wy += k * tay; A.wz += k * taz;
        // r_B x (-j) with r_B = -r_A  ->  same as r_A x j
        B.wx += k * tax; B.wy += k * tay; B.wz += k * taz;
    }
    // events
    if (s.ev.firstHit < 0) {
        if (i == 0) s.ev.firstHit = j; else if (j == 0) s.ev.firstHit = i;
    }
    s.ev.touched |= (1u << i) | (1u << j);
    s.ev.nBallColl++;
    pushSound(s, 1, vrel, (A.x + B.x) / 2, (A.y + B.y) / 2);
}

inline void resolveCushion(Sim& s, int i, real nx, real ny) {
    const Params& P = s.P;
    Ball& B = s.b[i];
    const real R = P.R;
    real vn = B.vx * nx + B.vy * ny;
    if (vn >= 0) return;
    real e = pclamp(P.e_c0 - P.e_c1 * (-vn), P.e_cmin, 1.0);
    real Jn = -(1 + e) * vn;                   // per unit mass
    B.vx += Jn * nx; B.vy += Jn * ny;
    // contact point r = R(-cos n + sin z)
    real rx = -R * P.cushCos * nx, ry = -R * P.cushCos * ny, rz = R * P.cushSin;
    // u = v + w x r
    real ux = B.vx + (B.wy * rz - B.wz * ry);
    real uy = B.vy + (B.wz * rx - B.wx * rz);
    real uz = 0 + (B.wx * ry - B.wy * rx);
    // remove component along r_hat
    real rh = (ux * rx + uy * ry + uz * rz) / (R * R);
    ux -= rh * rx; uy -= rh * ry; uz -= rh * rz;
    real ut = psqrt(ux * ux + uy * uy + uz * uz);
    if (ut > 1e-12) {
        real Jt = ut * 2.0 / 7.0;
        real Jmax = P.mu_cush * Jn;
        if (Jt > Jmax) Jt = Jmax;
        real jx = -Jt * ux / ut, jy = -Jt * uy / ut, jz = -Jt * uz / ut;
        B.vx += jx; B.vy += jy;
        real k = 2.5 / (R * R);
        B.wx += k * (ry * jz - rz * jy);
        B.wy += k * (rz * jx - rx * jz);
        B.wz += k * (rx * jy - ry * jx);
    }
    if (s.ev.firstHit >= 0) s.ev.railAfter |= (1u << i);
    s.ev.railAny |= (1u << i);
    s.ev.nCushColl++;
    pushSound(s, 2, -vn, B.x, B.y);
}

inline void potBall(Sim& s, int i, int pk) {
    Ball& b = s.b[i];
    b.on = 0; b.pocket = pk;
    b.vx = b.vy = b.wx = b.wy = b.wz = 0;
    if (s.ev.nPot < MAXB) { s.ev.pot[s.ev.nPot] = i; s.ev.potPocket[s.ev.nPot] = pk; s.ev.nPot++; }
    pushSound(s, 3, 1, s.T.pk[pk].cx, s.T.pk[pk].cy);
}

inline void checkPockets(Sim& s) {
    const Table& T = s.T;
    for (int i = 0; i < s.n; ++i) {
        Ball& b = s.b[i];
        if (!b.on) continue;
        for (int k = 0; k < T.npk; ++k) {
            const Pocket& p = T.pk[k];
            real dx = b.x - p.mx, dy = b.y - p.my;
            real dep = dx * p.nx + dy * p.ny;
            real lat = dx * p.tx + dy * p.ty;
            if (dep > p.depth && pabs(lat) < p.halfw + s.P.R) { potBall(s, i, k); break; }
        }
        if (b.on && (pabs(b.x) > T.L / 2 + 0.01 || pabs(b.y) > T.W / 2 + 0.01)) {
            // safety: escaped the table -> nearest pocket
            int best = 0; real bd = 1e9;
            for (int k = 0; k < T.npk; ++k) {
                real dx = b.x - T.pk[k].cx, dy = b.y - T.pk[k].cy, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = k; }
            }
            potBall(s, i, best);
        }
    }
}

// Advance all balls by dt with exact collision handling inside the step.
inline void drift(Sim& s, real dt) {
    const real R = s.P.R;
    const Table& T = s.T;
    real trem = dt;
    int mv[MAXB], nm;
    for (int guard = 0; guard < 256 && trem > 0; ++guard) {
        nm = 0;
        for (int i = 0; i < s.n; ++i) if (s.b[i].on && (s.b[i].vx != 0 || s.b[i].vy != 0)) mv[nm++] = i;
        if (!nm) return;
        real tmin = trem; int type = 0, ia = -1, ib = -1; real cnx = 0, cny = 0;
        for (int q = 0; q < nm; ++q) {
            const int i = mv[q];
            const Ball& A = s.b[i];
            const real spA = psqrt(A.vx * A.vx + A.vy * A.vy);
            // ball-ball: each moving ball against every ball (pairs of two movers once)
            for (int j = 0; j < s.n; ++j) {
                if (j == i) continue;
                const Ball& B = s.b[j];
                if (!B.on) continue;
                bool bm = (B.vx != 0 || B.vy != 0);
                if (bm && j < i) continue;
                real px = B.x - A.x, py = B.y - A.y, vx = B.vx - A.vx, vy = B.vy - A.vy;
                real reach = 2 * R + psqrt(vx * vx + vy * vy) * tmin;
                if (px * px + py * py > reach * reach) continue;
                real t = toiCircle(px, py, vx, vy, 2 * R, tmin);
                if (t >= 0 && (t < tmin || type == 0)) { tmin = t; type = 1; ia = i < j ? i : j; ib = i < j ? j : i; }
            }
            // cushions: broadphase against the safe inner rectangle
            const real reachW = spA * tmin;
            if (pabs(A.x) + reachW < T.safeX && pabs(A.y) + reachW < T.safeY) continue;
            for (int k = 0; k < T.nseg; ++k) {
                const Seg& g = T.seg[k];
                real vn = A.vx * g.nx + A.vy * g.ny;
                if (vn >= 0) continue;
                real d = (A.x - g.ax) * g.nx + (A.y - g.ay) * g.ny;
                if (d < R - 0.005) continue;           // other side / far inside: not this face
                real t = (d - R) / (-vn);
                if (t < 0) t = 0;
                if (t > tmin) continue;
                real cx = A.x + A.vx * t - g.ax, cy = A.y + A.vy * t - g.ay;
                real sp = cx * g.dx + cy * g.dy;
                if (sp < 0 || sp > g.len) continue;
                if (t < tmin || type == 0) { tmin = t; type = 2; ia = i; cnx = g.nx; cny = g.ny; }
            }
            for (int k = 0; k < T.npt; ++k) {
                real px = A.x - T.ptx[k], py = A.y - T.pty[k];
                real t = toiCircle(px, py, A.vx, A.vy, R + T.ptr[k], tmin);
                if (t >= 0 && (t < tmin || type == 0)) {
                    real hx = px + A.vx * t, hy = py + A.vy * t, l = psqrt(hx * hx + hy * hy);
                    tmin = t; type = 2; ia = i; cnx = hx / l; cny = hy / l;
                }
            }
        }
        for (int q = 0; q < nm; ++q) { Ball& b = s.b[mv[q]]; b.x += b.vx * tmin; b.y += b.vy * tmin; }
        trem -= tmin;
        if (type == 1) resolveBallBall(s, ia, ib);
        else if (type == 2) { resolveCushion(s, ia, cnx, cny); checkPockets(s); }
        else break;
    }
}

// One simulation substep (use SIM_DT)
inline void step(Sim& s, real dt) {
    for (int i = 0; i < s.n; ++i) if (s.b[i].on) applyFriction(s, s.b[i], dt * 0.5);
    drift(s, dt);
    checkPockets(s);
    for (int i = 0; i < s.n; ++i) {
        Ball& b = s.b[i];
        if (!b.on) continue;
        applyFriction(s, b, dt * 0.5);
        if (s.trackOrient) integrateOrientation(b, dt);
    }
    s.t += dt;
}

// Map power fraction p in [0,1] to cue-ball speed for a centre hit.
inline real powerToSpeed(const Params& P, real p) {
    p = pclamp(p, 0, 1);
    return P.vmin + (P.vmax - P.vmin) * pexp(P.powExp * plog(p > 1e-9 ? p : 1e-9));
}

// Cue strike on ball 0. phi: cue direction (rad), V: centre-hit ball speed,
// a: side offset (+ = right of centre, units of R), b: height (+ = above centre).
inline void strike(Sim& s, real phi, real V, real a, real b) {
    const Params& P = s.P;
    real off2 = a * a + b * b;
    if (off2 > P.maxOffset * P.maxOffset) {
        real k = P.maxOffset / psqrt(off2); a *= k; b *= k; off2 = P.maxOffset * P.maxOffset;
    }
    real mr = P.massRatio;
    real v0 = V * (1 + mr) / (1 + mr + 2.5 * off2);
    real dx = pcos(phi), dy = psin(phi);
    real phis = phi + P.squirt * a;                 // squirt: deflect opposite to english
    Ball& c = s.b[0];
    c.vx = v0 * pcos(phis); c.vy = v0 * psin(phis);
    real k = 2.5 * v0 / P.R;
    c.wz = k * a;
    c.wx = -k * b * dy;
    c.wy = k * b * dx;
    clearEvents(s);
    s.nSnd = 0;
}

// Fixed-step simulation until rest (or tmax). Returns simulated time. The game and the bots
// use the event-driven engine (events.h, simulate()); this stepper is the reference it is
// validated against (tests/ev_test.cpp) and is used when POOL_STEPPER is defined.
inline real simulateStep(Sim& s, real dt = SIM_DT, real tmax = 60) {
    while (anyMoving(s) && s.t < tmax) step(s, dt);
    for (int i = 0; i < s.n; ++i) { s.b[i].wz = 0; s.b[i].vx = s.b[i].vy = 0; s.b[i].wx = s.b[i].wy = 0; }
    s.ev.tEnd = s.t;
    return s.t;
}

// -------------------------------------------------------------- aiming aid
struct AimInfo {
    int type;              // 0 none, 1 ball, 2 cushion
    int target;
    real gx, gy;           // ghost ball (cue-ball centre at contact)
    real ox, oy;           // object ball direction (unit)
    real cx, cy;           // cue ball direction after contact (stun line, unit)
    real dirx, diry;       // actual travel direction of the cue ball
};

inline void aimInfo(const Sim& s, real phi, real a, AimInfo& out) {
    const real R = s.P.R;
    const Ball& c = s.b[0];
    real phis = phi + s.P.squirt * a;
    real dx = pcos(phis), dy = psin(phis);
    out.dirx = dx; out.diry = dy;
    real tbest = 1e9; out.type = 0; out.target = -1;
    for (int j = 1; j < s.n; ++j) {
        const Ball& B = s.b[j];
        if (!B.on) continue;
        real t = toiCircle(c.x - B.x, c.y - B.y, dx, dy, 2 * R, 1e9);
        if (t >= 0 && t < tbest) { tbest = t; out.type = 1; out.target = j; }
    }
    real nxB = 0, nyB = 0;
    for (int k = 0; k < s.T.nseg; ++k) {
        const Seg& g = s.T.seg[k];
        real vn = dx * g.nx + dy * g.ny;
        if (vn >= 0) continue;
        real d = (c.x - g.ax) * g.nx + (c.y - g.ay) * g.ny;
        if (d < R - 1e-6) continue;
        real t = (d - R) / (-vn);
        real px = c.x + dx * t - g.ax, py = c.y + dy * t - g.ay, sp = px * g.dx + py * g.dy;
        if (sp < 0 || sp > g.len) continue;
        if (t < tbest) { tbest = t; out.type = 2; out.target = k; nxB = g.nx; nyB = g.ny; }
    }
    for (int k = 0; k < s.T.npt; ++k) {
        real t = toiCircle(c.x - s.T.ptx[k], c.y - s.T.pty[k], dx, dy, R + s.T.ptr[k], 1e9);
        if (t >= 0 && t < tbest) {
            tbest = t; out.type = 2; out.target = -2;
            real hx = c.x + dx * t - s.T.ptx[k], hy = c.y + dy * t - s.T.pty[k], l = psqrt(hx * hx + hy * hy);
            nxB = hx / l; nyB = hy / l;
        }
    }
    if (out.type == 0) { tbest = 3; }
    out.gx = c.x + dx * tbest; out.gy = c.y + dy * tbest;
    if (out.type == 1) {
        const Ball& B = s.b[out.target];
        real nx = B.x - out.gx, ny = B.y - out.gy, l = psqrt(nx * nx + ny * ny);
        nx /= l; ny /= l;
        out.ox = nx; out.oy = ny;
        real vn = dx * nx + dy * ny;
        real tx = dx - vn * nx, ty = dy - vn * ny, tl = psqrt(tx * tx + ty * ty);
        if (tl > 1e-9) { out.cx = tx / tl; out.cy = ty / tl; } else { out.cx = 0; out.cy = 0; }
    } else if (out.type == 2) {
        real vn = dx * nxB + dy * nyB;
        out.cx = dx - 2 * vn * nxB; out.cy = dy - 2 * vn * nyB;
        out.ox = nxB; out.oy = nyB;
    } else { out.ox = out.oy = out.cx = out.cy = 0; }
}

// -------------------------------------------------- human-error (noise) model
// Signal-dependent motor noise (Harris & Wolpert 1998), per input channel c:
//   s = V / vmax                                  (stroke speed as a fraction of the maximum)
//   sigma_c(s) = skill * ( lo_c + (hi_c - lo_c) * s^k_c )
//   executed_c = intended_c + mu_c + m * sigma_c(s) * N(0,1)
// channels: 0 aim [rad], 1 speed [relative], 2 side tip offset a [R], 3 height tip offset b [R].
// lo = slip at a gentle touch, hi = slip at full power, k = curve shape (1 = linear),
// mu = systematic tendency (bias). With probability missProb a stroke is "bad":
// every sigma is multiplied by m = missMul for that stroke.
enum { NZ_AIM = 0, NZ_SPEED = 1, NZ_SIDE = 2, NZ_HEIGHT = 3 };
struct NoiseModel {
    real skill;
    real lo[4], hi[4], k[4], mu[4];
    real missProb, missMul;
};
inline void defaultNoise(NoiseModel& n) {
    n.skill = 1.0;
    n.lo[0] = 0.0010; n.hi[0] = 0.0080;   // aim: 1 -> 8 mm per metre
    n.lo[1] = 0.015;  n.hi[1] = 0.075;    // speed: 1.5 % -> 7.5 %
    n.lo[2] = 0.015;  n.hi[2] = 0.095;    // tip side / height [R]
    n.lo[3] = 0.015;  n.hi[3] = 0.095;
    for (int c = 0; c < 4; ++c) { n.k[c] = 1; n.mu[c] = 0; }
    n.missProb = 0; n.missMul = 3;
}
inline real noiseSigma(const NoiseModel& n, int c, real s) {
    if (s < 0) s = 0; if (s > 1) s = 1;
    real f = s <= 0 ? 0 : pexp(n.k[c] * plog(s));
    return n.skill * (n.lo[c] + (n.hi[c] - n.lo[c]) * f);
}
struct ShotParams { real phi, V, a, b; };
inline void applyNoise(const NoiseModel& n, const Params& P, Rng& rng, ShotParams& sp) {
    real s = sp.V / P.vmax;
    real m = 1;
    if (n.missProb > 0 && rng.uniform() < n.missProb) m = n.missMul;
    sp.phi += n.mu[0] + m * noiseSigma(n, 0, s) * rng.gauss();
    sp.V *= 1 + n.mu[1] + m * noiseSigma(n, 1, s) * rng.gauss();
    if (sp.V < 0.02) sp.V = 0.02;
    sp.a += n.mu[2] + m * noiseSigma(n, 2, s) * rng.gauss();
    sp.b += n.mu[3] + m * noiseSigma(n, 3, s) * rng.gauss();
}

} // namespace pool

#include "events.h"   // simulate(), simulateFirstHit()
