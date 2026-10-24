// abstract_snk.cpp — abstract snooker frame simulator -> win table W(D, R)
//
// W(D, R) = P(player to move wins | score difference D = mine - theirs, R points left).
// No physics: a visit is a chain of pot attempts with probabilities calibrated from
// physics self-play (stats files written by selfplay), plus safeties, fouls and,
// when a player needs snookers, deliberate snookers that force fouls (miss rule).
// Millions of frames per minute, so the score context (snookers required, being
// far ahead with few balls left, ...) is learned without playing physics games.
//
// usage: abstract_snk --out table.bin [--stats a.stats b.stats ...] [--frames 300]
//                     [--calib-out calib.txt] [--seed 1]
#include "common.h"
using namespace train;

struct Model {
    double pOpen = 0.55;          // chance a visit starts with a pottable ball
    double pOpenAfterMiss = 0.75; // ... after the opponent missed a pot
    double pPotRed = 0.72, pPotCol = 0.68;
    double pFoulPot = 0.03, pFoulSafety = 0.06;
    double pSnooker = 0.45;       // a deliberate snooker works
    double pEscapeFail = 0.45;    // the snookered player fouls (again, miss rule)
    double colW[6] = {0.03, 0.03, 0.04, 0.12, 0.20, 0.58};   // colour chosen after a red: yellow..black
};

struct State { int score[2]; int turn; int reds; int phase; int next; bool open; bool afterMiss; };

static int remaining(const State& s) {
    if (s.phase == 2) { int r = 0; for (int c = s.next; c <= 7; ++c) r += c; return r; }
    return s.reds * 8 + 27 + (s.phase == 1 ? 7 : 0);
}

static int playFrame(State s, const Model& m, pool::Rng& r) {
    int visits = 0;
    bool visitStart = !s.open;            // s.open: the player is already in a break
    for (int guard = 0; guard < 2000; ++guard) {
        const int P = s.turn, O = 1 - P;
        int R = remaining(s);
        if (R == 0) {
            if (s.score[0] == s.score[1]) return r.uniform() < 0.5 ? 0 : 1;   // re-spotted black
            return s.score[0] > s.score[1] ? 0 : 1;
        }
        int onVal = s.phase == 0 ? 1 : s.phase == 1 ? 7 : s.next;
        // snookers required: lay snookers instead of potting
        if (visitStart && s.score[O] - s.score[P] > R) {
            if (r.uniform() < m.pSnooker) {
                int oVal = s.phase == 2 ? s.next : 1;
                int k = 0; while (r.uniform() < m.pEscapeFail && k < 8) { s.score[P] += oVal > 4 ? oVal : 4; ++k; }
                visitStart = true; s.afterMiss = false;   // escape gives the striker a chance
                continue;
            }
            s.turn = O; s.afterMiss = true; visitStart = true; ++visits; continue;
        }
        if (visitStart) {
            double pOpen = s.afterMiss ? m.pOpenAfterMiss : m.pOpen;
            if (r.uniform() >= pOpen) {                 // safety
                if (r.uniform() < m.pFoulSafety) s.score[O] += onVal > 4 ? onVal : 4;
                s.turn = O; s.afterMiss = false; visitStart = true; ++visits; continue;
            }
        }
        visitStart = false;
        // pot attempt
        double p = s.phase == 0 ? m.pPotRed : m.pPotCol;
        if (r.uniform() < p) {
            if (s.phase == 0) { s.score[P] += 1; s.reds--; s.phase = 1; }
            else if (s.phase == 1) {
                double u = r.uniform(), acc = 0; int c = 7;
                for (int k = 0; k < 6; ++k) { acc += m.colW[k]; if (u < acc) { c = k + 2; break; } }
                s.score[P] += c; s.phase = s.reds > 0 ? 0 : 2; s.next = 2;
            } else { s.score[P] += s.next; s.next++; if (s.next > 7) { s.phase = 2; } }
            if (s.phase == 2 && s.next > 7) continue;   // all gone -> R == 0 next loop
        } else {
            if (r.uniform() < m.pFoulPot) s.score[O] += onVal > 4 ? onVal : 4;
            if (s.phase == 1) s.phase = s.reds > 0 ? 0 : 2, s.next = 2;
            s.turn = O; s.afterMiss = true; visitStart = true; ++visits;
        }
    }
    return s.score[0] >= s.score[1] ? 0 : 1;
}

