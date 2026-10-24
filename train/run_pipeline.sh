#!/usr/bin/env bash
# train/run_pipeline.sh — iterative training loop for both bots:
#   self-play (all cores) -> win table (snooker) -> tune (Texel / TD(lambda)) -> arena (SPRT) -> accept
# Everything is driven by environment variables, e.g.
#   HOURS=8 ./train/run_pipeline.sh
#   HOURS=2 GAMES="snk" SP_MIN=20 ./train/run_pipeline.sh
# Results: runs/<stamp>/REPORT.md, runs/<stamp>/progress.jsonl, runs/<stamp>/best/*, and the
# accepted evaluators exported to train/out/*.h (compiled into the browser game by ./build.sh).
set -u
cd "$(dirname "$0")/.."
HOURS=${HOURS:-6}              # total wall-clock budget
SP_MIN=${SP_MIN:-25}           # self-play minutes per iteration (on every core)
ARENA_MIN=${ARENA_MIN:-15}     # max arena minutes per candidate (SPRT may stop earlier)
BUDGET=${BUDGET:-250}          # bot simulations per shot during self-play and arena
LAMBDA=${LAMBDA:-0.7}          # TD(lambda) for the tuning targets
SKILL=${SKILL:-1.0}            # human-error multiplier of the self-play profile (1 = Club player)
CORES=${CORES:-$(nproc)}
GAMES=${GAMES:-"snk 8noisy 8std"}
RUN=${RUN:-runs/$(date +%Y%m%d_%H%M%S)}
BUILD_WEB=${BUILD_WEB:-1}

