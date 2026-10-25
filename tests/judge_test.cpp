// judge_test.cpp — human-like judgement (BotCtx::posSigma) and the pot-chance estimate.
//  1. perturbBalls keeps every position legal (on the bed, no overlaps) and moves balls by ~sigma
//  2. bots with human error play snooker with judgement off and on: the predicted pot chance
//     of every pot they choose against how often it went in, plus thinking time
// usage: judge_test [mm 3] [shots 120] [budget 600]
#include "../engine/bot.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
using namespace pool;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static bool legal(const Sim& s) {
    const real R = s.P.R;
    for (int i = 0; i < s.n; ++i) {
        if (!s.b[i].on) continue;
        if (pabs(s.b[i].x) > s.T.L / 2 - R + 1e-12 || pabs(s.b[i].y) > s.T.W / 2 - R + 1e-12) return false;
        for (int j = i + 1; j < s.n; ++j) {
            if (!s.b[j].on) continue;
            real dx = s.b[i].x - s.b[j].x, dy = s.b[i].y - s.b[j].y;
            if (dx * dx + dy * dy < 4 * R * R) return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    const real mm = argc > 1 ? atof(argv[1]) : 3;
    const int maxShots = argc > 2 ? atoi(argv[2]) : 120;
    const real budget = argc > 3 ? atof(argv[3]) : 600;
    const real sigma = mm * 0.001;
    NoiseModel nm; defaultNoise(nm);
    int bad = 0;

    // 1. legality, from the break-off pack and from scattered positions
    {
        int n = 0, moved = 0; real d2 = 0;
        for (int t = 0; t < 400; ++t) {
            GameSnk g; newGameSnk(g, 1000 + t, 0); g.sim.trackOrient = 0;
            if (t % 2) {   // scatter: a hard break-off
                Rng r; r.seed(t); strike(g.sim, r.uniform(-0.2, 0.2), 6.0, 0, 0); simulate(g.sim, SIM_DT, 25);
                for (int i = 0; i < g.sim.n; ++i) { g.sim.b[i].vx = g.sim.b[i].vy = g.sim.b[i].wx = g.sim.b[i].wy = g.sim.b[i].wz = 0; }
                if (!g.sim.b[0].on) resetBall(g.sim.b[0], -1.0, 0);
                if (!legal(g.sim)) continue;
            }
            Sim s = g.sim; Rng r; r.seed(77 + t);
            perturbBalls(s, sigma, r);
            if (!legal(s)) bad++;
            for (int i = 0; i < s.n; ++i) if (s.b[i].on) {
                real dx = s.b[i].x - g.sim.b[i].x, dy = s.b[i].y - g.sim.b[i].y;
                n++; if (dx || dy) { moved++; d2 += dx * dx + dy * dy; }
                if (dx * dx + dy * dy > 9 * sigma * sigma * 1.0001) bad++;
            }
        }
        printf("1. perturbation %.1f mm: %d balls, %.0f%% moved, rms move %.2f mm, illegal %d\n",
               mm, n, 100.0 * moved / n, 1000 * psqrt(d2 / pmax(1, moved)), bad);
    }

    // 2. play with judgement off / on (same seeds)
    for (int pass = 0; pass < 2; ++pass) {
        const real ps = pass ? sigma : 0;
        GameSnk g; newGameSnk(g, 4242, 0); g.sim.trackOrient = 0;
        Rng rng; rng.seed(99);
        int pots = 0, potted = 0, fouls = 0, pts = 0; real pred = 0, brier = 0, think = 0;
        for (int shots = 0; shots < maxShots && g.winner < 0; ++shots) {
            BotCtx c; botDefaults(c, true, 0, 500 + shots); c.budget = budget; c.nm = nm; c.posSigma = ps;
            BotDecision d; double t0 = now(); botThink(c, g, d); think += now() - t0;
            real pp = botPotChance(c, g, d, 48);
            if (d.place) resetBall(g.sim.b[0], d.px, d.py);
            prepShot(g, d.tgt);
            int me = g.turn, before = g.score[me];
            executeShot(g, d.sp, true, nm, rng); simulate(g.sim);
            bool in = false; for (int i = 0; i < g.sim.ev.nPot; ++i) if (g.sim.ev.pot[i] == d.tgt) in = true;
            endShotSnk(g);
            if (g.pendingChoice) snkApplyChoice(g, g, CH_PLAY);
            fouls += g.lastFoul != 0; pts += g.score[me] - before > 0 ? g.score[me] - before : 0;
            if (pp >= 0) { pots++; potted += in; pred += pp; brier += (pp - in) * (pp - in); }
            if (pp < 0 || pp > 1) { if (pp != -1) bad++; }
        }
        printf("2. judgement %.1f mm: %d pots chosen, predicted %.0f%%, went in %.0f%%, brier %.3f | fouls %d, points %d | %.2f s per shot\n",
               ps * 1000, pots, 100 * pred / pmax(1, pots), 100.0 * potted / pmax(1, pots), brier / pmax(1, pots), fouls, pts, think / maxShots);
    }
    printf(bad ? "FAILED (%d)\n" : "all checks passed\n", bad);
    return bad ? 1 : 0;
}
