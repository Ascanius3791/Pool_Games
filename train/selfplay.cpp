// selfplay.cpp — physics self-play data generator (both games), append-only shards.
//
// usage: selfplay --game 8|snk --out DIR --minutes M --seed S
//                 [--noisy 1] [--skill 1.0] [--budget 250] [--random-start 0.5]
//                 [--weights8 FILE] [--snk-params FILE] [--snk-table FILE] [--snk-net FILE]
// Writes DIR/<game>_<seed>.games (see common.h) and, for snooker, DIR/<game>_<seed>.stats
// (one line per shot: kind targetClass potted foul visitStart) for calibrating the abstract
// simulator, and DIR/snk_<seed>.posrec: the full raw positions with the frame result, for
// training a neural evaluator later (format: SnkPosRec in common.h, documented in TRAINING.md).
// Every finished game is flushed, so killing the process loses at most one game.
#include "common.h"
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
        bool random = rng.uniform() < randomStart;
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
            if (random) randomStartSnk(g, rng);
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
                if (fs) {
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
        if (rec.winner < 0) { unfinished++; continue; }
        positions += (int)rec.pos.size();
        writeGame(fg, rec);
        if (fs) fflush(fs);
        if (fp && snk) {
            int32_t h[4] = {0x534E4B50, 2, (int32_t)raw.size(), rec.winner};
            float sk = noisy ? (float)skill : 0.0f; int32_t bud = (int32_t)budget;
            fwrite(h, 4, 4, fp); fwrite(&sk, 4, 1, fp); fwrite(&bud, 4, 1, fp);
            fwrite(raw.data(), sizeof(SnkPosRec), raw.size(), fp); fflush(fp);
        }
        if (games % 5 == 0) { printf("[%s seed %llu] games %d (unfinished %d) positions %d  %.1f min\n", snk ? "snk" : "8", (unsigned long long)seed, games, unfinished, positions, (now() - t0) / 60); fflush(stdout); }
    }
    printf("[%s seed %llu] done: %d games, %d positions\n", snk ? "snk" : "8", (unsigned long long)seed, games - unfinished, positions);
    fclose(fg); if (fs) fclose(fs); if (fp) fclose(fp);
    return 0;
}
