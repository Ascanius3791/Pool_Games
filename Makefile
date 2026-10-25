# Native tools for testing and training. The browser build is ./build.sh.
CXX ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++17 -ffp-contract=off
HDRS := $(wildcard engine/*.h) train/common.h
TOOLS := train/bin/selfplay train/bin/tune train/bin/arena train/bin/abstract_snk train/bin/snkfeat train/bin/replay

.PHONY: tools tests web clean smoke
tools: $(TOOLS)
HDRS += train/positions.h

train/bin/%: train/%.cpp $(HDRS)
	@mkdir -p train/bin
	$(CXX) $(CXXFLAGS) -o $@ $<

build/%: tests/%.cpp $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

tests: build/test_physics build/snk_rules_test build/bot_test build/ev_test build/snk_escape_test build/judge_test
	./build/test_physics
	./build/ev_test quick
	./build/snk_rules_test 1.0 1 120 0
	./build/snk_escape_test 0.35 20 600 0
	./build/judge_test 3 60 400

web:
	./build.sh

# 3-minute end-to-end check of the training pipeline
smoke: tools
	HOURS=0.15 SP_MIN=1.5 ARENA_MIN=1 BUDGET=80 BUILD_WEB=0 EXPORT=0 NET_EPOCHS=1 FEAT=runs/smoke/feat RUN=runs/smoke ./train/run_pipeline.sh

clean:
	rm -rf train/bin build/test_physics build/snk_rules_test build/bot_test build/ev_test build/snk_escape_test build/judge_test

# starting-position families (train/positions.h): validity of every family (and of the position one
# bot shot later), incl. 'perturbed' when self-play data exists (POS_FROM). About 10 s.
.PHONY: positions-test
POS_FROM ?= data/snk
positions-test: build/positions_test
	./build/positions_test --n 2000 --shots 150 $(if $(wildcard $(POS_FROM)),--pos-from $(POS_FROM))
