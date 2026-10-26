// replay.cpp — replays a game saved by the page (text format: see web/template.html, "replays")
// with the native engine, for looking into what happened.
//
// usage: replay FILE                       one line per stroke: result, score, choice
//        replay FILE --at N                ball positions before stroke N (1-based)
//        replay FILE --think N [--budget B] [--snk-params F --snk-table F --snk-net F]
//                                          the bot's decision for the position before stroke N
//                                          (the replay's human-error profile and referee) and, if
//                                          stroke N was a foul with options, the incoming player's
//                                          values for them. Evaluator defaults: train/baseline.
#include "common.h"
#include <fstream>
#include <sstream>
#include <map>
using namespace train;

struct Move { int p = 0, nom = 0, fb = 0, ch = 0; bool at = false; double x = 0, y = 0, s[4] = {0, 0, 0, 0}; std::string c; };
struct Replay { std::map<std::string, std::string> tags; std::vector<Move> moves; };

static bool nums(const std::string& t, double* v, int n) {
    std::stringstream ss(t); std::string x; int k = 0;
    while (std::getline(ss, x, ',')) { if (k >= n) return false; char* e; v[k++] = strtod(x.c_str(), &e); if (*e) return false; }
    return k == n;
}
static bool parse(const std::string& path, Replay& r) {
    std::ifstream f(path); if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    std::string line; int ln = 0;
    while (std::getline(f, line)) {
        ++ln;
        size_t b = line.find_first_not_of(" \t\r"); if (b == std::string::npos) continue;
        line = line.substr(b);
        if (line[0] == '[') {
            size_t sp = line.find(' '), q1 = line.find('"'), q2 = line.rfind('"');
            if (sp != std::string::npos && q1 != std::string::npos && q2 > q1) r.tags[line.substr(1, sp - 1)] = line.substr(q1 + 1, q2 - q1 - 1);
            continue;
        }
        Move m;
        size_t c0 = line.find('{'), c1 = line.find('}');
        if (c0 != std::string::npos && c1 != std::string::npos && c1 > c0) { m.c = line.substr(c0 + 1, c1 - c0 - 1); line.erase(c0, c1 - c0 + 1); }
        std::stringstream ss(line); std::string t; bool haveS = false;
        ss >> t; ss >> t; m.p = t == "B";
        while (ss >> t) {
            const char k = t[0]; const std::string rest = t.substr(1);
            if (k == '@') { double v[2]; if (!nums(rest, v, 2)) goto bad; m.at = true; m.x = v[0]; m.y = v[1]; }
            else if (k == 's') { if (!nums(rest, m.s, 4)) goto bad; haveS = true; }
            else if (k == 'i') {}
            else if (k == 'n') m.nom = atoi(rest.c_str());
            else if (k == 'f') m.fb = atoi(rest.c_str());
            else if (k == '!') m.ch = rest == "P" ? CH_PLAY : rest == "B" ? CH_PUTBACK : rest == "R" ? CH_REPLACE : 0;
            else goto bad;
        }
        if (!haveS) goto bad;
        r.moves.push_back(m);
        continue;
    bad:
        fprintf(stderr, "%s:%d: cannot read stroke\n", path.c_str(), ln); return false;
    }
    return !r.moves.empty();
}
static const char* foulName(int f) {
    static const char* n[] = {"", "in-off", "missed every ball", "wrong ball first", "potted a ball not on", "snookered behind the free ball"};
    return f >= 0 && f <= 5 ? n[f] : "?";
}
static const char* ballName(int id) {
    static const char* c[] = {"yellow", "green", "brown", "blue", "pink", "black"};
    return id == 0 ? "cue" : id <= 15 ? "red" : id <= 21 ? c[id - 16] : "?";
}
static void printPos(const GameSnk& g) {
    printf("  score %d-%d, %s to play, phase %s%s%s\n", g.score[0], g.score[1], g.turn ? "B" : "A",
           g.phase == SNK_RED_ON ? "red on" : g.phase == SNK_COLOUR_ON ? "colour on" : ballName(g.nextColour),
           g.ballInHand ? ", ball in hand" : "", g.freeBall ? ", free ball" : "");
    for (int i = 0; i < g.sim.n; ++i) if (g.sim.b[i].on) printf("  %2d %-6s %+.4f %+.4f\n", i, ballName(i), g.sim.b[i].x, g.sim.b[i].y);
}

