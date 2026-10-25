# Training run gh_1

Started 2026-10-25T02:27Z, budget 5 h on 4 cores. Self-play 25 min/iteration (snk), bot budget 2000 sims/shot, TD lambda 0.7, evaluator skill x1.0, snooker self-play skills: 1.0 0 1.0 2.0.

## Iteration 1
- **snk**: arena {"a":225,"b":178,"games":403,"score":0.558,"elo":41,"elo_ci":34,"llr":2.56,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4700 val_result_loss 0.4342 params -0.423 0.378 2.417 -4.536 -2.330 2.362
- snooker calibration: pPotRed 0.5940 pPotCol 0.5913 pOpen 0.4926 pFoulPot 0.0151 pFoulSafety 0.1755 
- data so far: 56M, games files: 7

## Iteration 2
- **snk**: arena {"a":167,"b":120,"games":287,"score":0.582,"elo":57,"elo_ci":41,"llr":2.99,"verdict":"A"}, accepted: 1. Tuning: RESULT snooker val_target_loss 0.4825 val_result_loss 0.4467 params -0.388 0.377 1.896 -4.585 -2.529 2.497
- snooker calibration: pPotRed 0.5930 pPotCol 0.5941 pOpen 0.4920 pFoulPot 0.0145 pFoulSafety 0.1763 
- data so far: 105M, games files: 14

## Iteration 3
- **snk**: arena {"a":203,"b":175,"games":378,"score":0.537,"elo":26,"elo_ci":35,"llr":1.01,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4816 val_result_loss 0.4611 params -0.289 0.390 1.955 -4.669 -2.599 2.507
- snooker calibration: pPotRed 0.5847 pPotCol 0.5702 pOpen 0.4618 pFoulPot 0.0156 pFoulSafety 0.2191 
- data so far: 162M, games files: 21

## Iteration 4
- **snk**: arena {"a":184,"b":196,"games":380,"score":0.484,"elo":-11,"elo_ci":35,"llr":-2.45,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4839 val_result_loss 0.4646 params -0.139 0.397 2.034 -4.644 -2.644 2.521
- snooker calibration: pPotRed 0.5756 pPotCol 0.5505 pOpen 0.4395 pFoulPot 0.0164 pFoulSafety 0.2478 
- data so far: 219M, games files: 28

## Iteration 5
- **snk**: arena {"a":180,"b":193,"games":373,"score":0.483,"elo":-12,"elo_ci":35,"llr":-2.51,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4917 val_result_loss 0.4749 params -0.142 0.411 2.073 -4.626 -2.885 2.548
- snooker calibration: pPotRed 0.5716 pPotCol 0.5404 pOpen 0.4303 pFoulPot 0.0170 pFoulSafety 0.2596 
- data so far: 277M, games files: 35

## Iteration 6
- **snk**: arena {"a":202,"b":176,"games":378,"score":0.534,"elo":24,"elo_ci":35,"llr":0.84,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4863 val_result_loss 0.4717 params -0.044 0.401 2.190 -4.864 -2.853 2.543
- snooker calibration: pPotRed 0.5685 pPotCol 0.5317 pOpen 0.4224 pFoulPot 0.0174 pFoulSafety 0.2697 
- data so far: 334M, games files: 42

## Iteration 7
- **snk**: arena {"a":199,"b":184,"games":383,"score":0.520,"elo":14,"elo_ci":35,"llr":-0.13,"verdict":"undecided"}, accepted: 0. Tuning: RESULT snooker val_target_loss 0.4860 val_result_loss 0.4710 params -0.033 0.413 2.082 -4.951 -2.617 2.533
- snooker calibration: pPotRed 0.5664 pPotCol 0.5266 pOpen 0.4184 pFoulPot 0.0176 pFoulSafety 0.2752 
- data so far: 391M, games files: 49

Finished 2026-10-25T07:13Z after 7 iterations. Best evaluators: `runs/gh_1/best`, exported to `train/out/`.
