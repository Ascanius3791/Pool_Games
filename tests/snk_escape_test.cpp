// Snookered positions: does the bot make an honest attempt to hit a ball on, and what does the
// incoming player choose after a foul? Uses the evaluator in train/baseline (net + win table).
// usage: snk_escape_test [skill 0.35] [positions 40] [budget 1000] [refMode 0] [verbose 0|1] [foul] [noguard]
// "foul": the striker deliberately plays a soft shot straight at the snookering ball instead.
// "noguard": without the final-choice foul guard (BotCtx::foulGuard), to see what the evaluator alone does.
#include "../train/common.h"
#include <cstring>
using namespace train;

// cue ball hidden behind a colour, reds either in the pack or scattered over the top half
static int blocker = -1;
static bool snookeredPosition(GameSnk& g, Rng& r, int variant) {
    for (int tries = 0; tries < 400; ++tries) {
        newGameSnk(g, r.next(), 0); g.sim.trackOrient = 0;
        Sim& s = g.sim; const real R = s.P.R;
        if (variant == 1) {
            int reds = 1 + r.below(15);
            for (int i = 1; i <= 15; ++i) s.b[i].on = 0;
            for (int i = 1; i <= reds; ++i) { s.b[i].on = 1; if (!randomSpot(g, i, r, s.T.L / 2 * 0.9)) s.b[i].on = 0; }
        }
        int blk = 16 + r.below(6); blocker = blk;
        const Ball& B = s.b[blk];
        // away from the reds' centre of mass, 0.2-3 cm behind the blocker
        real mx = 0, my = 0; int n = 0;
        for (int i = 1; i <= 15; ++i) if (s.b[i].on) { mx += s.b[i].x; my += s.b[i].y; n++; }
        if (!n) continue;
        mx /= n; my /= n;
        real ux = B.x - mx, uy = B.y - my, ul = psqrt(ux * ux + uy * uy); ux /= ul; uy /= ul;
        real ang = r.uniform(-0.5, 0.5), d = 2 * R + r.uniform(0.002, 0.03);
        real vx = ux * pcos(ang) - uy * psin(ang), vy = ux * psin(ang) + uy * pcos(ang);
        real x = B.x + vx * d, y = B.y + vy * d;
        if (!snkFree(g, x, y, 0)) continue;
        resetBall(s.b[0], x, y);
        g.isBreak = 0; g.ballInHand = 0; g.turn = 0;
        int taken = (15 - n) * 8; g.score[0] = (int)(taken * r.uniform()); g.score[1] = taken - g.score[0];
        if (snkSnookered(g, -1) && !snkDirectSight(g) && !snkNeedsSnookers(g, 0)) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    const real skill = argc > 1 ? atof(argv[1]) : 0.35;
    const int N = argc > 2 ? atoi(argv[2]) : 40;
    const real budget = argc > 3 ? atof(argv[3]) : 1000;
    const int mode = argc > 4 ? atoi(argv[4]) : 0;
    const bool verbose = argc > 5 && atoi(argv[5]);
    bool forceFoul = false, guard = true;
    for (int k = 6; k < argc; ++k) { if (!strcmp(argv[k], "foul")) forceFoul = true; if (!strcmp(argv[k], "noguard")) guard = false; }
    SnkEvalOwned se;
    if (!loadSnkEval("train/baseline/snk_params.txt", "train/baseline/snk_table.bin", se) ||
        !loadSnkNet("train/baseline/snk_net.bin", skill, se)) return 1;
    NoiseModel nm; defaultNoise(nm); nm.skill = skill;
    Rng r; r.seed(12345);
    int honest = 0, fouls = 0, misses = 0, replaces = 0, plays = 0, backs = 0, done = 0;
    double t0 = now();
    for (int i = 0; i < N; ++i) {
        GameSnk g;
        if (!snookeredPosition(g, r, i % 2)) continue;
        done++;
        BotCtx c; botDefaults(c, true, 0, 1000 + i); c.budget = budget; c.nm = nm; c.snk = &se.ev;
        c.refMode = mode; c.foulGuard = guard;
        BotDecision d; botThink(c, g, d);
        if (forceFoul) {
            d.place = 0; d.tgt = -1; d.kind = 9; d.sp.a = 0; d.sp.b = 0; d.sp.V = 1.2;
            d.sp.phi = patan2(g.sim.b[blocker].y - g.sim.b[0].y, g.sim.b[blocker].x - g.sim.b[0].x);
        }
        if (d.place) resetBall(g.sim.b[0], d.px, d.py);
        prepShot(g, d.tgt);
        GameSnk pre = g;
        int h = snkFirstOn(pre, d.sp, 0, 0);
        honest += h;
        Rng er; er.seed(777 + i);
        executeShot(g, d.sp, true, nm, er); simulate(g.sim); endShotSnk(g);
        const char* ch = "-";
        RefInfo ri = {};
        if (g.lastFoul) fouls++;
        if (g.pendingChoice && g.winner < 0) {
            snkReferee(pre, g, d.sp, nm, mode, 0.5, 99 + i, ri);
            misses += ri.miss;
            if (g.winner < 0 && g.pendingChoice) {
                BotCtx cc; botDefaults(cc, true, 0, 5000 + i); cc.budget = budget; cc.nm = nm; cc.snk = &se.ev; cc.refMode = mode;
                real v[3]; int o = snkChooseOption(cc, g, pre, v);
                if (o == CH_REPLACE) { replaces++; ch = "replace"; } else if (o == CH_PUTBACK) { backs++; ch = "put back"; } else { plays++; ch = "play"; }
                if (verbose) printf("    values: play %.3f  back %.3f  replace %.3f\n", v[0], v[1], v[2]);
            }
        }
        if (verbose) printf("pos %2d v%d: kind %d tgt %2d honest %d value %.3f | foul %d miss %d (p* %.2f played %.2f) -> %s\n",
                            i, i % 2, d.kind, d.tgt, h, d.value, g.lastFoul, ri.miss, ri.pStar, ri.pPlayed, ch);
    }
    printf("skill %.2f budget %.0f mode %d%s: %d positions, honest attempts %d (%.0f%%), fouls %d, misses %d | after a foul: replace %d, put back %d, play %d | %.1f s\n",
           skill, budget, mode, guard ? "" : " (no guard)", done, honest, 100.0 * honest / (done ? done : 1), fouls, misses, replaces, backs, plays, now() - t0);
}
