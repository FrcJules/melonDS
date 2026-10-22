/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

// Null OpenGL ES 3.0 implementation for melonDS-bench (BENCH_NULLGL builds only).
// Every GL entry point the OpenGL renderer calls does nothing but count the call, the bytes
// it hands to the driver, and fold the call and its data into a running hash of the command
// stream. This makes the CPU side of the OpenGL renderer measurable without a GPU: the cost
// of building commands and data is real, the driver and the GPU are absent.
// Same command stream (hash) = same output on any conforming driver.

#ifndef NULLGL_H
#define NULLGL_H

#include <stdint.h>

namespace NullGL
{

struct Stats
{
    uint64_t Calls;
    uint64_t UploadBytes;   // texture and buffer data handed to the driver
    uint64_t ReadPixels;    // glReadPixels calls: a CPU/GPU sync point on a real driver
};

// per-function call counts since the last ResetCounters(): fills names/counts, returns the number of functions
int GetCounters(const char** names, uint64_t* counts, int max);
Stats GetStats();
void ResetCounters();

// hash of the command stream since the last ResetHash() (off by default: hashing the uploads costs time)
void EnableHash(bool on);
uint64_t StreamHash();
void ResetHash();

}

#endif
