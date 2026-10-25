# Pool / Snooker project: status and design notes

## Status (2026-10-25)
- Paul approved the physics ("wonderful"). Artifact "Felt & Chalk 8-Ball": https://claude.ai/artifact/AAwk4E6Dqq8nhTBde8zxKk
- v2 changes:
  - removed the pocket liner shell
  - right-click aborts a charging shot
  - fixed-width spin readout (no more flicker)
  - flat orthographic 2D view, and the chosen view is remembered
  - Versus bot / Watch bots modes
- Bots v1 were trained under a 10-minute cap that Paul set. Longer training is to happen in the cloud.
  - std: 191 self-play games, 1908 samples. Value-net val logloss 0.456 (acc 0.78) vs heuristic 0.725 (0.66). Play vs heuristic evaluator: 35-35.
  - noisy: 56 games, 910 samples. Val logloss 0.614 (acc 0.68) vs heuristic 0.747 (0.60). Play: 13-10.
  - Takeaway: the net predicts outcomes better, but playing strength comes mostly from the simulation search. Next step is more data and value/TD iterations.
  - The standard bot executes exactly what it simulates (same engine), so it often runs the rack. A difficulty setting is a likely follow-up.
- v3 (2026-10-24):
  - Snooker added (12 ft table, rounded jaws, 22 balls, simplified rules in engine/rulesSnk.h). The snooker bot uses a points-based heuristic value and is not NN-trained yet.
  - Bot effort slider: simulations per decision, 20-3000 on a log scale. Root-finding probes count 1/4.
  - Two-phase search. Phase 1: Newton/secant root finding on the object-ball launch angle, plus breadth. Phase 2: best-first refinement (re-centre the aim, then pattern search on V/b/a), switching to another candidate whenever its stored value is higher.
  - A/B at equal budget 300 vs phase-1-only, 8-ball:
    - std: 251-201 (452 games)
    - noisy: 155-44 (199 games). The noisy gap is partly because phase-1-only never evaluates under noise.
  - Engine speed-up:
    - SIM_DT 1 ms -> 4 ms; kick-drift-kick is exact for constant deceleration, and the result differs by mm over multi-cushion shots
    - cushion broadphase (safe inner rectangle) and a moving-ball list
    - break simulation 6.8 ms -> 1.05 ms native, about 6.5x faster
  - Bot decision speed in WASM: about 0.2-0.5 s at 400 sims (std) and about 1 s at 700 (noisy).

- v4/v5 (2026-10-24):
  - Display panel (U) with per-panel toggles. Slider texts are written for players.
  - Player profiles: per-channel (aim, strength, tip side, tip height) gentle sigma, full-power sigma, curve exponent k and tendency (bias), plus bad-stroke chance and size. sigma_c(s) = lo + (hi-lo) s^k, with s = V/vmax. There are 5 built-in presets and the custom ones are kept in localStorage. Engine export: setNoiseProfile; the bot uses the same NoiseModel.
  - 2D view: the orthographic frustum is fitted into the screen area left free by the panels (hudInsets).

- v6 (2026-10-24): snooker foul options, free ball, miss rule
  - After a foul the incoming player chooses: play on (with a free ball if snookered by balls not on), make the offender play again, or (after a miss) replace the balls. Engine: GameSnk.pendingChoice, snkApplyChoice(g, pre, opt). wasm keeps PRE = position as struck.
  - Free ball: the nominated ball acts as the ball on (counts as a red while reds are on, else the value of the ball on) and is re-spotted. Snookering the opponent behind the free ball is a foul (SFOUL_FB_SNOOKER) unless only pink and black remain.
  - Human nomination: the computer's pick (botFavourite, about 0.2 s of budget) is shown with a gold ring; right-click a ball to override. Option 'auto-nominate' off means you must right-click before shooting.
  - Miss referee (bot.h snkReferee): only for a failure to hit a ball on, and not when the striker needs snookers (before or after the penalty).
    - Mode 0 (default), judged by the player profile: always a miss with a clear straight sight. Otherwise p* = best escape hit chance (direct, 1 cushion, 2 cushions via mirror images; aim-window Gaussian prefilter, then 24 noisy partial sims under the profile). Miss if p* >= tau (default 0.5), or if the shot played (24 noisy sims) hits < 0.5 p* while p* >= 0.15 (effort test).
    - Mode 1: pro rules, every failure to hit is a miss.
    - Three misses in a row with a clear sight forfeit the frame.
  - Bot: valueAfter accounts for the opponent's choice and an expected miss with direct sight. snkBotChoose gives the values of the options.
  - Test: tests/snk_rules_test.cpp. With beginner skill (2.5x): 25 fouls and 17 misses in 3 frames, replaces used 7 times, 4 free balls. The referee takes 50-200 ms native.

