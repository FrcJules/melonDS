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
//   audiointerp=0..4 (none, linear, cosine, cubic, gaussian; default 3)   autosave=0|1   autoload=1|0 (default on)
// Save states (next to the config, named after the ROM: <rom>.ss<slot>, slot 0 = automatic):
//   hold ZL + ZR, then X = save, Y = load, left/right = choose slot 1..4 (squares on the left show the slot, colour = result).
//   The state is also saved automatically on exit (slot 0) and loaded at start.
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

#include <dirent.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <switch.h>

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "Savestate.h"

using namespace melonDS;

namespace melonDS { extern u64 SwitchJitTransitions; extern u64 SwitchJitTransitionNs; extern const char* SwitchCrashLogPath; extern u64 SwitchDbgWin9, SwitchDbgWin7, SwitchDbgCodeMem; extern u64 SwitchFaultCount, SwitchLastFaultAddr, SwitchLastFaultPC; }

extern std::string SwitchSavePath;
extern std::string SwitchLogPath;
namespace melonDS::Platform { extern int SwitchHelperCore; }

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

static std::map<std::string, std::string> ReadConfig(const char* file = "config.txt")
{
    std::map<std::string, std::string> cfg;
    FILE* f = fopen((std::string(ConfigDir) + "/" + file).c_str(), "r");
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


// ---- minimal UI: text with the console's shared font, ROM browser ----
static TTF_Font* UIFont = nullptr;
static const SDL_Color White {240, 240, 240, 255}, Grey {150, 160, 175, 255}, Accent {120, 210, 255, 255};

static void DrawText(SDL_Renderer* r, const std::string& text, int x, int y, SDL_Color c)
{
    if (!UIFont || text.empty()) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(UIFont, text.c_str(), c);
    if (!surf) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect d {x, y, surf->w, surf->h};
    if (t) SDL_RenderCopy(r, t, nullptr, &d);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}

static bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string BaseName(const std::string& path)
{
    size_t sl = path.find_last_of('/');
    return sl == std::string::npos ? path : path.substr(sl + 1);
}

static std::string StripExt(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

static std::string LowerCase(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// returns the chosen .nds path, or "" if the user quit (PLUS)
static std::string BrowseRom(SDL_Renderer* r, std::string dir)
{
    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> entries;
    int sel = 0, top = 0;
    const int rows = 14;

    auto join = [](const std::string& d, const std::string& n) { return d.back() == '/' ? d + n : d + "/" + n; };
    auto load = [&]() {
        entries.clear();
        if (dir.size() > 6) entries.push_back({"..", true});
        if (DIR* d = opendir(dir.c_str()))
        {
            std::vector<Entry> dirs, files;
            while (dirent* e = readdir(d))
            {
                std::string n = e->d_name;
                if (n.empty() || n[0] == '.') continue;
                struct stat st;
                bool isDir = e->d_type == DT_DIR || (stat(join(dir, n).c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (isDir) dirs.push_back({n, true});
                else
                {
                    std::string l = LowerCase(n);
                    if (l.size() > 4 && (l.compare(l.size() - 4, 4, ".nds") == 0 || l.compare(l.size() - 4, 4, ".srl") == 0))
                        files.push_back({n, false});
                }
            }
            closedir(d);
            auto byName = [](const Entry& a, const Entry& b) { return LowerCase(a.name) < LowerCase(b.name); };
            std::sort(dirs.begin(), dirs.end(), byName);
            std::sort(files.begin(), files.end(), byName);
            entries.insert(entries.end(), dirs.begin(), dirs.end());
            entries.insert(entries.end(), files.begin(), files.end());
        }
        sel = 0; top = 0;
    };
    load();

    while (appletMainLoop())
    {
        SDL_SetRenderDrawColor(r, 18, 22, 30, 255);
        SDL_RenderClear(r);
        DrawText(r, "Choisir une ROM", 60, 24, Accent);
        DrawText(r, dir, 60, 66, Grey);
        for (int i = 0; i < rows && top + i < (int)entries.size(); i++)
        {
            const Entry& e = entries[top + i];
            int y = 120 + i * 36;
            if (top + i == sel)
            {
                SDL_SetRenderDrawColor(r, 40, 70, 110, 255);
                SDL_Rect bar {40, y - 2, 1200, 36};
                SDL_RenderFillRect(r, &bar);
            }
            DrawText(r, (e.isDir ? "[dossier]  " : "") + e.name, 60, y, top + i == sel ? White : Grey);
        }
        if (entries.empty()) DrawText(r, "(aucune ROM .nds ici)", 60, 120, Grey);
        DrawText(r, "A : choisir     B : dossier parent     +/- : quitter", 60, 656, Grey);
        SDL_RenderPresent(r);

        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, 200) || ev.type != SDL_JOYBUTTONDOWN) continue;
        int n = (int)entries.size();
        switch (ev.jbutton.button)
        {
        case 13: if (n) sel = std::max(sel - 1, 0); break;
        case 15: if (n) sel = std::min(sel + 1, n - 1); break;
        case 12: if (n) sel = std::max(sel - rows, 0); break;
        case 14: if (n) sel = std::min(sel + rows, n - 1); break;
        case 10: case 11: return "";
        case 1:
        {
            if (dir.size() > 6)
            {
                size_t sl = dir.find_last_of('/');
                dir = sl <= 5 ? "sdmc:/" : dir.substr(0, sl);
                load();
            }
            break;
        }
        case 0:
        {
            if (!n) break;
            const Entry& e = entries[sel];
            if (e.isDir)
            {
                if (e.name == "..")
                {
                    size_t sl = dir.find_last_of('/');
                    dir = sl <= 5 ? "sdmc:/" : dir.substr(0, sl);
                }
                else dir = join(dir, e.name);
                load();
            }
            else return join(dir, e.name);
            break;
        }
        }
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
    return "";
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
    if (cfg.count("helpercore")) Platform::SwitchHelperCore = atoi(cfg["helpercore"].c_str());
    // defaults (best measured setup); config.txt overrides, then ui.txt (menu), then the defaults below
    auto ui0 = ReadConfig("ui.txt");
    const bool useJit = !cfg.count("jit") || cfg["jit"] != "0";
    logf("=== melonDS fork (Switch) start, jit=%d ===\n", useJit ? 1 : 0);

    // optional CPU overclock (off by default), restored on exit
    ClkrstSession cpuSession {};
    bool clkOk = false;
    u32 oldClock = 0;
    int overclockMHz = cfg.count("overclock") ? atoi(cfg["overclock"].c_str()) : (ui0.count("cpu") ? atoi(ui0["cpu"].c_str()) : 1224);
    bool threaded3d = cfg.count("threaded3d") ? cfg["threaded3d"] == "1" : (ui0.count("threaded") ? ui0["threaded"] == "1" : true);
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
                u32 applied = 0;
                clkrstGetClockRate(&cpuSession, &applied);
                logf("cpu clock after set: %u MHz\n", applied / 1000000);
            }
        }
    }

    // memory and GPU clocks, to see what else changes with the CPU clock
    ClkrstSession emcSession {}, gpuSession {};
    bool emcOk = clkOk && R_SUCCEEDED(clkrstOpenSession(&emcSession, PcvModuleId_EMC, 3));
    bool gpuOk = clkOk && R_SUCCEEDED(clkrstOpenSession(&gpuSession, PcvModuleId_GPU, 3));

    // temperature probe (board sensor), logged each second
    TsSession tsSession {};
    bool tsOk = R_SUCCEEDED(tsInitialize()) && R_SUCCEEDED(tsOpenSession(&tsSession, TsDeviceCode_LocationExternal));
    logf("temperature sensor: %s\n", tsOk ? "ok" : "unavailable");

    memlog("before SDL");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK);
    SDL_Window* window = SDL_CreateWindow("melonDS", 0, 0, 1280, 720, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = nullptr;
    std::vector<SDL_Joystick*> joysticks;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        joysticks.push_back(SDL_JoystickOpen(i));

    // text: the console's shared font
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && TTF_Init() == 0)
    {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)))
            UIFont = TTF_OpenFontRW(SDL_RWFromConstMem(fd.address, fd.size), 0, 28);
    }
    logf("ui font: %s\n", UIFont ? "ok" : "unavailable");

    // display settings (menu), kept in ui.txt
    auto ui = ReadConfig("ui.txt");
    int layout = ui.count("layout") ? atoi(ui["layout"].c_str()) : 0;     // 0 stacked, 1 side by side
    int filter = ui.count("filter") ? atoi(ui["filter"].c_str()) : 0;     // 0 sharp pixels, 1 smooth
    bool showFps = ui.count("showfps") && ui["showfps"] == "1";
    auto saveUi = [&]() {
        FILE* f = fopen((std::string(ConfigDir) + "/ui.txt").c_str(), "w");
        if (!f) return;
        fprintf(f, "layout=%d\nfilter=%d\nshowfps=%d\ncpu=%d\nthreaded=%d\n", layout, filter, showFps ? 1 : 0, overclockMHz, threaded3d ? 1 : 0);
        fclose(f);
    };

    auto makeTexture = [&]() {
        if (tex) SDL_DestroyTexture(tex);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, filter ? "1" : "0");
        tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 384);
    };

    // which ROM: rom= in config.txt wins, else the last one chosen (lastrom.txt); the browser opens if none, or with --browse
    bool wantBrowse = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--browse")) wantBrowse = true;
    std::string romPath = cfg.count("rom") ? cfg["rom"] : "";
    const std::string configRom = romPath;
    if (romPath.empty())
    {
        auto last = ReadConfig("lastrom.txt");
        if (last.count("rom")) romPath = last["rom"];
    }
    if (wantBrowse || romPath.empty() || !FileExists(romPath))
    {
        std::string startDir = cfg.count("romdir") ? cfg["romdir"] : "";
        if (startDir.empty() && !romPath.empty())
        {
            size_t sl = romPath.find_last_of('/');
            if (sl != std::string::npos) startDir = romPath.substr(0, sl);
        }
        if (startDir.empty()) startDir = "sdmc:/";
        romPath = BrowseRom(renderer, startDir);
        if (romPath.empty())
        {
            logf("no ROM chosen\n");
            return 0;
        }
        FILE* f = fopen((std::string(ConfigDir) + "/lastrom.txt").c_str(), "w");
        if (f) { fprintf(f, "rom=%s\n", romPath.c_str()); fclose(f); }
    }
    logf("rom: %s\n", romPath.c_str());
    makeTexture();

    SDL_AudioSpec want {}, have {};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    const int audioRate = have.freq ? have.freq : want.freq;
    logf("audio: %d Hz, %d channels, format 0x%x, buffer %d samples\n", have.freq, have.channels, (unsigned)have.format, have.samples);

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
            if (idle >= 1 && DebugStage != 99)
                logf("watchdog: no frame progress for %ds (frames=%d, stage=%d, faults=%llu, lastfault addr=%llx pc=%llx)\n", idle, frames, (int)DebugStage, (unsigned long long)SwitchFaultCount, (unsigned long long)SwitchLastFaultAddr, (unsigned long long)SwitchLastFaultPC);
        }
    });

    std::unique_ptr<u8[]> romData;
    u32 romLen;
    if (!ReadFile(romPath, romData, romLen))
    {
        logf("cannot read rom %s\n", romPath.c_str());
        return 1;
    }

    NDSArgs args;
    if (!useJit)
        args.JIT = std::nullopt;
    else if (cfg.count("fastmem") && cfg["fastmem"] == "0")
        args.JIT->FastMemory = false;
    args.OutputSampleRate = audioRate;
    args.Interpolation = static_cast<AudioInterpolation>(cfg.count("audiointerp") ? std::clamp(atoi(cfg["audiointerp"].c_str()), 0, 4) : 3);
    logf("audio interpolation: %d\n", (int)args.Interpolation);
    DebugStage = 10;
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    logf("addrs: main=%llx win9=%llx win7=%llx codemem=%llx\n", (unsigned long long)(u64)&main, (unsigned long long)SwitchDbgWin9, (unsigned long long)SwitchDbgWin7, (unsigned long long)SwitchDbgCodeMem);
    memlog("after NDS");

    NDSCart::NDSCartArgs cartArgs {};
    {
        // sav= only applies to the ROM named in config.txt; any other ROM gets <name>.sav next to the config
        SwitchSavePath = (cfg.count("sav") && romPath == configRom) ? cfg["sav"] : std::string(ConfigDir) + "/" + StripExt(BaseName(romPath)) + ".sav";
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
    if (threaded3d)
    {
        RendererSettings rendererSettings {};
        rendererSettings.ScaleFactor = 1;
        rendererSettings.Threaded = true;
        nds->GetRenderer().SetRenderSettings(rendererSettings);
        logf("threaded 3D renderer enabled\n");
    }

    DebugStage = 13;
    nds->SetupDirectBoot(romPath);
    DebugStage = 14;
    nds->Start();
    DebugStage = 15;
    logf("fastmem: %s, main thread on core %d, helper core %d\n", nds->JIT.FastMemoryEnabled() ? "on" : "off", (int)svcGetCurrentProcessorNumber(), Platform::SwitchHelperCore);
    memlog("after Start");

    // screens: stacked (480x720, centred) or side by side (2 x 640x480)
    SDL_Rect topR {}, botR {};
    auto computeLayout = [&]() {
        if (layout == 1) { topR = {0, 120, 640, 480}; botR = {640, 120, 640, 480}; }
        else { topR = {400, 0, 480, 360}; botR = {400, 360, 480, 360}; }
    };
    computeLayout();
    SDL_Texture* fpsTex = nullptr;
    int fpsW = 0, fpsH = 0;
    bool relaunch = false;

    // save states
    const std::string romBase = std::string(ConfigDir) + "/" + StripExt(BaseName(romPath));
    auto statePath = [&](int slot) { return romBase + ".ss" + std::to_string(slot); };
    auto saveState = [&](const std::string& path) -> bool {
        Savestate st;
        if (st.Error) return false;
        if (!nds->DoSavestate(&st) || st.Error) return false;
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(st.Buffer(), 1, st.Length(), f) == st.Length();
        ok = (fclose(f) == 0) && ok;
        if (!ok) { remove(tmp.c_str()); return false; }
        remove(path.c_str());
        return rename(tmp.c_str(), path.c_str()) == 0;
    };
    auto loadState = [&](const std::string& path) -> bool {
        std::unique_ptr<u8[]> data;
        u32 len = 0;
        if (!ReadFile(path, data, len) || len < 16) return false;
        // keep the running state so that a bad file does not leave the emulator half-loaded
        Savestate backup;
        if (backup.Error || !nds->DoSavestate(&backup) || backup.Error) return false;
        Savestate st(data.get(), len, false);
        if (!st.Error && nds->DoSavestate(&st) && !st.Error) return true;
        Savestate restore(backup.Buffer(), backup.Length(), false);
        nds->DoSavestate(&restore);
        return false;
    };
    const bool autosave = !cfg.count("autosave") || cfg["autosave"] != "0";
    if (!cfg.count("autoload") || cfg["autoload"] != "0")
    {
        FILE* f = fopen(statePath(0).c_str(), "rb");
        if (f)
        {
            fclose(f);
            logf("autoload slot 0: %s\n", loadState(statePath(0)) ? "ok" : "FAILED");
        }
    }
    int slot = 1;
    u32 pad = 0;                      // raw SDL buttons held (bit = button number)
    u8 fbR = 0, fbG = 0, fbB = 0;     // feedback colour of the last state action
    int fbSlot = 0;
    auto fbUntil = std::chrono::steady_clock::now();
    auto feedback = [&](u8 r, u8 g, u8 b, int sl) { fbR = r; fbG = g; fbB = b; fbSlot = sl; fbUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000); };

    auto drawScreens = [&]() {
        SDL_Rect srcTop {0, 0, 256, 192}, srcBot {0, 192, 256, 192};
        SDL_RenderCopy(renderer, tex, &srcTop, &topR);
        SDL_RenderCopy(renderer, tex, &srcBot, &botR);
    };

    // in-game menu (ZL + ZR + PLUS): emulation is paused. Returns 0 = resume, 1 = change ROM, 2 = quit
    auto runMenu = [&]() -> int {
        if (audio) SDL_PauseAudioDevice(audio, 1);
        const int prevStage = DebugStage;
        DebugStage = 99;   // paused: the watchdog must not report a hang
        const char* names[] = {"Reprendre", "Disposition", "Filtre", "Afficher les fps", "Fréquence CPU", "3D en thread séparé", "Emplacement d'état", "Sauvegarder l'état", "Charger l'état", "Changer de ROM", "Quitter"};
        const int count = 11;
        int sel = 0, result = -1;
        std::string note;
        while (result < 0 && appletMainLoop())
        {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            drawScreens();
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 10, 14, 22, 215);
            SDL_Rect full {0, 0, 1280, 720};
            SDL_RenderFillRect(renderer, &full);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            DrawText(renderer, "Menu", 420, 40, Accent);
            for (int i = 0; i < count; i++)
            {
                std::string label = names[i];
                if (i == 1) label += std::string(" : ") + (layout ? "côte à côte" : "empilées");
                if (i == 2) label += std::string(" : ") + (filter ? "lissé" : "pixels nets");
                if (i == 3) label += std::string(" : ") + (showFps ? "oui" : "non");
                if (i == 4) label += " : " + std::to_string(overclockMHz) + " MHz";
                if (i == 5) label += std::string(" : ") + (threaded3d ? "oui" : "non");
                if (i == 6) label += " : " + std::to_string(slot);
                int y = 90 + i * 46;
                if (i == sel)
                {
                    SDL_SetRenderDrawColor(renderer, 40, 70, 110, 255);
                    SDL_Rect bar {400, y - 4, 480, 42};
                    SDL_RenderFillRect(renderer, &bar);
                }
                DrawText(renderer, label, 420, y, i == sel ? White : Grey);
            }
            if (!note.empty()) DrawText(renderer, note, 420, 90 + count * 46 + 8, Accent);
            DrawText(renderer, "A : valider   gauche/droite : changer   B : retour", 330, 672, Grey);
            SDL_RenderPresent(renderer);

            SDL_Event ev;
            if (!SDL_WaitEventTimeout(&ev, 200) || ev.type != SDL_JOYBUTTONDOWN) continue;
            int b = ev.jbutton.button;
            int dir = b == 14 ? 1 : b == 12 ? -1 : 0;
            if (b == 13) sel = (sel + count - 1) % count;
            else if (b == 15) sel = (sel + 1) % count;
            else if (b == 1 || b == 10) result = 0;
            else if (b == 0 || dir)
            {
                bool act = b == 0;
                switch (sel)
                {
                case 0: if (act) result = 0; break;
                case 1: layout ^= 1; computeLayout(); saveUi(); break;
                case 2: filter ^= 1; makeTexture(); saveUi(); break;
                case 3: showFps = !showFps; saveUi(); break;
                case 4:
                {
                    const int steps[] = {1020, 1224, 1581, 1785};
                    int idx = 0;
                    for (int k = 0; k < 4; k++) if (steps[k] <= overclockMHz) idx = k;
                    idx = dir ? std::clamp(idx + dir, 0, 3) : (idx + 1) % 4;
                    overclockMHz = steps[idx];
                    if (clkOk) clkrstSetClockRate(&cpuSession, (u32)overclockMHz * 1000000u);
                    saveUi();
                    break;
                }
                case 5:
                {
                    threaded3d = !threaded3d;
                    RendererSettings rs {};
                    rs.ScaleFactor = 1;
                    rs.Threaded = threaded3d;
                    nds->GetRenderer().SetRenderSettings(rs);
                    saveUi();
                    break;
                }
                case 6: slot = dir ? std::clamp(slot + dir, 1, 4) : slot % 4 + 1; break;
                case 7: if (act) { bool ok = saveState(statePath(slot)); logf("save state slot %d: %s\n", slot, ok ? "ok" : "FAILED"); note = ok ? "État sauvegardé." : "Échec de la sauvegarde."; } break;
                case 8: if (act) { bool ok = loadState(statePath(slot)); logf("load state slot %d: %s\n", slot, ok ? "ok" : "FAILED"); note = ok ? "État chargé." : "Aucun état dans cet emplacement."; if (ok) result = 0; } break;
                case 9: if (act) result = 1; break;
                case 10: if (act) result = 2; break;
                }
            }
        }
        DebugStage = prevStage;
        if (audio) { SDL_ClearQueuedAudio(audio); SDL_PauseAudioDevice(audio, 0); }
        return result < 0 ? 2 : result;
    };

    u32 keys = 0;
    bool running = true;
    bool touching = false;
    std::vector<s16> audioBuf(2048 * 2);

    using clock = std::chrono::steady_clock;
    auto secStart = clock::now();
    int frames = 0;
    double sumMs = 0, maxMs = 0;
    int seconds = 0;
    u64 audioUnderruns = 0, audioDrops = 0;

    while (running && appletMainLoop())
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_JOYBUTTONDOWN)
            {
                int b = ev.jbutton.button;
                if (b < 32) pad |= 1u << b;
                if ((pad & (1u << 8)) && (pad & (1u << 9)))   // ZL + ZR held: hotkeys, the game gets no buttons
                {
                    if (b == 2)
                    {
                        bool ok = saveState(statePath(slot));
                        logf("save state slot %d: %s\n", slot, ok ? "ok" : "FAILED");
                        feedback(ok ? 40 : 220, ok ? 200 : 40, 40, slot);
                    }
                    else if (b == 3)
                    {
                        bool ok = loadState(statePath(slot));
                        logf("load state slot %d: %s\n", slot, ok ? "ok" : "FAILED");
                        if (ok && audio) SDL_ClearQueuedAudio(audio);
                        feedback(ok ? 50 : 220, ok ? 120 : 40, ok ? 230 : 40, slot);
                    }
                    else if (b == 10)
                    {
                        int choice = runMenu();
                        pad = 0;
                        keys = 0;
                        if (choice == 1) relaunch = true;
                        if (choice != 0) running = false;
                    }
                    else if (b == 12 || b == 14)
                    {
                        slot = b == 14 ? std::min(slot + 1, 4) : std::max(slot - 1, 1);
                        feedback(230, 200, 40, slot);
                    }
                }
                else keys |= DSKeyForButton(b);
            }
            else if (ev.type == SDL_JOYBUTTONUP)
            {
                int b = ev.jbutton.button;
                if (b < 32) pad &= ~(1u << b);
                keys &= ~DSKeyForButton(b);
            }
            else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION)
            {
                int px = (int)(ev.tfinger.x * 1280), py = (int)(ev.tfinger.y * 720);
                int sx = (px - botR.x) * 256 / botR.w;
                int sy = (py - botR.y) * 192 / botR.h;
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
        nds->SetKeyMask(((pad & (1u << 8)) && (pad & (1u << 9))) ? 0xFFF : (~keys & 0xFFF));

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
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        drawScreens();
        if (showFps && fpsTex)
        {
            SDL_Rect d {1280 - fpsW - 16, 8, fpsW, fpsH};
            SDL_RenderCopy(renderer, fpsTex, nullptr, &d);
        }
        if (clock::now() < fbUntil)
        {
            SDL_SetRenderDrawColor(renderer, fbR, fbG, fbB, 255);
            for (int i = 0; i < std::max(fbSlot, 1); i++)
            {
                SDL_Rect r {40 + i * 48, 40, fbSlot == 0 ? 16 : 36, 36};
                SDL_RenderFillRect(renderer, &r);
            }
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        }
        SDL_RenderPresent(renderer);

        DebugStage = 5;
        // drain the SPU every frame. The DS runs at 59.83 fps and the display at 60: instead of dropping audio
        // (clicks), nudge the SPU output rate so that the SDL queue stays around 100 ms.
        if (audio)
        {
            if (SDL_GetQueuedAudioSize(audio) == 0) audioUnderruns++;
            double queuedMs = SDL_GetQueuedAudioSize(audio) / 4.0 * 1000.0 / audioRate;
            double err = std::clamp((queuedMs - 100.0) / 100.0, -1.0, 1.0);
            nds->SPU.SetOutputSkew(1.0029 + 0.004 * err);
        }
        for (;;)
        {
            int avail = nds->SPU.GetOutputSize();
            if (avail <= 0) break;
            int n = std::min(avail, (int)(audioBuf.size() / 2));
            n = nds->SPU.ReadOutput(audioBuf.data(), n);
            if (n <= 0) break;
            if (audio && SDL_GetQueuedAudioSize(audio) < (u32)audioRate * 4 * 400 / 1000)
                SDL_QueueAudio(audio, audioBuf.data(), n * 2 * sizeof(s16));
            else
                audioDrops++;
        }

        auto now = clock::now();
        if (now - secStart >= std::chrono::seconds(1))
        {
            seconds++;
            if (showFps && UIFont)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d fps", frames);
                if (fpsTex) SDL_DestroyTexture(fpsTex);
                fpsTex = nullptr;
                if (SDL_Surface* sf = TTF_RenderUTF8_Blended(UIFont, buf, SDL_Color {120, 255, 140, 255}))
                {
                    fpsTex = SDL_CreateTextureFromSurface(renderer, sf);
                    fpsW = sf->w; fpsH = sf->h;
                    SDL_FreeSurface(sf);
                }
            }
            u32 curClock = 0;
            if (clkOk) clkrstGetClockRate(&cpuSession, &curClock);
            u32 emcClock = 0, gpuClock = 0;
            if (emcOk) clkrstGetClockRate(&emcSession, &emcClock);
            if (gpuOk) clkrstGetClockRate(&gpuSession, &gpuClock);
            float temp = 0;
            if (tsOk) tsSessionGetTemperature(&tsSession, &temp);
            logf("%d %d %.1f %.1f jit_trans=%llu jit_trans_ms=%llu audio_ms=%u under=%llu drops=%llu faults=%llu clk=%u emc=%u gpu=%u temp=%.1f\n", seconds, frames, sumMs / frames, maxMs, (unsigned long long)SwitchJitTransitions, (unsigned long long)(SwitchJitTransitionNs / 1000000), audio ? (unsigned)(SDL_GetQueuedAudioSize(audio) / 4 * 1000 / audioRate) : 0u, (unsigned long long)audioUnderruns, (unsigned long long)audioDrops, (unsigned long long)SwitchFaultCount, curClock / 1000000, emcClock / 1000000, gpuClock / 1000000, temp);
            frames = 0; sumMs = 0; maxMs = 0;
            secStart = now;
        }
    }

    if (autosave)
        logf("autosave slot 0: %s\n", saveState(statePath(0)) ? "ok" : "FAILED");

    watchdogStop = true;
    watchdog.join();
    logf("=== stop ===\n");
    if (audio) SDL_CloseAudioDevice(audio);
    for (SDL_Joystick* j : joysticks)
        if (j) SDL_JoystickClose(j);
    if (fpsTex) SDL_DestroyTexture(fpsTex);
    if (UIFont) TTF_CloseFont(UIFont);
    TTF_Quit();
    logf("exit step 1\n");
    plExit();
    logf("exit step 2\n");
    SDL_DestroyTexture(tex);
    logf("exit step 3\n");
    SDL_DestroyRenderer(renderer);
    logf("exit step 4\n");
    SDL_DestroyWindow(window);
    logf("exit step 5\n");
    SDL_Quit();
    logf("exit step 6\n");
    if (clkOk)
    {
        if (oldClock) clkrstSetClockRate(&cpuSession, oldClock);
        clkrstCloseSession(&cpuSession);
    }
    if (emcOk) clkrstCloseSession(&emcSession);
    if (gpuOk) clkrstCloseSession(&gpuSession);
    if (tsOk) tsSessionClose(&tsSession);
    tsExit();
    logf("exit step 7\n");
    clkrstExit();
    logf("exit step 8\n");
    if (relaunch)
    {
        // come back to this homebrew with the ROM browser open
        std::string line = std::string("\"") + argv[0] + "\" --browse";
        envSetNextLoad(argv[0], line.c_str());
    }
    logf("exit: main returns, emulator destructors next\n");
    return 0;
}
