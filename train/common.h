// common.h — shared helpers for the native training tools (self-play, tuning, arena).
// Not used by the browser build.
#pragma once
#include "../engine/bot.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

namespace train {
using namespace pool;

inline double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ tiny CLI parser
struct Args {
    std::vector<std::string> v;
    Args(int argc, char** argv) { for (int i = 1; i < argc; ++i) v.push_back(argv[i]); }
    bool has(const char* k) const { for (auto& s : v) if (s == k) return true; return false; }
    std::string str(const char* k, const std::string& def) const {
        for (size_t i = 0; i + 1 < v.size(); ++i) if (v[i] == k) return v[i + 1];
        return def;
    }
    double num(const char* k, double def) const { std::string s = str(k, ""); return s.empty() ? def : atof(s.c_str()); }
    std::vector<std::string> list(const char* k) const {   // all values after k until the next --flag
        std::vector<std::string> out;
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == k) { for (size_t j = i + 1; j < v.size() && v[j].rfind("--", 0) != 0; ++j) out.push_back(v[j]); }
        return out;
    }
};

// ------------------------------------------------------------------ weights / evaluator files
// 8-ball MLP: raw float32[NWEIGHTS]
inline bool loadFloats(const std::string& path, std::vector<float>& w, size_t n) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    w.resize(n); size_t got = fread(w.data(), sizeof(float), n, f); fclose(f);
    return got == n;
}
inline bool saveFloats(const std::string& path, const std::vector<float>& w) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    fwrite(w.data(), sizeof(float), w.size(), f); fclose(f); return true;
}
// snooker win table: int32 tD0, tND, tNR, then uint8[tNR*tND]
struct SnkTable { int tD0 = 160, tND = 321, tNR = 148; std::vector<unsigned char> t; };
inline bool loadTable(const std::string& path, SnkTable& T) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    int h[3]; if (fread(h, 4, 3, f) != 3) { fclose(f); return false; }
    T.tD0 = h[0]; T.tND = h[1]; T.tNR = h[2]; T.t.resize((size_t)T.tND * T.tNR);
    size_t got = fread(T.t.data(), 1, T.t.size(), f); fclose(f);
    return got == T.t.size();
}
inline bool saveTable(const std::string& path, const SnkTable& T) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
    int h[3] = {T.tD0, T.tND, T.tNR}; fwrite(h, 4, 3, f); fwrite(T.t.data(), 1, T.t.size(), f); fclose(f); return true;
}
// snooker evaluator parameters: text "a0 a1 a2 a3 a4 eScale"
inline bool loadSnkParams(const std::string& path, SnkEval& e) {
    FILE* f = fopen(path.c_str(), "r"); if (!f) return false;
    double v[6]; int n = fscanf(f, "%lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]); fclose(f);
    if (n != 6) return false;
    e.a0 = v[0]; e.a1 = v[1]; e.a2 = v[2]; e.a3 = v[3]; e.a4 = v[4]; e.eScale = v[5]; return true;
}
inline bool saveSnkParams(const std::string& path, const SnkEval& e) {
    FILE* f = fopen(path.c_str(), "w"); if (!f) return false;
    fprintf(f, "%.6f %.6f %.6f %.6f %.6f %.6f\n", e.a0, e.a1, e.a2, e.a3, e.a4, e.eScale); fclose(f); return true;
}
// a complete snooker evaluator owned by the caller
struct SnkEvalOwned { SnkEval ev; SnkTable tab; std::vector<float> net; };
inline bool loadSnkEval(const std::string& params, const std::string& table, SnkEvalOwned& o) {
    snkEvalDefaults(o.ev);
    if (!params.empty() && params != "none" && !loadSnkParams(params, o.ev)) { fprintf(stderr, "cannot read %s\n", params.c_str()); return false; }
    if (!table.empty() && table != "none") {
        if (!loadTable(table, o.tab)) { fprintf(stderr, "cannot read %s\n", table.c_str()); return false; }
        o.ev.table = o.tab.t.data(); o.ev.tD0 = o.tab.tD0; o.ev.tND = o.tab.tND; o.ev.tNR = o.tab.tNR;
    }
    return true;
}
// optional neural evaluator on top (needs the table); skill = human-error level it judges for
inline bool loadSnkNet(const std::string& path, double skill, SnkEvalOwned& o) {
    o.ev.skill = skill;
    if (path.empty() || path == "none") return true;
    if (!loadFloats(path, o.net, SNK_NET_NW)) { fprintf(stderr, "cannot read %s (need %d floats)\n", path.c_str(), SNK_NET_NW); return false; }
    if (!o.ev.table) { fprintf(stderr, "the neural evaluator needs a win table\n"); return false; }
    o.ev.net = o.net.data(); return true;
}

// ------------------------------------------------------------------ self-play records
// A shard file is a sequence of games:
//   int32 'GAME', int32 type (0 8-ball, 1 snooker), int32 n, int32 winner, int32 nf
//   n x { int32 mover, float searchValue (mover's win chance), float f[nf] }
struct Pos { int mover; float v; std::vector<float> f; };
struct GameRec { int type, winner; std::vector<Pos> pos; };
const int GAME_MAGIC = 0x454d4147;
inline void writeGame(FILE* f, const GameRec& g) {
    int nf = g.pos.empty() ? 0 : (int)g.pos[0].f.size();
    int h[5] = {GAME_MAGIC, g.type, (int)g.pos.size(), g.winner, nf};
    fwrite(h, 4, 5, f);
    for (auto& p : g.pos) { fwrite(&p.mover, 4, 1, f); fwrite(&p.v, 4, 1, f); fwrite(p.f.data(), 4, nf, f); }
    fflush(f);
}
inline bool readGames(const std::string& path, std::vector<GameRec>& out, int type) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    for (;;) {
        int h[5]; if (fread(h, 4, 5, f) != 5 || h[0] != GAME_MAGIC) break;
        GameRec g; g.type = h[1]; g.winner = h[3]; g.pos.resize(h[2]);
        bool ok = true;
        for (auto& p : g.pos) {
            p.f.resize(h[4]);
            if (fread(&p.mover, 4, 1, f) != 1 || fread(&p.v, 4, 1, f) != 1 || fread(p.f.data(), 4, h[4], f) != (size_t)h[4]) { ok = false; break; }
        }
        if (!ok) break;              // truncated tail (killed process): drop it
        if (g.type == type) out.push_back(std::move(g));
    }
    fclose(f); return true;
}

