// arena.cpp — A/B matches between two evaluators, with a sequential probability ratio test.
//
// usage: arena --game 8|snk --minutes M [--noisy 1] [--skill 1] [--budget 250] [--seed S]
//              8-ball:  --a-weights8 FILE|none  --b-weights8 FILE|none
//              snooker: --a-snk-params F --a-snk-table F  --b-snk-params F --b-snk-table F
//                       [--a-snk-net F] [--b-snk-net F]  (neural evaluator on top of the table)
//              [--elo1 30]  (SPRT H0: elo 0 vs H1: elo1, alpha = beta = 0.05)  [--sprt 0] (no early stop)
//              [--pairs N]  (stop after N pairs)
//              snooker starting positions: [--suite "break:3 snookers:1 ..."] [--pos-from DIR[,DIR]]
// Games come in pairs with the same seed and swapped sides (A is player 0 in the first game of a
// pair, player 1 in the second); a pair is always completed. With --suite every pair starts from
// a position of one of the families in train/positions.h, the families interleaved by weight
// (smooth weighted round robin, so a short arena still covers all of them), and the result is
// also reported per family:
//   FAMILY {"family":"snookers","games":N,"a":W,"b":L,"score":s,"elo":e,"elo_ci":c,"pairs":P,
//           "swept_a":x,"split":y,"swept_b":z,"mover_won":m,"shots":avg}
// (swept: one side won both games of a pair; mover_won: share of games won by the side to move
// in the starting position). Last line:
//   ARENA {"a":W,"b":L,"games":N,"score":s,"elo":e,"elo_ci":c,"llr":x,"verdict":"A|B|undecided"}
#include "positions.h"
using namespace train;

