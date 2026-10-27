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

// Minimal implementation of the Platform interface for the Nintendo Switch
// (SDL2 homebrew frontend): plain stdio files, std threads, cartridge saves
// written back to a file, and stubs for everything that is not supported yet
// (microphone, camera, wifi, add-ons). Derived from the melonDS-bench platform.

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <switch.h>

#include "Platform.h"

std::string SwitchSavePath;
std::string SwitchLogPath;

namespace melonDS::Platform
{

std::atomic<bool> StopRequested {false};

void SignalStop(StopReason reason, void* userdata)
{
    StopRequested = true;
}


struct FileHandle
{
    FILE* File;
};

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    bool read = mode & FileMode::Read;
    bool write = mode & FileMode::Write;
    bool preserve = mode & FileMode::Preserve;
    bool noCreate = mode & FileMode::NoCreate;

    FILE* file = nullptr;
    if (mode & FileMode::Append)
    {
        file = fopen(path.c_str(), read ? "a+b" : "ab");
    }
    else if (write && preserve)
    {
        // open as-is, creating the file unless told not to
        file = fopen(path.c_str(), "r+b");
        if (!file && !noCreate)
            file = fopen(path.c_str(), read ? "w+b" : "wb");
    }
    else if (write)
    {
        file = fopen(path.c_str(), read ? "w+b" : "wb");
    }
    else if (read)
    {
        file = fopen(path.c_str(), "rb");
    }

    if (!file)
        return nullptr;

    return new FileHandle {file};
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string& name)
{
    FILE* file = fopen(name.c_str(), "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

bool LocalFileExists(const std::string& name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string& filepath)
{
    FILE* file = fopen(filepath.c_str(), "ab");
    if (!file) return false;
    fclose(file);
    return true;
}

bool CheckLocalFileWritable(const std::string& filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle* file)
{
    bool ok = fclose(file->File) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle* file)
{
    return feof(file->File) != 0;
}

bool FileReadLine(char* str, int count, FileHandle* file)
{
    return fgets(str, count, file->File) != nullptr;
}

u64 FilePosition(FileHandle* file)
{
    return (u64)ftell(file->File);
}

bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    int whence = SEEK_SET;
    if (origin == FileSeekOrigin::Current) whence = SEEK_CUR;
    else if (origin == FileSeekOrigin::End)   whence = SEEK_END;

    return fseek(file->File, (long)offset, whence) == 0;
}

void FileRewind(FileHandle* file)
{
    rewind(file->File);
}

u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    return fread(data, size, count, file->File);
}

bool FileFlush(FileHandle* file)
{
    return fflush(file->File) == 0;
}

u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    return fwrite(data, size, count, file->File);
}

u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vfprintf(file->File, fmt, args);
    va_end(args);
    return ret < 0 ? 0 : (u64)ret;
}

u64 FileLength(FileHandle* file)
{
    long pos = ftell(file->File);
    fseek(file->File, 0, SEEK_END);
    long len = ftell(file->File);
    fseek(file->File, pos, SEEK_SET);
    return (u64)len;
}


void Log(LogLevel level, const char* fmt, ...)
{
    // only warnings and errors, they go to the log file (stderr is not visible on the console)
    if (level < LogLevel::Warn || SwitchLogPath.empty())
        return;

    FILE* file = fopen(SwitchLogPath.c_str(), "a");
    if (!file)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fclose(file);
}


struct Thread
{
    std::thread Handle;
};

// core the emulation helper threads (threaded 3D renderer) are pinned to; -1 = let the scheduler decide
int SwitchHelperCore = -1;

Thread* Thread_Create(std::function<void()> func)
{
    return new Thread {std::thread([func = std::move(func)]() {
        if (SwitchHelperCore >= 0)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, SwitchHelperCore, 1u << SwitchHelperCore);
        func();
    })};
}

void Thread_Free(Thread* thread)
{
    if (thread->Handle.joinable())
        thread->Handle.join();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread->Handle.joinable())
        thread->Handle.join();
}


struct Semaphore
{
    std::mutex Lock;
    std::condition_variable Cond;
    int Count = 0;
};

Semaphore* Semaphore_Create()
{
    return new Semaphore();
}

void Semaphore_Free(Semaphore* sema)
{
    delete sema;
}

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> guard(sema->Lock);
    sema->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> guard(sema->Lock);
    sema->Cond.wait(guard, [sema] { return sema->Count > 0; });
    sema->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> guard(sema->Lock);
    if (!sema->Cond.wait_for(guard, std::chrono::milliseconds(timeout_ms), [sema] { return sema->Count > 0; }))
        return false;

    sema->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    {
        std::lock_guard<std::mutex> guard(sema->Lock);
        sema->Count += count;
    }
    sema->Cond.notify_all();
}


struct Mutex
{
    std::mutex Lock;
};

Mutex* Mutex_Create()
{
    return new Mutex();
}

void Mutex_Free(Mutex* mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex* mutex)
{
    mutex->Lock.lock();
}

void Mutex_Unlock(Mutex* mutex)
{
    mutex->Lock.unlock();
}

bool Mutex_TryLock(Mutex* mutex)
{
    return mutex->Lock.try_lock();
}


void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

u64 GetUSCount()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}


// the cartridge save is written back to the file given in the config
void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (SwitchSavePath.empty() || writeoffset + writelen > savelen)
        return;

    FILE* file = fopen(SwitchSavePath.c_str(), "r+b");
    if (!file)
        file = fopen(SwitchSavePath.c_str(), "wb");
    if (!file)
        return;

    fseek(file, (long)writeoffset, SEEK_SET);
    fwrite(savedata + writeoffset, 1, writelen, file);
    fclose(file);
}
void WriteGBASave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata) {}
void WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void* userdata) {}
void WriteDateTime(int year, int month, int day, int hour, int minute, int second, void* userdata) {}


// no multiplayer, no network
void MP_Begin(void* userdata) {}
void MP_End(void* userdata) {}
int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_RecvPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata) { return 0; }
int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata) { return 0; }
int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata) { return 0; }
u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata) { return 0; }

int Net_SendPacket(u8* data, int len, void* userdata) { return 0; }
int Net_RecvPacket(u8* data, void* userdata) { return 0; }


// no camera
void Camera_Start(int num, void* userdata) {}
void Camera_Stop(int num, void* userdata) {}
void Camera_CaptureFrame(int num, u32* frame, int width, int height, bool yuv, void* userdata) {}

// silent microphone
void Mic_Start(void* userdata) {}
void Mic_Stop(void* userdata) {}
int Mic_ReadInput(s16* data, int maxlength, void* userdata)
{
    memset(data, 0, maxlength * sizeof(s16));
    return maxlength;
}

// no AAC decoding (DSi sound files)
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder* dec) {}
bool AAC_Configure(AACDecoder* dec, int frequency, int channels) { return false; }
bool AAC_DecodeFrame(AACDecoder* dec, const void* input, int inputlen, void* output, int outputlen) { return false; }

// no add-ons
bool Addon_KeyDown(KeyType type, void* userdata) { return false; }
void Addon_RumbleStart(u32 len, void* userdata) {}
void Addon_RumbleStop(void* userdata) {}
float Addon_MotionQuery(MotionQueryType type, void* userdata) { return 0.f; }

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary* lib) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return nullptr; }

}
