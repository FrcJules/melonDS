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

// Minimal Nintendo Switch frontend (SDL2, software renderer, FreeBIOS direct boot).
// Config: sdmc:/switch/melonds-fork/config.txt (also threaded3d=0|1), one "key=value" per line:
//   rom=<path of the .nds>   sav=<path of the save file>   jit=0|1   overclock=<MHz, 0 = off>
// Every second a line "t fps avg_ms max_ms" is appended to sdmc:/switch/melonds-fork/log.txt.
// Quit: hold L + R + PLUS + MINUS.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>
#include <switch.h>

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"

using namespace melonDS;

namespace melonDS { extern u64 SwitchJitTransitions; extern u64 SwitchJitTransitionNs; extern const char* SwitchCrashLogPath; extern u64 SwitchDbgWin9, SwitchDbgWin7, SwitchDbgCodeMem; extern u64 SwitchFaultCount, SwitchLastFaultAddr, SwitchLastFaultPC; }

extern std::string SwitchSavePath;
extern std::string SwitchLogPath;

// where the main loop currently is, logged once per second by a watchdog thread (to locate hangs)
static std::atomic<int> DebugStage {0};
static std::atomic<int> DebugFrames {0};

// free memory as seen by the kernel, to find out whether an applet runs out of heap
static void MemLog(const char* label);
#define memlog(label) MemLog(label)

static const char* ConfigDir = "sdmc:/switch/melonds-fork";

static bool ReadFile(const std::string& path, std::unique_ptr<u8[]>& data, u32& len)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = std::make_unique<u8[]>(size);
    len = (u32)fread(data.get(), 1, size, f);
    fclose(f);
    return len == (u32)size;
}

static std::map<std::string, std::string> ReadConfig()
{
    std::map<std::string, std::string> cfg;
    FILE* f = fopen((std::string(ConfigDir) + "/config.txt").c_str(), "r");
    if (!f) return cfg;
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('=');
        if (eq != std::string::npos)
            cfg[l.substr(0, eq)] = l.substr(eq + 1);
    }
    fclose(f);
    return cfg;
}

// SDL joystick button numbers of a Switch controller -> DS key bits
static u32 DSKeyForButton(int button)
{
    switch (button)
    {
    case 0: return 1 << 0;   // A
    case 1: return 1 << 1;   // B
    case 2: return 1 << 10;  // X
    case 3: return 1 << 11;  // Y
    case 6: return 1 << 9;   // L
    case 7: return 1 << 8;   // R
    case 10: return 1 << 3;  // PLUS -> Start
    case 11: return 1 << 2;  // MINUS -> Select
    case 12: return 1 << 5;  // left
    case 13: return 1 << 6;  // up
    case 14: return 1 << 4;  // right
    case 15: return 1 << 7;  // down
    }
    return 0;
}

static void MemLog(const char* label)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    FILE* f = fopen((SwitchLogPath).c_str(), "a");
    if (!f) return;
    fprintf(f, "mem %s: used %llu / %llu MB\n", label, (unsigned long long)(used >> 20), (unsigned long long)(total >> 20));
    fclose(f);
}

