// train.cpp — self-play data generation, MLP value training and evaluation.
// usage: train <std|noisy> <selfplay_seconds> <eval_seconds> <out_prefix> <seed>
#include "../engine/bot.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <string>
using namespace pool;

static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Sample { double f[NFEAT]; double y; };

// play one game; returns winner (-1 = draw/turn cap). Records samples if rec != nullptr.
static int playGame(u64 seed, bool noisy, BotCtx& b0, BotCtx& b1, std::vector<Sample>* rec, int* turnsOut) {
    Game8 g; newGame8(g, seed, 0); g.sim.trackOrient = 0;
    Rng exe; exe.seed(seed ^ 0xabcdefULL);
    NoiseModel nm; defaultNoise(nm);
    std::vector<std::pair<int, Sample>> st;
    int turns = 0;
    while (g.winner < 0 && turns < 90) {
        BotCtx& c = g.turn == 0 ? b0 : b1;
        if (rec && !g.isBreak) { Sample s; features(g, g.turn, g.ballInHand, s.f); st.push_back({g.turn, s}); }
        BotDecision d; botThink(c, g, d);
        if (d.place) resetBall(g.sim.b[0], d.px, d.py);
        executeShot(g, d.sp, noisy, nm, exe);
        simulate(g.sim, SIM_DT, 30);
        endShot8(g);
        turns++;
    }
    if (turnsOut) *turnsOut = turns;
    if (rec && g.winner >= 0)
        for (auto& p : st) { p.second.y = (p.first == g.winner) ? 1 : 0; rec->push_back(p.second); }
    return g.winner;
}

// ------------------------------------------------------------------ MLP training (double precision, Adam)
struct Net { std::vector<double> w; };
static double fwd(const std::vector<double>& w, const double* x, double* h1, double* h2) {
    const double* W1 = w.data(); const double* b1 = W1 + H1 * NFEAT;
    const double* W2 = b1 + H1; const double* b2 = W2 + H2 * H1;
    const double* W3 = b2 + H2; const double* b3 = W3 + H2;
    for (int i = 0; i < H1; ++i) { double s = b1[i]; for (int j = 0; j < NFEAT; ++j) s += W1[i * NFEAT + j] * x[j]; h1[i] = std::tanh(s); }
    for (int i = 0; i < H2; ++i) { double s = b2[i]; for (int j = 0; j < H1; ++j) s += W2[i * H1 + j] * h1[j]; h2[i] = std::tanh(s); }
    double z = b3[0]; for (int j = 0; j < H2; ++j) z += W3[j] * h2[j];
    return z;
}
static void backward(const std::vector<double>& w, const double* x, const double* h1, const double* h2, double dz, std::vector<double>& g) {
    const double* W2 = w.data() + H1 * NFEAT + H1;
    const double* W3 = W2 + H2 * H1 + H2;
    double* gW1 = g.data(); double* gb1 = gW1 + H1 * NFEAT; double* gW2 = gb1 + H1; double* gb2 = gW2 + H2 * H1;
    double* gW3 = gb2 + H2; double* gb3 = gW3 + H2;
    gb3[0] += dz;
    double d2[H2];
    for (int j = 0; j < H2; ++j) { gW3[j] += dz * h2[j]; d2[j] = dz * W3[j] * (1 - h2[j] * h2[j]); }
    double d1[H1] = {0};
    for (int i = 0; i < H2; ++i) { gb2[i] += d2[i]; for (int j = 0; j < H1; ++j) { gW2[i * H1 + j] += d2[i] * h1[j]; d1[j] += d2[i] * W2[i * H1 + j]; } }
    for (int i = 0; i < H1; ++i) { double di = d1[i] * (1 - h1[i] * h1[i]); gb1[i] += di; for (int j = 0; j < NFEAT; ++j) gW1[i * NFEAT + j] += di * x[j]; }
}
static double logloss(double z, double y) { double p = 1 / (1 + std::exp(-z)); p = std::min(std::max(p, 1e-7), 1 - 1e-7); return -(y * std::log(p) + (1 - y) * std::log(1 - p)); }