static void calibrate(Model& m, const std::vector<std::string>& files) {
    // stats lines: kind targetClass potted foul visitStart   (kind 0 pot, 1 safety)
    double redA = 0, redP = 0, colA = 0, colP = 0, potA = 0, potF = 0, safA = 0, safF = 0, vs = 0, vsPot = 0;
    for (auto& fn : files) {
        FILE* f = fopen(fn.c_str(), "r"); if (!f) continue;
        int kind, tc, pot, foul, vst;
        while (fscanf(f, "%d %d %d %d %d", &kind, &tc, &pot, &foul, &vst) == 5) {
            if (vst) { vs++; if (kind == 0) vsPot++; }
            if (kind == 0) { potA++; potF += foul; if (tc == 1) { redA++; redP += pot; } else if (tc == 2) { colA++; colP += pot; } }
            else if (kind == 1) { safA++; safF += foul; }
        }
        fclose(f);
    }
    auto est = [](double s, double n, double prior, double w) { return (s + prior * w) / (n + w); };   // shrink toward the prior
    if (redA + colA + safA > 50) {
        m.pPotRed = est(redP, redA, m.pPotRed, 30); m.pPotCol = est(colP, colA, m.pPotCol, 30);
        m.pFoulPot = est(potF, potA, m.pFoulPot, 30); m.pFoulSafety = est(safF, safA, m.pFoulSafety, 30);
        m.pOpen = est(vsPot, vs, m.pOpen, 30);
        m.pOpenAfterMiss = pool::pmin(0.95, m.pOpen + 0.2);
    }
    printf("calibration from %.0f shots: pPotRed %.3f pPotCol %.3f pOpen %.3f pFoulPot %.3f pFoulSafety %.3f\n",
           redA + colA + safA, m.pPotRed, m.pPotCol, m.pOpen, m.pFoulPot, m.pFoulSafety);
}

int main(int argc, char** argv) {
    Args a(argc, argv);
    std::string out = a.str("--out", "table.bin");
    int frames = (int)a.num("--frames", 300);
    Model m;
    calibrate(m, a.list("--stats"));
    if (a.has("--calib-out")) {
        FILE* f = fopen(a.str("--calib-out", "").c_str(), "w");
        if (f) { fprintf(f, "pPotRed %.4f\npPotCol %.4f\npOpen %.4f\npFoulPot %.4f\npFoulSafety %.4f\n", m.pPotRed, m.pPotCol, m.pOpen, m.pFoulPot, m.pFoulSafety); fclose(f); }
    }
    pool::Rng rng; rng.seed((pool::u64)a.num("--seed", 1));
    SnkTable T;
    T.t.assign((size_t)T.tND * T.tNR, 0);
    std::vector<int> have(T.tNR, 0);
    std::vector<std::vector<double>> rows(T.tNR, std::vector<double>(T.tND, 0));
    double t0 = now();
    // discrete states: reds 0..15 with red on (phase 0) or colour on (phase 1); colour sequence
    std::vector<State> states;
    for (int reds = 0; reds <= 15; ++reds) {
        State s{}; s.reds = reds; s.phase = reds > 0 ? 0 : 2; s.next = 2; states.push_back(s);
        if (reds < 15) { State c{}; c.reds = reds; c.phase = 1; c.next = 2; c.open = true; states.push_back(c); }
    }
    for (int c = 3; c <= 7; ++c) { State s{}; s.reds = 0; s.phase = 2; s.next = c; states.push_back(s); }
    for (auto st : states) {
        int R = remaining(st);
        if (R >= T.tNR) continue;
        std::vector<double>& row = rows[R];
        for (int di = 0; di < T.tND; di += 2) {
            int D = di - T.tD0;
            int wins = 0;
            for (int k = 0; k < frames; ++k) {
                State s = st; s.turn = 0; s.score[0] = D > 0 ? D : 0; s.score[1] = D < 0 ? -D : 0;
                s.afterMiss = false;
                wins += playFrame(s, m, rng) == 0;
            }
            row[di] = (double)wins / frames;
        }
        for (int di = 1; di < T.tND; di += 2) row[di] = 0.5 * (row[di - 1] + row[di + 1 < T.tND ? di + 1 : di - 1]);
        // smooth and force monotone in D
        std::vector<double> sm(T.tND);
        for (int i = 0; i < T.tND; ++i) { double s = 0; int n = 0; for (int j = i - 3; j <= i + 3; ++j) if (j >= 0 && j < T.tND) { s += row[j]; ++n; } sm[i] = s / n; }
        for (int i = 1; i < T.tND; ++i) if (sm[i] < sm[i - 1]) sm[i] = sm[i - 1];
        row = sm; have[R] = 1;
    }
    // rows that are not a reachable state: interpolate between the nearest computed rows
    for (int R = 0; R < T.tNR; ++R) {
        if (have[R]) continue;
        int lo = R - 1; while (lo >= 0 && !have[lo]) --lo;
        int hi = R + 1; while (hi < T.tNR && !have[hi]) ++hi;
        for (int i = 0; i < T.tND; ++i) {
            if (lo < 0) rows[R][i] = rows[hi][i];
            else if (hi >= T.tNR) rows[R][i] = rows[lo][i];
            else { double w = (double)(R - lo) / (hi - lo); rows[R][i] = (1 - w) * rows[lo][i] + w * rows[hi][i]; }
        }
    }
    for (int R = 0; R < T.tNR; ++R) for (int i = 0; i < T.tND; ++i)
        T.t[(size_t)R * T.tND + i] = (unsigned char)pool::pclamp(rows[R][i] * 255.0 + 0.5, 0, 255);
    saveTable(out, T);
    auto W = [&](int D, int R) { return T.t[(size_t)R * T.tND + D + T.tD0] / 255.0; };
    printf("wrote %s in %.1fs. Samples: W(0,147)=%.2f W(+20,147)=%.2f W(-20,35)=%.2f W(-30,27)=%.2f W(-40,27)=%.2f W(+10,13)=%.2f\n",
           out.c_str(), now() - t0, W(0, 147), W(20, 147), W(-20, 35), W(-30, 27), W(-40, 27), W(10, 13));
    return 0;
}
