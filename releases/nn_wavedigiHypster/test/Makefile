# Host tests: make -C test
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
BIN ?= build

all: run

$(BIN):
	mkdir -p $(BIN)

$(BIN)/test_wdf: test_wdf.cpp ../src/wdf.h ../src/hypster_circuit.h | $(BIN)
	$(CXX) $(CXXFLAGS) -I../src $< -o $@

$(BIN)/test_card: test_card.cpp ../src/*.h ../src/main.cpp mock/ComputerCard.h | $(BIN)
	$(CXX) $(CXXFLAGS) -Wno-return-type -Imock -include mock/ComputerCard.h -I../src $< -o $@

$(BIN)/sim: sim.cpp ../src/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) -I../src $< -o $@

run: $(BIN)/test_wdf $(BIN)/test_card $(BIN)/sim
	$(BIN)/test_wdf
	$(BIN)/test_card
	$(BIN)/sim ref

clean:
	rm -rf $(BIN)

.PHONY: all run clean
