// positions_test.cpp — checks of the starting-position families (train/positions.h)
//
//   build/positions_test [--n 400] [--shots 40] [--seed 1] [--pos-from DIR[,DIR]]
//       validity: every family generates N positions, each must pass snkCheck (no overlaps,
//       clear of cushions and jaws, phase/nextColour/scores/free ball consistent), the
//       evaluator inputs must be finite and its win chance in [0, 1]; then `shots` positions
//       per family get one bot shot (noisy, small budget) and the position after endShotSnk
//       (and the foul choice) must again be consistent. Prints per-family statistics.
//       Exit code 1 on any failure.
//   build/positions_test --demo FAMILY [--n 12] [--budget 2000] [--skill 1] [--samples 16]
//                        [--escapes N] [--verbose 1]  (evaluators: train/baseline net vs the linear evaluator
//                        train/baseline/snk_params.txt + win table, as in the arena)
//       first decision of each evaluator on N positions of FAMILY: shot kind, value, and over
//       `samples` noisy executions how often the shot pots, fouls, or leaves the opponent
//       snookered (laid snooker).
#include "../train/positions.h"
using namespace train;

static bool finiteInputs(const GameSnk& g, const SnkEval* ev, std::string& why) {
    float bf[21 * SNK_NB], gf[SNK_NG]; real f[SNK_NF];
    snkNetInputs(g, g.ballInHand, 1.0, bf, gf, f);
    for (int j = 0; j < 21 * SNK_NB; ++j) if (!std::isfinite(bf[j])) { why = "non-finite ball input"; return false; }
    for (int j = 0; j < SNK_NG; ++j) if (!std::isfinite(gf[j])) { why = "non-finite global input"; return false; }
    if (ev) { real p = snkWinNow(*ev, g, g.ballInHand); if (!(p >= 0 && p <= 1)) { why = "win chance out of range"; return false; } }
    return true;
}
static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> o; std::string cur;
    for (char ch : s + ",") { if (ch == ',') { if (!cur.empty()) o.push_back(cur); cur.clear(); } else cur += ch; }
    return o;
}
static const char* phaseName(const GameSnk& g) {
    static char b[32];
    if (g.phase == SNK_RED_ON) { snprintf(b, sizeof b, "red on (%d reds)", snkRedsOn(g)); return b; }
    if (g.phase == SNK_COLOUR_ON) return "colour on";
    static const char* cn[6] = {"yellow", "green", "brown", "blue", "pink", "black"};
    snprintf(b, sizeof b, "%s on", cn[g.nextColour - 16]); return b;
}

// one stroke of decision d from g with noise sample k; returns the position after the shot
static GameSnk playOnce(const GameSnk& g, const BotDecision& d, const NoiseModel& nm, u64 seed) {
    GameSnk a = g;
    if (d.place) resetBall(a.sim.b[0], d.px, d.py);
    prepShot(a, d.tgt);
    Rng r; r.seed(seed);
    executeShot(a, d.sp, true, nm, r); simulate(a.sim); endShotSnk(a);
    return a;
}