- v7 (2026-10-24): colour nomination after a red
  - GameSnk.nomRequired (default on) + nominated. With a nomination, only that colour may be hit first and potted. Without one, the first colour struck counts as before.
  - Bots always nominate their target (prepShot).
  - Humans nominate by right-click, or automatically when the choice is obvious: nomGuess plays the stroke 120 times with 3x the profile's slips (partial sims until first contact). If every run meets the same colour first, it is nominated automatically. If the runs differ, the shot is stored and a panel asks which colour (buttons show the counts; clicking nominates and plays the stored shot). If every run hits a red or nothing, the shot goes ahead (a foul either way).
  - Settings: 'Colours must be nominated' and 'Nominate obvious colours for me' (both on by default).

- v8 (2026-10-24):
  - Unclear nomination: the stroke is locked. No counts are shown and it cannot be cancelled; nominating a colour (button or right-click) plays it at once.
  - Training pipeline prepared (git repo, local commit b3adf47; bundle and tarball delivered to Paul). See README.md and TRAINING.md. Tools: train/selfplay, abstract_snk, tune, arena, run_pipeline.sh, export.py; baseline in train/baseline.
  - Snooker win-table evaluator (SnkEval) in bot.h. The game uses it only once the arena accepts it (SNK_USE_TABLE in train/out/snk_eval.h).
  - Smoke test (make smoke, about 3 min) passes end to end.

- Local run runs/local1 (2026-10-24, 15 min on 2 cores, budget 200, GAMES="snk 8noisy"): 55 snooker frames (3217 positions), 283 noisy 8-ball games (4004 positions).
  - Tuning: 8noisy val target loss 0.652 -> 0.623. Snooker params moved to (-0.55, 0.40, -0.01, -4.81, -2.05, eScale 2.04), val loss 0.390 -> 0.389.
  - Calibration: pPotRed 0.55, pPotCol 0.51, pOpen 0.51, pFoulSafety 0.16.
  - Arena: 8noisy 63-67 (-11 +- 60 Elo); snooker win table vs legacy 14-11 (+42 +- 146).
  - Nothing accepted, so the game is unchanged. The data is delivered as selfplay-data-local1.tar.gz.

- v9 (2026-10-25): event-driven physics engine (engine/events.h)
  - Analytic trajectories between events: sliding parabola, rolling line, rest; wz decays analytically. Events: phase changes, ball-ball and jaw-nose contacts (quartics), rail/facing lines and pocket capture (quadratics, closed form). Roots: polynomial split at critical points into monotone pieces, first decreasing piece reaching <= 0 = entry (Illinois regula falsi). Bezier-triangle and bounding-box culls.
  - Ordering: always the earliest event; every prediction of a ball is recomputed when its state changes; simultaneous events processed one after another in a fixed order. Contacts end separating at >= 1 mm/s (Zeno guard), cap 6000 events/shot.
  - Same collision responses as physics.h. simulate()/simulateFirstHit() now use it (POOL_STEPPER selects the old stepper, kept as simulateStep for reference). wasm advance() runs the live shot frame by frame; results are bit-identical to a single-call simulation.
  - Validation (tests/ev_test.cpp, vs 20 us stepper): jaw grid 4347/4347 pot outcomes agree (4 ms stepper: 4341); object balls near pockets 561/561; invariants over 1500 shots (snooker + pool, frozen balls, balls in jaws): no overlap, no penetration, no energy gain.
  - Bugs found and fixed on the way: rail contacts missed after a ball came out of the pocket mouth (old 5 mm "behind" skip), overlapping-and-approaching pairs ignored, both racks had up to 25 um initial overlaps (jitter reduced to 15 um).
  - Speed: snooker shot ~55 us vs ~1.1 ms (about 20x), first-contact partial sims about 55x, bot decision at budget 400: 0.17 s -> 0.006 s native. The referee went from 50-200 ms to about 1 ms.
  - Middle pockets: the 1 cm "left the table" net acts before the 1.0R capture depth (same in both engines); kept for identical pocket behaviour.

