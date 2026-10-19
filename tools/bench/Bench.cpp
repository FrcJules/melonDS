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

// melonDS-bench: headless runner for reproducible measurements.
// Runs a ROM for a number of frames with the software renderer and reports
// timings, screenshot hashes and an audio hash. See README.md.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "Savestate.h"
#include "xxhash/xxhash.h"

#ifdef BENCH_GL
#include <EGL/egl.h>
#include "GPU_OpenGL.h"
#endif

using namespace melonDS;

namespace melonDS::Platform
{
extern std::atomic<bool> StopRequested;
}

struct FrameAction
{
    u32 Frame;
    std::string File;
};

struct InputEvent
{
    enum { Keys, Touch, Release } Type;
    u32 Frame;
    u32 Mask;       // Keys: pressed buttons (bit set = pressed)
    u16 X, Y;       // Touch
};

struct Options
{
    std::string RomPath;
    u32 Frames = 600;
    u32 Warmup = 0;
    bool JIT = true;
    bool FastMem = true;
    bool Threaded3D = false;
    bool OpenGL = false;   // --gl : renderer OpenGL natif (EGL sans fenêtre + Mesa), échelle 1 ; ignoré sans BENCH_GL
    bool AudioHash = false;
    std::string StatePath;
    std::string InputPath;
    std::string JsonPath;
    std::string FrameTimesPath;
    std::string FrameHashesPath;
    std::string AudioDumpPath;
    std::vector<FrameAction> Shots;
    std::vector<FrameAction> SaveStates;
};

static const char* kKeyNames[] =
{
    "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"
};

static void Usage()
{
    fprintf(stderr,
        "Usage: melonDS-bench <rom.nds> [options]\n"
        "  --frames N            number of frames to run (default 600)\n"
        "  --warmup N            frames run before the timed ones (default 0, included in --frames)\n"
        "  --no-jit              use the interpreter instead of the JIT\n"
        "  --threaded-3d         render the 3D on a second thread (software renderer option)\n"
        "  --no-fastmem          JIT without the fast memory mapping (needed under valgrind and qemu)\n"
        "  --state FILE          load a savestate before running\n"
        "  --save-state N:FILE   write a savestate after N frames (repeatable)\n"
        "  --shot N:FILE         write both screens as a PPM after N frames (repeatable)\n"
        "  --input FILE          input script (see README.md)\n"
        "  --audio-hash          include the audio hash in the report\n"
        "  --frame-times FILE    write the time of every frame (frame,ms) as CSV\n"
        "  --audio-dump FILE     write the raw audio (stereo s16 little-endian) to FILE\n"
        "  --frame-hashes FILE   write a hash of every frame (frame,screens,audio so far) as CSV\n"
        "  --json FILE           write the report to FILE instead of stdout\n");
}

static bool ParseFrameAction(const char* arg, std::vector<FrameAction>& out)
{
    const char* colon = strchr(arg, ':');
    if (!colon || colon == arg || !colon[1])
        return false;

    FrameAction action;
    action.Frame = (u32)strtoul(arg, nullptr, 10);
    action.File = colon + 1;
    out.push_back(action);
    return true;
}

static bool ParseArgs(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        bool hasValue = (i + 1 < argc);

        if (arg == "--frames" && hasValue)          opt.Frames = (u32)strtoul(argv[++i], nullptr, 10);
        else if (arg == "--warmup" && hasValue)     opt.Warmup = (u32)strtoul(argv[++i], nullptr, 10);
        else if (arg == "--no-jit")                 opt.JIT = false;
        else if (arg == "--no-fastmem")             opt.FastMem = false;
        else if (arg == "--threaded-3d")            opt.Threaded3D = true;
        else if (arg == "--gl")                     opt.OpenGL = true;
        else if (arg == "--state" && hasValue)      opt.StatePath = argv[++i];
        else if (arg == "--input" && hasValue)      opt.InputPath = argv[++i];
        else if (arg == "--json" && hasValue)       opt.JsonPath = argv[++i];
        else if (arg == "--frame-times" && hasValue) opt.FrameTimesPath = argv[++i];
        else if (arg == "--frame-hashes" && hasValue) opt.FrameHashesPath = argv[++i];
        else if (arg == "--audio-dump" && hasValue) opt.AudioDumpPath = argv[++i];
        else if (arg == "--audio-hash")             opt.AudioHash = true;
        else if (arg == "--shot" && hasValue)
        {
            if (!ParseFrameAction(argv[++i], opt.Shots)) return false;
        }
        else if (arg == "--save-state" && hasValue)
        {
            if (!ParseFrameAction(argv[++i], opt.SaveStates)) return false;
        }
        else if (!arg.empty() && arg[0] != '-' && opt.RomPath.empty())
            opt.RomPath = arg;
        else
            return false;
    }

    return !opt.RomPath.empty() && opt.Warmup <= opt.Frames;
}

