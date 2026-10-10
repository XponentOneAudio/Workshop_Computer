# Host tests for the Budgie voice. Needs a C++17 compiler, Python 3, numpy, scipy.
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion

render: render.cpp ../budgie_voice.h ../budgie_data.h
	$(CXX) $(CXXFLAGS) -o $@ render.cpp -lm

card_sim: card_sim.cpp ../main.cpp ../budgie_voice.h ../budgie_data.h
	$(CXX) $(CXXFLAGS) -Wno-unused-parameter -o $@ card_sim.cpp -lm

test: render card_sim
	./render --mods
	./card_sim --test
	python3 test_parity.py

tour.wav: card_sim
	./card_sim $@

clean:
	rm -f render card_sim *.bin tour.wav

.PHONY: test clean