mkdir -p "$RUN/best" data/8std data/8noisy data/snk
if [ -n "${BEST_FROM:-}" ]; then cp "$BEST_FROM"/* "$RUN/best/"; else cp train/baseline/* "$RUN/best/"; fi
BEST="$RUN/best"
make -s tools || { echo "build failed"; exit 1; }
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$RUN/pipeline.log"; }
{
  echo "# Training run $(basename "$RUN")"
  echo
  echo "Started $(date -u +%Y-%m-%dT%H:%MZ), budget ${HOURS} h on ${CORES} cores. Self-play ${SP_MIN} min/iteration, bot budget ${BUDGET} sims/shot, TD lambda ${LAMBDA}, profile skill x${SKILL}."
  echo
} > "$RUN/REPORT.md"

deadline=$(python3 -c "import time; print(int(time.time() + ${HOURS} * 3600))")
need=$(python3 -c "print(int((${SP_MIN} + ${ARENA_MIN} + 3) * 60))")
read -r -a G <<< "$GAMES"
iter=0
while [ $(( $(date +%s) + need )) -lt "$deadline" ]; do
  iter=$((iter + 1)); IT="$RUN/iter_$iter"; mkdir -p "$IT/cand"
  log "iteration $iter: self-play ${SP_MIN} min on $CORES cores"
  pids=()
  for c in $(seq 1 "$CORES"); do
    g=${G[$(( (c - 1 + iter - 1) % ${#G[@]} ))]}
    seed=$(( iter * 1000 + c ))
    case $g in
      8std)   cmd=(train/bin/selfplay --game 8 --noisy 0 --weights8 "$BEST/w8_std.bin" --out data/8std) ;;
      8noisy) cmd=(train/bin/selfplay --game 8 --noisy 1 --weights8 "$BEST/w8_noisy.bin" --out data/8noisy) ;;
      snk)    cmd=(train/bin/selfplay --game snk --noisy 1 --snk-params "$BEST/snk_params.txt" --snk-table "$BEST/snk_table.bin" --out data/snk) ;;
    esac
    "${cmd[@]}" --minutes "$SP_MIN" --seed "$seed" --budget "$BUDGET" --skill "$SKILL" --random-start 0.5 > "$IT/selfplay_${g}_$c.log" 2>&1 &
    pids+=($!)
  done
  wait "${pids[@]}"

  # snooker win table from the calibrated abstract simulator
  if ls data/snk/*.stats > /dev/null 2>&1; then
    train/bin/abstract_snk --stats data/snk/*.stats --frames 2000 --out "$IT/cand/snk_table.bin" --calib-out "$IT/calibration.txt" > "$IT/abstract.log" 2>&1
  fi
  # tune
  [ -n "$(ls data/8std/*.games 2>/dev/null)" ] && train/bin/tune --game 8 --data data/8std --init "$BEST/w8_std.bin" --lambda "$LAMBDA" --out "$IT/cand/w8_std.bin" > "$IT/tune_8std.log" 2>&1
  [ -n "$(ls data/8noisy/*.games 2>/dev/null)" ] && train/bin/tune --game 8 --data data/8noisy --init "$BEST/w8_noisy.bin" --lambda "$LAMBDA" --out "$IT/cand/w8_noisy.bin" > "$IT/tune_8noisy.log" 2>&1
  if [ -f "$IT/cand/snk_table.bin" ] && [ -n "$(ls data/snk/*.games 2>/dev/null)" ]; then
    train/bin/tune --game snk --data data/snk --table "$IT/cand/snk_table.bin" --init "$BEST/snk_params.txt" --lambda "$LAMBDA" --out "$IT/cand/snk_params.txt" > "$IT/tune_snk.log" 2>&1
  fi

  # arena: candidate (A) vs current best (B)
  log "iteration $iter: arena"
  apids=()
  if [ -f "$IT/cand/w8_std.bin" ]; then
    train/bin/arena --game 8 --noisy 0 --minutes "$ARENA_MIN" --budget "$BUDGET" --seed $((iter * 7 + 1)) --a-weights8 "$IT/cand/w8_std.bin" --b-weights8 "$BEST/w8_std.bin" > "$IT/arena_8std.log" 2>&1 & apids+=($!)
  fi
  if [ -f "$IT/cand/w8_noisy.bin" ]; then
    train/bin/arena --game 8 --noisy 1 --skill "$SKILL" --minutes "$ARENA_MIN" --budget "$BUDGET" --seed $((iter * 7 + 2)) --a-weights8 "$IT/cand/w8_noisy.bin" --b-weights8 "$BEST/w8_noisy.bin" > "$IT/arena_8noisy.log" 2>&1 & apids+=($!)
  fi
  if [ -f "$IT/cand/snk_params.txt" ]; then
    if [ "$(cat "$BEST/snk_use_table" 2>/dev/null)" = "1" ]; then bp="$BEST/snk_params.txt"; bt="$BEST/snk_table.bin"; else bp=none; bt=none; fi
    train/bin/arena --game snk --noisy 1 --skill "$SKILL" --minutes "$ARENA_MIN" --budget "$BUDGET" --seed $((iter * 7 + 3)) --a-snk-params "$IT/cand/snk_params.txt" --a-snk-table "$IT/cand/snk_table.bin" --b-snk-params "$bp" --b-snk-table "$bt" > "$IT/arena_snk.log" 2>&1 & apids+=($!)
  fi
  [ ${#apids[@]} -gt 0 ] && wait "${apids[@]}"

  # accept: SPRT says A, or a clear margin over at least 60 games
  echo "## Iteration $iter" >> "$RUN/REPORT.md"
  for kind in 8std 8noisy snk; do
    f="$IT/arena_$kind.log"; [ -f "$f" ] || continue
    line=$(grep '^ARENA' "$f" | tail -1 | sed 's/^ARENA //')
    [ -n "$line" ] || continue
    accept=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(1 if d['verdict']=='A' or (d['games']>=60 and d['score']>=0.56) else 0)" "$line")
    tl=$(grep '^RESULT' "$IT/tune_$kind.log" 2>/dev/null | tail -1)
    echo "- **$kind**: arena $line, accepted: $accept. Tuning: ${tl:-n/a}" >> "$RUN/REPORT.md"
    echo "{\"iter\":$iter,\"kind\":\"$kind\",\"accepted\":$accept,\"arena\":$line}" >> "$RUN/progress.jsonl"
    if [ "$accept" = "1" ]; then
      case $kind in
        8std)   cp "$IT/cand/w8_std.bin" "$BEST/" ;;
        8noisy) cp "$IT/cand/w8_noisy.bin" "$BEST/" ;;
        snk)    cp "$IT/cand/snk_params.txt" "$IT/cand/snk_table.bin" "$BEST/"; echo 1 > "$BEST/snk_use_table" ;;
      esac
      log "iteration $iter: $kind candidate accepted"
    fi
  done
  [ -f "$IT/calibration.txt" ] && { echo "- snooker calibration: $(tr '\n' ' ' < "$IT/calibration.txt")"; } >> "$RUN/REPORT.md"
  echo "- data so far: $(du -sh data | cut -f1), games files: $(ls data/*/*.games 2>/dev/null | wc -l)" >> "$RUN/REPORT.md"
  echo >> "$RUN/REPORT.md"
done

log "finished $iter iterations; exporting the best evaluators"
python3 train/export.py --best "$BEST" | tee -a "$RUN/pipeline.log"
if [ "$BUILD_WEB" = "1" ] && [ -d build/npm/node_modules/three ]; then ./build.sh | tee -a "$RUN/pipeline.log"; fi
echo "Finished $(date -u +%Y-%m-%dT%H:%MZ) after $iter iterations. Best evaluators: \`$BEST\`, exported to \`train/out/\`." >> "$RUN/REPORT.md"
log "report: $RUN/REPORT.md"
