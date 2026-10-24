# Pool / Snooker project: status and design notes

## Status (2026-10-24)
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
engine/pmath.h, engine/physics.h, engine/rules8.h, engine/rulesSnk.h, engine/bot.h, engine/wasm_api.cpp, web/template.html, build.sh, tests/test_physics.cpp, train/train.cpp, train/out/*_weights.h, tests/bot_test.cpp, tests/match.cpp.
build.sh needs `npm i three@0.128.0 binaryen@116.0.0` in build/npm for the local test harness and wasm2js.
