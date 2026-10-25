// selfplay.cpp — physics self-play data generator (both games), append-only shards.
//
// usage: selfplay --game 8|snk --out DIR --minutes M --seed S
//                 [--noisy 1] [--skill 1.0] [--budget 250] [--random-start 0.5]
//                 [--start-mix "break:4 snookers:1 ..."] [--pos-from DIR[,DIR...]] [--pool 20000]
//                 [--weights8 FILE] [--snk-params FILE] [--snk-table FILE] [--snk-net FILE]
// Snooker starting positions: --start-mix draws every frame's start from the families in
// train/positions.h with the given weights (it replaces --random-start, which keeps the old
// behaviour: break-off, or randomStartSnk with that probability). The 'perturbed' family takes
// real positions from the *.posrec files under --pos-from (a sample of --pool positions).
// The family is stored in the frame header of the raw records (posrec version 3); only
// frames from the break or the random family write shot statistics (.stats), so the
// abstract simulator stays calibrated on natural play.
// Writes DIR/<game>_<seed>.games (see common.h) and, for snooker, DIR/<game>_<seed>.stats
// (one line per shot: kind targetClass potted foul visitStart) for calibrating the abstract
// simulator, and DIR/snk_<seed>.posrec: the full raw positions with the frame result, for
// training a neural evaluator later (format: SnkPosRec in common.h, documented in TRAINING.md).
// Every finished game is flushed, so killing the process loses at most one game.
#include "positions.h"
using namespace train;

static int snkTargetClass(int t) { return t >= 1 && t <= 15 ? 1 : (t >= 16 ? 2 : 0); }

