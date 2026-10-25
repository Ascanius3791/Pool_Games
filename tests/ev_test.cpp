// ev_test.cpp — validation of the event-driven engine (engine/events.h) against the
// fixed-step integrator (physics.h stepSimulate), plus invariant checks and speed.
//
//   build/ev_test [quick]
#include <initializer_list>
#include "../engine/rulesSnk.h"
#include "../engine/events.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <algorithm>
using namespace pool;

static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static Sim snkBase() {
    GameSnk g; newGameSnk(g, 1, 0); g.sim.trackOrient = 0; return g.sim;
}
static Sim snkRack(u64 seed) { GameSnk g; newGameSnk(g, seed, 0); g.sim.trackOrient = 0; return g.sim; }
static Sim poolRack(u64 seed) { Game8 g; newGame8(g, seed, 0); g.sim.trackOrient = 0; return g.sim; }
static void allOff(Sim& s) { for (int i = 1; i < s.n; ++i) { s.b[i].on = 0; } }

// random non-overlapping positions for balls 0..k-1 (others off)
static void scatter(Sim& s, Rng& r, int k, real margin) {
    const real R = s.P.R;
    for (int i = 0; i < s.n; ++i) s.b[i].on = 0;
    for (int i = 0; i < k; ++i) {
        for (int tries = 0; tries < 1000; ++tries) {
            real x = r.uniform(-s.T.L / 2 + R + margin, s.T.L / 2 - R - margin);
            real y = r.uniform(-s.T.W / 2 + R + margin, s.T.W / 2 - R - margin);
            bool ok = true;
            for (int j = 0; j < i; ++j) { real dx = s.b[j].x - x, dy = s.b[j].y - y; if (dx * dx + dy * dy < 4.2 * R * R) { ok = false; break; } }
            if (ok) { resetBall(s.b[i], x, y); break; }
        }
    }
}

struct Outcome {
    Events ev; real x[MAXB], y[MAXB]; int on[MAXB], pk[MAXB]; int nSnd; SoundEvent snd[64]; real t; double us;
};
static Outcome take(const Sim& s, double us) {
    Outcome o; o.ev = s.ev; o.t = s.t; o.us = us; o.nSnd = s.nSnd;
    for (int i = 0; i < s.nSnd; ++i) o.snd[i] = s.snd[i];
    for (int i = 0; i < MAXB; ++i) { o.x[i] = s.b[i].x; o.y[i] = s.b[i].y; o.on[i] = i < s.n ? s.b[i].on : 0; o.pk[i] = s.b[i].pocket; }
    return o;
}
static Outcome runStep(Sim s, real phi, real V, real a, real b, real dt) {
    strike(s, phi, V, a, b);
    double t0 = nowSec(); simulateStep(s, dt, 60); double t1 = nowSec();
    return take(s, (t1 - t0) * 1e6);
}
static Outcome runEv(Sim s, real phi, real V, real a, real b) {
    strike(s, phi, V, a, b);
    double t0 = nowSec(); evSimulate(s, 60); double t1 = nowSec();
    return take(s, (t1 - t0) * 1e6);
}
static double maxPosDiff(const Outcome& A, const Outcome& B, int n) {
    double m = 0;
    for (int i = 0; i < n; ++i) {
        if (A.on[i] != B.on[i]) return 1e9;
        if (!A.on[i]) continue;
        m = std::max(m, std::hypot(A.x[i] - B.x[i], A.y[i] - B.y[i]));
    }
    return m;
}
static bool samePots(const Outcome& A, const Outcome& B, int n) {
    for (int i = 0; i < n; ++i) { if (A.on[i] != B.on[i]) return false; if (!A.on[i] && A.pk[i] != B.pk[i]) return false; }
    return true;
}
// number of leading sound events that agree (type and position within tol)
static int seqAgree(const Outcome& A, const Outcome& B, double tol) {
    int n = std::min(A.nSnd, B.nSnd), k = 0;
    for (; k < n; ++k) {
        if (A.snd[k].type != B.snd[k].type) break;
        if (std::hypot(A.snd[k].x - B.snd[k].x, A.snd[k].y - B.snd[k].y) > tol) break;
    }
    return k;
}

