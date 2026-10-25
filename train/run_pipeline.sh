#!/usr/bin/env bash
# train/run_pipeline.sh — iterative training loop for both bots:
#   self-play (all cores) -> win table (snooker) -> tune (Texel / TD(lambda)) -> neural snooker
#   evaluator (features + train/snknet.py) -> arena (SPRT) -> accept
# Everything is driven by environment variables, e.g.
#   HOURS=8 ./train/run_pipeline.sh
#   HOURS=2 GAMES="snk 8noisy" SP_MIN=20 ./train/run_pipeline.sh
# Snooker self-play rotates over SKILLS (human-error multipliers, 0 = no error). Only the
# frames at SKILL feed the current evaluator (tuning + arena); all of them are kept as raw
# positions (data/snk*/ *.posrec), which train the skill-conditioned neural evaluator (NET=1).
# Snooker frames start from a mix of position families (SP_MIX, train/positions.h: needing
# snookers, laid snookers, free balls, safety battles, clearances, black-ball finishes, perturbed
# real positions, ...), and the snooker arenas grade candidates on a suite of them (ARENA_SUITE),
# with the result per family in the report.
# Results: runs/<stamp>/REPORT.md, runs/<stamp>/progress.jsonl, runs/<stamp>/best/*, and the
# accepted evaluators exported to train/out/*.h (compiled into the browser game by ./build.sh).
set -u
cd "$(dirname "$0")/.."
HOURS=${HOURS:-6}              # total wall-clock budget
SP_MIN=${SP_MIN:-25}           # self-play minutes per iteration (on every core)
ARENA_MIN=${ARENA_MIN:-15}     # max arena minutes per candidate (SPRT may stop earlier)
BUDGET=${BUDGET:-2000}         # bot simulations per shot during self-play and arena
LAMBDA=${LAMBDA:-0.7}          # TD(lambda) for the tuning targets
SKILL=${SKILL:-1.0}            # human-error multiplier the evaluator is tuned and tested at (1 = Club player)
SKILLS=${SKILLS:-"1.0 0 1.0 2.0"}  # snooker self-play skill per core (rotating); data at other skills goes to data/snk_s<skill>
CORES=${CORES:-$(nproc)}
GAMES=${GAMES:-"snk"}
RUN=${RUN:-runs/$(date +%Y%m%d_%H%M%S)}
BUILD_WEB=${BUILD_WEB:-1}
EXPORT=${EXPORT:-1}            # 1: write the best evaluators to train/out/*.h at the end
NET=${NET:-1}                  # 1: also train the neural snooker evaluator (needs python3 with torch + numpy)
NET_EPOCHS=${NET_EPOCHS:-3}    # passes over all snooker positions per iteration
FEAT=${FEAT:-feat}             # feature cache for the net (rebuilt from data/ when missing)
# snooker starting positions (train/positions.h). SP_MIX="" restores the old 50% break / 50% random.
SP_MIX=${SP_MIX-"break:4 random:1 snookers:1 lead:0.5 snookered:1 freeball:0.5 safety:1 clearance:1 black:0.5 perturbed:1"}
ARENA_SUITE=${ARENA_SUITE-"break:4 random:1 snookers:1 lead:1 snookered:1 freeball:1 safety:1 clearance:1 black:0.5 perturbed:1"}
POS_FROM=${POS_FROM:-data/snk}  # real positions for the 'perturbed' family (a sample of them)