int main(int argc, char** argv) {
    Args a(argc, argv);
    const bool snk = a.str("--game", "8") == "snk";
    const std::string dir = a.str("--out", "data");
    const double minutes = a.num("--minutes", 5);
    const u64 seed = (u64)a.num("--seed", 1);
    const bool noisy = a.num("--noisy", 1) != 0;
    const double skill = a.num("--skill", 1.0);
    const double budget = a.num("--budget", 250);
    const double randomStart = a.num("--random-start", 0.5);
    FamMix mix; bool useMix = false;
    if (a.has("--start-mix")) {
        std::string err;
        if (!mix.parse(a.str("--start-mix", ""), err)) { fprintf(stderr, "--start-mix: %s\n", err.c_str()); return 1; }
        useMix = true;
    }
    PosPool pool;
    if (snk && useMix && mix.w[FAM_PERTURBED] > 0) {
        std::vector<std::string> dirs; std::string pf = a.str("--pos-from", ""), cur;
        for (char ch : pf + ",") { if (ch == ',') { if (!cur.empty()) dirs.push_back(cur); cur.clear(); } else cur += ch; }
        size_t n = dirs.empty() ? 0 : loadPosPool(pool, dirs, (size_t)a.num("--pool", 20000), seed);
        if (!n) { fprintf(stderr, "note: no positions for the perturbed family (--pos-from '%s'), its weight goes to break\n", pf.c_str()); mix.w[FAM_BREAK] += mix.w[FAM_PERTURBED]; mix.w[FAM_PERTURBED] = 0; }
        else printf("perturbed family: %zu positions from %s\n", n, pf.c_str());
    }
    if (snk && useMix) printf("start mix: %s\n", mix.str().c_str());
    long famFrames[FAM_N] = {0}, famPos[FAM_N] = {0}, famMoverWon[FAM_N] = {0}, famUnfinished[FAM_N] = {0}, famFailed = 0;
    std::vector<float> w8; const float* nn = 0;
    if (a.has("--weights8") && a.str("--weights8", "none") != "none") {
        if (!loadFloats(a.str("--weights8", ""), w8, NWEIGHTS)) { fprintf(stderr, "bad --weights8\n"); return 1; }
        nn = w8.data();
    }
    SnkEvalOwned se;
    if (snk && (!loadSnkEval(a.str("--snk-params", "none"), a.str("--snk-table", "none"), se) ||
                !loadSnkNet(a.str("--snk-net", "none"), noisy ? skill : 0, se))) return 1;
    if (snk && !se.ev.table) fprintf(stderr, "warning: no --snk-table, search values are points, not win chances (TD targets will be poor)\n");

    char path[512];
    snprintf(path, sizeof path, "%s/%s_%llu.games", dir.c_str(), snk ? "snk" : "8", (unsigned long long)seed);
    FILE* fg = fopen(path, "ab"); if (!fg) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    FILE* fs = 0; FILE* fp = 0;
    if (snk) {
        snprintf(path, sizeof path, "%s/snk_%llu.stats", dir.c_str(), (unsigned long long)seed); fs = fopen(path, "a");
        snprintf(path, sizeof path, "%s/snk_%llu.posrec", dir.c_str(), (unsigned long long)seed); fp = fopen(path, "ab");
    }
    std::vector<SnkPosRec> raw;

    NoiseModel nm; defaultNoise(nm); nm.skill = skill;
    Rng rng; rng.seed(seed * 7919 + 3);
    const double t0 = now();
    int games = 0, positions = 0, unfinished = 0;
    while (now() - t0 < minutes * 60) {
        GameRec rec; rec.type = snk ? 1 : 0; rec.winner = -1;
        u64 gseed = seed * 1000003ULL + games;
        bool random = useMix ? false : rng.uniform() < randomStart;
        int fam = random ? FAM_RANDOM : FAM_BREAK, startMover = 0;
        int shots = 0, cap = snk ? 400 : 150;
        if (!snk) {
            Game8 g; newGame8(g, gseed, 0); g.sim.trackOrient = 0;
            if (random) randomStart8(g, rng);
            while (g.winner < 0 && shots < cap) {
                BotCtx c; botDefaults(c, noisy, nn, gseed * 31 + shots); c.budget = budget; c.nm = nm;
                Pos p; p.mover = g.turn;
                if (!g.isBreak) { real f[NFEAT]; features(g, g.turn, g.ballInHand, f); p.f.assign(f, f + NFEAT); }
                BotDecision d; botThink(c, g, d);
                if (!g.isBreak) { p.v = (float)d.value; rec.pos.push_back(p); }
                if (d.place) resetBall(g.sim.b[0], d.px, d.py);
                executeShot(g, d.sp, noisy, nm, rng); simulate(g.sim); endShot8(g); shots++;
            }
            rec.winner = g.winner;
        } else {
            GameSnk g; newGameSnk(g, gseed, 0); g.sim.trackOrient = 0;
            if (useMix) { fam = mix.sample(rng); if (!genSnkStart(g, fam, rng, &pool)) { famFailed++; fam = FAM_BREAK; } }
            else if (random) randomStartSnk(g, rng);
            startMover = g.turn;
            raw.clear();
            bool visitStart = true; int lastTurn = g.turn;
            while (g.winner < 0 && shots < cap) {
                BotCtx c; botDefaults(c, noisy, 0, gseed * 31 + shots); c.budget = budget; c.nm = nm; c.snk = &se.ev;
                Pos p; p.mover = g.turn;
                real f[SNK_NF]; snkFeatures(g, g.ballInHand, f); p.f.assign(f, f + SNK_NF);
                BotDecision d; botThink(c, g, d);
                p.v = (float)(se.ev.table ? d.value : 0.5);
                rec.pos.push_back(p);
                { SnkPosRec r; snkRaw(g, (float)d.value, r); raw.push_back(r); }
                GameSnk pre = g;
                if (d.place) resetBall(g.sim.b[0], d.px, d.py);
                prepShot(g, d.tgt);
                pre = g;
                executeShot(g, d.sp, noisy, nm, rng); simulate(g.sim);
                const int potsBefore = pre.score[pre.turn];
                endShotSnk(g); shots++;
                if (fs && famNatural(fam)) {
                    int potted = (g.lastFoul == 0 && g.score[pre.turn] > potsBefore && d.kind == 0) ? 1 : 0;
                    fprintf(fs, "%d %d %d %d %d\n", d.kind, snkTargetClass(d.tgt), potted, g.lastFoul ? 1 : 0, visitStart ? 1 : 0);
                }
                if (g.pendingChoice && g.winner < 0) {
                    RefInfo ri; snkReferee(pre, g, d.sp, nm, 0, 0.5, gseed + shots, ri);
                    if (g.winner < 0 && g.pendingChoice) {
                        BotCtx cc; botDefaults(cc, noisy, 0, gseed * 37 + shots); cc.budget = budget; cc.nm = nm; cc.snk = &se.ev;
                        real v[3]; snkApplyChoice(g, pre, snkChooseOption(cc, g, pre, v));
                    }
                }
                visitStart = g.turn != lastTurn || g.lastFoul; lastTurn = g.turn;
            }
            rec.winner = g.winner;
        }
        games++;
        if (snk) famFrames[fam]++;
        if (rec.winner < 0) { unfinished++; if (snk) famUnfinished[fam]++; continue; }
        if (snk) { famPos[fam] += (long)rec.pos.size(); famMoverWon[fam] += rec.winner == startMover; }
        positions += (int)rec.pos.size();
        writeGame(fg, rec);
        if (fs) fflush(fs);
        if (fp && snk) writeRawFrame(fp, rec.winner, noisy ? (float)skill : 0.0f, (int)budget, fam, raw);
        if (games % 5 == 0) { printf("[%s seed %llu] games %d (unfinished %d) positions %d  %.1f min\n", snk ? "snk" : "8", (unsigned long long)seed, games, unfinished, positions, (now() - t0) / 60); fflush(stdout); }
    }
    printf("[%s seed %llu] done: %d games, %d positions\n", snk ? "snk" : "8", (unsigned long long)seed, games - unfinished, positions);
    if (snk) {
        printf("%-10s %7s %9s %8s %10s %10s\n", "family", "frames", "positions", "pos/frm", "mover won", "unfinished");
        for (int f = 0; f < FAM_N; ++f) {
            if (!famFrames[f]) continue;
            long fin = famFrames[f] - famUnfinished[f];
            printf("%-10s %7ld %9ld %8.1f %9.1f%% %10ld\n", FAM_NAMES[f], famFrames[f], famPos[f], fin ? (double)famPos[f] / fin : 0.0, fin ? 100.0 * famMoverWon[f] / fin : 0.0, famUnfinished[f]);
        }
        if (famFailed) printf("starts that fell back to the break: %ld\n", famFailed);
    }
    fclose(fg); if (fs) fclose(fs); if (fp) fclose(fp);
    return 0;
}
