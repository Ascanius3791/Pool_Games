// botbench: time bot decisions on a fixed set of snooker positions (self-play prefix)
#include "../engine/bot.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
using namespace pool;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(int argc, char** argv) {
  int noisy = argc > 1 ? atoi(argv[1]) : 1; real budget = argc > 2 ? atof(argv[2]) : 1000; int N = argc > 3 ? atoi(argv[3]) : 20;
  NoiseModel nm; defaultNoise(nm);
  // positions: play a frame with a cheap bot, record positions
  std::vector<GameSnk> pos; GameSnk g; newGameSnk(g, 11, 0); g.sim.trackOrient = 0; Rng r; r.seed(5);
  while ((int)pos.size() < N && g.winner < 0) {
    pos.push_back(g);
    BotCtx c; botDefaults(c, true, nullptr, 77 + pos.size()); c.budget = 60; c.nm = nm;
    BotDecision d; botThink(c, g, d); if (d.place) resetBall(g.sim.b[0], d.px, d.py);
    executeShot(g, d.sp, true, nm, r); simulate(g.sim); endShotSnk(g);
    if (g.pendingChoice) snkApplyChoice(g, g, CH_PLAY);
  }
  double t = 0, sims = 0, val = 0; long full = 0, part = 0;
  for (size_t k = 0; k < pos.size(); ++k) {
    BotCtx c; botDefaults(c, noisy, nullptr, 1000 + k); c.budget = budget; c.nm = nm;
    BotDecision d; double t0 = now(); botThink(c, pos[k], d); t += now() - t0;
    sims += d.sims; val += d.value + d.sp.phi; full += d.nFull; part += d.nPartial;
  }
  printf("noisy %d budget %.0f: %.1f ms/decision, sims %.0f, full %ld part %ld per dec, checksum %.9f\n", noisy, budget, t / pos.size() * 1e3, sims / pos.size(), full / (long)pos.size(), part / (long)pos.size(), val);
}
