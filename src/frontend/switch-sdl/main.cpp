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
// Config: sdmc:/switch/melonds-fork/config.txt, one "key=value" per line:
//   rom=<path of the .nds>   sav=<path of the save file>   jit=0|1   overclock=<MHz, 0 = off>
// Every second a line "t fps avg_ms max_ms" is appended to sdmc:/switch/melonds-fork/log.txt.
// Quit: hold L + R + PLUS + MINUS.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
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

extern std::string SwitchSavePath;
extern FILE* SwitchLogFile;

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

int main(int argc, char** argv)
{
    mkdir(ConfigDir, 0777);
    SwitchLogFile = fopen((std::string(ConfigDir) + "/log.txt").c_str(), "a");
    auto logf = [](const char* fmt, auto... a) {
        if (!SwitchLogFile) return;
        fprintf(SwitchLogFile, fmt, a...);
        fflush(SwitchLogFile);
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
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);

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
    nds->Reset();
    nds->SetupDirectBoot(cfg["rom"]);
    nds->Start();

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

        nds->SetKeyMask(~keys & 0xFFF);

        auto t0 = clock::now();
        nds->RunFrame();
        double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
        frames++;

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
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_RenderPresent(renderer);

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
            logf("%d %d %.1f %.1f\n", seconds, frames, sumMs / frames, maxMs);
            frames = 0; sumMs = 0; maxMs = 0;
            secStart = now;
        }
    }

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
    if (SwitchLogFile) fclose(SwitchLogFile);
    return 0;
}
