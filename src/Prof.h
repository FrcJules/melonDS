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

#ifndef PROF_H
#define PROF_H

// Optional low-overhead timing of the emulation loop (define MELONDS_PROF, AArch64 only): ticks of the
// virtual counter summed per category and read/reset by the frontend. Disabled (no code at all) by default.
#if defined(MELONDS_PROF) && defined(__aarch64__)

extern unsigned long long MelonProf[16];

enum
{
    ProfARM9 = 0,       // ARM9 execution (JIT or interpreter, includes block compilation)
    ProfARM7,           // ARM7 execution and its timers
    ProfEvtLCD,         // scanline events (GPU 2D, renderer hooks)
    ProfEvtSPU,         // audio mixing
    ProfEvtOther,       // other events (DMA, cart, SPI, ...)
    ProfCompile,        // JIT block compilation (already counted in ARM9/ARM7)
    ProfGeom,           // 3D geometry engine (GPU3D::Run), separate from ProfARM9
    ProfTimers,         // timers of both CPUs, separate from ProfARM9/ARM7
};

static inline unsigned long long ProfNow()
{
    unsigned long long t;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

#define PROF_BEGIN() unsigned long long ProfT0 = ProfNow()
#define PROF_END(cat) (MelonProf[cat] += ProfNow() - ProfT0)
#define PROF_RESTART() (ProfT0 = ProfNow())

#else

#define PROF_BEGIN()
#define PROF_END(cat)
#define PROF_RESTART()

#endif

#endif