static int demo(const Args& a) {
    const int fam = famIndex(a.str("--demo", "snookers"));
    if (fam < 0) { fprintf(stderr, "unknown family\n"); return 1; }
    const int N = (int)a.num("--n", 12), samples = (int)a.num("--samples", 16);
    const double budget = a.num("--budget", 2000), skill = a.num("--skill", 1.0);
    const bool verbose = a.num("--verbose", 1) != 0;
    SnkEvalOwned net, lin;
    if (!loadSnkEval("train/baseline/snk_params.txt", "train/baseline/snk_table.bin", net) || !loadSnkNet("train/baseline/snk_net.bin", skill, net)) return 1;
    if (!loadSnkEval("train/baseline/snk_params.txt", "train/baseline/snk_table.bin", lin)) return 1;
    PosPool pool; if (fam == FAM_PERTURBED) loadPosPool(pool, splitComma(a.str("--pos-from", "data/snk")), 20000, 1);
    NoiseModel nm; defaultNoise(nm); nm.skill = skill;
    const char* who[2] = {"net", "linear"};
    double sumLay[2] = {0}, sumFoul[2] = {0}, sumPot[2] = {0}, nSafe[2] = {0}, sumHit[2] = {0}, sumPStar = 0; int nSn = 0;
    Rng r; r.seed((u64)a.num("--seed", 5) * 977 + fam);
    for (int i = 0; i < N; ++i) {
        GameSnk g; newGameSnk(g, 4242 + i, 0); g.sim.trackOrient = 0;
        if (!genSnkStart(g, fam, r, &pool)) { printf("position %d: generation failed\n", i); continue; }
        const int me = g.turn, R = snkRemaining(g), D = lead(g);
        const bool snookered = !g.ballInHand && snkSnookered(g, -1);
        real pStar = -1;
        if (snookered) { int t, r1, r2; pStar = snkEscapeChance(g, nm, 99 + i, t, r1, r2); sumPStar += pStar; nSn++; }
        if (verbose) printf("\n#%d %s, mover %s by %d, %d points left%s%s%s%s\n", i, phaseName(g), D >= 0 ? "ahead" : "behind", D >= 0 ? D : -D, R,
                            snkNeedsSnookers(g, me) ? ", NEEDS SNOOKERS" : "", snkNeedsSnookers(g, 1 - me) ? ", opponent needs snookers" : "",
                            snookered ? ", mover snookered" : "", g.freeBall ? ", free ball" : "");
        if (verbose && snookered) printf("   referee's best escape (direct / 1 / 2 cushions) hits a ball on with p* = %.2f\n", pStar);
        for (int e = 0; e < 2; ++e) {
            const SnkEval* ev = e == 0 ? &net.ev : &lin.ev;
            BotCtx c; botDefaults(c, true, 0, 1000 + i); c.budget = budget; c.nm = nm; c.snk = ev; c.nEscape = (int)a.num("--escapes", 0);
            BotDecision d; botThink(c, g, d);
            int lay = 0, foul = 0, pot = 0;
            if (snookered) sumHit[e] += snkHitChance(g, d.sp, nm, 555 + i, 24);
            for (int k = 0; k < samples; ++k) {
                GameSnk after = playOnce(g, d, nm, 77 + 31 * k + 1000003ULL * i);
                if (after.lastFoul) foul++;
                else if (after.turn == me && after.winner < 0) pot++;
                else if (after.winner < 0 && after.turn != me && !after.pendingChoice && snkSnookered(after, -1)) lay++;
            }
            sumLay[e] += (double)lay / samples; sumFoul[e] += (double)foul / samples; sumPot[e] += (double)pot / samples; nSafe[e] += d.kind == 1;
            if (verbose) printf("   %-6s: %-6s on ball %2d%s  value %.3f  | over %d noisy strokes: pot %2d, foul %2d, opponent left snookered %2d\n",
                                who[e], d.kind == 0 ? "pot" : "safety", d.tgt, d.place ? " (ball in hand)" : "", d.value, samples, pot, foul, lay);
        }
    }
    printf("\n%s, %d positions, budget %.0f, skill %.1f — mean over positions of the share of noisy strokes that:\n", famName(fam), N, budget, skill);
    for (int e = 0; e < 2; ++e)
        printf("  %-6s: safety chosen %4.0f%% | pot %4.0f%%  foul %4.0f%%  leave a snooker %4.0f%%%s\n", who[e], 100 * nSafe[e] / N, 100 * sumPot[e] / N, 100 * sumFoul[e] / N, 100 * sumLay[e] / N,
               nSn ? (std::string("  | snookered: chosen shot hits a ball on ") + std::to_string((int)lround(100 * sumHit[e] / nSn)) + "%").c_str() : "");
    if (nSn) printf("  %d snookered positions: the referee's best escape would hit a ball on %.0f%% of the time (p*)\n", nSn, 100 * sumPStar / nSn);
    return 0;
}

