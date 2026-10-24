// tune.cpp — fit the evaluators to self-play data (Texel tuning with TD(lambda) targets).
//
// 8-ball:  tune --game 8   --data DIR... --out w.bin [--init w.bin] [--lambda 0.7] [--epochs 200]
// snooker: tune --game snk --data DIR... --table table.bin --out params.txt [--init params.txt] [--lambda 0.7]
//
// The target of a position is the TD(lambda) return built from the search values of the
// following positions and the final result (common.h: tdTargets). lambda = 1 is classic
// Texel tuning on game results only; smaller lambda lowers variance via the search.
// Games are split 85/15 into train/validation by game, so positions of one game never leak.
#include "common.h"
#include <random>
#include <dirent.h>
using namespace train;

static std::vector<std::string> gameFiles(const std::vector<std::string>& dirs) {
    std::vector<std::string> out;
    for (auto& d : dirs) {
        DIR* dp = opendir(d.c_str());
        if (!dp) { out.push_back(d); continue; }   // a file
        while (dirent* e = readdir(dp)) { std::string n = e->d_name; if (n.size() > 6 && n.substr(n.size() - 6) == ".games") out.push_back(d + "/" + n); }
        closedir(dp);
    }
    std::sort(out.begin(), out.end());
    return out;
}
static double bce(double p, double y) { p = std::min(std::max(p, 1e-6), 1 - 1e-6); return -(y * std::log(p) + (1 - y) * std::log(1 - p)); }

// ------------------------------------------------------------------ 8-ball MLP
static double fwd(const std::vector<double>& w, const float* xf, double* h1, double* h2) {
    double x[NFEAT]; for (int j = 0; j < NFEAT; ++j) x[j] = xf[j];
    const double* W1 = w.data(); const double* b1 = W1 + H1 * NFEAT; const double* W2 = b1 + H1; const double* b2 = W2 + H2 * H1;
    const double* W3 = b2 + H2; const double* b3 = W3 + H2;
    for (int i = 0; i < H1; ++i) { double s = b1[i]; for (int j = 0; j < NFEAT; ++j) s += W1[i * NFEAT + j] * x[j]; h1[i] = std::tanh(s); }
    for (int i = 0; i < H2; ++i) { double s = b2[i]; for (int j = 0; j < H1; ++j) s += W2[i * H1 + j] * h1[j]; h2[i] = std::tanh(s); }
    double z = b3[0]; for (int j = 0; j < H2; ++j) z += W3[j] * h2[j];
    return z;
}
static void bwd(const std::vector<double>& w, const float* xf, const double* h1, const double* h2, double dz, std::vector<double>& g) {
    const double* W2 = w.data() + H1 * NFEAT + H1; const double* W3 = W2 + H2 * H1 + H2;
    double* gW1 = g.data(); double* gb1 = gW1 + H1 * NFEAT; double* gW2 = gb1 + H1; double* gb2 = gW2 + H2 * H1; double* gW3 = gb2 + H2; double* gb3 = gW3 + H2;
    gb3[0] += dz; double d2[H2], d1[H1] = {0};
    for (int j = 0; j < H2; ++j) { gW3[j] += dz * h2[j]; d2[j] = dz * W3[j] * (1 - h2[j] * h2[j]); }
    for (int i = 0; i < H2; ++i) { gb2[i] += d2[i]; for (int j = 0; j < H1; ++j) { gW2[i * H1 + j] += d2[i] * h1[j]; d1[j] += d2[i] * W2[i * H1 + j]; } }
    for (int i = 0; i < H1; ++i) { double di = d1[i] * (1 - h1[i] * h1[i]); gb1[i] += di; for (int j = 0; j < NFEAT; ++j) gW1[i * NFEAT + j] += di * xf[j]; }
}

struct Sample { const float* f; double y; double res; };