static bool ReadFile(const std::string& path, std::unique_ptr<u8[]>& data, u32& len)
{
    Platform::FileHandle* file = Platform::OpenFile(path, Platform::FileMode::Read);
    if (!file)
        return false;

    len = (u32)Platform::FileLength(file);
    data = std::make_unique<u8[]>(len);
    bool ok = Platform::FileRead(data.get(), len, 1, file) == 1;
    Platform::CloseFile(file);
    return ok;
}

static bool WriteFile(const std::string& path, const void* data, u32 len)
{
    Platform::FileHandle* file = Platform::OpenFile(path, Platform::FileMode::Write);
    if (!file)
        return false;

    bool ok = Platform::FileWrite(data, len, 1, file) == 1;
    Platform::CloseFile(file);
    return ok;
}

// Input script, one event per line, '#' starts a comment:
//   <frame> keys <A,B,START,...|none>   set the pressed buttons before running that frame
//   <frame> touch <x> <y>               press the touch screen
//   <frame> release                     release the touch screen
static bool LoadInputScript(const std::string& path, std::vector<InputEvent>& events)
{
    std::unique_ptr<u8[]> data;
    u32 len;
    if (!ReadFile(path, data, len))
    {
        fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }

    std::string text((const char*)data.get(), len);
    size_t pos = 0;
    int lineNum = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        lineNum++;

        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        char kind[16] = {}, rest[128] = {};
        unsigned frame;
        int n = sscanf(line.c_str(), "%u %15s %127[^\n]", &frame, kind, rest);
        if (n < 2)
        {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            fprintf(stderr, "%s:%d: bad line\n", path.c_str(), lineNum);
            return false;
        }

        InputEvent ev {};
        ev.Frame = frame;
        if (!strcmp(kind, "keys"))
        {
            ev.Type = InputEvent::Keys;
            std::string names = rest;
            size_t p = 0;
            while (p < names.size())
            {
                size_t comma = names.find(',', p);
                if (comma == std::string::npos) comma = names.size();
                std::string name = names.substr(p, comma - p);
                p = comma + 1;

                while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
                while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                if (name.empty() || name == "none") continue;

                bool found = false;
                for (int k = 0; k < 12; k++)
                {
                    if (name == kKeyNames[k]) { ev.Mask |= 1u << k; found = true; }
                }
                if (!found)
                {
                    fprintf(stderr, "%s:%d: unknown key '%s'\n", path.c_str(), lineNum, name.c_str());
                    return false;
                }
            }
        }
        else if (!strcmp(kind, "touch"))
        {
            unsigned x, y;
            if (sscanf(rest, "%u %u", &x, &y) != 2 || x > 255 || y > 191)
            {
                fprintf(stderr, "%s:%d: touch needs x (0-255) and y (0-191)\n", path.c_str(), lineNum);
                return false;
            }
            ev.Type = InputEvent::Touch;
            ev.X = (u16)x;
            ev.Y = (u16)y;
        }
        else if (!strcmp(kind, "release"))
        {
            ev.Type = InputEvent::Release;
        }
        else
        {
            fprintf(stderr, "%s:%d: unknown event '%s'\n", path.c_str(), lineNum, kind);
            return false;
        }

        events.push_back(ev);
    }

    std::stable_sort(events.begin(), events.end(),
                     [](const InputEvent& a, const InputEvent& b) { return a.Frame < b.Frame; });
    return true;
}

