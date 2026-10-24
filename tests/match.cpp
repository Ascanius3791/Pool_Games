// A/B match: two-phase search (A) vs phase-1 only (B) at equal simulation budget
#include "../engine/bot.h"
#include "../train/out/std_weights.h"
#include "../train/out/noisy_weights.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
using namespace pool;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(int argc, char** argv) {
  bool noisy = atoi(argv[1]); real budget = atof(argv[2]); double tlim = atof(argv[3]);
  int aw = 0, bw = 0, games = 0; double t0 = now();
  while (now() - t0 < tlim) {
    Game8 g; newGame8(g, 4242 + games / 2, 0); g.sim.trackOrient = 0;
    NoiseModel nm; defaultNoise(nm); Rng r; r.seed(777 + games / 2);
    bool aFirst = games % 2 == 0; int shots = 0;
    while (g.winner < 0 && shots < 120) {
      bool isA = (g.turn == 0) == aFirst;
      BotCtx c; botDefaults(c, noisy, noisy ? NN_NOISY : NN_STD, 31 * games + shots); c.budget = budget; c.phase2 = isA;
      BotDecision d; botThink(c, g, d);
      if (d.place) resetBall(g.sim.b[0], d.px, d.py);
      executeShot(g, d.sp, noisy, nm, r); simulate(g.sim); endShot8(g); shots++;
    }
    if (g.winner >= 0) { bool aWon = (g.winner == 0) == aFirst; aWon ? aw++ : bw++; }
    games++;
    printf("[%s] game %d: two-phase %d - %d phase-1-only\n", noisy ? "noisy" : "std", games, aw, bw); fflush(stdout);
  }
}
