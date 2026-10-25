// Bot-vs-bot snooker frames with the full foul options, free ball and miss referee
#include "../engine/bot.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
using namespace pool;
static double now(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main(int argc, char** argv) {
  real skill = argc > 1 ? atof(argv[1]) : 1.0; int frames = argc > 2 ? atoi(argv[2]) : 1; real budget = argc > 3 ? atof(argv[3]) : 250;
  int mode = argc > 4 ? atoi(argv[4]) : 0; bool verbose = argc > 5;
  NoiseModel nm; defaultNoise(nm); nm.skill = skill;
  int fouls = 0, misses = 0, replaces = 0, putbacks = 0, freeballs = 0, fbUsed = 0, forfeits = 0, direct = 0, effort = 0, expected = 0, hard = 0, snook = 0, shotsT = 0;
  double refTime = 0; int refN = 0;
  for (int fr = 0; fr < frames; ++fr) {
    GameSnk g; newGameSnk(g, 100 + fr, 0); g.sim.trackOrient = 0;
    Rng r; r.seed(7 + fr);
    int shots = 0;
    while (g.winner < 0 && shots < 400) {
      BotCtx c; botDefaults(c, true, nullptr, 1000 * fr + shots); c.budget = budget; c.nm = nm; c.refMode = mode;
      BotDecision d; botThink(c, g, d);
      GameSnk pre = g;
      if (d.place) resetBall(g.sim.b[0], d.px, d.py);
      if (g.freeBall) fbUsed++;
      prepShot(g, d.tgt);
      pre = g;   // position as struck (after placement)
      executeShot(g, d.sp, true, nm, r); simulate(g.sim); endShotSnk(g); shots++;
      if (g.lastFoul) fouls++;
      if (g.pendingChoice) {
        RefInfo ri; double t0 = now();
        snkReferee(pre, g, d.sp, nm, mode, 0.5, 99 + shots, ri);
        if (ri.checked) { refTime += now() - t0; refN++; }
        if (ri.miss) misses++;
        if (ri.reason == MISS_DIRECT) direct++; else if (ri.reason == MISS_EXPECTED) expected++; else if (ri.reason == MISS_EFFORT) effort++; else if (ri.reason == NOMISS_HARD) hard++; else if (ri.reason == NOMISS_SNOOKERS) snook++;
        if (ri.forfeit) forfeits++;
        if (verbose && ri.checked) printf("  shot %d p%d foul=%d miss=%d reason=%d direct=%d p*=%.2f played=%.2f esc=%d(%d,%d) misscount=%d\n", shots, pre.turn, g.lastFoul, ri.miss, ri.reason, ri.direct, ri.pStar, ri.pPlayed, ri.escType, ri.escR1, ri.escR2, g.missCount[pre.turn]);
        if (g.winner < 0 && g.pendingChoice) {
          if (g.freeAvail) freeballs++;
          BotCtx cc; botDefaults(cc, true, nullptr, 7000 * fr + shots); cc.budget = budget; cc.nm = nm; cc.refMode = mode;
          real v[3]; int ch = snkChooseOption(cc, g, pre, v);
          if (ch == CH_REPLACE) replaces++; else if (ch == CH_PUTBACK) putbacks++;
          if (verbose) printf("  choice by p%d: %s (play %.1f, back %.1f, replace %.1f) freeAvail=%d\n", g.turn, ch == 1 ? "play" : ch == 2 ? "put back" : "replace", v[0], v[1], v[2], g.freeAvail);
          snkApplyChoice(g, pre, ch);
        }
      }
    }
    shotsT += shots;
    printf("frame %d: %d-%d winner %d shots %d\n", fr, g.score[0], g.score[1], g.winner, shots);
  }
  printf("skill %.2f mode %d: fouls %d, misses %d (direct %d, expected %d, effort %d), not called: hard %d, needs-snookers %d | replace %d, put back %d, free balls offered %d used %d, forfeits %d | referee avg %.0f ms over %d calls\n",
         skill, mode, fouls, misses, direct, expected, effort, hard, snook, replaces, putbacks, freeballs, fbUsed, forfeits, refN ? 1000 * refTime / refN : 0, refN);
}