// TD(lambda) targets in the mover's perspective. lambda = 1: pure game result (Texel),
// lambda = 0: the next position's search value (pure bootstrap).
inline void tdTargets(const GameRec& g, double lambda, std::vector<double>& y) {
    const int n = (int)g.pos.size();
    y.assign(n, 0.5);
    if (g.winner < 0 || n == 0) return;
    double G = g.winner == 0 ? 1 : 0;           // player-0 perspective
    for (int t = n - 1; t >= 0; --t) {
        if (t < n - 1) {
            double u = g.pos[t + 1].mover == 0 ? g.pos[t + 1].v : 1 - g.pos[t + 1].v;
            G = (1 - lambda) * u + lambda * G;
        }
        y[t] = g.pos[t].mover == 0 ? G : 1 - G;
    }
}

// ------------------------------------------------------------------ raw snooker positions
// raw snooker position record (little-endian). Per frame: header {int32 magic 'SNKP',
// int32 version 2, int32 nPos, int32 winner (0/1), float skill (0 = no human error),
// int32 budget}, then nPos x SnkPosRec (104 bytes). Coordinates in units of 0.1 mm,
// table centre = 0, x along the long axis (baulk at negative x).
#pragma pack(push, 1)
struct SnkPosRec {
    int16_t score0, score1;
    uint8_t mover, phase, nextColour, flags;   // flags: 1 ball in hand, 2 free ball
    float value;                               // search value of the chosen shot (mover's view)
    uint32_t onMask;                           // bit i: ball i on the table
    int16_t x[22], y[22];
};
#pragma pack(pop)
static_assert(sizeof(SnkPosRec) == 104, "SnkPosRec layout");
inline void snkRaw(const GameSnk& g, float value, SnkPosRec& r) {
    r.score0 = (int16_t)g.score[0]; r.score1 = (int16_t)g.score[1];
    r.mover = (uint8_t)g.turn; r.phase = (uint8_t)g.phase; r.nextColour = (uint8_t)g.nextColour;
    r.flags = (uint8_t)((g.ballInHand ? 1 : 0) | (g.freeBall ? 2 : 0));
    r.value = value; r.onMask = 0;
    for (int i = 0; i < 22; ++i) {
        const Ball& b = g.sim.b[i];
        if (i < g.sim.n && b.on) r.onMask |= 1u << i;
        r.x[i] = (int16_t)lround(b.x * 1e4); r.y[i] = (int16_t)lround(b.y * 1e4);
    }
}

// rebuild the game state of a raw record (enough for evaluation: balls, scores, phase, flags)
inline void snkFromRaw(const SnkPosRec& r, GameSnk& g) {
    newGameSnk(g, 1, 0); g.sim.trackOrient = 0;
    for (int i = 0; i < 22; ++i) { resetBall(g.sim.b[i], r.x[i] * 1e-4, r.y[i] * 1e-4); g.sim.b[i].on = (r.onMask >> i) & 1; }
    g.score[0] = r.score0; g.score[1] = r.score1; g.turn = r.mover; g.phase = r.phase; g.nextColour = r.nextColour;
    g.ballInHand = r.flags & 1; g.freeBall = (r.flags >> 1) & 1; g.isBreak = 0; g.pendingChoice = 0;
}
// a frame of raw records: header {int32 magic, int32 version, int32 nPos, int32 winner, float skill, int32 budget}
struct SnkRawFrame { int winner, budget; float skill; std::vector<SnkPosRec> pos; };
inline bool readRawFrames(const std::string& path, std::vector<SnkRawFrame>& out) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
    for (;;) {
        int32_t h[4]; float sk; int32_t bud;
        if (fread(h, 4, 4, f) != 4 || h[0] != 0x534E4B50 || fread(&sk, 4, 1, f) != 1 || fread(&bud, 4, 1, f) != 1) break;
        SnkRawFrame fr; fr.winner = h[3]; fr.skill = sk; fr.budget = bud; fr.pos.resize(h[2]);
        if (fread(fr.pos.data(), sizeof(SnkPosRec), fr.pos.size(), f) != fr.pos.size()) break;   // truncated tail
        out.push_back(std::move(fr));
    }
    fclose(f); return true;
}

