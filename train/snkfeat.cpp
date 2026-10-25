// snkfeat.cpp — turn raw snooker positions (*.posrec) into training rows for the neural
// evaluator (train/snknet.py). The inputs come from the engine's own snkNetInputs, so the
// game and the trainer see exactly the same numbers.
//
// usage: snkfeat --data DIR|FILE... --out PREFIX [--snk-params F --snk-table F] [--snk-net F]
// Writes PREFIX.x: float16 rows of 21*SNK_NB ball inputs + SNK_NG globals (position order), and
// PREFIX.meta: float32 rows {f[0..7], pOld, value, result, skill, frame}, where f are the record
// features (snkFeatures), pOld the linear evaluator's win chance, value the search value of
// the chosen shot (mover's view), result 1 if the mover won the frame, and PREFIX.fam: uint8
// per row, the starting-position family of the frame (train/positions.h; 255 = unknown, for
// frames written before posrec version 3). With --snk-net, also
// PREFIX.pnet: float32 win chance of that net per position (to check the trainer's mirror).
#include "common.h"
#include <dirent.h>
using namespace train;

static std::vector<std::string> rawFiles(const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    for (auto& d : dirs) {
        DIR* dp = opendir(d.c_str());
        if (!dp) { out.push_back(d); continue; }
        while (dirent* e = readdir(dp)) { std::string n = e->d_name; if (n.size() > 7 && n.substr(n.size() - 7) == ".posrec") out.push_back(d + "/" + n); }
        closedir(dp);
    }
    std::sort(out.begin(), out.end());
    return out;
}
static uint16_t toHalf(float v) {   // round to nearest even, no NaN inputs
    uint32_t x; memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xff, m = x & 0x7fffff;
    int ex = (int)e - 127 + 15;
    if (ex >= 31) return (uint16_t)(sign | 0x7bff);
    if (ex <= 0) {
        if (ex < -10) return (uint16_t)sign;
        m |= 0x800000; int sh = 14 - ex; uint32_t hm = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1fff;
    uint32_t h = sign | ((uint32_t)ex << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) h++;
    return (uint16_t)h;
}

int main(int argc, char** argv) {
    Args a(argc, argv);
    const std::string out = a.str("--out", "snkfeat");
    SnkEvalOwned se;
    if (!loadSnkEval(a.str("--snk-params", "train/baseline/snk_params.txt"), a.str("--snk-table", "train/baseline/snk_table.bin"), se) || !se.ev.table) { fprintf(stderr, "need the linear evaluator (params + table)\n"); return 1; }
    SnkEvalOwned sn;
    const std::string netPath = a.str("--snk-net", "none");
    if (netPath != "none") { sn.tab = se.tab; sn.ev = se.ev; sn.ev.table = sn.tab.t.data(); if (!loadSnkNet(netPath, 1, sn)) return 1; }
    FILE* fn = netPath != "none" ? fopen((out + ".pnet").c_str(), "wb") : 0;
    FILE* fx = fopen((out + ".x").c_str(), "wb"); FILE* fm = fopen((out + ".meta").c_str(), "wb"); FILE* ff = fopen((out + ".fam").c_str(), "wb");
    if (!fx || !fm || !ff) { fprintf(stderr, "cannot write %s.*\n", out.c_str()); return 1; }
    const int NX = 21 * SNK_NB + SNK_NG;
    std::vector<uint16_t> row(NX);
    long frames = 0, positions = 0; double maxDiff = 0;
    const double t0 = now();
    for (auto& path : rawFiles(a.list("--data"))) {
        std::vector<SnkRawFrame> fr;
        readRawFrames(path, fr);
        for (auto& F : fr) {
            if (F.winner < 0 || F.winner > 1) continue;
            for (auto& r : F.pos) {
                GameSnk g; snkFromRaw(r, g);
                float bf[21 * SNK_NB], gf[SNK_NG]; real f[SNK_NF];
                snkNetInputs(g, g.ballInHand, F.skill, bf, gf, f);
                real pOld = snkWinNow(se.ev, g, g.ballInHand);
                { real f2[SNK_NF]; snkFeatures(g, g.ballInHand, f2); for (int j = 0; j < SNK_NF; ++j) maxDiff = std::max(maxDiff, (double)std::fabs(f2[j] - f[j])); }
                for (int j = 0; j < 21 * SNK_NB; ++j) row[j] = toHalf(bf[j]);
                for (int j = 0; j < SNK_NG; ++j) row[21 * SNK_NB + j] = toHalf(gf[j]);
                fwrite(row.data(), 2, NX, fx);
                float m[13];
                for (int j = 0; j < 8; ++j) m[j] = (float)f[j];
                m[8] = (float)pOld; m[9] = r.value; m[10] = F.winner == r.mover ? 1.f : 0.f; m[11] = F.skill; m[12] = (float)frames;
                fwrite(m, 4, 13, fm);
                { uint8_t fam = (uint8_t)(F.family >= 0 && F.family < 255 ? F.family : 255); fwrite(&fam, 1, 1, ff); }
                if (fn) { sn.ev.skill = F.skill; float pn = (float)snkWinNow(sn.ev, g, g.ballInHand); fwrite(&pn, 4, 1, fn); }
                positions++;
            }
            frames++;
        }
        printf("%s: %ld frames, %ld positions so far (%.0f s)\n", path.c_str(), frames, positions, now() - t0); fflush(stdout);
    }
    fclose(fx); fclose(fm); fclose(ff); if (fn) fclose(fn);
    printf("done: %ld frames, %ld positions, %d inputs per row; max |snkNetInputs f - snkFeatures| = %.3g\n", frames, positions, NX, maxDiff);
    return 0;
}