// ---------------------------------------------------------------- invariants
struct Inv { int skipped = 0; int ovShot = -1, ovI = -1, ovJ = -1; int badEsc = 0; int shot = -1, worstShot = -1, worstK = -1; double worstX = 0, worstY = 0; double maxOverlap = 0, maxPen = 0, maxOut = 0; int nChecks = 0; int capped = 0; long events = 0; double eUp = 0; };
static double energy(const Sim& s) {
    double E = 0; const real R = s.P.R;
    for (int i = 0; i < s.n; ++i) {
        const Ball& b = s.b[i]; if (!b.on) continue;
        E += 0.5 * (b.vx * b.vx + b.vy * b.vy) + 0.5 * 0.4 * R * R * (b.wx * b.wx + b.wy * b.wy + b.wz * b.wz);
    }
    return E;
}
static void checkState(const Sim& s, Inv& inv) {
    const real R = s.P.R; const Table& T = s.T;
    inv.nChecks++;
    for (int i = 0; i < s.n; ++i) {
        const Ball& A = s.b[i]; if (!A.on) continue;
        for (int j = i + 1; j < s.n; ++j) {
            const Ball& B = s.b[j]; if (!B.on) continue;
            double d = std::hypot(A.x - B.x, A.y - B.y);
            if (2 * R - d > inv.maxOverlap) { inv.maxOverlap = 2 * R - d; inv.ovShot = inv.shot; inv.ovI = i; inv.ovJ = j; }
        }
        for (int k = 0; k < T.nseg; ++k) {
            const Seg& g = T.seg[k];
            double d = (A.x - g.ax) * g.nx + (A.y - g.ay) * g.ny;
            double sp = (A.x - g.ax) * g.dx + (A.y - g.ay) * g.dy;
            if (sp < 0 || sp > g.len) continue;
            if (d > -R && d < R && R - d > inv.maxPen) { inv.maxPen = R - d; inv.worstShot = inv.shot; inv.worstK = k; inv.worstX = A.x; inv.worstY = A.y; }
        }
        for (int k = 0; k < T.npt; ++k) {
            double d = std::hypot(A.x - T.ptx[k], A.y - T.pty[k]);
            if (R + T.ptr[k] - d > inv.maxPen) { inv.maxPen = R + T.ptr[k] - d; inv.worstShot = inv.shot; inv.worstK = 100 + k; inv.worstX = A.x; inv.worstY = A.y; }
        }
        inv.maxOut = std::max(inv.maxOut, std::max(std::fabs(A.x) - T.L / 2, std::fabs(A.y) - T.W / 2));
    }
}
// run with the engine and check the invariants every `dt` of simulated time
static int g_dbg = -1;
static Outcome runEvChecked(Sim s, real phi, real V, real a, real b, real dt, Inv& inv) {
    { Inv pre; checkState(s, pre); if (pre.maxOverlap > 0 || pre.maxPen > 0 || pre.maxOut > 0) { inv.skipped++; return take(s, 0); } }
    bool dbg = inv.shot == g_dbg;
    if (dbg) { printf("debug shot: phi %.6f V %.4f a %.3f b %.3f\n", phi, V, a, b); for (int i = 0; i < s.n; ++i) if (s.b[i].on) printf("  ball %d at (%.6f, %.6f)\n", i, s.b[i].x, s.b[i].y); }
    strike(s, phi, V, a, b);
    EvEngine E; E.logOn = dbg; evInit(E, s);
    int lastLog = 0;
    double e0 = energy(s);
    real t = 0;
    while (true) {
        t += dt;
        int mv = evRun(E, s, t, 0);
        double pen0 = inv.maxPen;
        checkState(s, inv);
        if (dbg) {
            for (; lastLog < E.nLog; ++lastLog) { auto& r = E.log[lastLog]; printf("  t=%.6f type %d a %d b %d k %d\n", r.t, r.type, r.a, r.b, r.k); }
            if (inv.maxPen > pen0 || ((int)(t * 1000) % 20 == 0)) { printf("  t=%.3f:", t); for (int i = 0; i < 3; ++i) printf(" b%d (%.4f,%.4f v %.3f,%.3f ph %d te %.4f tc %.4f k%d/%d)", i, s.b[i].x, s.b[i].y, s.b[i].vx, s.b[i].vy, E.e[i].ph, E.e[i].te, E.tc[i], E.ck[i], E.ci[i]); printf("\n"); }
        }
        double e1 = energy(s);
        if (e1 > e0 + 1e-9) inv.eUp = std::max(inv.eUp, e1 - e0);
        e0 = e1;
        if (!mv || t > 60) break;
    }
    inv.capped += E.capped; inv.events += E.nEv; inv.badEsc += E.nBadEscape;
    for (int i = 0; i < s.n; ++i) { s.b[i].wz = 0; s.b[i].vx = s.b[i].vy = 0; s.b[i].wx = s.b[i].wy = 0; }
    return take(s, 0);
}

