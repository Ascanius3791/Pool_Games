#include "../engine/rules8.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <initializer_list>
using namespace pool;

static Sim base() {
    Sim s; defaultPoolParams(s.P); buildPoolTable(s.T, s.P); s.trackOrient = 0; s.n = 16;
    for (int i = 0; i < 16; ++i) { s.b[i] = Ball(); s.b[i].on = 0; s.b[i].qw = 1; }
    clearEvents(s); return s;
}
static void put(Sim& s, int i, double x, double y) { resetBall(s.b[i], x, y); }

int main() {
    // 1. rolling distance
    for (double V : {0.5, 1.0, 2.0, 4.0}) {
        Sim s = base(); put(s, 0, -1.2, 0.3);
        s.P.mu_r = 0.010;
        strike(s, 0, V, 0, 0); simulate(s);
        printf("roll V=%.1f: x_end=%.3f  t=%.2fs cushions=%d\n", V, s.b[0].x, s.t, s.ev.nCushColl);
    }
    // 2. stun/follow/draw: cue at -0.5, object at 0.0 (0.5m apart, straight)
    for (double b : {0.4, 0.0, -0.4, -0.6}) {
        for (double V : {1.5, 3.0}) {
            Sim s = base(); put(s, 0, -0.5, 0); put(s, 1, 0, 0);
            strike(s, 0, V, 0, b); simulate(s);
            printf("b=%+.1f V=%.1f: cue end x=%.3f (contact at %.3f)  obj end x=%.3f on=%d\n",
                   b, V, s.b[0].x, -2 * s.P.R, s.b[1].x, s.b[1].on);
        }
    }
    // 3. cushion: 45 deg into long rail, with side spin
    for (double a : {-0.5, 0.0, 0.5}) {
        Sim s = base(); put(s, 0, -0.6, 0.0);
        strike(s, PI / 4, 1.5, a, 0.4);
        // run until first cushion
        while (s.ev.nCushColl == 0 && anyMoving(s)) step(s, 0.001);
        for (int k = 0; k < 30; ++k) step(s, 0.001);
        double ang = atan2(-s.b[0].vy, s.b[0].vx) * 180 / PI;
        printf("cushion 45deg a=%+.1f: rebound angle from rail = %.1f deg, speed=%.2f\n", a,
               ang, hypot(s.b[0].vx, s.b[0].vy));
    }
    // 4. pocketing: from table centre into top-right corner at various offsets
    {
        Sim s0 = base();
        double cx = s0.T.L / 2, cy = s0.T.W / 2;
        int ok = 0, tot = 0;
        for (double off = -0.06; off <= 0.0601; off += 0.01) {
            Sim s = base(); put(s, 0, 0.3, 0.0);
            double tx = cx + off * 0.7071, ty = cy - off * 0.7071;
            double phi = atan2(ty - 0.0, tx - 0.3);
            strike(s, phi, 2.0, 0, 0); simulate(s);
            printf("corner off=%+.3f -> %s\n", off, s.b[0].on ? "miss" : "POT");
            tot++; ok += !s.b[0].on;
        }
        // along the rail into the corner
        for (double yo : {0.0, 0.002, 0.01}) {
            Sim s = base(); put(s, 0, 0.0, cy - s.P.R - yo);
            strike(s, 0.0, 1.5, 0, 0); simulate(s);
            printf("along rail gap=%.3f -> %s (x=%.3f)\n", yo, s.b[0].on ? "miss" : "POT", s.b[0].x);
        }
        // side pocket straight on and at angle
        for (double ang : {90.0, 70.0, 55.0, 45.0}) {
            Sim s = base(); double r = 0.6;
            put(s, 0, -r * cos(ang * PI / 180), cy - r * sin(ang * PI / 180));
            strike(s, ang * PI / 180, 1.5, 0, 0); simulate(s);
            printf("side pocket at %2.0f deg -> %s\n", ang, s.b[0].on ? "miss" : "POT");
        }
    }
    // 5. break + performance
    {
        Game8 g; int pots = 0, scr = 0; double tsum = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        int N = 200;
        for (int k = 0; k < N; ++k) {
            newGame8(g, 1234 + k, 0); g.sim.trackOrient = 0;
            strike(g.sim, 0.0 + (k % 7 - 3) * 0.002, 8.0, 0, -0.1); simulate(g.sim);
            int np = 0; for (int i = 1; i < 16; ++i) np += !g.sim.b[i].on;
            pots += np; scr += !g.sim.b[0].on; tsum += g.sim.t;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("break: avg potted %.2f, scratch %d/%d, avg duration %.1fs, %.2f ms/break\n",
               pots / (double)N, scr, N, tsum / N, ms / N);
    }
    // 6. throw: 30 deg cut, check object ball direction vs ghost-ball line
    {
        Sim s = base(); put(s, 0, -0.5, 0); put(s, 1, 0, 0);
        double cut = 30 * PI / 180; double R = s.P.R;
        double gx = -2 * R * cos(cut), gy = 2 * R * sin(cut);   // ghost ball for object to go at -30deg
        for (double a : {0.0, 0.5, -0.5}) {
            Sim t = s; double phi = atan2(gy - 0, gx + 0.5);
            strike(t, phi, 1.0, a, 0);
            while (t.ev.nBallColl == 0) step(t, 0.001);
            double ang = atan2(t.b[1].vy, t.b[1].vx) * 180 / PI;
            printf("30deg cut, a=%+.1f: object dir %.2f deg (ideal -30 w/o squirt)\n", a, ang);
        }
    }
    return 0;
}
