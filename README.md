# Felt & Chalk — pool and snooker with a physics engine and search bots

A 3D 8-ball and snooker game for the browser. The physics engine is a freestanding C++ engine that
compiles both natively (for training and testing) and to WebAssembly (for the game), so the bots
plan with exactly the physics you play on.

Playable build: the artifact "Felt & Chalk 8-Ball" on claude.ai (`web/index.html` is the same page).

## Layout

| Path | What |
|---|---|
| `engine/pmath.h` | libc-free math (sin, log, exp, atan2) and a xoshiro RNG, bit-identical natively and in WASM |
| `engine/physics.h` | Ball/cloth/cushion physics, table builder (pool and snooker pockets), human-error noise model, reference fixed-step integrator |
| `engine/events.h` | Event-driven simulation: analytic trajectories, exact event times, used by the game, the bots and training |
| `engine/rules8.h`, `engine/rulesSnk.h` | 8-ball rules; snooker rules incl. free ball, foul options, colour nomination |
| `engine/bot.h` | Two-phase search bot (root-finding + best-first pattern search), evaluators, snooker miss referee |
| `engine/wasm_api.cpp` | C ABI used by the page |
| `web/template.html` | three.js front end; `./build.sh` embeds the WASM and a JS fallback into `web/index.html` |
| `train/` | Native training tools and the pipeline (see `TRAINING.md`) |
| `train/baseline/` | Starting evaluators (binary), `train/out/` the headers compiled into the game |
| `train/replay.cpp` | Replays a game saved by the page (`train/bin/replay FILE --think N`) |
| `tests/` | Physics checks, bot tests, snooker rule and escape tests, replay round trip (`node tests/replay_node.js`), headless-browser UI tests |

## Build

```bash
scripts/setup.sh        # compilers + npm deps for the web build (three@0.128.0, binaryen@116.0.0)
make tools              # native training tools in train/bin
make tests              # physics + snooker rules checks
make build/ev_test && build/ev_test   # event engine vs reference stepper, invariants, speed (~2.5 min)
./build.sh              # web/index.html
```

## Train

```bash
HOURS=8 ./train/run_pipeline.sh     # see TRAINING.md
```
