# Host tests. No hardware, no Arduino toolchain.
#   make -C test          build and run everything
#   make -C test clean

CXX      ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -I stub -I ..

LINK_SRC := test_rpg_link.cpp ../rpg_link.cpp
SESS_SRC := test_session.cpp ../rpg_session.cpp ../rpg_link.cpp
HDRS     := ../rpg_link.h ../rpg_session.h stub/Arduino.h sim_channel.h

.PHONY: all clean
all: run run_session
	./run
	./run_session

run: $(LINK_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(LINK_SRC)

run_session: $(SESS_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SESS_SRC)

clean:
	rm -f run run_session