int main(int argc, char** argv) {
    Args a(argc, argv);
    const bool snk = a.str("--game", "8") == "snk";
    const double lambda = a.num("--lambda", 0.7);
    const int epochs = (int)a.num("--epochs", snk ? 400 : 200);
    std::vector<GameRec> games;
    for (auto& f : gameFiles(a.list("--data"))) readGames(f, games, snk ? 1 : 0);
    std::mt19937_64 gen((u64)a.num("--seed", 1));
    std::shuffle(games.begin(), games.end(), gen);
    size_t nVal = games.size() * 15 / 100;
    std::vector<Sample> tr, va;
    std::vector<double> y;
    for (size_t gi = 0; gi < games.size(); ++gi) {
        tdTargets(games[gi], lambda, y);
        for (size_t t = 0; t < games[gi].pos.size(); ++t) {
            const Pos& p = games[gi].pos[t];
            double res = (games[gi].winner == p.mover) ? 1 : 0;
            (gi < nVal ? va : tr).push_back({p.f.data(), y[t], res});
        }
    }
    printf("%s: %zu games, %zu train / %zu validation positions, lambda %.2f\n", snk ? "snooker" : "8-ball", games.size(), tr.size(), va.size(), lambda);
    if (tr.size() < 50) { fprintf(stderr, "not enough data\n"); return 1; }

    if (!snk) {
        std::vector<double> w(NWEIGHTS), m(NWEIGHTS, 0), v(NWEIGHTS, 0), g(NWEIGHTS);
        std::vector<float> init;
        if (a.has("--init") && loadFloats(a.str("--init", ""), init, NWEIGHTS)) for (int k = 0; k < NWEIGHTS; ++k) w[k] = init[k];
        else { std::normal_distribution<double> nd(0, 1); int k = 0;
            for (int i = 0; i < H1 * NFEAT; ++i) w[k++] = nd(gen) / std::sqrt((double)NFEAT); for (int i = 0; i < H1; ++i) w[k++] = 0;
            for (int i = 0; i < H2 * H1; ++i) w[k++] = nd(gen) / std::sqrt((double)H1); for (int i = 0; i < H2; ++i) w[k++] = 0;
            for (int i = 0; i < H2; ++i) w[k++] = nd(gen) / std::sqrt((double)H2) * 0.5; w[k++] = 0; }
        double h1[H1], h2[H2];
        auto evalSet = [&](std::vector<Sample>& S, const std::vector<double>& ww, double& lossT, double& lossR) {
            lossT = lossR = 0; for (auto& s : S) { double p = 1 / (1 + std::exp(-fwd(ww, s.f, h1, h2))); lossT += bce(p, s.y); lossR += bce(p, s.res); }
            lossT /= std::max<size_t>(1, S.size()); lossR /= std::max<size_t>(1, S.size()); };
        auto evalHeur = [&](std::vector<Sample>& S) { double L = 0; for (auto& s : S) { real f[NFEAT]; for (int j = 0; j < NFEAT; ++j) f[j] = s.f[j]; L += bce(psigmoid(heuristicLogit(f)), s.res); } return L / std::max<size_t>(1, S.size()); };
        double l0t, l0r; evalSet(va, w, l0t, l0r);
        printf("validation before: target loss %.4f, result loss %.4f | heuristic result loss %.4f\n", l0t, l0r, evalHeur(va));
        double lr = a.num("--lr", 0.002), wd = 1e-4, b1 = 0.9, b2 = 0.999; int step = 0;
        std::vector<double> best = w; double bestL = 1e9; int bs = 128;
        for (int ep = 0; ep < epochs; ++ep) {
            std::shuffle(tr.begin(), tr.end(), gen);
            for (size_t s0 = 0; s0 < tr.size(); s0 += bs) {
                std::fill(g.begin(), g.end(), 0);
                size_t s1 = std::min(tr.size(), s0 + bs);
                for (size_t i = s0; i < s1; ++i) { double z = fwd(w, tr[i].f, h1, h2); double p = 1 / (1 + std::exp(-z)); bwd(w, tr[i].f, h1, h2, (p - tr[i].y) / (s1 - s0), g); }
                step++;
                for (int k = 0; k < NWEIGHTS; ++k) {
                    double gk = g[k] + wd * w[k];
                    m[k] = b1 * m[k] + (1 - b1) * gk; v[k] = b2 * v[k] + (1 - b2) * gk * gk;
                    w[k] -= lr * (m[k] / (1 - std::pow(b1, step))) / (std::sqrt(v[k] / (1 - std::pow(b2, step))) + 1e-8);
                }
            }
            double lt, lrr; evalSet(va, w, lt, lrr);
            if (lt < bestL) { bestL = lt; best = w; }
            if (ep % 25 == 0 || ep == epochs - 1) { printf("epoch %d: validation target loss %.4f result loss %.4f\n", ep, lt, lrr); fflush(stdout); }
        }
        double lt, lrr; evalSet(va, best, lt, lrr);
        printf("RESULT 8ball val_target_loss %.4f val_result_loss %.4f (before %.4f / %.4f)\n", lt, lrr, l0t, l0r);
        std::vector<float> out(best.begin(), best.end());
        saveFloats(a.str("--out", "w8.bin"), out);
        return 0;
    }

    // ---- snooker: 6 parameters of E (positional points) + eScale, table fixed
    SnkEvalOwned se;
    if (!loadSnkEval(a.str("--init", "none"), a.str("--table", ""), se) || !se.ev.table) { fprintf(stderr, "need --table\n"); return 1; }
    auto P = [&](const SnkEval& e, const float* f) { real ff[SNK_NF]; for (int j = 0; j < SNK_NF; ++j) ff[j] = f[j]; return snkWin(e, ff[0] + e.eScale * snkVisitP(e, ff), (int)ff[1]); };
    auto loss = [&](const SnkEval& e, std::vector<Sample>& S, bool res) { double L = 0; for (auto& s : S) L += bce(P(e, s.f), res ? s.res : s.y); return L / std::max<size_t>(1, S.size()); };
    auto get = [](SnkEval& e, int k) -> real& { real* p[6] = {&e.a0, &e.a1, &e.a2, &e.a3, &e.a4, &e.eScale}; return *p[k]; };
    SnkEval e = se.ev;
    printf("validation before: target loss %.4f, result loss %.4f  params %.3f %.3f %.3f %.3f %.3f %.3f\n", loss(e, va, false), loss(e, va, true), e.a0, e.a1, e.a2, e.a3, e.a4, e.eScale);
    double m[6] = {0}, v[6] = {0}; const double lr = a.num("--lr", 0.03);
    SnkEval best = e; double bestL = loss(e, va, false);
    for (int it = 1; it <= epochs; ++it) {
        double g[6];
        for (int k = 0; k < 6; ++k) {
            const double h = 1e-3;
            SnkEval ep = e, em = e; get(ep, k) += h; get(em, k) -= h;
            g[k] = (loss(ep, tr, false) - loss(em, tr, false)) / (2 * h);
        }
        for (int k = 0; k < 6; ++k) {
            m[k] = 0.9 * m[k] + 0.1 * g[k]; v[k] = 0.999 * v[k] + 0.001 * g[k] * g[k];
            get(e, k) -= lr * (m[k] / (1 - std::pow(0.9, it))) / (std::sqrt(v[k] / (1 - std::pow(0.999, it))) + 1e-8);
        }
        if (e.eScale < 0.05) e.eScale = 0.05;
        double lv = loss(e, va, false);
        if (lv < bestL) { bestL = lv; best = e; }
        if (it % 50 == 0) { printf("iter %d: validation target loss %.4f\n", it, lv); fflush(stdout); }
    }
    printf("RESULT snooker val_target_loss %.4f val_result_loss %.4f params %.3f %.3f %.3f %.3f %.3f %.3f\n", loss(best, va, false), loss(best, va, true), best.a0, best.a1, best.a2, best.a3, best.a4, best.eScale);
    saveSnkParams(a.str("--out", "snk_params.txt"), best);
    return 0;
}
