CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -I. -MMD -MP

BUILD := build

SIM_SRCS := \
	sim/messages/format.cpp \
	sim/exchange/order_book.cpp \
	sim/exchange/exchange.cpp \
	sim/replay/scenario.cpp \
	sim/runtime/capture.cpp \
	sim/runtime/simulator.cpp
SIM_OBJS := $(SIM_SRCS:%.cpp=$(BUILD)/%.o)
SIM_LIB := $(BUILD)/libsim.a

APP_SRCS := \
	app/market_data/book.cpp \
	app/execution/orders.cpp \
	app/market_maker.cpp \
	app/main.cpp
APP_OBJS := $(APP_SRCS:%.cpp=$(BUILD)/%.o)

# Scenarios with a recorded expected order stream (<name>.expected).
VERIFY_FIXTURES := $(patsubst %.expected,%,$(wildcard sim/fixtures/*.expected))

.PHONY: all test verify update-expected clean
all: $(BUILD)/market_maker $(BUILD)/sim_replay $(BUILD)/sim_tests

$(SIM_LIB): $(SIM_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/sim_replay: $(BUILD)/sim/tools/replay_main.o $(SIM_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/sim_tests: $(BUILD)/tests/sim_tests.o $(SIM_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/market_maker: $(APP_OBJS) $(SIM_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(BUILD)/sim_tests
	./$(BUILD)/sim_tests

# Runs the app on every fixture and checks that its orders are unchanged.
verify: $(BUILD)/market_maker
	@status=0; \
	for f in $(VERIFY_FIXTURES); do \
		name=$$(basename $$f); \
		./$(BUILD)/market_maker $$f.scn --out $(BUILD)/verify/$$name > /dev/null || { status=1; continue; }; \
		if diff -u $$f.expected $(BUILD)/verify/$$name/orders.txt > $(BUILD)/verify/$$name/diff.txt; then \
			echo "PASS $$name"; \
		else \
			echo "FAIL $$name (see $(BUILD)/verify/$$name/diff.txt)"; status=1; \
		fi; \
	done; \
	exit $$status

# TA only: records the current app's orders as the expected output for every
# fixture. Students should never need this.
update-expected: $(BUILD)/market_maker
	@for f in $(wildcard sim/fixtures/*.scn); do \
		name=$$(basename $$f .scn); \
		./$(BUILD)/market_maker $$f --out $(BUILD)/verify/$$name > /dev/null && \
		cp $(BUILD)/verify/$$name/orders.txt sim/fixtures/$$name.expected && \
		echo "recorded $$name"; \
	done

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
