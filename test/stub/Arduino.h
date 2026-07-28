// Minimal Arduino.h stub so rpg_link.cpp and rpg_session.cpp build on the host.
//
// DO NOT ADD SYMBOLS HERE. Its emptiness is what enforces the layering rule:
// because nothing is declared, a millis() call or a hardware include in those
// two files fails to compile, which is exactly the intended outcome. Adding the
// missing symbol to "fix" such an error would disable that enforcement silently
// and permanently. The check-stub target in ../Makefile fails the build if this
// file grows, and CLAUDE.md explains why.
#pragma once
#include <stdint.h>
#include <stddef.h>