static void report(const char* name, std::vector<double>& d) {
    if (d.empty()) { printf("  %s: -\n", name); return; }
    std::sort(d.begin(), d.end());
    auto q = [&](double p) { return d[std::min(d.size() - 1, (size_t)(p * d.size()))]; };
    printf("  %-34s n=%4zu  median %.3g  p90 %.3g  max %.3g\n", name, d.size(), q(0.5), q(0.9), d.back());
}

int main(int argc, char** argv) {
    bool quick = argc > 1 && !strcmp(argv[1], "quick");
    int only = argc > 2 ? atoi(argv[2]) : 0;
    if (argc > 3) g_dbg = atoi(argv[3]);
    auto sec = [&](int k) { return only == 0 || only == k; };
    const real FINE = quick ? 0.0001 : 0.00002;
    int fails = 0;
    Sim base = snkBase();
    const real R = base.P.R;
    printf("== event engine vs stepper (fine dt = %.0f us, game dt = %.0f ms)\n", FINE * 1e6, SIM_DT * 1e3);

    // ---- 1. single ball: rolling, draw/follow, side spin into cushions
    if (sec(1)) {
        std::vector<double> dFine, dGame;
        Rng r; r.seed(11);
        for (int k = 0; k < (quick ? 40 : 120); ++k) {
            Sim s = base; allOff(s);
            resetBall(s.b[0], r.uniform(-1.2, 1.2), r.uniform(-0.6, 0.6));
            real phi = r.uniform(-PI, PI), V = r.uniform(0.3, 6), a = r.uniform(-0.5, 0.5), b = r.uniform(-0.5, 0.5);
            Outcome E = runEv(s, phi, V, a, b), F = runStep(s, phi, V, a, b, FINE), G = runStep(s, phi, V, a, b, SIM_DT);
            if (E.on[0] && F.on[0]) dFine.push_back(maxPosDiff(E, F, 1) * 1000);
            if (E.on[0] && G.on[0]) dGame.push_back(maxPosDiff(E, G, 1) * 1000);
            if (E.on[0] != F.on[0]) printf("  single ball %d: pot differs (ev %d, fine %d)\n", k, E.on[0], F.on[0]);
        }
        printf("1. single ball, random shots incl. spin and cushions (final position difference, mm)\n");
        report("event vs fine stepper", dFine);
        report("event vs game stepper (4 ms)", dGame);
    }

    // ---- 2. corners and jaws with spin: cue ball alone, aimed across the pocket mouth
    if (sec(2)) {
        int agreeF = 0, agreeG = 0, total = 0, potE = 0, potF = 0;
        std::vector<double> dpos;
        const real hx = base.T.L / 2, hy = base.T.W / 2;
        for (int pocket = 0; pocket < 2; ++pocket)            // 0 corner (+,+), 1 middle (0,+)
        for (int ai = 0; ai < (pocket == 0 ? 4 : 3); ++ai)
        for (real V : {0.6, 1.5, 4.0})
        for (real a : {-0.5, 0.0, 0.5})
        for (real b : {-0.5, 0.0, 0.5})
        for (real off = -0.066; off <= 0.0661; off += (quick ? 0.012 : 0.006)) {
            static const real angC[4] = {45, 30, 15, 60}, angM[3] = {90, 70, 55};
            const real ang = pocket == 0 ? angC[ai] : angM[ai];
            Sim s = base; allOff(s);
            real A = ang * PI / 180, d = 0.7;
            real tx = pocket == 0 ? hx : 0, ty = hy;
            resetBall(s.b[0], tx - d * cos(A) - off * sin(A), ty - d * sin(A) + off * cos(A));
            real phi = A;
            Outcome E = runEv(s, phi, V, a, b), F = runStep(s, phi, V, a, b, FINE), G = runStep(s, phi, V, a, b, SIM_DT);
            total++;
            potE += !E.on[0]; potF += !F.on[0];
            if (samePots(E, F, 1)) agreeF++;
            else if (!quick && total - agreeF <= 12)
                printf("  differs: pocket %s ang %.0f V %.1f a %+.1f b %+.1f off %+.0f mm: ev %s, fine %s, game %s  (ev %d cushion contacts, fine %d)\n",
                       pocket ? "middle" : "corner", ang, V, a, b, off * 1000, E.on[0] ? "stays" : "potted", F.on[0] ? "stays" : "potted",
                       G.on[0] ? "stays" : "potted", E.ev.nCushColl, F.ev.nCushColl);
            if (samePots(E, G, 1)) agreeG++;
            if (E.on[0] && F.on[0]) dpos.push_back(maxPosDiff(E, F, 1) * 1000);
        }
        printf("2. jaw grid (%d shots: corner/middle, 3 speeds, side -0.5/0/+0.5, screw/stun/top, mouth offsets +-66 mm)\n", total);
        printf("  pot outcome agrees with fine stepper: %d/%d (%.1f%%), with game stepper: %d/%d; potted ev %d, fine %d\n",
               agreeF, total, 100.0 * agreeF / total, agreeG, total, potE, potF);
        report("final position diff vs fine (mm)", dpos);
        if (agreeF < total * 0.97) { printf("  FAIL: too many jaw disagreements\n"); fails++; }
    }

    // ---- 3. object ball into the jaws: cue hits a ball near the corner / middle pocket
    if (sec(3)) {
        int total = 0, agree = 0;
        Rng r; r.seed(5);
        const real hx = base.T.L / 2, hy = base.T.W / 2;
        for (int k = 0; k < (quick ? 150 : 600); ++k) {
            Sim s = base; allOff(s);
            int corner = k % 3 != 0;
            real px = corner ? hx - r.uniform(0.08, 0.5) : r.uniform(-0.3, 0.3), py = hy - r.uniform(0.05, 0.4);
            resetBall(s.b[1], px, py);
            real tx = corner ? hx : 0, ty = hy;
            real ux = tx - px, uy = ty - py, ul = std::hypot(ux, uy); ux /= ul; uy /= ul;
            real gx = px - 2 * R * ux, gy = py - 2 * R * uy;              // ghost ball
            real ca = r.uniform(-0.9, 0.9), dist = r.uniform(0.3, 1.2);  // cut angle (rad)
            real cx = gx - dist * (ux * cos(ca) - uy * sin(ca)), cy = gy - dist * (uy * cos(ca) + ux * sin(ca));
            if (std::fabs(cx) > hx - 2 * R || std::fabs(cy) > hy - 2 * R) continue;
            resetBall(s.b[0], cx, cy);
            real phi = atan2(gy - cy, gx - cx) + r.uniform(-0.03, 0.03);
            real V = r.uniform(0.5, 5), a = r.uniform(-0.55, 0.55), b = r.uniform(-0.55, 0.55);
            Outcome E = runEv(s, phi, V, a, b), F = runStep(s, phi, V, a, b, FINE);
            total++;
            if (samePots(E, F, 2)) agree++;
            else if (!quick && total - agree <= 8) printf("  differs #%d: ev pots %d%d fine pots %d%d (ev %d/%d contacts, fine %d/%d)\n", k,
                !E.on[0], !E.on[1], !F.on[0], !F.on[1], E.ev.nBallColl, E.ev.nCushColl, F.ev.nBallColl, F.ev.nCushColl);
        }
        printf("3. object ball near the pockets, random cuts with spin: pot outcomes agree %d/%d (%.1f%%)\n", agree, total, 100.0 * agree / total);
        if (agree < total * 0.95) { printf("  FAIL\n"); fails++; }
    }

    // ---- 4. full snooker positions: event sequence and outcomes
    if (sec(4)) {
        std::vector<double> lead, dpos;
        int total = 0, potAgree = 0, simple = 0, simpleAgree = 0;
        Rng r; r.seed(77);
        for (int k = 0; k < (quick ? 40 : 160); ++k) {
            Sim s = snkRack(100 + k);
            if (k % 2) scatter(s, r, 22, 0.002);
            real phi = r.uniform(-PI, PI), V = r.uniform(0.5, 6), a = r.uniform(-0.5, 0.5), b = r.uniform(-0.5, 0.5);
            if (!(k % 2)) { phi = atan2(s.b[1].y - s.b[0].y, s.b[1].x - s.b[0].x) + r.uniform(-0.03, 0.03); }
            Outcome E = runEv(s, phi, V, a, b), F = runStep(s, phi, V, a, b, FINE);
            total++;
            int nAg = seqAgree(E, F, 0.002);
            lead.push_back(nAg >= std::min(E.nSnd, F.nSnd) ? 1.0 : (double)nAg / std::max(1, std::min(E.nSnd, F.nSnd)));
            if (samePots(E, F, s.n)) potAgree++;
            if (E.ev.nBallColl <= 3 && F.ev.nBallColl <= 3) { simple++; if (samePots(E, F, s.n) && maxPosDiff(E, F, s.n) < 0.01) simpleAgree++; dpos.push_back(maxPosDiff(E, F, s.n) * 1000); }
        }
        printf("4. snooker positions (%d shots: break-like and scattered)\n", total);
        report("fraction of event sequence agreeing", lead);
        printf("  all pots agree: %d/%d;  shots with <= 3 ball contacts: %d, of which pots agree and positions < 10 mm: %d\n", potAgree, total, simple, simpleAgree);
        report("final pos diff, <=3 contacts (mm)", dpos);
        if (simpleAgree < simple * 0.9) { printf("  FAIL\n"); fails++; }
    }

    // ---- 5. invariants: no overlap, no penetration, nothing leaves the table, energy never grows
    if (sec(5)) {
        Inv inv; Rng r; r.seed(99);
        int shots = quick ? 300 : 1500;
        for (int k = 0; k < shots; ++k) {
            int mode = k % 5, pool = (k / 5) % 3 == 2;
            Sim s = pool ? poolRack(500 + k) : snkRack(500 + k);
            const real R = s.P.R;
            real phi, V, a = r.uniform(-0.6, 0.6), b = r.uniform(-0.6, 0.6);
            if (mode == 0) { phi = atan2(s.b[1].y - s.b[0].y, s.b[1].x - s.b[0].x) + r.uniform(-0.02, 0.02); V = r.uniform(4, 9); }   // break-off
            else if (mode == 1) { scatter(s, r, 22, 0.0); phi = r.uniform(-PI, PI); V = r.uniform(0.3, 9); }
            else if (mode == 2) {   // frozen balls: cue ball and an object ball touching a cushion / each other
                allOff(s);
                real y = s.T.W / 2 - R - 1e-7;
                resetBall(s.b[1], r.uniform(-0.39, 0.39) * s.T.L, y);
                resetBall(s.b[2], s.b[1].x + 2 * R + 1e-6, y);
                resetBall(s.b[0], pmax(s.b[1].x - r.uniform(0.1, 0.6), -s.T.L / 2 + 2 * R), y - r.uniform(0, 0.3));
                phi = atan2(s.b[1].y - s.b[0].y, s.b[1].x - s.b[0].x) + r.uniform(-0.01, 0.01); V = r.uniform(0.3, 6);
            } else if (mode == 4) { // middle pocket jaws, heavy spin, sometimes a ball in the jaws
                allOff(s);
                real hy = s.T.W / 2, sy = r.uniform() < 0.5 ? 1 : -1;
                if (r.uniform() < 0.5) resetBall(s.b[1], r.uniform(-0.09, 0.09), sy * (hy - r.uniform(0.03, 0.10)));
                resetBall(s.b[0], r.uniform(-0.9, 0.9), sy * (hy - r.uniform(0.1, 0.8)));
                phi = atan2(sy * hy - s.b[0].y, r.uniform(-0.08, 0.08) - s.b[0].x); V = r.uniform(0.5, 8);
            } else {                // into a corner, heavy spin, a ball sitting in the jaws
                allOff(s);
                real hx = s.T.L / 2, hy = s.T.W / 2;
                resetBall(s.b[1], hx - r.uniform(0.04, 0.12), hy - r.uniform(0.04, 0.12));
                resetBall(s.b[0], hx - r.uniform(0.3, 1.0), hy - r.uniform(0.2, 0.7));
                phi = atan2(hy - s.b[0].y, hx - s.b[0].x) + r.uniform(-0.08, 0.08); V = r.uniform(0.5, 8);
            }
            inv.shot = k;
            runEvChecked(s, phi, V, a, b, 0.001, inv);
        }
        if (inv.maxOverlap > 1e-6) printf("  worst overlap: shot %d balls %d %d\n", inv.ovShot, inv.ovI, inv.ovJ);
        printf("   balls escaping away from a pocket mouth: %d; set-ups skipped (overlapping start): %d\n", inv.badEsc, inv.skipped);
        printf("5. invariants over %d shots (%d checks every 1 ms, %ld events): max overlap %.2e m, max cushion penetration %.2e m, max outside %.2e m, energy increase %.2e J/kg, capped shots %d\n",
               shots, inv.nChecks, inv.events, inv.maxOverlap, inv.maxPen, inv.maxOut, inv.eUp, inv.capped);
        if (inv.maxPen > 1e-6) printf("  worst penetration: shot %d, %s %d, ball at (%.4f, %.4f)\n", inv.worstShot, inv.worstK >= 100 ? "jaw circle" : "segment", inv.worstK % 100, inv.worstX, inv.worstY);
        if (inv.maxOverlap > 1e-6 || inv.maxPen > 1e-6 || inv.maxOut > 0.0100001 || inv.capped || inv.badEsc) { printf("  FAIL\n"); fails++; }
    }

    // ---- 6. determinism: frame-by-frame (random frame lengths) == one call
    if (sec(6)) {
        Rng r; r.seed(3); int same = 0, total = 0;
        for (int k = 0; k < 60; ++k) {
            Sim s = snkRack(900 + k); if (k % 2) scatter(s, r, 22, 0);
            real phi = r.uniform(-PI, PI), V = r.uniform(0.5, 8), a = r.uniform(-0.5, 0.5), b = r.uniform(-0.5, 0.5);
            Outcome A = runEv(s, phi, V, a, b);
            Sim t = s; t.trackOrient = 1; strike(t, phi, V, a, b);
            EvEngine E; E.logOn = 0; evInit(E, t);
            real tt = 0; while (true) { tt += r.uniform(0.001, 0.05); if (!evRun(E, t, tt, 0)) break; }
            bool eq = true;
            for (int i = 0; i < s.n; ++i) if (t.b[i].on != A.on[i] || (t.b[i].on && (t.b[i].x != A.x[i] || t.b[i].y != A.y[i]))) eq = false;
            if (t.ev.nBallColl != A.ev.nBallColl || t.ev.nCushColl != A.ev.nCushColl || t.ev.firstHit != A.ev.firstHit) eq = false;
            same += eq; total++;
        }
        printf("6. determinism (frame-by-frame vs single call, bit-identical): %d/%d\n", same, total);
        if (same != total) { printf("  FAIL\n"); fails++; }
    }

    // ---- 7. speed
    if (sec(7)) {
        Rng r; r.seed(21);
        double tE = 0, tG = 0; int N = quick ? 200 : 600; long evs = 0;
        for (int k = 0; k < N; ++k) {
            Sim s = snkRack(2000 + k);
            if (k % 3) scatter(s, r, 8 + k % 14, 0.003);
            real phi = k % 3 ? r.uniform(-PI, PI) : atan2(s.b[1].y - s.b[0].y, s.b[1].x - s.b[0].x);
            real V = k % 3 ? r.uniform(0.5, 5) : 8, a = r.uniform(-0.4, 0.4), b = r.uniform(-0.4, 0.4);
            Outcome E = runEv(s, phi, V, a, b), G = runStep(s, phi, V, a, b, SIM_DT);
            tE += E.us; tG += G.us; evs += E.ev.nBallColl + E.ev.nCushColl;
        }
        printf("7. speed (snooker, %d shots: break-offs and positions with 8-22 balls): event %.1f us/shot, stepper %.1f us/shot -> %.1fx\n",
               N, tE / N, tG / N, tG / tE);
        // first-hit partial simulations (what the bot's root finding uses)
        double pE = 0, pG = 0;
        for (int k = 0; k < N; ++k) {
            Sim s = snkRack(3000 + k); scatter(s, r, 22, 0.003);
            real phi = r.uniform(-PI, PI), V = r.uniform(0.5, 5);
            Sim a1 = s; strike(a1, phi, V, 0, 0); double t0 = nowSec(); evSimulateFirstHit(a1, 12); double t1 = nowSec();
            Sim a2 = s; strike(a2, phi, V, 0, 0); double t2 = nowSec(); while (a2.ev.firstHit < 0 && anyMoving(a2) && a2.t < 12) step(a2, SIM_DT); double t3 = nowSec();
            pE += t1 - t0; pG += t3 - t2;
            if (a1.ev.firstHit != a2.ev.firstHit) printf("  first hit differs: ev %d step %d\n", a1.ev.firstHit, a2.ev.firstHit);
        }
        printf("   partial (until first contact): event %.1f us, stepper %.1f us -> %.1fx\n", pE / N * 1e6, pG / N * 1e6, pG / pE);
    }
    printf(fails ? "FAILED (%d)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