#ifdef BENCH_GL
// OpenGL 4.3 core context without a window (EGL surfaceless), functions loaded through glad
static bool InitGL()
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { fprintf(stderr, "EGL: no display\n"); return false; }
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfgAttrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttrs, &cfg, 1, &n) || n < 1) { fprintf(stderr, "EGL: no config\n"); return false; }
    const EGLint ctxAttrs[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
                                EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "EGL: no GL 4.3 core context\n"); return false; }
    const EGLint pbAttrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttrs);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "EGL: make current failed\n"); return false; }
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress)) { fprintf(stderr, "glad: cannot load GL functions\n"); return false; }
    fprintf(stderr, "GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

// the GL renderer exposes one array texture with both screens (layer 0 top, 1 bottom)
static bool ReadGLScreens(NDS& nds, std::vector<u8>& rgb)
{
    void* top; void* bottom;
    if (nds.GPU.GetFramebuffers(&top, &bottom))
        return false;
    GLuint tex = *(GLuint*)top;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    rgb.resize(256 * 384 * 3);
    std::vector<u8> px(256 * 192 * 4);
    for (int layer = 0; layer < 2; layer++)
    {
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // glReadPixels starts at the bottom row
        for (int y = 0; y < 192; y++)
            for (int x = 0; x < 256; x++)
            {
                const u8* src = &px[((191 - y) * 256 + x) * 4];
                u8* dst = &rgb[((layer * 192 + y) * 256 + x) * 3];
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return true;
}
#endif

// both screens as one 256x384 RGB image, converted from the renderer's BGRA
static bool ScreensToRGB(NDS& nds, std::vector<u8>& rgb)
{
#ifdef BENCH_GL
    if (ReadGLScreens(nds, rgb))
        return true;
#endif
    void* top; void* bottom;
    if (!nds.GPU.GetFramebuffers(&top, &bottom))
        return false;

    rgb.resize(256 * 384 * 3);
    for (int s = 0; s < 2; s++)
    {
        const u32* src = (const u32*)(s == 0 ? top : bottom);
        for (int i = 0; i < 256 * 192; i++)
        {
            u8* dst = &rgb[(s * 256 * 192 + i) * 3];
            dst[0] = (src[i] >> 16) & 0xFF;
            dst[1] = (src[i] >> 8) & 0xFF;
            dst[2] = src[i] & 0xFF;
        }
    }

    return true;
}

static bool WriteScreenshot(NDS& nds, const std::string& path, u64* hash)
{
    std::vector<u8> rgb;
    if (!ScreensToRGB(nds, rgb))
        return false;

    *hash = XXH64(rgb.data(), rgb.size(), 0);

    Platform::FileHandle* file = Platform::OpenFile(path, Platform::FileMode::Write);
    if (!file)
        return false;

    Platform::FileWriteFormatted(file, "P6\n256 384\n255\n");
    Platform::FileWrite(rgb.data(), rgb.size(), 1, file);
    Platform::CloseFile(file);
    return true;
}

static double Percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    size_t idx = (size_t)(p * (values.size() - 1) + 0.5);
    return values[idx];
}

int main(int argc, char** argv)
{
    Options opt;
    if (!ParseArgs(argc, argv, opt))
    {
        Usage();
        return 2;
    }

    std::vector<InputEvent> inputs;
    if (!opt.InputPath.empty() && !LoadInputScript(opt.InputPath, inputs))
        return 2;

    std::unique_ptr<u8[]> romData;
    u32 romLen;
    if (!ReadFile(opt.RomPath, romData, romLen))
    {
        fprintf(stderr, "cannot read %s\n", opt.RomPath.c_str());
        return 1;
    }

    NDSArgs args;
    if (!opt.JIT)
        args.JIT = std::nullopt;
    else if (!opt.FastMem)
        args.JIT->FastMemory = false;

    // the console is big, keep it off the stack
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);

    // no save data: homebrew and retail ROMs both start with an empty cart
    auto cart = NDSCart::ParseROM(std::move(romData), romLen, nullptr, NDSCart::NDSCartArgs {});
    if (!cart)
    {
        fprintf(stderr, "failed to parse %s as a DS ROM\n", opt.RomPath.c_str());
        return 1;
    }

    nds->SetNDSCart(std::move(cart));
    nds->Reset();

#ifdef BENCH_GL
    if (opt.OpenGL)
    {
        if (!InitGL())
            return 1;
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
        RendererSettings settings {};
        settings.ScaleFactor = 1;
        settings.Threaded = false;
        settings.HiresCoordinates = true;
        settings.BetterPolygons = false;
        nds->GetRenderer().SetRenderSettings(settings);
        int cur, cnt;
        while (nds->GetRenderer().NeedsShaderCompile())
            nds->GetRenderer().ShaderCompileStep(cur, cnt);
    }
    else
#endif
    if (opt.Threaded3D)
    {
        RendererSettings settings {};
        settings.ScaleFactor = 1;
        settings.Threaded = true;
        nds->GetRenderer().SetRenderSettings(settings);
    }

    // FreeBIOS and generated firmware cannot boot a firmware menu
    nds->SetupDirectBoot(opt.RomPath);

    // fixed clock so that games reading the RTC behave the same on every run
    nds->RTC.SetDateTime(2024, 1, 1, 12, 0, 0);

    nds->Start();

    if (!opt.StatePath.empty())
    {
        std::unique_ptr<u8[]> stateData;
        u32 stateLen;
        if (!ReadFile(opt.StatePath, stateData, stateLen))
        {
            fprintf(stderr, "cannot read %s\n", opt.StatePath.c_str());
            return 1;
        }

        Savestate state(stateData.get(), stateLen, false);
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
        {
            fprintf(stderr, "failed to load savestate %s\n", opt.StatePath.c_str());
            return 1;
        }
    }

    XXH64_state_t* audioState = XXH64_createState();
    XXH64_reset(audioState, 0);
    u64 audioSamples = 0;
    int audioPeak = 0;
    std::vector<s16> audioBuf(8192 * 2);
    std::vector<u8> audioDump;

    std::vector<double> frameMs;
    frameMs.reserve(opt.Frames);
    std::string frameTimes = "frame,ms\n";
    std::string frameHashes = "frame,screens,audio\n";
    std::vector<u8> hashRgb;

    struct ShotResult { u32 Frame; std::string File; u64 Hash; };
    std::vector<ShotResult> shotResults;

    size_t nextInput = 0;
    u32 keyMask = 0;
    using Clock = std::chrono::steady_clock;
    double timedSeconds = 0;

    for (u32 frame = 0; frame < opt.Frames; frame++)
    {
        while (nextInput < inputs.size() && inputs[nextInput].Frame <= frame)
        {
            const InputEvent& ev = inputs[nextInput++];
            if (ev.Type == InputEvent::Keys)
            {
                keyMask = ev.Mask;
                nds->SetKeyMask(~keyMask & 0xFFF);
            }
            else if (ev.Type == InputEvent::Touch)
                nds->TouchScreen(ev.X, ev.Y);
            else
                nds->ReleaseScreen();
        }

        auto t0 = Clock::now();
        nds->RunFrame();
        auto t1 = Clock::now();

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (!opt.FrameTimesPath.empty())
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u,%.4f\n", frame, ms);
            frameTimes += buf;
        }

        if (frame >= opt.Warmup)
        {
            frameMs.push_back(ms);
            timedSeconds += ms / 1000.0;
        }

        // the SPU output buffer must be drained every frame
        for (;;)
        {
            int avail = nds->SPU.GetOutputSize();
            if (avail <= 0) break;
            int n = std::min(avail, (int)(audioBuf.size() / 2));
            n = nds->SPU.ReadOutput(audioBuf.data(), n);
            if (n <= 0) break;

            XXH64_update(audioState, audioBuf.data(), n * 2 * sizeof(s16));
            if (!opt.AudioDumpPath.empty())
                audioDump.insert(audioDump.end(), (u8*)audioBuf.data(), (u8*)(audioBuf.data() + n * 2));
            audioSamples += n;
            for (int i = 0; i < n * 2; i++)
                audioPeak = std::max(audioPeak, abs((int)audioBuf[i]));
        }

        if (!opt.FrameHashesPath.empty() && ScreensToRGB(*nds, hashRgb))
        {
            char buf[80];
            snprintf(buf, sizeof(buf), "%u,%016llx,%016llx\n", frame, (unsigned long long)XXH64(hashRgb.data(), hashRgb.size(), 0),
                     (unsigned long long)XXH64_digest(audioState));
            frameHashes += buf;
        }

        u32 done = frame + 1;

        for (const FrameAction& shot : opt.Shots)
        {
            if (shot.Frame != done) continue;

            u64 hash = 0;
            if (!WriteScreenshot(*nds, shot.File, &hash))
            {
                fprintf(stderr, "cannot write screenshot %s\n", shot.File.c_str());
                return 1;
            }
            shotResults.push_back({done, shot.File, hash});
        }

        for (const FrameAction& save : opt.SaveStates)
        {
            if (save.Frame != done) continue;

            Savestate state;
            if (!nds->DoSavestate(&state) || state.Error ||
                !WriteFile(save.File, state.Buffer(), state.Length()))
            {
                fprintf(stderr, "cannot write savestate %s\n", save.File.c_str());
                return 1;
            }
        }

        if (Platform::StopRequested)
        {
            fprintf(stderr, "the console stopped after %u frames\n", done);
            break;
        }
    }

    if (!opt.FrameTimesPath.empty() && !WriteFile(opt.FrameTimesPath, frameTimes.data(), (u32)frameTimes.size()))
    {
        fprintf(stderr, "cannot write %s\n", opt.FrameTimesPath.c_str());
        return 1;
    }

    if (!opt.FrameHashesPath.empty() && !WriteFile(opt.FrameHashesPath, frameHashes.data(), (u32)frameHashes.size()))
    {
        fprintf(stderr, "cannot write %s\n", opt.FrameHashesPath.c_str());
        return 1;
    }

    if (!opt.AudioDumpPath.empty() && !WriteFile(opt.AudioDumpPath, audioDump.data(), (u32)audioDump.size()))
    {
        fprintf(stderr, "cannot write %s\n", opt.AudioDumpPath.c_str());
        return 1;
    }

    u64 audioHash = XXH64_digest(audioState);
    XXH64_freeState(audioState);

    double mean = frameMs.empty() ? 0 : timedSeconds * 1000.0 / frameMs.size();
    double fps = timedSeconds > 0 ? frameMs.size() / timedSeconds : 0;

    std::string report;
    char line[512];
    auto add = [&](const char* fmt, auto... a)
    {
        snprintf(line, sizeof(line), fmt, a...);
        report += line;
    };

    add("{\n");
    add("  \"rom\": \"%s\",\n", opt.RomPath.c_str());
    add("  \"jit\": %s,\n", opt.JIT ? "true" : "false");
    add("  \"frames\": %u,\n", (u32)(frameMs.size() + opt.Warmup));
    add("  \"timed_frames\": %u,\n", (u32)frameMs.size());
    add("  \"seconds\": %.6f,\n", timedSeconds);
    add("  \"fps\": %.2f,\n", fps);
    add("  \"ms_per_frame\": {\"mean\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f},\n",
        mean, Percentile(frameMs, 0.5), Percentile(frameMs, 0.95), Percentile(frameMs, 1.0));
    if (opt.AudioHash)
        add("  \"audio\": {\"samples\": %llu, \"peak\": %d, \"hash\": \"%016llx\"},\n",
            (unsigned long long)audioSamples, audioPeak, (unsigned long long)audioHash);
    add("  \"shots\": [");
    for (size_t i = 0; i < shotResults.size(); i++)
        add("%s{\"frame\": %u, \"file\": \"%s\", \"hash\": \"%016llx\"}", i ? ", " : "",
            shotResults[i].Frame, shotResults[i].File.c_str(), (unsigned long long)shotResults[i].Hash);
    add("]\n}\n");

    if (opt.JsonPath.empty())
        fputs(report.c_str(), stdout);
    else if (!WriteFile(opt.JsonPath, report.data(), (u32)report.size()))
    {
        fprintf(stderr, "cannot write %s\n", opt.JsonPath.c_str());
        return 1;
    }

    return 0;
}