int main(int argc, char** argv) {
    Args a(argc, argv);
    if (a.has("--demo")) return demo(a);
    const int N = (int)a.num("--n", 400), shots = (int)a.num("--shots", 40);
    const u64 seed = (u64)a.num("--seed", 1);
    SnkEvalOwned se; const SnkEval* ev = 0;
    if (loadSnkEval("train/baseline/snk_params.txt", "train/baseline/snk_table.bin", se) && loadSnkNet("train/baseline/snk_net.bin", 1.0, se)) ev = &se.ev;
    else fprintf(stderr, "note: train/baseline evaluator not found, checking the inputs only\n");
    PosPool pool;
    const std::string pf = a.str("--pos-from", "");
    if (!pf.empty()) printf("perturbed: %zu positions loaded from %s\n", loadPosPool(pool, splitComma(pf), 20000, seed), pf.c_str());
    NoiseModel nm; defaultNoise(nm);
    int bad = 0;
    printf("%-10s %5s %5s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %7s\n", "family", "ok", "fail", "R", "D", "reds", "needSn", "oppSn", "snook", "free", "bih", "pWin", "shots", "after");
    for (int fam = 0; fam < FAM_N; ++fam) {
        if (fam == FAM_PERTURBED && pool.rec.empty()) { printf("%-10s (skipped: no --pos-from)\n", famName(fam)); continue; }
        Rng r; r.seed(seed * 1000 + fam);
        int ok = 0, failed = 0, nShot = 0, afterOk = 0;
        double sR = 0, sD = 0, sReds = 0, sNeed = 0, sOpp = 0, sSn = 0, sFree = 0, sBih = 0, sP = 0;
        for (int i = 0; i < N; ++i) {
            GameSnk g; newGameSnk(g, seed * 7 + i, 0); g.sim.trackOrient = 0;
            if (!genSnkStart(g, fam, r, &pool)) { failed++; continue; }
            std::string why = snkCheck(g);
            if (why.empty() && !finiteInputs(g, ev, why)) {}
            if (!why.empty()) { if (bad < 20) printf("  %s #%d: %s\n", famName(fam), i, why.c_str()); bad++; continue; }
            ok++;
            const int me = g.turn;
            sR += snkRemaining(g); sD += lead(g); sReds += snkRedsOn(g);
            sNeed += snkNeedsSnookers(g, me); sOpp += snkNeedsSnookers(g, 1 - me);
            sSn += !g.ballInHand && snkSnookered(g, -1); sFree += g.freeBall; sBih += g.ballInHand;
            if (ev) sP += snkWinNow(*ev, g, g.ballInHand);
            if (nShot < shots) {   // one bot shot, then the rules must leave a consistent position
                nShot++;
                BotCtx c; botDefaults(c, true, 0, seed + i); c.budget = 150; c.nm = nm; c.snk = ev;
                BotDecision d; botThink(c, g, d);
                GameSnk b = playOnce(g, d, nm, seed * 31 + i);
                if (b.pendingChoice && b.winner < 0) { real v[3]; GameSnk pre = g; if (d.place) resetBall(pre.sim.b[0], d.px, d.py); snkApplyChoice(b, pre, snkBotChoose(b, -1, v)); }
                std::string w2 = b.winner >= 0 ? std::string() : snkCheck(b, true);
                if (w2.empty() && b.winner < 0 && !finiteInputs(b, ev, w2)) {}
                if (!w2.empty()) { if (bad < 20) printf("  %s #%d after one shot: %s\n", famName(fam), i, w2.c_str()); bad++; }
                else afterOk++;
            }
        }
        const double n = ok ? ok : 1;
        printf("%-10s %5d %5d %6.1f %6.1f %6.1f %5.0f%% %5.0f%% %5.0f%% %5.0f%% %5.0f%% %6.3f %6d %7d\n", famName(fam), ok, failed, sR / n, sD / n, sReds / n,
               100 * sNeed / n, 100 * sOpp / n, 100 * sSn / n, 100 * sFree / n, 100 * sBih / n, sP / n, nShot, afterOk);
        if (failed > N / 50) { printf("  %s: %d of %d generations failed\n", famName(fam), failed, N); bad++; }
    }
    printf("columns: R points left, D mover's lead, needSn/oppSn mover/opponent needs snookers, snook mover snookered, pWin net win chance of the mover, after = positions still consistent after one bot shot\n");
    printf(bad ? "FAILED: %d problems\n" : "all positions valid\n", bad);
    return bad ? 1 : 0;
}
