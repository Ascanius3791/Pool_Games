// events.h — event-driven (analytic) simulation for the physics model in physics.h
//
// Between events every ball moves on a closed-form trajectory:
//   sliding : r(t) = r0 + v0 t + a t^2 / 2, a = -mu_s g u_hat (u = contact slip, constant
//             direction), until the slip vanishes at t = |u0| / (3.5 mu_s g)
//   rolling : straight line, a = -mu_r g v_hat, until v = 0 at t = |v0| / (mu_r g)
//   wz      : decays linearly at spinDecel (does not move the ball)
// Events:
//   phase changes (slide -> roll -> rest)
//   ball-ball contact     |d(t)|^2 = (2R)^2              quartic
//   jaw nose (circle)     |r(t) - C|^2 = (R + r_jaw)^2   quartic
//   rail / facing line    n.(r(t) - a) = R              quadratic, contact point inside the segment
//   pocket capture        depth past the mouth line      quadratic
// Roots are found robustly: the polynomial is split at its critical points (roots of the
// derivative, found the same way) into monotone pieces, and the first piece on which the
// function is decreasing and reaches <= 0 holds the event ("entering" contact). A contact
// that is touching and approaching counts at once; touching and separating does not.
//
// Ordering: the earliest event is always processed next. Every prediction involving a ball
// is recomputed as soon as that ball's state changes, so no event is ever processed with a
// stale trajectory. Simultaneous events are processed one after another at the same time
// in a fixed order (phase changes, then table events by ball index, then pairs by index);
// after each one the affected predictions are recomputed, so e.g. a ball pinned between a
// cushion and another ball bounces back and forth correctly at the same instant.
//
// The collision responses are exactly those of physics.h (resolveBallBall, resolveCushion,
// potBall), so the physics is unchanged; only the time integration is exact.
#pragma once
#include "physics.h"