mkdir -p "$RUN/best" data/8std data/8noisy data/snk "$FEAT"
if [ -n "${BEST_FROM:-}" ]; then cp "$BEST_FROM"/* "$RUN/best/"; else cp train/baseline/* "$RUN/best/"; fi
BEST="$RUN/best"
make -s tools || { echo "build failed"; exit 1; }
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$RUN/pipeline.log"; }
{
  echo "# Training run $(basename "$RUN")"
  echo
  echo "Started $(date -u +%Y-%m-%dT%H:%MZ), budget ${HOURS} h on ${CORES} cores. Self-play ${SP_MIN} min/iteration (${GAMES}), bot budget ${BUDGET} sims/shot, TD lambda ${LAMBDA}, evaluator skill x${SKILL}, snooker self-play skills: ${SKILLS}."
  echo
  echo "Snooker starting positions: self-play \`${SP_MIX:-break:1 random:1 (legacy)}\`, arena suite \`${ARENA_SUITE:-break only}\`."
  echo
} > "$RUN/REPORT.md"

deadline=$(python3 -c "import time; print(int(time.time() + ${HOURS} * 3600))")
need=$(python3 -c "print(int((${SP_MIN} + ${ARENA_MIN} + 3) * 60))")
read -r -a G <<< "$GAMES"
read -r -a SK <<< "$SKILLS"
# launch_selfplay MINUTES SLOTS SLOT_OFFSET DIR: start SLOTS self-play processes (pids -> $pids)
launch_selfplay() {
  local minutes=$1 slots=$2 off=$3 dir=$4 c g seed sk out nz cmd
  for c in $(seq $((off + 1)) $((off + slots))); do
    g=${G[$(( (c - 1 + iter - 1) % ${#G[@]} ))]}
    seed=$(( iter * 1000 + c ))
    sk=$SKILL
    case $g in
      8std)   cmd=(train/bin/selfplay --game 8 --noisy 0 --weights8 "$BEST/w8_std.bin" --out data/8std --random-start 0.5) ;;
      8noisy) cmd=(train/bin/selfplay --game 8 --noisy 1 --weights8 "$BEST/w8_noisy.bin" --out data/8noisy --random-start 0.5) ;;
      snk)    sk=${SK[$(( (c - 1 + iter - 1) % ${#SK[@]} ))]}
              out=data/snk; nz=1
              if [ "$(python3 -c "print(1 if abs(float('$sk') - float('$SKILL')) < 1e-9 else 0)")" != "1" ]; then out=data/snk_s$sk; fi
              [ "$(python3 -c "print(1 if float('$sk') == 0 else 0)")" = "1" ] && nz=0
              mkdir -p "$out"
              cmd=(train/bin/selfplay --game snk --noisy $nz --snk-params "$BEST/snk_params.txt" --snk-table "$BEST/snk_table.bin" --out "$out")
              [ "$(cat "$BEST/snk_use_net" 2>/dev/null)" = "1" ] && cmd+=(--snk-net "$BEST/snk_net.bin")
              if [ -n "$SP_MIX" ]; then cmd+=(--start-mix "$SP_MIX" --pos-from "$POS_FROM"); else cmd+=(--random-start 0.5); fi ;;
    esac
    "${cmd[@]}" --minutes "$minutes" --seed "$seed" --budget "$BUDGET" --skill "$sk" > "$dir/selfplay_${g}_s${sk}_$c.log" 2>&1 &
    pids+=($!)
  done
}

iter=0
while [ $(( $(date +%s) + need )) -lt "$deadline" ]; do
  iter=$((iter + 1)); IT="$RUN/iter_$iter"; mkdir -p "$IT/cand"
  log "iteration $iter: self-play ${SP_MIN} min on $CORES cores"
  pids=()
  launch_selfplay "$SP_MIN" "$CORES" 0 "$IT"
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

  # neural snooker evaluator: features of every raw-position shard not converted yet (in
  # parallel), then training from the current best net (or from the linear evaluator)
  if [ "$NET" = "1" ] && ls data/snk*/*.posrec > /dev/null 2>&1; then
    for f in data/snk*/*.posrec; do
      pre="$FEAT/$(basename "$(dirname "$f")")_$(basename "$f" .posrec)"
      [ "$pre.meta" -nt "$f" ] || echo "$f $pre"
    done | xargs -r -P "$CORES" -n 2 sh -c 'train/bin/snkfeat --data "$0" --out "$1" --snk-params "'"$BEST"'/snk_params.txt" --snk-table "'"$BEST"'/snk_table.bin" > "$1.log" 2>&1'
    init=(); [ -f "$BEST/snk_net.bin" ] && init=(--init "$BEST/snk_net.bin")
    python3 train/snknet.py --data $(ls "$FEAT"/*.meta | sed 's/\.meta$//') "${init[@]}" --params "$BEST/snk_params.txt" --table "$BEST/snk_table.bin" \
      --lambda "$LAMBDA" --epochs "$NET_EPOCHS" --threads "$CORES" --seed "$iter" --out "$IT/cand/snk_net.bin" > "$IT/tune_snknet.log" 2>&1 \
      || { log "iteration $iter: net training failed (see $IT/tune_snknet.log)"; rm -f "$IT/cand/snk_net.bin"; }
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
  # current best snooker evaluator (B side): net, win table + linear E, or the legacy points evaluation
  if [ "$(cat "$BEST/snk_use_table" 2>/dev/null)" = "1" ]; then bp="$BEST/snk_params.txt"; bt="$BEST/snk_table.bin"; else bp=none; bt=none; fi
  bn=none; [ "$(cat "$BEST/snk_use_net" 2>/dev/null)" = "1" ] && bn="$BEST/snk_net.bin"
  suite=(); [ -n "$ARENA_SUITE" ] && suite=(--suite "$ARENA_SUITE" --pos-from "$POS_FROM")
  if [ -f "$IT/cand/snk_net.bin" ]; then
    train/bin/arena --game snk --noisy 1 --skill "$SKILL" --minutes "$ARENA_MIN" --budget "$BUDGET" --seed $((iter * 7 + 4)) --a-snk-params "$BEST/snk_params.txt" --a-snk-table "$BEST/snk_table.bin" --a-snk-net "$IT/cand/snk_net.bin" --b-snk-params "$bp" --b-snk-table "$bt" --b-snk-net "$bn" "${suite[@]}" > "$IT/arena_snknet.log" 2>&1 & apids+=($!)
  fi
  # once the net is in use, the linear candidate (and the table under the net) stays fixed
  if [ -f "$IT/cand/snk_params.txt" ] && [ "$bn" = "none" ]; then
    train/bin/arena --game snk --noisy 1 --skill "$SKILL" --minutes "$ARENA_MIN" --budget "$BUDGET" --seed $((iter * 7 + 3)) --a-snk-params "$IT/cand/snk_params.txt" --a-snk-table "$IT/cand/snk_table.bin" --b-snk-params "$bp" --b-snk-table "$bt" "${suite[@]}" > "$IT/arena_snk.log" 2>&1 & apids+=($!)
  fi
  # the cores the arenas leave free keep producing self-play data meanwhile
  pids=()
  free=$(( CORES - ${#apids[@]} ))
  [ "$free" -gt 0 ] && launch_selfplay "$ARENA_MIN" "$free" "$CORES" "$IT"
  [ ${#apids[@]} -gt 0 ] && wait "${apids[@]}"
  [ ${#pids[@]} -gt 0 ] && { kill "${pids[@]}" 2>/dev/null; wait "${pids[@]}" 2>/dev/null; }

  # accept: SPRT says A, or a clear margin over at least 60 games
  echo "## Iteration $iter" >> "$RUN/REPORT.md"
  netAccepted=0
  for kind in 8std 8noisy snknet snk; do
    f="$IT/arena_$kind.log"; [ -f "$f" ] || continue
    line=$(grep '^ARENA' "$f" | tail -1 | sed 's/^ARENA //')
    [ -n "$line" ] || continue
    accept=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(1 if d['verdict']=='A' or (d['games']>=60 and d['score']>=0.56) else 0)" "$line")
    [ "$kind" = "snk" ] && [ "$netAccepted" = "1" ] && accept=0   # the net was trained on the old table
    tl=$(grep '^RESULT' "$IT/tune_$kind.log" 2>/dev/null | tail -1)
    echo "- **$kind**: arena $line, accepted: $accept. Tuning: ${tl:-n/a}" >> "$RUN/REPORT.md"
    if grep -q '^FAMILY' "$f"; then   # per-family results of a snooker suite
      grep '^FAMILY' "$f" | sed 's/^FAMILY //' | python3 -c "
import json, sys
print()
print('  | family | games | A-B | score | Elo | pairs swept A / split / swept B | mover won |')
print('  |---|---|---|---|---|---|---|')
for l in sys.stdin:
    d = json.loads(l)
    print('  | %s | %d | %d-%d | %.3f | %+.0f +- %.0f | %d / %d / %d | %.0f%% |' % (d['family'], d['games'], d['a'], d['b'], d['score'], d['elo'], d['elo_ci'], d['swept_a'], d['split'], d['swept_b'], 100 * d['mover_won']))
print()" >> "$RUN/REPORT.md"
    fi
    echo "{\"iter\":$iter,\"kind\":\"$kind\",\"accepted\":$accept,\"arena\":$line}" >> "$RUN/progress.jsonl"
    if [ "$accept" = "1" ]; then
      case $kind in
        8std)   cp "$IT/cand/w8_std.bin" "$BEST/" ;;
        8noisy) cp "$IT/cand/w8_noisy.bin" "$BEST/" ;;
        snknet) cp "$IT/cand/snk_net.bin" "$BEST/"; echo 1 > "$BEST/snk_use_net"; echo 1 > "$BEST/snk_use_table"; netAccepted=1 ;;
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
if [ "$EXPORT" = "1" ]; then
  python3 train/export.py --best "$BEST" | tee -a "$RUN/pipeline.log"
  if [ "$BUILD_WEB" = "1" ] && [ -d build/npm/node_modules/three ]; then ./build.sh | tee -a "$RUN/pipeline.log"; fi
fi
echo "Finished $(date -u +%Y-%m-%dT%H:%MZ) after $iter iterations. Best evaluators: \`$BEST\`, exported to \`train/out/\`." >> "$RUN/REPORT.md"
log "report: $RUN/REPORT.md"