int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "usage\n"); return 1; }
    bool noisy = std::string(argv[1]) == "noisy";
    double tSelf = atof(argv[2]), tEval = atof(argv[3]);
    std::string out = argv[4];
    u64 seed = strtoull(argv[5], 0, 10);
    double t0 = now();

    // ---- self-play with the heuristic evaluator
    std::vector<Sample> data;
    int games = 0, decided = 0, totalTurns = 0;
    while (now() - t0 < tSelf) {
        BotCtx a, b; botDefaults(a, noisy, nullptr, seed + games * 2); botDefaults(b, noisy, nullptr, seed + games * 2 + 1);
        // full search settings (fast enough)
        int turns; int w = playGame(seed * 1000 + games, noisy, a, b, &data, &turns);
        games++; totalTurns += turns; if (w >= 0) decided++;
        if (games % 5 == 0) { printf("[%s] selfplay games=%d decided=%d samples=%zu avgTurns=%.1f t=%.0fs\n", argv[1], games, decided, data.size(), totalTurns / (double)games, now() - t0); fflush(stdout); }
    }
    printf("[%s] selfplay done: %d games, %zu samples\n", argv[1], games, data.size());

    // ---- train MLP
    std::mt19937_64 gen(seed);
    std::shuffle(data.begin(), data.end(), gen);
    size_t nVal = data.size() * 15 / 100, nTr = data.size() - nVal;
    std::vector<double> w(NWEIGHTS), m(NWEIGHTS, 0), v(NWEIGHTS, 0), grad(NWEIGHTS);
    std::normal_distribution<double> nd(0, 1);
    { int k = 0;
      for (int i = 0; i < H1 * NFEAT; ++i) w[k++] = nd(gen) / std::sqrt((double)NFEAT);
      for (int i = 0; i < H1; ++i) w[k++] = 0;
      for (int i = 0; i < H2 * H1; ++i) w[k++] = nd(gen) / std::sqrt((double)H1);
      for (int i = 0; i < H2; ++i) w[k++] = 0;
      for (int i = 0; i < H2; ++i) w[k++] = nd(gen) / std::sqrt((double)H2) * 0.5;
      w[k++] = 0; }
    double lr = 0.003, b1 = 0.9, b2 = 0.999, wd = 1e-4; int step = 0;
    double h1[H1], h2[H2];
    auto evalSet = [&](size_t from, size_t to, bool heur) {
        double L = 0; int acc = 0;
        for (size_t i = from; i < to; ++i) {
            double z = heur ? heuristicLogit(data[i].f) : fwd(w, data[i].f, h1, h2);
            L += logloss(z, data[i].y); acc += ((z > 0) == (data[i].y > 0.5));
        }
        return std::make_pair(L / std::max<size_t>(1, to - from), acc / (double)std::max<size_t>(1, to - from));
    };
    auto hv = evalSet(nTr, data.size(), true);
    double bestVal = 1e9; std::vector<double> bestW = w;
    int epochs = 300, bs = 64;
    for (int ep = 0; ep < epochs; ++ep) {
        std::shuffle(data.begin(), data.begin() + nTr, gen);
        for (size_t s0 = 0; s0 < nTr; s0 += bs) {
            std::fill(grad.begin(), grad.end(), 0);
            size_t s1 = std::min(nTr, s0 + bs);
            for (size_t i = s0; i < s1; ++i) {
                double z = fwd(w, data[i].f, h1, h2);
                double p = 1 / (1 + std::exp(-z));
                backward(w, data[i].f, h1, h2, (p - data[i].y) / (s1 - s0), grad);
            }
            step++;
            for (int k = 0; k < NWEIGHTS; ++k) {
                double gk = grad[k] + wd * w[k];
                m[k] = b1 * m[k] + (1 - b1) * gk; v[k] = b2 * v[k] + (1 - b2) * gk * gk;
                double mh = m[k] / (1 - std::pow(b1, step)), vh = v[k] / (1 - std::pow(b2, step));
                w[k] -= lr * mh / (std::sqrt(vh) + 1e-8);
            }
        }
        auto vl = evalSet(nTr, data.size(), false);
        if (vl.first < bestVal) { bestVal = vl.first; bestW = w; }
        if (ep % 50 == 0 || ep == epochs - 1) {
            auto tl = evalSet(0, nTr, false);
            printf("[%s] epoch %d train loss %.4f val loss %.4f val acc %.3f\n", argv[1], ep, tl.first, vl.first, vl.second);
        }
    }
    w = bestW;
    auto nv = evalSet(nTr, data.size(), false);
    printf("[%s] validation: NN logloss %.4f acc %.3f | heuristic logloss %.4f acc %.3f (n=%zu)\n", argv[1], nv.first, nv.second, hv.first, hv.second, nVal);

    // write weights
    std::vector<float> wf(w.begin(), w.end());
    { FILE* f = fopen((out + "_weights.h").c_str(), "w");
      fprintf(f, "// generated by train.cpp (%s): %d self-play games, %zu samples\n", argv[1], games, data.size());
      fprintf(f, "static const float %s[%d] = {\n", noisy ? "NN_NOISY" : "NN_STD", NWEIGHTS);
      for (int k = 0; k < NWEIGHTS; ++k) fprintf(f, "%.7gf,%s", wf[k], (k % 8 == 7) ? "\n" : " ");
      fprintf(f, "};\n"); fclose(f); }

    // ---- evaluation: NN evaluator vs heuristic evaluator (same search settings), alternating breaks
    double te = now();
    int nnW = 0, hW = 0, eg = 0;
    while (now() - te < tEval) {
        BotCtx nn, h; botDefaults(nn, noisy, wf.data(), seed + 777 + eg); botDefaults(h, noisy, nullptr, seed + 999 + eg);
        // full search settings
        bool nnFirst = (eg % 2 == 0);
        int w0 = nnFirst ? playGame(seed * 7 + eg / 2, noisy, nn, h, nullptr, nullptr) : playGame(seed * 7 + eg / 2, noisy, h, nn, nullptr, nullptr);
        if (w0 >= 0) { bool nnWon = (w0 == 0) == nnFirst; if (nnWon) nnW++; else hW++; }
        eg++;
        printf("[%s] eval game %d: NN %d - %d heuristic\n", argv[1], eg, nnW, hW); fflush(stdout);
    }
    printf("[%s] FINAL eval: NN %d - %d heuristic in %d games; total time %.0fs\n", argv[1], nnW, hW, eg, now() - t0);
    return 0;
}