int main(int argc, char** argv) {
    Args a(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: replay FILE [--at N] [--think N --budget B]\n"); return 1; }
    Replay r; if (!parse(argv[1], r)) return 1;
    if (r.tags["Game"] != "snooker") { fprintf(stderr, "only snooker replays for now (Game \"%s\")\n", r.tags["Game"].c_str()); return 1; }
    const int at = (int)a.num("--at", 0), think = (int)a.num("--think", 0);
    const double budget = a.num("--budget", 2000);
    // player A's profile (Noise), player B's (NoiseB) if B played with another one
    NoiseModel nmAB[2]; defaultNoise(nmAB[0]);
    auto readNoise = [&](const char* tag, NoiseModel& nm) {
        double v[18];
        if (!nums(r.tags[tag], v, 18)) return false;
        for (int c = 0; c < 4; ++c) { nm.lo[c] = v[c]; nm.hi[c] = v[4 + c]; nm.k[c] = v[8 + c]; nm.mu[c] = v[12 + c]; }
        nm.missProb = v[16]; nm.missMul = v[17]; nm.skill = 1; return true;
    };
    readNoise("Noise", nmAB[0]);
    if (!readNoise("NoiseB", nmAB[1])) nmAB[1] = nmAB[0];
    const NoiseModel& nm = nmAB[0];
    const bool noisy = r.tags["HumanError"] != "off" && !r.tags["HumanError"].empty();
    double ref[2] = {0, 0.5}; nums(r.tags["Referee"], ref, 2);
    const int refMode = (int)ref[0];
    SnkEvalOwned se;
    if (think && (!loadSnkEval(a.str("--snk-params", "train/baseline/snk_params.txt"), a.str("--snk-table", "train/baseline/snk_table.bin"), se) ||
                  !loadSnkNet(a.str("--snk-net", "train/baseline/snk_net.bin"), snkSkillOf(nm, noisy), se))) return 1;

    GameSnk g; newGameSnk(g, (u64)strtoull(r.tags["Seed"].c_str(), 0, 10), 0);
    g.nomRequired = r.tags["Nominate"] != "0";
    printf("%s v %s, %s, human error %s, referee %s, %zu strokes, result %s\n", r.tags["A"].c_str(), r.tags["B"].c_str(), r.tags["Id"].c_str(),
           r.tags["HumanError"].c_str(), refMode ? "pro rules" : "judged by profile", r.moves.size(), r.tags["Result"].c_str());
    for (size_t n = 1; n <= r.moves.size(); ++n) {
        const Move& m = r.moves[n - 1];
        const NoiseModel& nmP = nmAB[m.p ? 1 : 0];
        if (m.at) resetBall(g.sim.b[0], m.x, m.y);
        if (m.nom && g.phase == SNK_COLOUR_ON) g.nominated = m.nom;
        if (m.fb && g.freeBall) g.freeNom = m.fb;
        if ((int)n == at) { printf("before stroke %zu:\n", n); printPos(g); }
        if ((int)n == think) {
            BotCtx c; botDefaults(c, noisy, 0, 4242); c.budget = budget; c.nm = nmP; c.snk = &se.ev; c.refMode = refMode; c.refTau = ref[1];
            BotDecision d; botThink(c, g, d);
            printf("before stroke %zu:\n", n); printPos(g);
            printf("  bot (budget %.0f%s): kind %s, target %s, value %.3f, phi %.4f V %.2f a %+.2f b %+.2f%s | played: phi %.4f V %.2f a %+.2f b %+.2f\n",
                   budget, noisy ? ", human error" : "", d.kind == 0 ? "pot" : d.kind == 1 ? "safety" : d.kind == 3 ? "escape" : "break",
                   d.tgt > 0 ? ballName(d.tgt) : "-", d.value, d.sp.phi, d.sp.V, d.sp.a, d.sp.b, d.place ? " (placed)" : "", m.s[0], m.s[1], m.s[2], m.s[3]);
            if (c.mxDirect >= 0) printf("  referee as the bot sees it: clear sight %d, escape chance p* %.2f, %d escape windows\n", c.mxDirect, c.mxPStar, c.nEsc);
        }
        GameSnk pre = g;
        ShotParams sp; sp.phi = m.s[0]; sp.V = m.s[1]; sp.a = m.s[2]; sp.b = m.s[3];
        Rng rng; rng.seed(1);
        executeShot(g, sp, false, nmP, rng);
        simulate(g.sim);
        const int fh = g.sim.ev.firstHit, nPot = g.sim.ev.nPot; int pots[22]; for (int k = 0; k < nPot && k < 22; ++k) pots[k] = g.sim.ev.pot[k];
        endShotSnk(g);
        RefInfo ri = {};
        if (g.pendingChoice && g.winner < 0) snkReferee(pre, g, sp, nmP, refMode, ref[1], 13 * n, ri);
        printf("%3zu. %s  first %-6s", n, m.p ? "B" : "A", fh > 0 ? ballName(fh) : "none");
        std::string pot; for (int k = 0; k < nPot && k < 22; ++k) { pot += " "; pot += ballName(pots[k]); }
        if (!pot.empty()) printf(" potted%s", pot.c_str());
        if (g.lastFoul) printf("  FOUL %s (%d)%s", foulName(g.lastFoul), g.foulPts, ri.miss ? ", miss" : "");
        else if (g.lastPts) printf("  +%d", g.lastPts);
        printf("  %d-%d", g.score[0], g.score[1]);
        if ((int)n == think && g.pendingChoice && g.winner < 0) {
            BotCtx c; botDefaults(c, noisy, 0, 777); c.budget = budget; c.nm = nmAB[g.turn]; c.snk = &se.ev; c.refMode = refMode; c.refTau = ref[1];
            real v[3]; int best = snkChooseOption(c, g, pre, v);
            printf("\n     options for %s: play %.3f, put back %.3f, replace %s -> %s", g.turn ? "B" : "A", v[0], v[1],
                   v[2] < -1e8 ? "n/a" : std::to_string(v[2]).c_str(), best == CH_PLAY ? "play" : best == CH_PUTBACK ? "put back" : "replace");
        }
        if (m.ch) { g.pendingChoice |= m.ch; snkApplyChoice(g, pre, m.ch); printf("  -> %s", m.ch == CH_PLAY ? "plays on" : m.ch == CH_PUTBACK ? "puts back" : "balls replaced"); }
        if (!m.c.empty()) printf("   {%s}", m.c.c_str());
        printf("\n");
        if (g.winner >= 0) { printf("frame won by %s %d-%d\n", g.winner ? "B" : "A", g.score[0], g.score[1]); break; }
    }
    return 0;
}
