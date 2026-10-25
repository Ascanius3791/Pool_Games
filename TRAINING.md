# Training runbook

This is the plan for a long, unattended training run in the cloud. One command runs everything.
Results come back as a report plus evaluator files that are compiled into the game.

**Current focus: snooker.** The defaults train only the snooker evaluator (`GAMES=snk`); 8-ball can be added with `GAMES="snk 8noisy 8std"`.

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

1. **Self-play** on every core for `SP_MIN` minutes. The game types rotate over the cores (`GAMES`), and for snooker the human-error level rotates too (`SKILLS`, default `1.0 0 1.0 2.0`: club player, no error, club, beginner-ish). Half the games start from random mid-game positions, so rare situations are covered. Every finished game is appended to `data/<kind>/*.games`, so a killed process loses at most one game. Frames at `SKILL` go to `data/snk/` and feed the current evaluator; frames at other skills go to `data/snk_s<skill>/` and are kept for the neural evaluator (see *Raw positions* below).
2. **Snooker win table**: recalibrated from all `*.stats` and recomputed (about 10 s).
3. **Tune** each evaluator on all data collected so far, starting from the current best.
4. **Arena**: each candidate plays the current best in paired games (same seed, sides swapped) with an SPRT (H0: +0 Elo, H1: +30 Elo, α = β = 0.05). The arena stops at the SPRT bound or after `ARENA_MIN`. The cores the arena does not use keep producing self-play data meanwhile.
5. **Accept** if the SPRT says so, or if the candidate scores ≥ 56% over ≥ 60 games.

At the end, the best evaluators are exported to `train/out/*.h` (`train/export.py`). The page is rebuilt (`./build.sh`) if the npm dependencies are present.

## Where to run it

**GitHub Actions (recommended).** Push the repo to GitHub, open *Actions → Train bots → Run workflow* and choose the hours (5.5 max, because a job is limited to 6 h). The runner has 4 cores for a public repo and 2 for a private one.
* Self-play data is kept between runs in the Actions cache, so every run builds on the previous ones.
* The accepted evaluators, the report and the rebuilt page are committed to a branch `training/<n>`. Merge it, and the next run starts from those evaluators (they are copied into `train/baseline/`).
* The full run directory is attached as a workflow artifact.

**Any Linux machine** with g++/clang/lld/python3/node works as well, using the commands below. A Claude cloud sandbox is not a good fit: its programs stop shortly after a reply ends, so a run would have to be chopped into 10-minute steps.

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
| `BUDGET` | 2000 | bot simulations per shot (self-play and arena) |
| `LAMBDA` | 0.7 | TD(λ) |
| `SKILL` | 1.0 | human-error multiplier the evaluator is tuned and tested at (1 = Club player) |
| `SKILLS` | `1.0 0 1.0 2.0` | snooker self-play skill per core, rotating; 0 = no human error |
| `EXPORT` | 1 | write the best evaluators to `train/out/*.h` at the end (`make smoke` sets 0) |
| `GAMES` | `snk` | which bots get self-play cores (rotating) |
| `CORES` | `nproc` | parallel processes |
| `BEST_FROM` | – | continue from a previous run's `runs/<stamp>/best` |

Throughput since the event-driven engine (v9/v10, about 20–25× faster simulation): snooker about 60–70 frames/min per core at budget 2000 with human error (about 3,500 positions/min, half of the frames start mid-frame). One 25-minute self-play phase on 4 cores gives about 6,000 frames / 350k positions; a 5-hour run about 2–3 million positions. Disk: about 100 bytes per position (raw records) plus about 40 for the tuning records.

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

## Raw positions (for the neural evaluator)

Snooker self-play also writes `data/snk*/snk_<seed>.posrec`, the full positions with the frame result, at every skill level:

```
per frame:    int32 magic 0x534E4B50 ('SNKP'), int32 version (2), int32 nPos, int32 winner (0/1),
              float skill (0 = no human error), int32 budget
per position (104 bytes, packed, little-endian):
              int16 score0, score1; uint8 mover, phase (0 red on, 1 colour on, 2 sequence), nextColour,
              flags (1 ball in hand, 2 free ball); float value (search value of the chosen shot, mover's view);
              uint32 onMask (bit i = ball i on the table); int16 x[22], y[22] (0.1 mm, table centre = 0)
```

Ball ids: 0 cue, 1–15 reds, 16 yellow … 21 black. This is the training data for the planned skill-conditioned network: pretrain on the no-error frames (skill 0), then refine on all skills with the skill as an input, and use the same net in the game for every human-error setting.

## Starting the cloud run (GitHub Actions)

1. The code is in `github.com/Ascanius3791/Pool_Games` (branch `main`).
2. On GitHub: **Actions** tab → **Train bots** → **Run workflow**. Keep the defaults (5 hours, `snk`, budget 2000, skill 1.0, skills `1.0 0 1.0 2.0`) and press the green button.
3. When it finishes (about 5 h 15 min), there is a new branch `training/<run number>` with `runs/gh_<n>/REPORT.md`, the accepted evaluators in `train/baseline/` and `train/out/`, and the rebuilt `web/index.html`. Open a pull request from that branch and merge it, and the game uses the new evaluator. The full run directory (all logs) is attached to the workflow run as an artifact.
4. Self-play data is kept in the Actions cache, so the next run builds on it.
