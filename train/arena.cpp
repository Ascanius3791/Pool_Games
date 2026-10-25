// arena.cpp — A/B matches between two evaluators, with a sequential probability ratio test.
//
// usage: arena --game 8|snk --minutes M [--noisy 1] [--skill 1] [--budget 250] [--seed S]
//              8-ball:  --a-weights8 FILE|none  --b-weights8 FILE|none
//              snooker: --a-snk-params F --a-snk-table F  --b-snk-params F --b-snk-table F
//                       [--a-snk-net F] [--b-snk-net F]  (neural evaluator on top of the table)
//              [--elo1 30]  (SPRT H0: elo 0 vs H1: elo1, alpha = beta = 0.05)
// Games come in pairs with the same seed and swapped sides. Last line:
//   ARENA {"a":W,"b":L,"games":N,"score":s,"elo":e,"elo_ci":c,"llr":x,"verdict":"A|B|undecided"}
#include "common.h"
using namespace train;

int main(int argc, char** argv) {
    Args a(argc, argv);
    const bool snk = a.str("--game", "8") == "snk";
    const double minutes = a.num("--minutes", 10);
    const bool noisy = a.num("--noisy", 1) != 0;
    const double skill = a.num("--skill", 1.0), budget = a.num("--budget", 250);
    const u64 seed = (u64)a.num("--seed", 1);
    const double elo1 = a.num("--elo1", 30);
    std::vector<float> wa, wb; const float* na = 0; const float* nb = 0;
    if (!snk) {
        std::string fa = a.str("--a-weights8", "none"), fb = a.str("--b-weights8", "none");
        if (fa != "none") { if (!loadFloats(fa, wa, NWEIGHTS)) { fprintf(stderr, "bad %s\n", fa.c_str()); return 1; } na = wa.data(); }
        if (fb != "none") { if (!loadFloats(fb, wb, NWEIGHTS)) { fprintf(stderr, "bad %s\n", fb.c_str()); return 1; } nb = wb.data(); }
    }
    SnkEvalOwned sa, sb;
    if (snk && (!loadSnkEval(a.str("--a-snk-params", "none"), a.str("--a-snk-table", "none"), sa) ||
                !loadSnkEval(a.str("--b-snk-params", "none"), a.str("--b-snk-table", "none"), sb) ||
                !loadSnkNet(a.str("--a-snk-net", "none"), noisy ? skill : 0, sa) || !loadSnkNet(a.str("--b-snk-net", "none"), noisy ? skill : 0, sb))) return 1;
    NoiseModel nm; defaultNoise(nm); nm.skill = skill;
    const double p0 = 0.5, p1 = 1 / (1 + std::pow(10, -elo1 / 400));
    int wA = 0, wB = 0, n = 0; double llr = 0; const double up = std::log(0.95 / 0.05), lo = std::log(0.05 / 0.95);
    const double t0 = now();
    while (now() - t0 < minutes * 60 && llr < up && llr > lo) {
        const u64 gs = seed * 100003ULL + n / 2;
        const bool aIs0 = n % 2 == 0;
        int winner = -1;
        Rng rng; rng.seed(gs * 13 + 7);
        if (!snk) {
            Game8 g; newGame8(g, gs, 0); g.sim.trackOrient = 0;
            for (int shots = 0; g.winner < 0 && shots < 150; ++shots) {
                bool isA = (g.turn == 0) == aIs0;
                BotCtx c; botDefaults(c, noisy, isA ? na : nb, gs * 31 + shots); c.budget = budget; c.nm = nm;
                BotDecision d; botThink(c, g, d);
                if (d.place) resetBall(g.sim.b[0], d.px, d.py);
                executeShot(g, d.sp, noisy, nm, rng); simulate(g.sim); endShot8(g);
            }
            winner = g.winner;
        } else {
            GameSnk g; newGameSnk(g, gs, 0); g.sim.trackOrient = 0;
            for (int shots = 0; g.winner < 0 && shots < 400; ++shots) {
                bool isA = (g.turn == 0) == aIs0;
                BotCtx c; botDefaults(c, noisy, 0, gs * 31 + shots); c.budget = budget; c.nm = nm; c.snk = isA ? &sa.ev : &sb.ev;
                BotDecision d; botThink(c, g, d);
                if (d.place) resetBall(g.sim.b[0], d.px, d.py);
                prepShot(g, d.tgt);
                GameSnk pre = g;
                executeShot(g, d.sp, noisy, nm, rng); simulate(g.sim); endShotSnk(g);
                if (g.pendingChoice && g.winner < 0) {
                    RefInfo ri; snkReferee(pre, g, d.sp, nm, 0, 0.5, gs + shots, ri);
                    if (g.winner < 0 && g.pendingChoice) {
                        BotCtx cc; botDefaults(cc, noisy, 0, gs * 37 + shots); cc.budget = budget; cc.nm = nm; cc.snk = ((g.turn == 0) == aIs0) ? &sa.ev : &sb.ev;
                        real v[3]; snkApplyChoice(g, pre, snkChooseOption(cc, g, pre, v));
                    }
                }
            }
            if (g.winner < 0) winner = g.score[0] == g.score[1] ? -1 : (g.score[0] > g.score[1] ? 0 : 1);
            else winner = g.winner;
        }
        ++n;
        if (winner < 0) continue;
        bool aWon = (winner == 0) == aIs0;
        if (aWon) { wA++; llr += std::log(p1 / p0); } else { wB++; llr += std::log((1 - p1) / (1 - p0)); }
        printf("game %d: A %d - %d B   llr %.2f\n", n, wA, wB, llr); fflush(stdout);
    }
    int N = wA + wB;
    double s = N ? (double)wA / N : 0.5;
    double sc = std::min(std::max(s, 0.01), 0.99);
    double elo = -400 * std::log10(1 / sc - 1);
    double se = N ? std::sqrt(sc * (1 - sc) / N) : 0.5;
    double eloHi = -400 * std::log10(1 / std::min(0.99, sc + 1.96 * se) - 1), eloLo = -400 * std::log10(1 / std::max(0.01, sc - 1.96 * se) - 1);
    const char* verdict = llr >= up ? "A" : (llr <= lo ? "B" : "undecided");
    printf("ARENA {\"a\":%d,\"b\":%d,\"games\":%d,\"score\":%.3f,\"elo\":%.0f,\"elo_ci\":%.0f,\"llr\":%.2f,\"verdict\":\"%s\"}\n", wA, wB, N, s, elo, (eloHi - eloLo) / 2, llr, verdict);
    return 0;
}
