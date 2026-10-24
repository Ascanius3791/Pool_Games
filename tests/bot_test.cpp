#include "../engine/bot.h"
#include <cstdio>
#include <chrono>
#include <cstdlib>
using namespace pool;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
template<class G> void play(G& g, bool noisy, real budget, int maxShots, bool verbose, double& think, int& shots) {
  NoiseModel nm; defaultNoise(nm); Rng r; r.seed(99);
  think = 0; shots = 0;
  while (g.winner < 0 && shots < maxShots) {
    BotCtx c; botDefaults(c, noisy, nullptr, 1000 + shots); c.budget = budget;
    BotDecision d; double t0 = now(); botThink(c, g, d); think += now() - t0;
    int me = g.turn;
    if (d.place) resetBall(g.sim.b[0], d.px, d.py);
    executeShot(g, d.sp, noisy, nm, r); simulate(g.sim); 
    if constexpr (std::is_same<G, GameSnk>::value) {
      int before = g.score[me]; endShotSnk(g);
      if (verbose) printf("shot %3d p%d val=%7.2f sims=%5.1f (p1 %.0f, full %d, part %d, secant %d, refine %d, switch %d, cands %d) foul=%d pts=%d score %d-%d phase=%d reds=%d\n",
        shots, me, d.value, d.sims, d.phase1, d.nFull, d.nPartial, d.nSecant, d.nRefine, d.nSwitch, d.nCand, g.lastFoul, g.score[me]-before, g.score[0], g.score[1], g.phase, snkRedsOn(g));
    } else {
      endShot8(g);
      if (verbose) printf("shot %3d p%d val=%.3f sims=%5.1f (p1 %.0f, full %d, part %d, secant %d, refine %d, switch %d, cands %d) foul=%d kept=%d\n",
        shots, me, d.value, d.sims, d.phase1, d.nFull, d.nPartial, d.nSecant, d.nRefine, d.nSwitch, d.nCand, g.lastFoul, g.keptTurn);
    }
    shots++;
  }
}
int main(int argc, char** argv) {
  const char* what = argc > 1 ? argv[1] : "snk";
  real budget = argc > 2 ? atof(argv[2]) : 400;
  bool noisy = argc > 3 && atoi(argv[3]);
  double think; int shots;
  if (what[0] == 's') {
    GameSnk g; newGameSnk(g, 7, 0); g.sim.trackOrient = 0;
    play(g, noisy, budget, 200, true, think, shots);
    printf("SNOOKER winner=%d score %d-%d shots=%d avg think %.3fs high breaks %d/%d\n", g.winner, g.score[0], g.score[1], shots, think / shots, g.highBreak[0], g.highBreak[1]);
  } else {
    Game8 g; newGame8(g, 5, 0); g.sim.trackOrient = 0;
    play(g, noisy, budget, 80, true, think, shots);
    printf("8BALL winner=%d shots=%d avg think %.3fs\n", g.winner, shots, think / shots);
  }
}