namespace pool {

const real EV_INF = 1e30;
const real EV_VSEP = 0.001;        // minimum separation speed after a contact (m/s), see evFixSep
const int EV_MAXEV = 6000;         // safety cap on events per shot

struct EvBall {
    real x, y, vx, vy, wx, wy, wz;  // state at time tb
    real ax, ay;                    // acceleration during the current phase
    real tb, te;                    // phase start / end (absolute), te = EV_INF at rest
    real bx0, bx1, by0, by1;        // bounding box of the centre over the whole phase
    int ph;                         // -1 off the table, 0 at rest, 1 sliding, 2 rolling
};

enum { EVT_NONE = 0, EVT_PHASE = 1, EVT_TABLE = 2, EVT_PAIR = 3 };
enum { EVK_SEG = 1, EVK_CIRC = 2, EVK_POCKET = 3, EVK_ESCAPE = 4 };

struct EvEngine {
    int n;
    EvBall e[MAXB];
    real tp[MAXB][MAXB];                 // tp[i][j] (i < j): next contact time of the pair
    real rowMin[MAXB]; int rowArg[MAXB];   // min over j > i of tp[i][j] (first such j)
    real tc[MAXB]; int ck[MAXB], ci[MAXB]; // next table event of ball i: time, kind, index
    real now;
    int nEv;                             // events processed this shot
    int capped;                          // 1 if the safety cap stopped the shot
    int nBadEscape;                      // balls that left the table away from a pocket mouth (bug indicator)
    real tDisp;                          // time of the last display materialisation
    // optional event log (tests)
    int logOn, nLog; struct Rec { real t; int type, a, b, k; } log[512];
};

// ------------------------------------------------------------------ polynomials
inline real evPoly(const real* c, int deg, real s) {
    real r = c[deg];
    for (int k = deg - 1; k >= 0; --k) r = r * s + c[k];
    return r;
}
// root of a polynomial that is monotone on [a,b] with fa, fb of opposite sign (fb may be 0).
// Illinois-modified regula falsi, falling back to bisection; ~1e-13 s precision.
inline real evMonoRoot(const real* c, int deg, real a, real b, real fa, real fb) {
    if (fb == 0) return b;
    if (fa == 0) return a;
    int side = 0;
    for (int it = 0; it < 80; ++it) {
        real m = (a * fb - b * fa) / (fb - fa);
        if (!(m > a && m < b)) m = 0.5 * (a + b);
        if (b - a < 1e-13 * (1 + b)) return b;
        real fm = evPoly(c, deg, m);
        if (fm == 0) return m;
        if ((fm < 0) == (fb < 0)) { b = m; fb = fm; if (side == -1) fa *= 0.5; side = -1; }
        else { a = m; fa = fm; if (side == 1) fb *= 0.5; side = 1; }
        if (it > 40) { // stubborn: plain bisection steps
            real h = 0.5 * (a + b), fh = evPoly(c, deg, h);
            if ((fh < 0) == (fb < 0)) { b = h; fb = fh; } else { a = h; fa = fh; }
        }
    }
    return b;
}
// roots of q2 s^2 + q1 s + q0 in (lo, hi), sorted; returns count
inline int evQuadRoots(real q2, real q1, real q0, real lo, real hi, real* r) {
    int n = 0; real x0, x1;
    if (q2 == 0) {
        if (q1 == 0) return 0;
        x0 = -q0 / q1; if (x0 > lo && x0 < hi) r[n++] = x0; return n;
    }
    real disc = q1 * q1 - 4 * q2 * q0;
    if (disc < 0) return 0;
    real sq = psqrt(disc);
    real q = -0.5 * (q1 + (q1 >= 0 ? sq : -sq));
    if (q == 0) { x0 = 0; x1 = 0; }
    else { x0 = q / q2; x1 = q0 / q; }
    if (x0 > x1) { real t = x0; x0 = x1; x1 = t; }
    if (x0 > lo && x0 < hi) r[n++] = x0;
    if (x1 > lo && x1 < hi && (n == 0 || x1 != r[0])) r[n++] = x1;
    return n;
}
// Earliest s in [0, S] at which f crosses from > 0 to <= 0 ("entering"); -1 if none.
// A piece that starts at f in [-tol, 0] and decreases counts as entering at once (touching
// or overlapping and approaching, e.g. right after a contact or for frozen balls); with a
// finite tol a piece starting below -tol is "inside" and not an entry. Balls and jaw noses
// use tol = infinity (convex: overlapping and approaching always collides); rail lines too,
// where the contact-point-inside-the-segment test rejects balls that are behind the line
// in a pocket mouth. deg is 2 or 4 (a quartic with zero leading terms is fine too).
inline real evFirstEntry(const real* c, int deg, real S, real tol) {
    real crit[4]; int nc = 0;
    real d[4];                                   // f'
    if (deg == 2) {
        d[0] = c[1]; d[1] = 2 * c[2];
        if (d[1] != 0) { real r = -d[0] / d[1]; if (r > 0 && r < S) crit[nc++] = r; }
    } else {
        d[0] = c[1]; d[1] = 2 * c[2]; d[2] = 3 * c[3]; d[3] = 4 * c[4];
        real q[2]; int nq = evQuadRoots(3 * d[3], 2 * d[2], d[1], 0, S, q);   // f'' roots
        real a = 0, fa = d[0];
        for (int k = 0; k <= nq; ++k) {
            real b = k < nq ? q[k] : S, fb = evPoly(d, 3, b);
            if ((fa < 0 && fb > 0) || (fa > 0 && fb < 0)) {
                real r = evMonoRoot(d, 3, a, b, fa, fb);
                if (r > 0 && r < S && (nc == 0 || r > crit[nc - 1])) crit[nc++] = r;
            }
            a = b; fa = fb;
        }
    }
    const int dd = deg == 2 ? 1 : 3;
    real a = 0, fa = c[0];
    for (int k = 0; k <= nc; ++k) {
        real b = k < nc ? crit[k] : S;
        real fb = evPoly(c, deg, b);
        real slope = evPoly(d, dd, 0.5 * (a + b));
        if (slope < 0 || (slope == 0 && fb < fa)) {
            if (fa <= 0) { if (fa >= -tol) return a; }
            else if (fb <= 0) return evMonoRoot(c, deg, a, b, fa, fb);
        }
        a = b; fa = fb;
    }
    return -1;
}

// Same semantics for a quadratic c0 + c1 s + c2 s^2 with tol = infinity, in closed form.
inline real evFirstEntry2(real c0, real c1, real c2, real S) {
    if (c0 <= 0) {
        if (c1 < 0 || (c1 == 0 && c2 < 0)) return 0;        // touching / inside and approaching
        if (c2 >= 0) return -1;                              // never decreasing again
        real sv = -c1 / (2 * c2);                            // vertex (maximum)
        if (sv >= S) return -1;
        real fv = c0 + (c1 + c2 * sv) * sv;
        if (fv <= 0) return sv;
        // crossing after the vertex: larger root
        real disc = c1 * c1 - 4 * c2 * c0;
        real sq = psqrt(disc > 0 ? disc : 0);
        real q = -0.5 * (c1 + (c1 >= 0 ? sq : -sq));
        real r0 = q / c2, r1 = q != 0 ? c0 / q : r0;
        real r = r0 > r1 ? r0 : r1;
        return (r >= 0 && r <= S) ? r : -1;
    }
    if (c2 == 0) {
        if (c1 >= 0) return -1;
        real r = -c0 / c1;
        return r <= S ? r : -1;
    }
    real disc = c1 * c1 - 4 * c2 * c0;
    if (disc < 0) return -1;                                 // stays positive
    real sq = psqrt(disc);
    real q = -0.5 * (c1 + (c1 >= 0 ? sq : -sq));
    real r0 = q / c2, r1 = q != 0 ? c0 / q : r0;
    if (r0 > r1) { real t = r0; r0 = r1; r1 = t; }
    // entering root: f' < 0 there -> smaller root if c2 > 0, larger if c2 < 0
    real r = c2 > 0 ? r0 : r1;
    if (r < 0) return -1;                                    // (c2 > 0, both roots < 0: never; c2 < 0: r1 > 0 always)
    return r <= S ? r : -1;
}

// Is the quadratic Bezier curve D0 -> (control Dc) -> D1 farther than r from the origin?
// (The curve lies inside the triangle of its control points.)  Cheap rejection test.
inline bool evFar(real x0, real y0, real xc, real yc, real x1, real y1, real r) {
    // bounding box first
    real mnx = pmin(x0, pmin(xc, x1)), mxx = pmax(x0, pmax(xc, x1));
    real mny = pmin(y0, pmin(yc, y1)), mxy = pmax(y0, pmax(yc, y1));
    if (mnx > r || mxx < -r || mny > r || mxy < -r) return true;
    // origin inside the triangle?
    real c0 = x0 * yc - y0 * xc, c1 = xc * y1 - yc * x1, c2 = x1 * y0 - y1 * x0;
    if ((c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0)) return false;
    // distance to the three edges
    real r2 = r * r;
    real px[3] = {x0, xc, x1}, py[3] = {y0, yc, y1};
    for (int k = 0; k < 3; ++k) {
        real ax = px[k], ay = py[k], bx = px[(k + 1) % 3], by = py[(k + 1) % 3];
        real ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
        real t = l2 > 0 ? -(ax * ex + ay * ey) / l2 : 0;
        t = pclamp(t, 0, 1);
        real qx = ax + ex * t, qy = ay + ey * t;
        if (qx * qx + qy * qy <= r2) return false;
    }
    return true;
}

// ------------------------------------------------------------------ ball states
inline void evPhase(EvBall& b, const Params& P) {
    const real R = P.R;
    real ux = b.vx - R * b.wy, uy = b.vy + R * b.wx;
    real us = psqrt(ux * ux + uy * uy);
    if (us > 1e-9) {
        real k = P.mu_s * P.g / us;
        b.ax = -k * ux; b.ay = -k * uy;
        b.ph = 1; b.te = b.tb + us / (3.5 * P.mu_s * P.g);
        return;
    }
    b.wx = -b.vy / R; b.wy = b.vx / R;
    real sp = psqrt(b.vx * b.vx + b.vy * b.vy);
    if (sp > 0) {
        real k = P.mu_r * P.g / sp;
        b.ax = -k * b.vx; b.ay = -k * b.vy;
        b.ph = 2; b.te = b.tb + sp / (P.mu_r * P.g);
    } else {
        b.vx = b.vy = b.wx = b.wy = 0; b.ax = b.ay = 0;
        b.ph = 0; b.te = EV_INF;
    }
}
inline real evWz(const EvBall& b, const Params& P, real t) {
    real d = P.spinDecel * (t - b.tb);
    if (b.wz > d) return b.wz - d;
    if (b.wz < -d) return b.wz + d;
    return 0;
}
// position and velocity at time t (t inside the current phase)
inline void evPV(const EvBall& b, real t, real& x, real& y, real& vx, real& vy) {
    if (b.ph <= 0) { x = b.x; y = b.y; vx = vy = 0; return; }
    real tau = t - b.tb;
    if (tau > b.te - b.tb) tau = b.te - b.tb;
    if (tau < 0) tau = 0;
    x = b.x + (b.vx + 0.5 * b.ax * tau) * tau;
    y = b.y + (b.vy + 0.5 * b.ay * tau) * tau;
    vx = b.vx + b.ax * tau; vy = b.vy + b.ay * tau;
}
// full state at time t into a Ball (orientation untouched)
inline void evAt(const EvBall& b, const Params& P, real t, Ball& o) {
    evPV(b, t, o.x, o.y, o.vx, o.vy);
    const real R = P.R;
    if (b.ph == 1) {
        real tau = pclamp(t - b.tb, 0, b.te - b.tb);
        o.wx = b.wx + 2.5 * b.ay / R * tau;
        o.wy = b.wy - 2.5 * b.ax / R * tau;
    } else if (b.ph == 2) { o.wx = -o.vy / R; o.wy = o.vx / R; }
    else { o.wx = o.wy = 0; }
    o.wz = b.ph < 0 ? 0 : evWz(b, P, t);
}
inline void evBox(EvBall& e) {
    if (e.ph <= 0) { e.bx0 = e.bx1 = e.x; e.by0 = e.by1 = e.y; return; }
    // the path is a quadratic Bezier: P0, P0 + v T/2, P(T) -> inside the box of these points
    real T = e.te - e.tb;
    real cx = e.x + 0.5 * e.vx * T, cy = e.y + 0.5 * e.vy * T;
    real ex = e.x + (e.vx + 0.5 * e.ax * T) * T, ey = e.y + (e.vy + 0.5 * e.ay * T) * T;
    e.bx0 = pmin(e.x, pmin(cx, ex)); e.bx1 = pmax(e.x, pmax(cx, ex));
    e.by0 = pmin(e.y, pmin(cy, ey)); e.by1 = pmax(e.y, pmax(cy, ey));
}
inline void evLoad(EvBall& e, const Ball& b, const Params& P, real t) {
    if (!b.on) { e.ph = -1; e.x = b.x; e.y = b.y; e.vx = e.vy = e.wx = e.wy = e.wz = 0; e.ax = e.ay = 0; e.tb = t; e.te = EV_INF; evBox(e); return; }
    e.x = b.x; e.y = b.y; e.vx = b.vx; e.vy = b.vy; e.wx = b.wx; e.wy = b.wy; e.wz = b.wz;
    e.tb = t;
    evPhase(e, P);
    evBox(e);
}

// First entry of the path D + V s + A s^2 / 2 into the disc |x| <= r (relative motion of two
// balls, or a ball against a jaw nose), s in [0, S]; -1 if none.
// Most paths are straight (A parallel to V: rolling balls, straight stun/screw/follow, one ball
// at rest), and then the entry is closed-form: the path length lambda(s) = |V| s + a s^2 / 2
// reaches the disc at lambda* (a quadratic in lambda), then s from lambda(s) = lambda*.
// Curved paths (sliding with sideways slip, two balls on different lines) use the quartic.
inline real evDiscEntry(real Dx, real Dy, real Vx, real Vy, real Ax, real Ay, real S, real r) {
    const real v2 = Vx * Vx + Vy * Vy, a2 = Ax * Ax + Ay * Ay;
    const real cr = Vx * Ay - Vy * Ax;
    if (v2 > 0 && cr * cr <= 1e-18 * v2 * a2) {
        const real v = psqrt(v2), ux = Vx / v, uy = Vy / v;
        const real a = Ax * ux + Ay * uy;                       // acceleration along the line
        if (a >= 0 || v + a * S >= -1e-12 * v) {                // lambda(s) increasing on [0, S]
            const real b = Dx * ux + Dy * uy, c = Dx * Dx + Dy * Dy - r * r;
            if (c <= 0) return b < 0 ? 0 : -1;                  // overlapping: approaching or not
            if (b >= 0) return -1;                              // moving away along the line
            real disc = b * b - c;
            if (disc < 0) return -1;                            // passes by
            real lam = -b - psqrt(disc);                        // path length to contact (>= 0)
            real lamMax = (v + 0.5 * a * S) * S;
            if (lam > lamMax) return -1;
            real rad = v2 + 2 * a * lam;
            real sq = psqrt(rad > 0 ? rad : 0);
            real t = 2 * lam / (v + sq);
            return t <= S ? t : S;
        }
    }
    real c[5];
    c[4] = 0.25 * a2;
    c[3] = Vx * Ax + Vy * Ay;
    c[2] = v2 + Dx * Ax + Dy * Ay;
    c[1] = 2 * (Dx * Vx + Dy * Vy);
    c[0] = Dx * Dx + Dy * Dy - r * r;
    return evFirstEntry(c, 4, S, EV_INF);
}

// ------------------------------------------------------------------ predictions
inline void evRowScan(EvEngine& E, int i) {
    real m = EV_INF; int a = -1;
    for (int j = i + 1; j < E.n; ++j) if (E.tp[i][j] < m) { m = E.tp[i][j]; a = j; }
    E.rowMin[i] = m; E.rowArg[i] = a;
}
inline void evSetTp(EvEngine& E, int i, int j, real v) {   // i < j
    real old = E.tp[i][j];
    E.tp[i][j] = v;
    if (v < E.rowMin[i] || (v == E.rowMin[i] && v < EV_INF && j < E.rowArg[i])) { E.rowMin[i] = v; E.rowArg[i] = j; }
    else if (E.rowArg[i] == j && v != old) evRowScan(E, i);
}
inline real evPairTime(const EvEngine& E, const Params& P, int i, int j);
inline void evPredPair(EvEngine& E, const Params& P, int i, int j) {
    if (i > j) { int t = i; i = j; j = t; }
    evSetTp(E, i, j, evPairTime(E, P, i, j));
}
inline real evPairTime(const EvEngine& E, const Params& P, int i, int j) {
    const EvBall& A = E.e[i]; const EvBall& B = E.e[j];
    if (A.ph < 0 || B.ph < 0 || (A.ph == 0 && B.ph == 0)) return EV_INF;
    const real r = 2 * P.R;
    if (A.bx0 > B.bx1 + r || B.bx0 > A.bx1 + r || A.by0 > B.by1 + r || B.by0 > A.by1 + r) return EV_INF;
    const real t0 = E.now, t1 = pmin(A.te, B.te);
    const real S = t1 - t0;
    if (!(S >= 0)) return EV_INF;
    real ax, ay, avx, avy, bx, by, bvx, bvy;
    evPV(A, t0, ax, ay, avx, avy); evPV(B, t0, bx, by, bvx, bvy);
    const real Dx = bx - ax, Dy = by - ay, Vx = bvx - avx, Vy = bvy - avy;
    const real Ax = (B.ph > 0 ? B.ax : 0) - (A.ph > 0 ? A.ax : 0), Ay = (B.ph > 0 ? B.ay : 0) - (A.ph > 0 ? A.ay : 0);
    // relative path is a quadratic Bezier: D0, D0 + V S/2, D(S)
    real x1 = Dx + (Vx + 0.5 * Ax * S) * S, y1 = Dy + (Vy + 0.5 * Ay * S) * S;
    if (evFar(Dx, Dy, Dx + 0.5 * Vx * S, Dy + 0.5 * Vy * S, x1, y1, r)) return EV_INF;
    real s = evDiscEntry(Dx, Dy, Vx, Vy, Ax, Ay, S, r);
    return s >= 0 ? t0 + s : EV_INF;
}

inline void evPredTable(EvEngine& E, const Sim& sim, int i) {
    E.tc[i] = EV_INF; E.ck[i] = 0; E.ci[i] = -1;
    const EvBall& A = E.e[i];
    if (A.ph <= 0) return;
    const Params& P = sim.P; const Table& T = sim.T; const real R = P.R;
    const real t0 = E.now, S = A.te - t0;
    if (!(S >= 0)) return;
    real px, py, vx, vy; evPV(A, t0, px, py, vx, vy);
    const real ax = A.ax, ay = A.ay;
    const real cx = px + 0.5 * vx * S, cy = py + 0.5 * vy * S;
    const real ex = px + (vx + 0.5 * ax * S) * S, ey = py + (vy + 0.5 * ay * S) * S;
    real mxx = pmax(pabs(px), pmax(pabs(cx), pabs(ex))), mxy = pmax(pabs(py), pmax(pabs(cy), pabs(ey)));
    if (mxx < T.safeX && mxy < T.safeY) return;            // cannot reach any cushion or pocket
    real best = EV_INF; int kind = 0, idx = -1;
    // rails and facings
    for (int k = 0; k < T.nseg; ++k) {
        const Seg& g = T.seg[k];
        real c[3];
        c[0] = (px - g.ax) * g.nx + (py - g.ay) * g.ny - R;
        c[1] = vx * g.nx + vy * g.ny;
        c[2] = 0.5 * (ax * g.nx + ay * g.ny);
        if (c[0] > 0 && c[1] >= 0 && c[2] >= 0) continue;     // moving away for good
        if (c[0] > (pabs(c[1]) + pabs(c[2]) * S) * S) continue;  // cannot get there in time
        real s = evFirstEntry2(c[0], c[1], c[2], S);
        if (s < 0 || t0 + s >= best) continue;
        real hx = px + (vx + 0.5 * ax * s) * s - g.ax, hy = py + (vy + 0.5 * ay * s) * s - g.ay;
        real sp = hx * g.dx + hy * g.dy;
        if (sp < 0 || sp > g.len) continue;
        best = t0 + s; kind = EVK_SEG; idx = k;
    }
    // jaw noses
    for (int k = 0; k < T.npt; ++k) {
        real Dx = px - T.ptx[k], Dy = py - T.pty[k], r = R + T.ptr[k];
        if (evFar(Dx, Dy, cx - T.ptx[k], cy - T.pty[k], ex - T.ptx[k], ey - T.pty[k], r)) continue;
        real s = evDiscEntry(Dx, Dy, vx, vy, ax, ay, S, r);
        if (s >= 0 && t0 + s < best) { best = t0 + s; kind = EVK_CIRC; idx = k; }
    }
    // pocket capture: depth past the mouth line exceeds p.depth (with the lateral check)
    for (int k = 0; k < T.npk; ++k) {
        const Pocket& p = T.pk[k];
        real c[3];
        c[0] = -((px - p.mx) * p.nx + (py - p.my) * p.ny - p.depth);
        c[1] = -(vx * p.nx + vy * p.ny);
        c[2] = -0.5 * (ax * p.nx + ay * p.ny);
        real s;
        if (c[0] < 0) s = 0;                                  // already past: capture now
        else if (c[0] > (pabs(c[1]) + pabs(c[2]) * S) * S) continue;
        else s = evFirstEntry2(c[0], c[1], c[2], S);
        if (s < 0 || t0 + s >= best) continue;
        real hx = px + (vx + 0.5 * ax * s) * s - p.mx, hy = py + (vy + 0.5 * ay * s) * s - p.my;
        real lat = hx * p.tx + hy * p.ty;
        if (pabs(lat) >= p.halfw + R) continue;
        best = t0 + s; kind = EVK_POCKET; idx = k;
    }
    // safety net: leaving the table area (should never happen)
    for (int k = 0; k < 4; ++k) {
        real nx = k == 0 ? 1 : k == 1 ? -1 : 0, ny = k == 2 ? 1 : k == 3 ? -1 : 0;
        real lim = (k < 2 ? T.L : T.W) / 2 + 0.01;
        real c[3];
        c[0] = -(px * nx + py * ny - lim); c[1] = -(vx * nx + vy * ny); c[2] = -0.5 * (ax * nx + ay * ny);
        real s = c[0] < 0 ? 0 : (c[0] > (pabs(c[1]) + pabs(c[2]) * S) * S ? -1 : evFirstEntry2(c[0], c[1], c[2], S));
        if (s >= 0 && t0 + s < best) { best = t0 + s; kind = EVK_ESCAPE; idx = k; }
    }
    E.tc[i] = best; E.ck[i] = kind; E.ci[i] = idx;
}

inline void evRepredict(EvEngine& E, const Sim& s, int i) {
    const EvBall& A = E.e[i];
    const real r = 2 * s.P.R;
    for (int j = 0; j < E.n; ++j) {
        if (j == i) continue;
        const int lo = i < j ? i : j, hi = i < j ? j : i;
        const EvBall& B = E.e[j];
        // cheap rejection (same tests evPairTime starts with): skip the call when the pair
        // cannot meet and has no stored prediction anyway
        if (A.ph < 0 || B.ph < 0 || (A.ph == 0 && B.ph == 0) ||
            A.bx0 > B.bx1 + r || B.bx0 > A.bx1 + r || A.by0 > B.by1 + r || B.by0 > A.by1 + r) {
            if (E.tp[lo][hi] < EV_INF) evSetTp(E, lo, hi, EV_INF);
            continue;
        }
        evSetTp(E, lo, hi, evPairTime(E, s.P, lo, hi));
    }
    evPredTable(E, s, i);
}

// ------------------------------------------------------------------ engine
inline void evInit(EvEngine& E, const Sim& s) {
    E.n = s.n; E.now = s.t; E.nEv = 0; E.capped = 0; E.nBadEscape = 0; E.tDisp = s.t; E.nLog = 0;
    for (int i = 0; i < s.n; ++i) evLoad(E.e[i], s.b[i], s.P, s.t);
    // pairs of two resting balls can never meet: start from "no event" and predict only
    // around the moving balls (usually just the cue ball)
    for (int i = 0; i < s.n; ++i) {
        for (int j = i + 1; j < s.n; ++j) E.tp[i][j] = EV_INF;
        E.rowMin[i] = EV_INF; E.rowArg[i] = -1; E.tc[i] = EV_INF; E.ck[i] = 0; E.ci[i] = -1;
    }
    for (int i = 0; i < s.n; ++i) if (E.e[i].ph > 0) evRepredict(E, s, i);
}
inline bool evMoving(const EvEngine& E) {
    for (int i = 0; i < E.n; ++i) if (E.e[i].ph > 0) return true;
    return false;
}
inline void evLog(EvEngine& E, int type, int a, int b, int k) {
    if (E.logOn && E.nLog < 512) { EvEngine::Rec& r = E.log[E.nLog++]; r.t = E.now; r.type = type; r.a = a; r.b = b; r.k = k; }
}

// A contact that ends with (almost) no separating speed would be found again at once
// (Zeno). Make every contact end separating at >= EV_VSEP; this only affects contacts
// slower than ~1 mm/s.
inline void evFixSepPair(Ball& A, Ball& B) {
    real nx = B.x - A.x, ny = B.y - A.y, d = psqrt(nx * nx + ny * ny);
    if (d <= 0) return;
    nx /= d; ny /= d;
    real sep = (B.vx - A.vx) * nx + (B.vy - A.vy) * ny;
    if (sep < EV_VSEP) {
        real h = 0.5 * (EV_VSEP - sep);
        A.vx -= h * nx; A.vy -= h * ny; B.vx += h * nx; B.vy += h * ny;
    }
}
inline void evFixSepCush(Ball& B, real nx, real ny) {
    real vn = B.vx * nx + B.vy * ny;
    if (vn < EV_VSEP) { B.vx += (EV_VSEP - vn) * nx; B.vy += (EV_VSEP - vn) * ny; }
}

inline void evMaterialize(EvEngine& E, Sim& s, real t) {
    for (int i = 0; i < s.n; ++i) if (E.e[i].ph >= 0) evAt(E.e[i], s.P, t, s.b[i]);
}
// display-only orientation update from tDisp to t (sub-steps of <= 4 ms, w sampled analytically)
inline void evOrient(EvEngine& E, Sim& s, real t) {
    real t0 = E.tDisp;
    if (!(t > t0)) return;
    int ns = (int)((t - t0) / 0.004) + 1;
    real h = (t - t0) / ns;
    for (int k = 0; k < ns; ++k) {
        real tm = t0 + (k + 0.5) * h;
        for (int i = 0; i < s.n; ++i) {
            const EvBall& e = E.e[i];
            if (e.ph < 0) continue;
            Ball tmp = s.b[i];
            evAt(e, s.P, pmax(tm, e.tb), tmp);
            if (tmp.wx == 0 && tmp.wy == 0 && tmp.wz == 0) continue;
            integrateOrientation(tmp, h);
            s.b[i].qw = tmp.qw; s.b[i].qx = tmp.qx; s.b[i].qy = tmp.qy; s.b[i].qz = tmp.qz;
        }
    }
}

// Process all events up to time tStop. untilFirstHit: return right after the cue ball's
// first ball contact. On return s.b holds the state at s.t (= tStop, the first-hit time, or
// the time the last ball stopped when tStop is infinite). Returns 1 if balls are still moving.
inline int evRun(EvEngine& E, Sim& s, real tStop, int untilFirstHit) {
    const Params& P = s.P;
    const int n = s.n;
    for (;;) {
        // earliest event; ties: phase changes before table events before pairs, lower index first
        real t1 = EV_INF, t2 = EV_INF, t3 = EV_INF; int a1 = -1, a2 = -1, a3 = -1;
        for (int i = 0; i < n; ++i) {
            if (E.e[i].te < t1) { t1 = E.e[i].te; a1 = i; }     // te = EV_INF unless moving
            if (E.tc[i] < t2) { t2 = E.tc[i]; a2 = i; }
            if (E.rowMin[i] < t3) { t3 = E.rowMin[i]; a3 = i; }
        }
        real tm = EV_INF; int type = EVT_NONE, a = -1, b = -1;
        if (a1 >= 0) { tm = t1; type = EVT_PHASE; a = a1; }
        if (a2 >= 0 && t2 < tm) { tm = t2; type = EVT_TABLE; a = a2; }
        if (a3 >= 0 && t3 < tm) { tm = t3; type = EVT_PAIR; a = a3; b = E.rowArg[a3]; }
        if (type == EVT_NONE || tm > tStop) break;
        if (tm < E.now) tm = E.now;                         // never step back in time
        E.now = tm;
        if (++E.nEv > EV_MAXEV) {                           // safety: freeze everything
            E.capped = 1;
            evMaterialize(E, s, tm);
            for (int i = 0; i < n; ++i) if (E.e[i].ph > 0) { s.b[i].vx = s.b[i].vy = s.b[i].wx = s.b[i].wy = 0; evLoad(E.e[i], s.b[i], P, tm); }
            for (int i = 0; i < n; ++i) { E.tc[i] = EV_INF; for (int j = 0; j < n; ++j) E.tp[i][j] = EV_INF; E.rowMin[i] = EV_INF; E.rowArg[i] = -1; }
            continue;
        }
        if (type == EVT_PHASE) {
            EvBall& e = E.e[a];
            Ball& B = s.b[a];
            evAt(e, P, tm, B);
            if (e.ph == 1) { B.wx = -B.vy / P.R; B.wy = B.vx / P.R; }      // slip is exactly zero now
            else { B.vx = B.vy = B.wx = B.wy = 0; }                           // stopped
            evLoad(e, B, P, tm);
            evLog(E, EVT_PHASE, a, -1, e.ph);
            evRepredict(E, s, a);
        } else if (type == EVT_TABLE) {
            int kind = E.ck[a], k = E.ci[a];
            Ball& B = s.b[a];
            evAt(E.e[a], P, tm, B);
            if (kind == EVK_SEG || kind == EVK_CIRC) {
                real nx, ny;
                if (kind == EVK_SEG) { nx = s.T.seg[k].nx; ny = s.T.seg[k].ny; }
                else { nx = B.x - s.T.ptx[k]; ny = B.y - s.T.pty[k]; real l = psqrt(nx * nx + ny * ny); nx /= l; ny /= l; }
                int before = s.ev.nCushColl;
                resolveCushion(s, a, nx, ny);
                if (s.ev.nCushColl == before) {             // grazing: record it anyway
                    if (s.ev.firstHit >= 0) s.ev.railAfter |= (1u << a);
                    s.ev.railAny |= (1u << a); s.ev.nCushColl++;
                }
                evFixSepCush(B, nx, ny);
                evLoad(E.e[a], B, P, tm);
                evLog(E, EVT_TABLE, a, kind, k);
                evRepredict(E, s, a);
            } else {
                int pk = k;
                if (kind == EVK_ESCAPE) {
                    real bd = 1e30;
                    for (int q = 0; q < s.T.npk; ++q) {
                        real dx = B.x - s.T.pk[q].cx, dy = B.y - s.T.pk[q].cy, d = dx * dx + dy * dy;
                        if (d < bd) { bd = d; pk = q; }
                    }
                    const Pocket& p = s.T.pk[pk];
                    if (pabs((B.x - p.mx) * p.tx + (B.y - p.my) * p.ty) > p.halfw) E.nBadEscape++;
                }
                potBall(s, a, pk);
                evLoad(E.e[a], B, P, tm);
                evLog(E, EVT_TABLE, a, kind, pk);
                for (int j = 0; j < n; ++j) if (j != a) evSetTp(E, a < j ? a : j, a < j ? j : a, EV_INF);
                E.tc[a] = EV_INF;
            }
        } else {
            Ball& A = s.b[a]; Ball& B = s.b[b];
            evAt(E.e[a], P, tm, A); evAt(E.e[b], P, tm, B);
            int before = s.ev.nBallColl;
            resolveBallBall(s, a, b);
            if (s.ev.nBallColl == before) {                 // grazing: record it anyway
                if (s.ev.firstHit < 0) { if (a == 0) s.ev.firstHit = b; else if (b == 0) s.ev.firstHit = a; }
                s.ev.touched |= (1u << a) | (1u << b); s.ev.nBallColl++;
            }
            evFixSepPair(A, B);
            evLoad(E.e[a], A, P, tm); evLoad(E.e[b], B, P, tm);
            evLog(E, EVT_PAIR, a, b, 0);
            evRepredict(E, s, a); evRepredict(E, s, b);
            if (untilFirstHit && s.ev.firstHit >= 0) {
                s.t = tm; evMaterialize(E, s, tm); E.tDisp = tm;
                return 1;
            }
        }
    }
    real tEnd = evMoving(E) ? tStop : E.now;     // at rest: the time the last ball stopped
    if (s.trackOrient) evOrient(E, s, tEnd);
    s.t = tEnd; evMaterialize(E, s, tEnd); E.tDisp = tEnd;
    return evMoving(E) ? 1 : 0;
}

// Drop-in replacements for the stepper loops
inline real evSimulate(Sim& s, real tmax = 60) {
    EvEngine E; E.logOn = 0;
    evInit(E, s);
    evRun(E, s, tmax, 0);
    for (int i = 0; i < s.n; ++i) { s.b[i].wz = 0; s.b[i].vx = s.b[i].vy = 0; s.b[i].wx = s.b[i].wy = 0; }
    s.ev.tEnd = s.t;
    return s.t;
}
// simulate only until the cue ball's first ball contact (or rest / tmax)
inline void evSimulateFirstHit(Sim& s, real tmax) {
    EvEngine E; E.logOn = 0;
    evInit(E, s);
    evRun(E, s, tmax, 1);
}

// Simulation entry points used by the game, the bots and the training tools.
#ifdef POOL_STEPPER
inline real simulate(Sim& s, real dt = SIM_DT, real tmax = 60) { return simulateStep(s, dt, tmax); }
inline void simulateFirstHit(Sim& s, real tmax) { while (s.ev.firstHit < 0 && anyMoving(s) && s.t < tmax) step(s, SIM_DT); }
#else
inline real simulate(Sim& s, real dt = SIM_DT, real tmax = 60) { (void)dt; return evSimulate(s, tmax); }
inline void simulateFirstHit(Sim& s, real tmax) { evSimulateFirstHit(s, tmax); }
#endif

} // namespace pool
