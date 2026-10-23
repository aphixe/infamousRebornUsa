/* Guest addresses that differ between the releases the port builds for (CMake INFAMOUS_REGION).
 *
 * EU  = BCES00220, the release the port was reverse engineered on (default).
 * USA = BCUS98119. Same compiled game: code is shifted in 26 regions (-0xF0 .. +0x1A8, tools/eu2usa.py),
 *       .data from the OPD table up (TOC, OPDs, the pointer tables near the TOC) is 0x48 lower, and the start of
 *       .data (language lists, class vtables) and all of .bss are at the same addresses. Each USA value below
 *       was checked against the USA executable (USA-PORT.md has the method).
 *
 * INF_RA(eu, usa): this build's address. Usable in C and C++ constant expressions.
 * Included from C (the ps3recomp runtime) and C++ (the port's hooks). */
#pragma once

#if defined(INFAMOUS_REGION_USA)
#define INF_RA(eu, usa) (usa)
#define INFAMOUS_REGION_NAME "BCUS98119"
#else
#define INF_RA(eu, usa) (eu)
#define INFAMOUS_REGION_NAME "BCES00220"
#endif

/* The executable's TOC (r2 of every module function). */
#define INFAMOUS_TOC INF_RA(0x0078BFC0u, 0x0078BF78u)