int main(int argc, char** argv)
{
    mkdir(ConfigDir, 0777);
    SwitchLogPath = std::string(ConfigDir) + "/log.txt";
    melonDS::SwitchCrashLogPath = SwitchLogPath.c_str();
    // the file is closed after every line: FAT only records its size on close, and the log must be readable while the app runs
    auto logf = [](const char* fmt, auto... a) {
        FILE* f = fopen(SwitchLogPath.c_str(), "a");
        if (!f) return;
        fprintf(f, fmt, a...);
        fclose(f);
    };

    auto cfg = ReadConfig();
    logf("=== melonDS fork (Switch) start, jit=%s ===\n", cfg.count("jit") ? cfg["jit"].c_str() : "0");

    if (!cfg.count("rom"))
    {
        logf("no rom= in config.txt\n");
        return 1;
    }

    // optional CPU overclock (off by default), restored on exit
    ClkrstSession cpuSession {};
    bool clkOk = false;
    u32 oldClock = 0;
    int overclockMHz = cfg.count("overclock") ? atoi(cfg["overclock"].c_str()) : 0;
    if (R_SUCCEEDED(clkrstInitialize()))
    {
        if (R_SUCCEEDED(clkrstOpenSession(&cpuSession, PcvModuleId_CpuBus, 3)))
        {
            clkOk = true;
            clkrstGetClockRate(&cpuSession, &oldClock);
            logf("cpu clock: %u MHz\n", oldClock / 1000000);
            if (overclockMHz > 0)
            {
                Result rc = clkrstSetClockRate(&cpuSession, (u32)overclockMHz * 1000000u);
                logf("overclock %d MHz: rc=0x%x\n", overclockMHz, rc);
            }
        }
    }

    memlog("before SDL");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
    SDL_Window* window = SDL_CreateWindow("melonDS", 0, 0, 1280, 720, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        SDL_JoystickOpen(i);

    SDL_AudioSpec want {}, have {};
    want.freq = 32823;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    memlog("after SDL");

    std::atomic<bool> watchdogStop {false};
    std::thread watchdog([&]() {
        int last = -1, idle = 0;
        while (!watchdogStop)
        {
            SDL_Delay(1000);
            int frames = DebugFrames;
            idle = (frames == last) ? idle + 1 : 0;
            last = frames;
            if (idle >= 1)
                logf("watchdog: no frame progress for %ds (frames=%d, stage=%d, faults=%llu, lastfault addr=%llx pc=%llx)\n", idle, frames, (int)DebugStage, (unsigned long long)SwitchFaultCount, (unsigned long long)SwitchLastFaultAddr, (unsigned long long)SwitchLastFaultPC);
        }
    });

    std::unique_ptr<u8[]> romData;
    u32 romLen;
    if (!ReadFile(cfg["rom"], romData, romLen))
    {
        logf("cannot read rom %s\n", cfg["rom"].c_str());
        return 1;
    }

    NDSArgs args;
    if (!cfg.count("jit") || cfg["jit"] != "1")
        args.JIT = std::nullopt;
    else if (cfg.count("fastmem") && cfg["fastmem"] == "0")
        args.JIT->FastMemory = false;
    DebugStage = 10;
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    logf("addrs: main=%llx win9=%llx win7=%llx codemem=%llx\n", (unsigned long long)(u64)&main, (unsigned long long)SwitchDbgWin9, (unsigned long long)SwitchDbgWin7, (unsigned long long)SwitchDbgCodeMem);
    memlog("after NDS");

    NDSCart::NDSCartArgs cartArgs {};
    if (cfg.count("sav"))
    {
        SwitchSavePath = cfg["sav"];
        std::unique_ptr<u8[]> sav;
        u32 savLen = 0;
        if (ReadFile(SwitchSavePath, sav, savLen))
        {
            cartArgs.SRAM = std::move(sav);
            cartArgs.SRAMLength = savLen;
        }
        else
            logf("no save at %s, starting empty\n", SwitchSavePath.c_str());
    }
    auto cart = NDSCart::ParseROM(std::move(romData), romLen, nullptr, std::move(cartArgs));
    if (!cart)
    {
        logf("failed to parse the ROM\n");
        return 1;
    }
    nds->SetNDSCart(std::move(cart));
    DebugStage = 11;
    nds->Reset();
    DebugStage = 12;

    // software renderer on its own thread (3 cores are available to homebrew)
    if (cfg.count("threaded3d") && cfg["threaded3d"] == "1")
    {
        RendererSettings rendererSettings {};
        rendererSettings.ScaleFactor = 1;
        rendererSettings.Threaded = true;
        nds->GetRenderer().SetRenderSettings(rendererSettings);
        logf("threaded 3D renderer enabled\n");
    }

    DebugStage = 13;
    nds->SetupDirectBoot(cfg["rom"]);
    DebugStage = 14;
    nds->Start();
    DebugStage = 15;
    logf("fastmem: %s\n", nds->JIT.FastMemoryEnabled() ? "on" : "off");
    memlog("after Start");

    // screens: 256x384 scaled to the 720 px height, centred
    const int dh = 720, dw = 480, dx = (1280 - dw) / 2;
    SDL_Rect dst {dx, 0, dw, dh};

    u32 keys = 0;
    bool running = true;
    bool touching = false;
    std::vector<s16> audioBuf(2048 * 2);

    using clock = std::chrono::steady_clock;
    auto secStart = clock::now();
    int frames = 0;
    double sumMs = 0, maxMs = 0;
    int seconds = 0;

    while (running && appletMainLoop())
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_JOYBUTTONDOWN) keys |= DSKeyForButton(ev.jbutton.button);
            else if (ev.type == SDL_JOYBUTTONUP) keys &= ~DSKeyForButton(ev.jbutton.button);
            else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION)
            {
                int px = (int)(ev.tfinger.x * 1280), py = (int)(ev.tfinger.y * 720);
                int sx = (px - dx) * 256 / dw;
                int sy = py * 384 / dh - 192;
                if (sx >= 0 && sx < 256 && sy >= 0 && sy < 192)
                {
                    nds->TouchScreen(sx, sy);
                    touching = true;
                }
            }
            else if (ev.type == SDL_FINGERUP && touching)
            {
                nds->ReleaseScreen();
                touching = false;
            }
        }

        const u32 quit = (1 << 8) | (1 << 9) | (1 << 3) | (1 << 2); // R L Start Select
        if ((keys & quit) == quit)
            break;

        DebugStage = 2;
        nds->SetKeyMask(~keys & 0xFFF);

        auto t0 = clock::now();
        nds->RunFrame();
        double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
        frames++;
        DebugFrames++;
        DebugStage = 3;

        void *top, *bottom;
        if (nds->GPU.GetFramebuffers(&top, &bottom))
        {
            u8* pixels; int pitch;
            if (SDL_LockTexture(tex, nullptr, (void**)&pixels, &pitch) == 0)
            {
                for (int y = 0; y < 192; y++)
                {
                    memcpy(pixels + y * pitch, (u8*)top + y * 256 * 4, 256 * 4);
                    memcpy(pixels + (y + 192) * pitch, (u8*)bottom + y * 256 * 4, 256 * 4);
                }
                SDL_UnlockTexture(tex);
            }
        }
        DebugStage = 4;
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_RenderPresent(renderer);

        DebugStage = 5;
        // drain the SPU every frame; drop audio if the device falls too far behind
        for (;;)
        {
            int avail = nds->SPU.GetOutputSize();
            if (avail <= 0) break;
            int n = std::min(avail, (int)(audioBuf.size() / 2));
            n = nds->SPU.ReadOutput(audioBuf.data(), n);
            if (n <= 0) break;
            if (audio && SDL_GetQueuedAudioSize(audio) < 32823 * 4 / 2)
                SDL_QueueAudio(audio, audioBuf.data(), n * 2 * sizeof(s16));
        }

        auto now = clock::now();
        if (now - secStart >= std::chrono::seconds(1))
        {
            seconds++;
            logf("%d %d %.1f %.1f jit_trans=%llu jit_trans_ms=%llu\n", seconds, frames, sumMs / frames, maxMs, (unsigned long long)SwitchJitTransitions, (unsigned long long)(SwitchJitTransitionNs / 1000000));
            frames = 0; sumMs = 0; maxMs = 0;
            secStart = now;
        }
    }

    watchdogStop = true;
    watchdog.join();
    logf("=== stop ===\n");
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (clkOk)
    {
        if (overclockMHz > 0 && oldClock) clkrstSetClockRate(&cpuSession, oldClock);
        clkrstCloseSession(&cpuSession);
    }
    clkrstExit();
    return 0;
}