- v10 (2026-10-25): optimisation pass, effort slider, training set-up for the cloud
  - Engine: predictions only around moving balls at start, cheap pair rejection before any work (bounding boxes of whole phases), row minima for the pair times, one fused event scan, closed-form contact time for straight paths (rolling balls, straight stun/screw, ball at rest; quartic only for curved paths; fuzzed against the quartic, max diff 3e-13 s), smaller Table arrays (Sim 10.1 -> ~7.3 kB per copy).
  - Bot: lane checks only when the cheap cut/entry-angle tests pass, and in the snooker evaluation only when the shot could enter the top two.
  - Builds: tools/tests -O3 -march=native (8% more; bit-identical results), WASM -O3. Native: full snooker shot about 42 us (stepper 1.08 ms, about 26x), partial 4.4 us (77x). Bot decision at 1000 sims: 25.8 -> 15.5 ms (noisy), 21.7 -> 14.1 ms (exact). Browser (WASM): about 45-85 us per sim.
  - Deep thinking: budgets above 1500 widen the search (up to 80 safeties, 6 powers x 4 spins, noisy K up to 24) and spend leftover budget on extra noisy samples of the top 6 candidates. Budgets <= 1500 behave exactly as before.
  - Effort slider: 20-50,000 sims on a log scale, default 3,000 (new storage key, so the old 400 is not reused). Browser: 50,000 noisy sims about 2.2 s at the break-off. Without human error the search usually converges well before a large budget.
  - Training defaults: GAMES=snk, BUDGET=2000, SKILLS="1.0 0 1.0 2.0" rotating over cores (frames at SKILL -> data/snk for tuning/arena, others -> data/snk_s<skill>), idle cores self-play during arenas, EXPORT=0 for make smoke. New raw-position records (*.posrec, 104 B/position, documented in TRAINING.md) for the skill-conditioned NN.
  - Throughput: about 67 snooker frames/min/core at budget 2000 (was about 2 frames/min/core at budget 200 with the stepper).
  - Smoke test (budget 80, 1.5 min): win-table evaluator beat the legacy points evaluator 192-143 (+51 +- 38 Elo, SPRT accept). Not exported (smoke).
  - GitHub repo: github.com/Ascanius3791/Pool_Games. The cloud sandbox cannot push to it (repo not in the session's authorised set).

- Cloud run gh_1 (2026-10-25, 5 h, 4 cores, 7 iterations): one candidate accepted (iteration 2: 167-120, +57 +- 41 Elo), which switched on the win-table evaluator (params -0.39 0.38 1.90 -4.59 -2.53 2.50). Iterations 3-7 stayed within noise of it. After the switch the self-play played more safeties (pFoulSafety 0.18 -> 0.28, pot rates down). Merged into main and the page rebuilt.
- Cloud data export: caches cannot be downloaded, so `.github/workflows/export-data.yml` repacks the cache as an artifact. gh_1 data: 46,439 frames, 2.64M positions (skill 0: 0.73M, 1.0: 1.29M, 2.0: 0.62M).

- v11 (2026-10-25): neural snooker evaluator (bot.h: snkNetInputs / snkNetForward / snkNetWin)
  - P = sigmoid(gamma * logit W(D + E_lin + e, R)), (e, log gamma) from a net: per-ball shared layer 12 -> 24, pooled sum/max for reds and colours, head 119 -> 48 -> 24 -> 2, 23 globals including skill. The last layer starts at zero, so the untrained net is exactly the linear evaluator. Inputs mirrored so the cue ball has y >= 0.
  - Forward in float with input-major weights (inner loops over outputs: vectorises without reordering sums, so native and WASM agree). Inputs reuse the snkFeatures bookkeeping, lane checks only above the needed thresholds, ordered pocket search for the open-pocket value. Cost: evaluation ~4 us forward + ~14 us inputs (linear ~6 us); decision at budget 2000 about 1.5x the linear evaluator.
  - Tools: train/bin/snkfeat (posrec -> float16 rows, engine-computed), train/snknet.py (PyTorch, TD(lambda) targets, chunked reading for low memory, exports engine layout), arena/selfplay --snk-net, export.py -> train/out/snk_net.h (SNK_USE_NET). Python and C++ forward agree to ~1e-5 (float16 inputs).
  - Pipeline: NET=1 (default) extracts new shards in parallel into feat/, trains NET_EPOCHS from the best net, arena 'snknet' against the current best. Once accepted, self-play uses the net and the linear candidate/table stay fixed. Workflow installs CPU torch. make smoke passes with it.
  - Browser: the net judges for the profile's level, snkSkillOf(NM) = geometric mean of the channel sigmas at s = 0.35 relative to the Club player (x small miss-chance factor), 0 without human error.
  - First result (net after 1 epoch on gh_1 data, arena skill 1.0, budget 2000): 99-29 vs the linear evaluator (2 processes); controls: same net on both sides 43-44, roles reversed 26-81. Validation result loss at skill 1.0: 0.472 -> 0.464, skill 0: 0.315 -> 0.266.
  - Net v2 (5 epochs on gh_1 + 79k local positions at skills 0-2.5; val result loss 0.4246 vs linear 0.4445) is the default (train/baseline/snk_net.bin, snk_use_net = 1). Arena at skill 1.0, budget 2000 (stopped early on Paul's call): 108-39 vs the linear evaluator, 69-47 vs the 1-epoch net.
  - Page: 'est. value' shows a win chance whenever the win table or net is in use (E.snkEvalKind()).
  - Local machine: 8 threads but only ~3.8 GB in the session's memory cgroup; the trainer was OOM-killed once when arenas ran beside it.

- v12 (2026-10-25): foul-and-miss fixes, replays
  - Paul saw a bot (Pro profile) play straight into the brown when snookered; the opponent then played on. Reproduced in tests/snk_escape_test.cpp: 40 snookered positions, 0 honest attempts, 40 fouls/misses, never a replace. Causes: (1) no escape candidates (only straight shots at the hidden balls), (2) valueAfter expected a miss only with a clear sight, (3) snkBotChoose valued "replace" at about -1.5 points.
  - Bot: per decision snkMissPrep (bot.h) computes sight, p* and the escape windows (snkEscapeScan, shared with the referee's snkEscapeChance, same results). snkMissCalled mirrors snkReferee (pro mode, clear sight, p* >= tau, effort test via the noise-free first contact; a cue ball we placed in a snooker counts as a miss; third clear-sight miss = loss). Escape candidates (kind 3): the best 4-6 windows (direct, 1 or 2 cushions) at 2.4/1.5/3.6 m/s, refined like safeties. BotCtx.refMode/refTau (page: the referee settings).
  - Foul cap (Paul's idea): a simulated foul that would be called a miss is worth at most rootW - fee, rootW = evaluator value of the position as it stands, fee = 0.02 (1 + 2 missCount) (consecutive clear-sight misses; the third loses). At the final choice a deliberate foul (fails to hit a ball on as intended) is also capped at the best real attempt - fee, because the static evaluator can overrate snookered positions (it learned from self-play where deliberate fouls were nearly free). Snooker safeties whose intended first contact fouls are redrawn (up to 4 tries), which fixed break-offs into the pink/brown in noise-free play.
  - Result: honest attempts 100% at skills 0/0.35/1.0 and both referee modes; fouls from snookers 40 -> 1-9 (real attempts that failed). Self-play speed unchanged (~25 frames/min/core at budget 2000 with the net).
  - Foul options by search: snkChooseOption (replaces snkBotChoose) searches each option's position with budget/4 (100-1500). The page shows the values as win chances. wasm endShot(noisy, budget); snkForceChoice for replays.
  - Data caveat: all self-play so far (gh_1 etc.) has the old behaviour (deliberate fouls, nobody replaces), so the evaluator overrates snookered positions. New self-play fixes the data; worth retraining.
  - Replays: text format (PGN-like, documented at the replay section of web/template.html): tags + one line per stroke with the executed stroke (bit-exact replay), placement, nominations and foul choice. Page: Replays button (R) / menu / game-over dialog: save with an ID, copy, paste, library; replay bar (step, seek, play, "bots play on from here"). Library = the artifact's db collection `replays` (doc id = replay ID, field `text`); outside claude.ai it falls back to localStorage. Read it with ArtifactData (url https://claude.ai/artifact/AAwk4E6Dqq8nhTBde8zxKk, collection replays).
  - train/bin/replay FILE [--at N] [--think N --budget B]: native replay (matches WASM stroke by stroke), bot decision and foul-option values at any stroke. Tests: tests/replay_node.js (live vs replay, bit-identical), tests/ui_replay.sh (headless Windows Chrome from WSL; Playwright needs a newer node than this machine's v12).
  - Foul guard (the final-choice cap) is BotCtx.foulGuard (default on). It is the searched form of the root cap (best real attempt vs the evaluator's rootW), so with an accurate evaluator it never binds; labels are frame results, so it only changes which strokes self-play plays (towards correct play). Measure after retraining: snk_escape_test ... noguard. Current net (gh_1 data) without the guard: honest 90% (skill 0.35) / 95% (skill 1.0), with it 100%. Turn it off for good once the retrained net alone reaches ~100%.
  - Pause (P) and Resign buttons on the page. Resign (two clicks): the human in bot games, the player to move in two-player games; engine export resign(p) sets the winner; the record gets the note and Result "X wins, Y resigned at a-b". Bot concession (menu option "Bots concede hopeless games", default on, page only): own win estimate < 10% on two of its decisions in a row -> concedes to a bot, offers once per frame to a human (Accept / Play on). Native self-play and the arena never resign. Test: tests/ui_replay.sh resign.
  - Perfect bot (no human error): frames differ by seed (rack jitter of 15 um plus the search's random safeties and placements); the same seed reproduces a frame exactly. Typical frame: a few safeties, then one visit clears 110-135.

## Next steps (for the next session)
1. Start "Train bots" (defaults now include NET=1): the cloud continues from the net in train/baseline, its self-play uses the net. Read runs/gh_<n>/REPORT.md on branch training/<n>, merge if good.
2. Possibly: equal-time comparison (the net costs ~1.5x per decision); a larger net if the arena keeps accepting; policy prior; two-shot lookahead for safeties.

## Strategy gaps and training plan (discussed 2026-10-24)
- The bot looks one shot ahead plus a static evaluation, so it has no multi-visit planning.
- The snooker value is linear in points (points this shot ± expected next visit). It ignores score context, so it does not play for snookers when it needs them and does not tighten up when far ahead. The 'snookered' term is a flat -3.
- Plan:
  1. Make the eval a win probability, P(win | score diff, points remaining, whose turn, position features).
  2. Learn the score-context part from a cheap abstract frame simulator (Markov model with pot/foul/safety probabilities calibrated from the physics bot). That gives millions of frames per second without physics.
  3. Tune the positional features Texel-style: fit a logistic eval on (position, final result) pairs, with TD(lambda) / search-bootstrapped targets to cut variance.
  4. Self-play in the cloud with noisy profiles and random mid-frame starting positions.
  5. Condition the value on the noise profile (sigmas as inputs).

## Decisions
- Game: 8-ball first. Snooker is a second mode later. Bots train on 8-ball.
- Platform: browser, three.js r128 (cdnjs). The physics engine is written once in freestanding C++ (no libc) and compiled:
  - natively with g++ for training (`-O2 -ffp-contract=off`),
  - to WASM with `clang++ --target=wasm32 -nostdlib`, embedded as base64,
  - and to a plain-JS fallback via binaryen `wasm2js`, in case CSP blocks WASM. Results are bit-identical.
- Training runs in the cloud sandbox (2 cores). Bot design is NN + search: the network evaluates positions and proposes shots, and the C++ simulation picks the best.

## Physics model (engine/physics.h)
- SI units, 9-ft table 2.54 x 1.27 m, R = 28.575 mm, m = 0.17 kg.
- Cloth: mu_s = 0.20 (sliding), mu_r = 0.010 (rolling), spin decay 10 rad/s^2. Integrated as half-kick / drift / half-kick with dt = 1 ms. The sliding-to-rolling switch is snapped exactly.
- Collisions: exact time of impact inside each substep.
- Ball-ball: e = 0.95, Coulomb friction mu = 0.06 (throw, gear effect).
- Cushion: e = max(0.60, 0.92 - 0.06*v_n), mu = 0.20, contact at nose height (sin theta = 0.27).
- Cue: tip offset (a, b) in units of R, max 0.6 (miscue beyond that). Ball speed v0 = V(1+m/M)/(1+m/M+2.5(a^2+b^2)), and omega follows from the impulse at the tip point. Squirt is 0.035 rad per unit of side offset.
- Power p in [0,1] maps to V = 0.15 + 8.85 p^1.6 m/s.
- Pockets: corner mouth 0.118 m, side mouth 0.130 m, angled jaw facings (142 deg / 104 deg). A ball drops once its centre passes the mouth line by 0.6R (corner) or 0.9R (side).

## Precision variant (NoiseModel)
Signal-dependent motor noise (Harris & Wolpert 1998). With s = V/vmax and a skill multiplier:
- sigma_phi = skill*(0.0010 + 0.0070 s) rad, i.e. 0.06 deg to 0.46 deg
- sigma_V/V = skill*(0.015 + 0.060 s)
- sigma_a,b = skill*(0.015 + 0.080 s) R
Power is chosen first, then the noise is applied at release.

## 8-ball rules (engine/rules8.h)
Simplified WPA with no called pockets. Open table after the break. The first legal pot assigns the groups. Fouls (scratch, no hit, wrong ball first, no rail) give ball in hand anywhere. A scratch on the break also gives ball in hand anywhere. An 8 potted on the break is re-spotted. The 8 wins only if the group is cleared and the shot has no foul.

## Bot (engine/bot.h)
- Candidates: ghost-ball pots for each legal ball x pocket with clear lanes. Aim is refined by simulation (throw), then expanded to 4 powers x 3 vertical spins. Plus 20 random safeties.
- Ball in hand: placements behind good pots. Break: hit the apex at full power.
- Every candidate is simulated and the rules are applied. The leave is scored by V = win probability of the player to move.
- V is an MLP 16 -> 24 -> 12 -> 1 (tanh, sigmoid output) on position features: remaining balls, open/on-8 flags, ball in hand, top-3 shot eases, counts of pottable balls, 8 clear. A heuristic logit is the fallback.
- Precision bot: the top 6 candidates are re-evaluated with 8 noisy executions each (same NoiseModel), and the best mean is chosen.
- train/train.cpp: self-play (heuristic V), then Adam/BCE training, then NN-vs-heuristic eval. Writes train/out/{std,noisy}_weights.h, which wasm_api.cpp includes.
- Browser: about 0.15 s per decision (std) and 0.3 s (noisy) in WASM.

## Files (in the project as src/...)
engine/pmath.h, engine/physics.h, engine/events.h, engine/rules8.h, engine/rulesSnk.h, engine/bot.h, engine/wasm_api.cpp, web/template.html, build.sh, tests/test_physics.cpp, train/train.cpp, train/out/*_weights.h, tests/bot_test.cpp, tests/match.cpp.
build.sh needs `npm i three@0.128.0 binaryen@116.0.0` in build/npm for the local test harness and wasm2js.
