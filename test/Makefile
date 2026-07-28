# Host tests. No hardware, no Arduino toolchain.
#   make -C test          build and run everything
#   make -C test clean

CXX      ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -g -I stub -I ..

LINK_SRC := test_rpg_link.cpp ../rpg_link.cpp
SESS_SRC := test_session.cpp ../rpg_session.cpp ../rpg_link.cpp
HDRS     := ../rpg_link.h ../rpg_session.h stub/Arduino.h sim_channel.h test_common.h

.PHONY: all clean check-stub
all: check-stub run run_session
	./run
	./run_session

# The layering rule is enforced by this build, not by convention: rpg_link.cpp
# and rpg_session.cpp compile against stub/Arduino.h, which declares nothing at
# all. A millis() call or an M5/RadioLib include therefore fails to compile
# here. That only holds while the stub stays empty — and the tempting fix when
# you hit "'millis' was not declared" is to add it to the stub, which would
# disable the rule silently and permanently. So: the stub may contain comments,
# blank lines, #pragma once, and the two size includes. Nothing else.
check-stub:
	@bad=$$(grep -vE '^[[:space:]]*(//|$$|#pragma once|#include <(stdint|stddef)\.h>)' stub/Arduino.h); \
	if [ -n "$$bad" ]; then \
	  echo "FAIL stub/Arduino.h has grown beyond its allowed contents:"; \
	  echo "$$bad" | sed 's/^/    /'; \
	  echo "That stub is what enforces the layering rule — see CLAUDE.md."; \
	  exit 1; \
	fi

run: $(LINK_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(LINK_SRC)

run_session: $(SESS_SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SESS_SRC)

clean:
	rm -f run run_session