int main(int argc, char** argv) {
    Args a(argc, argv);
    const bool snk = a.str("--game", "8") == "snk";
    const double minutes = a.num("--minutes", 10);
    const bool noisy = a.num("--noisy", 1) != 0;
    const double skill = a.num("--skill", 1.0), budget = a.num("--budget", 250);
    const u64 seed = (u64)a.num("--seed", 1);
    const double elo1 = a.num("--elo1", 30);
    const bool sprt = a.num("--sprt", 1) != 0;
    const long maxPairs = (long)a.num("--pairs", 1e9);
    FamMix mix; bool useSuite = false; PosPool pool;
    if (snk && a.has("--suite")) {
        std::string err;
        if (!mix.parse(a.str("--suite", ""), err)) { fprintf(stderr, "--suite: %s\n", err.c_str()); return 1; }
        useSuite = true;
        if (mix.w[FAM_PERTURBED] > 0) {
            std::vector<std::string> dirs; std::string pf = a.str("--pos-from", ""), cur;
            for (char ch : pf + ",") { if (ch == ',') { if (!cur.empty()) dirs.push_back(cur); cur.clear(); } else cur += ch; }
            if (dirs.empty() || !loadPosPool(pool, dirs, 20000, seed)) { fprintf(stderr, "note: no positions for the perturbed family, dropped from the suite\n"); mix.w[FAM_PERTURBED] = 0; }
        }
        if (mix.total() <= 0) { fprintf(stderr, "--suite: nothing left\n"); return 1; }
        printf("suite: %s\n", mix.str().c_str()); fflush(stdout);
    }
    struct FamStat { long games = 0, a = 0, b = 0, pairs = 0, sweptA = 0, split = 0, sweptB = 0, moverWon = 0, shots = 0; } fs[FAM_N];
    GameSnk start; int fam = FAM_BREAK, firstA = -1;
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
    while (n % 2 == 1 || (now() - t0 < minutes * 60 && n / 2 < maxPairs && (!sprt || (llr < up && llr > lo)))) {
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
            if (n % 2 == 0) {   // a new pair: its starting position
                newGameSnk(start, gs, 0); start.sim.trackOrient = 0; fam = FAM_BREAK; firstA = -1;
                if (useSuite) {
                    fam = mix.next();
                    Rng pr; pr.seed(gs * 7919 + 11);
                    if (!genSnkStart(start, fam, pr, &pool)) { fprintf(stderr, "pair %d: no %s position, playing from the break\n", n / 2, famName(fam)); fam = FAM_BREAK; }
                }
            }
            GameSnk g = start;
            const int mover0 = g.turn;
            int shots = 0;
            for (; g.winner < 0 && shots < 400; ++shots) {
                bool isA = (g.turn == 0) == aIs0;
                BotCtx c; botDefaults(c, noisy, 0, gs * 31 + shots); c.budget = budget; c.nm = nm; c.snk = isA ? &sa.ev : &sb.ev;
                BotDecision d; botThink(c, g, d);
                if (d.place) resetBall(g.sim.b[0], d.px, d.py);
                prepShot(g, d.tgt);
                GameSnk pre = g;
                executeShot(g, d.sp, noisy, nm, rng); simulate(g.sim); endShotSnk(g);
                if (g.pendingChoice && g.winner < 0) {
                    RefInfo ri; snkReferee(pre, g, d.sp, nm, 0, 0.5, gs + shots, ri);
                    if (g.winner < 0 && g.pendingChoice) { real v[3]; snkApplyChoice(g, pre, snkBotChoose(g, ri.pStar, v)); }
                }
            }
            if (g.winner < 0) winner = g.score[0] == g.score[1] ? -1 : (g.score[0] > g.score[1] ? 0 : 1);
            else winner = g.winner;
            FamStat& F = fs[fam];
            F.shots += shots;
            if (winner >= 0) {
                const bool aw = (winner == 0) == aIs0;
                F.games++; (aw ? F.a : F.b)++; F.moverWon += winner == mover0;
                if (n % 2 == 0) firstA = aw;
                else if (firstA >= 0) { F.pairs++; if (firstA && aw) F.sweptA++; else if (!firstA && !aw) F.sweptB++; else F.split++; }
            } else if (n % 2 == 0) firstA = -1;
        }
        ++n;
        if (winner < 0) continue;
        bool aWon = (winner == 0) == aIs0;
        if (aWon) { wA++; llr += std::log(p1 / p0); } else { wB++; llr += std::log((1 - p1) / (1 - p0)); }
        printf("game %d%s%s: A %d - %d B   llr %.2f\n", n, useSuite ? " " : "", useSuite ? famName(fam) : "", wA, wB, llr); fflush(stdout);
    }
    int N = wA + wB;
    double s = N ? (double)wA / N : 0.5;
    double sc = std::min(std::max(s, 0.01), 0.99);
    double elo = -400 * std::log10(1 / sc - 1);
    double se = N ? std::sqrt(sc * (1 - sc) / N) : 0.5;
    double eloHi = -400 * std::log10(1 / std::min(0.99, sc + 1.96 * se) - 1), eloLo = -400 * std::log10(1 / std::max(0.01, sc - 1.96 * se) - 1);
    const char* verdict = llr >= up ? "A" : (llr <= lo ? "B" : "undecided");
    if (useSuite) {
        auto eloOf = [](long w, long nn, double& ci) {
            double sc = nn ? (double)w / nn : 0.5; sc = std::min(std::max(sc, 0.01), 0.99);
            double se = nn ? std::sqrt(sc * (1 - sc) / nn) : 0.5;
            double hi = -400 * std::log10(1 / std::min(0.99, sc + 1.96 * se) - 1), lo2 = -400 * std::log10(1 / std::max(0.01, sc - 1.96 * se) - 1);
            ci = (hi - lo2) / 2; return -400 * std::log10(1 / sc - 1);
        };
        printf("\n%-10s %6s %6s %6s %7s %12s %6s %21s %7s %6s\n", "family", "games", "A", "B", "score", "elo", "pairs", "swept A / split / B", "mover", "shots");
        for (int f = 0; f < FAM_N; ++f) {
            const FamStat& F = fs[f]; if (!F.games) continue;
            double ci, e = eloOf(F.a, F.games, ci);
            printf("%-10s %6ld %6ld %6ld %7.3f %6.0f+-%-4.0f %6ld %7ld / %4ld / %4ld %6.0f%% %6.1f\n", famName(f), F.games, F.a, F.b, (double)F.a / F.games, e, ci, F.pairs, F.sweptA, F.split, F.sweptB, 100.0 * F.moverWon / F.games, (double)F.shots / F.games);
        }
        for (int f = 0; f < FAM_N; ++f) {
            const FamStat& F = fs[f]; if (!F.games) continue;
            double ci, e = eloOf(F.a, F.games, ci);
            printf("FAMILY {\"family\":\"%s\",\"games\":%ld,\"a\":%ld,\"b\":%ld,\"score\":%.3f,\"elo\":%.0f,\"elo_ci\":%.0f,\"pairs\":%ld,\"swept_a\":%ld,\"split\":%ld,\"swept_b\":%ld,\"mover_won\":%.3f,\"shots\":%.1f}\n",
                   famName(f), F.games, F.a, F.b, (double)F.a / F.games, e, ci, F.pairs, F.sweptA, F.split, F.sweptB, (double)F.moverWon / F.games, (double)F.shots / F.games);
        }
    }
    printf("ARENA {\"a\":%d,\"b\":%d,\"games\":%d,\"score\":%.3f,\"elo\":%.0f,\"elo_ci\":%.0f,\"llr\":%.2f,\"verdict\":\"%s\"}\n", wA, wB, N, s, elo, (eloHi - eloLo) / 2, llr, verdict);
    return 0;
}
