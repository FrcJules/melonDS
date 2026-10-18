# melonDS-bench

Headless runner for reproducible measurements: no window, no audio device, software renderer only.
It links the emulation core and does not touch it.

    cmake -B build -DBUILD_BENCH=ON
    cmake --build build --target melonDS-bench

To build only the tool, without the Qt frontend: `-DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF`
(the OpenGL renderer needs the frontend's glad headers).

    melonDS-bench game.nds --frames 600 --warmup 60 --audio-hash --shot 300:frame300.ppm

The console boots directly (FreeBIOS and generated firmware, no dumps needed), with a fixed RTC
(2024-01-01 12:00:00), no save data, and no network. Frames are counted from 0; "after N frames"
means after N calls to `RunFrame`.

| Option | Effect |
|---|---|
| `--frames N` | number of frames to run (default 600) |
| `--warmup N` | the first N frames are run but not timed |
| `--no-jit` | interpreter instead of the JIT |
| `--no-fastmem` | JIT without the fast memory mapping (required under valgrind and qemu-user) |
| `--state FILE` | load a savestate before running |
| `--save-state N:FILE` | write a savestate after N frames |
| `--shot N:FILE` | write both screens (256x384, top then bottom) as PPM after N frames |
| `--input FILE` | input script, see below |
| `--audio-hash` | report sample count, peak and an xxh64 of the whole audio stream |
| `--frame-times FILE` | write the time of every frame as `frame,ms` CSV (to find stutter) |
| `--frame-hashes FILE` | write `frame,screens,audio-so-far` hashes for every frame (find the first diverging frame between two builds) |
| `--audio-dump FILE` | write the raw audio (stereo s16 LE) to compare waveforms |
| `--json FILE` | write the JSON report to FILE instead of stdout |

The report contains the frame rate, the mean, median, 95th percentile and maximum time per frame,
and the hash of each screenshot (xxh64 of the RGB pixels).

## Input scripts

    # frame  event
    0    keys none
    300  touch 128 150      # x 0-255, y 0-191
    304  release
    500  keys A,START       # A B SELECT START RIGHT LEFT UP DOWN R L X Y

An event applies before the given frame runs. `keys` replaces the whole set of pressed buttons.

## Comparing against references: `suite.py`

`suite.py` runs a list of cases and compares screenshots and audio hashes with references that
**you** validated (`--bless` stores the current results as references). Keep ROMs, savestates and
references out of the repository. Speed is printed for information only.

## Things to know
- Results are bit-exact between runs of the same build, and a savestate resumed in a new process
  gives the same frames as a continuous run.
- The JIT and the interpreter do **not** give identical output (timings differ), so keep separate
  references per mode. `suite.py` runs the interpreter unless a case sets `"jit": true`.
- Only the software renderer is supported. OpenGL output depends on the driver and is not covered.
- Homebrew that expects DLDI storage has no SD card here.

## AArch64 (the Switch CPU architecture) without hardware
The core has no external dependency, so the tool cross-compiles and runs under `qemu-user`
(correctness only, the speed is meaningless):

    sudo apt install g++-aarch64-linux-gnu qemu-user
    cmake -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=aarch64.cmake \
          -DBUILD_BENCH=ON -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_GDBSTUB=OFF \
          -DCMAKE_EXE_LINKER_FLAGS=-static
    cmake --build build-arm64 --target melonDS-bench
    qemu-aarch64 build-arm64/melonDS-bench game.nds --frames 300 --no-fastmem --shot 300:a64.ppm

with a toolchain file setting `CMAKE_SYSTEM_NAME Linux`, `CMAKE_SYSTEM_PROCESSOR aarch64` and the
`aarch64-linux-gnu-gcc/g++` compilers. This exercises the ARM64 JIT backend.
