# Training run local1

Started 2026-10-24T16:45Z, budget 0.27 h on 2 cores. Self-play 8 min/iteration, bot budget 200 sims/shot, TD lambda 0.7, profile skill x1.0.

## Iteration 1
- **8noisy**: arena {"a":63,"b":67,"games":130,"score":0.485,"elo":-11,"elo_ci":60,"llr":-0.83,"verdict":"undecided"}, accepted: 0. Tuning: RESULT 8ball val_target_loss 0.6234 val_result_loss 0.6398 (before 0.6520 / 0.6761)
- **snk**: arena {"a":14,"b":11,"games":25,"score":0.560,"elo":42,"elo_ci":146,"llr":0.17,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.3892 val_result_loss 0.3103 params -0.545 0.395 -0.012 -4.813 -2.050 2.035
- snooker calibration: pPotRed 0.5531 pPotCol 0.5138 pOpen 0.5132 pFoulPot 0.0198 pFoulSafety 0.1642 
- data so far: 476K, games files: 2

Finished 2026-10-24T16:59Z after 1 iterations. Best evaluators: `runs/local1/best`, exported to `train/out/`.