// ------------------------------------------------------------------ random mid-game positions
template <class G> bool randomSpot(G& g, int id, Rng& r, real marginX = 0, real marginY = 0) {
    const Sim& s = g.sim; const real R = s.P.R;
    for (int k = 0; k < 200; ++k) {
        real x = r.uniform(-s.T.L / 2 + R + 0.01 + marginX, s.T.L / 2 - R - 0.01);
        real y = r.uniform(-s.T.W / 2 + R + 0.01 + marginY, s.T.W / 2 - R - 0.01 - marginY);
        bool ok = true;
        for (int i = 0; i < s.n; ++i) {
            if (i == id || !s.b[i].on) continue;
            real dx = s.b[i].x - x, dy = s.b[i].y - y;
            if (dx * dx + dy * dy < 4.4 * R * R) { ok = false; break; }
        }
        if (ok) { resetBall(g.sim.b[id], x, y); return true; }
    }
    return false;
}
inline void randomStart8(Game8& g, Rng& r) {
    Sim& s = g.sim;
    for (int i = 0; i < 16; ++i) s.b[i].on = 0;
    int keepS = 1 + r.below(7), keepT = 1 + r.below(7);
    int solids[7] = {1, 2, 3, 4, 5, 6, 7}, stripes[7] = {9, 10, 11, 12, 13, 14, 15};
    for (int i = 6; i > 0; --i) { int j = r.below(i + 1); std::swap(solids[i], solids[j]); j = r.below(i + 1); std::swap(stripes[i], stripes[j]); }
    std::vector<int> ids = {0, 8};
    for (int i = 0; i < keepS; ++i) ids.push_back(solids[i]);
    for (int i = 0; i < keepT; ++i) ids.push_back(stripes[i]);
    for (int id : ids) if (!randomSpot(g, id, r)) resetBall(s.b[id], 0, 0);
    g.isBreak = 0; g.kitchenOnly = 0; g.ballInHand = r.uniform() < 0.15;
    g.turn = r.below(2);
    if (keepS + keepT < 14 || r.uniform() < 0.7) { g.group[0] = 1 + r.below(2); g.group[1] = 3 - g.group[0]; }
    else g.group[0] = g.group[1] = GROUP_NONE;
}
inline void randomStartSnk(GameSnk& g, Rng& r) {
    Sim& s = g.sim;
    for (int i = 0; i < s.n; ++i) s.b[i].on = 0;
    int reds = r.below(16);                     // 0..15 reds left
    std::vector<int> ids = {0};
    for (int i = 1; i <= reds; ++i) ids.push_back(i);
    int firstCol = 16;
    if (reds == 0) firstCol = 16 + r.below(6);  // somewhere in the colour sequence
    for (int c = firstCol; c <= 21; ++c) ids.push_back(c);
    SnkGeo G; snkGeo(s.T, G);
    // colours mostly on their spots, the rest scattered
    for (int id : ids) {
        if (id >= 16 && r.uniform() < 0.6) { resetBall(s.b[id], G.spotX[id], G.spotY[id]); bool clash = false;
            for (int j = 0; j < s.n; ++j) if (j != id && s.b[j].on) { real dx = s.b[j].x - s.b[id].x, dy = s.b[j].y - s.b[id].y; if (dx * dx + dy * dy < 4.4 * s.P.R * s.P.R) clash = true; }
            if (!clash) continue; }
        if (!randomSpot(g, id, r)) resetBall(s.b[id], 0, 0.5);
    }
    g.phase = reds > 0 ? SNK_RED_ON : SNK_SEQUENCE; g.nextColour = firstCol;
    int taken = (15 - reds) * 8 + 0; for (int c = 16; c < firstCol; ++c) taken += snkValue(c);
    int a = (int)(taken * r.uniform()), d = (int)r.uniform(-40, 40);
    g.score[0] = a; g.score[1] = pmax(0, taken - a + d);
    g.turn = r.below(2); g.isBreak = 0; g.ballInHand = 0; g.brk = 0;
    g.freeBall = 0; g.pendingChoice = 0;
}

} // namespace train
