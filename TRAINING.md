# Training runbook

This is the plan for a long, unattended training run in the cloud. One command runs everything.
Results come back as a report plus evaluator files that are compiled into the game.

## What is trained

| Bot | Evaluator | Inputs | Trained by |
|---|---|---|---|
| 8-ball, exact play (`8std`) | MLP 16→24→12→1, win probability | position features (balls left, shot eases, pottable counts, ...) | Texel tuning on TD(λ) targets |
| 8-ball, human error (`8noisy`) | same, separate weights | same | same |
| Snooker (`snk`) | win table W(D, R) + 6 positional parameters | score difference, points remaining, next-shot ease, snookered, ball in hand | abstract frame simulator (table) + Texel tuning (parameters) |

The shot search is unchanged: two phases, with the physics simulated exactly. Training only improves how the bot *judges the positions* it can reach. That judgement is where strategy lives: playing for snookers when behind, tightening up when ahead, and leaving the opponent nothing.

### The snooker evaluator

```
E     = a0 + a1·e1·(v1 + cont) + a2·e2 + a3·[snookered] + a4·[ball in hand]   (expected points of the coming visit)
P(win) = W(D + eScale·E, R)
```

`W(D, R)` comes from `train/abstract_snk.cpp`. This is a simulator without physics in which a visit is a chain of pot attempts, safeties, fouls and, when a player needs snookers, deliberate snookers that force fouls. Its probabilities are **calibrated** from the physics self-play statistics (`data/snk/*.stats`). It produces millions of frames per minute, so the score context (snookers required, a lead that is safe with few balls left) is learned without physics games. The six parameters are then fitted to real self-play outcomes.

As long as the arena hasn't accepted a win-table evaluator (`train/baseline/snk_use_table` = 0), the game keeps using the old points-based snooker evaluation. The first accepted candidate switches it on.

### Targets: TD(λ) instead of raw results

For position t, the target is

```
G_t = (1 − λ) · v_{t+1} + λ · G_{t+1},     G_T = final result
```

Here `v` is the search value of the next position, converted into the mover's perspective. λ = 1 is plain Texel tuning, which uses only the results. Smaller λ cuts the variance from luck later in the frame, so each game teaches more.

## The loop (`train/run_pipeline.sh`)

Each iteration does the following:

1. **Self-play** on every core for `SP_MIN` minutes. The game types rotate over the cores (`GAMES`). Half the games start from random mid-game positions, so rare situations are covered. Every finished game is appended to `data/<kind>/*.games`, so a killed process loses at most one game.
2. **Snooker win table**: recalibrated from all `*.stats` and recomputed (about 10 s).
3. **Tune** each evaluator on all data collected so far, starting from the current best.
4. **Arena**: each candidate plays the current best in paired games (same seed, sides swapped) with an SPRT (H0: +0 Elo, H1: +30 Elo, α = β = 0.05). The arena stops at the SPRT bound or after `ARENA_MIN`.
5. **Accept** if the SPRT says so, or if the candidate scores ≥ 56% over ≥ 60 games.

At the end, the best evaluators are exported to `train/out/*.h` (`train/export.py`). The page is rebuilt (`./build.sh`) if the npm dependencies are present.

## Running it

```bash
scripts/setup.sh                               # once
make smoke                                     # 3-minute end-to-end check
HOURS=8 ./train/run_pipeline.sh                # the real run
```

| Variable | Default | Meaning |
|---|---|---|
| `HOURS` | 6 | wall-clock budget |
| `SP_MIN` | 25 | self-play minutes per iteration |
| `ARENA_MIN` | 15 | max arena minutes per candidate |
| `BUDGET` | 250 | bot simulations per shot (self-play and arena) |
| `LAMBDA` | 0.7 | TD(λ) |
| `SKILL` | 1.0 | human-error multiplier of the self-play profile (1 = Club player) |
| `GAMES` | `snk 8noisy 8std` | which bots get self-play cores (rotating) |
| `CORES` | `nproc` | parallel processes |
| `BEST_FROM` | – | continue from a previous run's `runs/<stamp>/best` |

Rough throughput per core: 8-ball about 1–2 games/min, snooker about 0.3–1 frames/min at budget 250 with human error. With 2 cores, 8 hours give roughly 300 snooker frames (about 20k positions) and about 500 8-ball games. That is enough for Texel tuning of a few dozen parameters. More cores scale linearly.

## Outputs

* `runs/<stamp>/REPORT.md`: per iteration, the tuning losses (validation, against targets and against results), the arena result (W–L, Elo ± CI, LLR) and what was accepted, plus the snooker calibration.
* `runs/<stamp>/progress.jsonl`: the same information, machine-readable.
* `runs/<stamp>/best/`: the accepted evaluators (`w8_std.bin`, `w8_noisy.bin`, `snk_params.txt`, `snk_table.bin`, `snk_use_table`).
* `train/out/*.h`: exported headers. Commit them, run `./build.sh`, and republish the page.

## Afterwards

* Commit `train/out/`, `runs/<stamp>/REPORT.md` and `runs/<stamp>/best/` (small files). Leave `data/` out (it's large and reproducible).
* To continue, set `BEST_FROM=runs/<stamp>/best` and keep `data/`.
* Possible next steps once this works:
  * value-conditioning on the profile (sigmas as inputs, so one evaluator covers all skill levels);
  * a learned policy prior that saves search budget;
  * a two-shot lookahead for safety play.
