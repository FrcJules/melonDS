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

#include <math.h>
#include <string.h>

#include "ShaderChain.h"

#include "OpenGLSupport.h"
#include "Platform.h"
#include "frontend/qt_sdl/toml/toml.hpp"

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

// the shaders only use features common to GLSL 1.50 and GLSL ES 3.00
#ifdef MELONDS_GLES
static const char* const kVersionHeader =
    "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler2DArray;\n";
#else
static const char* const kVersionHeader = "#version 150\n";
#endif

// optional replacement for the file system (the WebAssembly build has none)
static ShaderChain::FileReader sFileReader;

static const char* const kCopyVS = R"(
in vec2 vPosition;
out vec2 fTexcoord;

void main()
{
    fTexcoord = vPosition;
    gl_Position = vec4(vPosition * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char* const kCopyFS = R"(
uniform sampler2DArray ScreenTex;
uniform float uLayer;

in vec2 fTexcoord;
out vec4 oColor;

void main()
{
    oColor = vec4(texture(ScreenTex, vec3(fTexcoord, uLayer)).rgb, 1.0);
}
)";

// a quad covering the unit square: x, y, u, v
static const float kQuad[] =
{
    0.f, 0.f,  0.f, 0.f,
    0.f, 1.f,  0.f, 1.f,
    1.f, 1.f,  1.f, 1.f,
    0.f, 0.f,  0.f, 0.f,
    1.f, 1.f,  1.f, 1.f,
    1.f, 0.f,  1.f, 0.f
};

static const int kMaxTargetSize = 16384;


static bool ReadTextFile(const std::string& path, std::string& out)
{
    if (sFileReader)
        return sFileReader(path, out);

    Platform::FileHandle* file = Platform::OpenFile(path, Platform::FileMode::Read);
    if (!file)
        return false;

    u64 len = Platform::FileLength(file);
    if (len == 0 || len > 1024*1024)
    {
        Platform::CloseFile(file);
        return false;
    }

    out.resize(len);
    u64 read = Platform::FileRead(out.data(), 1, len, file);
    Platform::CloseFile(file);

    return read == len;
}

static std::string DirectoryOf(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
        return "";
    return path.substr(0, pos + 1);
}


void ShaderChain::SetFileReader(FileReader reader)
{
    sFileReader = std::move(reader);
}

ShaderChain::~ShaderChain()
{
    Unload();
}

void ShaderChain::Unload()
{
    for (Pass& pass : Passes)
    {
        if (pass.Program) glDeleteProgram(pass.Program);
    }
    Passes.clear();

    for (Slot& slot : Slots)
    {
        for (Target& target : slot.Targets)
            FreeTarget(target);
    }
    Slots.clear();

    if (CopyProgram) glDeleteProgram(CopyProgram);
    CopyProgram = 0;

    if (VertexArray) glDeleteVertexArrays(1, &VertexArray);
    if (VertexBuffer) glDeleteBuffers(1, &VertexBuffer);
    VertexArray = 0;
    VertexBuffer = 0;

    Loaded = false;
    Name.clear();
}

bool ShaderChain::CompileCopyProgram()
{
    std::string vs = std::string(kVersionHeader) + kCopyVS;
    std::string fs = std::string(kVersionHeader) + kCopyFS;

    if (!OpenGL::CompileVertexFragmentProgram(CopyProgram, vs, fs, "ShaderChainCopy",
                                              {{"vPosition", 0}}, {{"oColor", 0}}))
    {
        CopyProgram = 0;
        return false;
    }

    CopyLayerLoc = glGetUniformLocation(CopyProgram, "uLayer");
    return true;
}

bool ShaderChain::Load(const std::string& presetName)
{
    Unload();
    Error.clear();

    std::string presetPath = Platform::GetLocalFilePath("shaders/" + presetName);
    std::string baseDir = DirectoryOf(presetPath);

    std::string content;
    if (!ReadTextFile(presetPath, content))
    {
        Error = "cannot read preset " + presetName;
        Log(LogLevel::Error, "ShaderChain: %s (%s)\n", Error.c_str(), presetPath.c_str());
        return false;
    }

    struct PassDesc
    {
        std::string Shader;
        bool ViewportScale;
        int Scale;
        bool Linear;
    };
    std::vector<PassDesc> descs;
    std::string name;

    try
    {
        toml::value data = toml::parse_str(content);

        name = toml::find_or<std::string>(data, "name", presetName);

        if (!data.contains("pass") || !data.at("pass").is_array())
            throw std::runtime_error("no [[pass]] entry");

        for (const toml::value& p : data.at("pass").as_array())
        {
            PassDesc desc;
            desc.Shader = toml::find_or<std::string>(p, "shader", "");
            desc.ViewportScale = toml::find_or<std::string>(p, "scale_type", "source") == "viewport";
            desc.Scale = toml::find_or<int>(p, "scale", 1);
            desc.Linear = toml::find_or<std::string>(p, "filter", "nearest") == "linear";

            if (desc.Shader.empty())
                throw std::runtime_error("pass without shader");
            if (desc.Scale < 1 || desc.Scale > 16)
                throw std::runtime_error("pass scale must be between 1 and 16");

            descs.push_back(desc);
        }

        if (descs.empty() || descs.size() > kMaxPasses)
            throw std::runtime_error("a preset needs 1 to " + std::to_string(kMaxPasses) + " passes");
    }
    catch (const std::exception& e)
    {
        Error = "invalid preset " + presetName;
        Log(LogLevel::Error, "ShaderChain: %s: %s\n", Error.c_str(), e.what());
        return false;
    }

    if (!CompileCopyProgram())
    {
        Error = "cannot compile internal shader";
        Unload();
        return false;
    }

    for (const PassDesc& desc : descs)
    {
        std::string source;
        if (!ReadTextFile(baseDir + desc.Shader, source))
        {
            Error = "cannot read shader " + desc.Shader;
            Log(LogLevel::Error, "ShaderChain: %s\n", Error.c_str());
            Unload();
            return false;
        }

        std::string vs = std::string(kVersionHeader) + "#define VERTEX\n" + source;
        std::string fs = std::string(kVersionHeader) + "#define FRAGMENT\n" + source;

        Pass pass;
        if (!OpenGL::CompileVertexFragmentProgram(pass.Program, vs, fs, desc.Shader,
                                                  {{"VertexCoord", 0}, {"TexCoord", 1}},
                                                  {{"FragColor", 0}}))
        {
            Error = "cannot compile " + desc.Shader;
            Unload();
            return false;
        }

        pass.MVPLoc = glGetUniformLocation(pass.Program, "MVPMatrix");
        pass.TextureLoc = glGetUniformLocation(pass.Program, "Texture");
        pass.TextureSizeLoc = glGetUniformLocation(pass.Program, "TextureSize");
        pass.InputSizeLoc = glGetUniformLocation(pass.Program, "InputSize");
        pass.OutputSizeLoc = glGetUniformLocation(pass.Program, "OutputSize");
        pass.FrameCountLoc = glGetUniformLocation(pass.Program, "FrameCount");
        pass.FrameDirectionLoc = glGetUniformLocation(pass.Program, "FrameDirection");
        pass.ViewportScale = desc.ViewportScale;
        pass.Scale = desc.Scale;
        pass.Linear = desc.Linear;

        Passes.push_back(pass);
    }

    glGenBuffers(1, &VertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);

    glGenVertexArrays(1, &VertexArray);
    glBindVertexArray(VertexArray);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(0));
    glEnableVertexAttribArray(1); // texcoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(2*4));

    Name = name;
    Loaded = true;
    return true;
}

void ShaderChain::FreeTarget(Target& target)
{
    if (target.FBO) glDeleteFramebuffers(1, &target.FBO);
    if (target.Tex) glDeleteTextures(1, &target.Tex);
    target = Target();
}

bool ShaderChain::EnsureTarget(Target& target, int w, int h)
{
    if (target.Tex && target.W == w && target.H == h)
        return true;

    if (w < 1 || h < 1 || w > kMaxTargetSize || h > kMaxTargetSize)
        return false;

    FreeTarget(target);

    glGenTextures(1, &target.Tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target.Tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &target.FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, target.FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.Tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        Log(LogLevel::Error, "ShaderChain: framebuffer incomplete (%dx%d)\n", w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        FreeTarget(target);
        return false;
    }

    target.W = w;
    target.H = h;
    return true;
}

bool ShaderChain::Draw(GLuint srcTex, int srcW, int srcH, int layer, int slotIndex, const float* m,
                       float logicalW, float logicalH, float scale, u32 frameCount)
{
    if (!Loaded || slotIndex < 0)
        return false;

    int numPasses = (int)Passes.size();

    // size of this screen on the surface, in pixels
    float sx = hypotf(m[0], m[1]);
    float sy = hypotf(m[2], m[3]);
    int outW = (int)roundf(256.f * sx * scale);
    int outH = (int)roundf(192.f * sy * scale);
    int surfaceW = (int)roundf(logicalW * scale);
    int surfaceH = (int)roundf(logicalH * scale);

    if ((int)Slots.size() <= slotIndex)
        Slots.resize(slotIndex + 1);
    Slot& slot = Slots[slotIndex];
    slot.Targets.resize(numPasses);

    // targets[0] holds the source layer, targets[i+1] the output of pass i (except the last one)
    if (!EnsureTarget(slot.Targets[0], srcW, srcH))
        return false;

    int w = srcW, h = srcH;
    for (int i = 0; i < numPasses - 1; i++)
    {
        const Pass& pass = Passes[i];
        if (pass.ViewportScale) { w = outW; h = outH; }
        else                    { w *= pass.Scale; h *= pass.Scale; }

        if (!EnsureTarget(slot.Targets[i + 1], w, h))
            return false;
    }

    glBindBuffer(GL_ARRAY_BUFFER, VertexBuffer);
    glBindVertexArray(VertexArray);

    // pass 0: copy the layer of the source array texture to a plain 2D texture
    glBindFramebuffer(GL_FRAMEBUFFER, slot.Targets[0].FBO);
    glViewport(0, 0, srcW, srcH);
    glUseProgram(CopyProgram);
    glUniform1f(CopyLayerLoc, (float)layer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, srcTex);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    for (int i = 0; i < numPasses; i++)
    {
        const Pass& pass = Passes[i];
        const Target& in = slot.Targets[i];
        bool last = (i == numPasses - 1);

        float mvp[16] = {};
        mvp[10] = 1.f;
        mvp[15] = 1.f;

        float outSizeW, outSizeH;
        if (last)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, surfaceW, surfaceH);

            // unit quad -> screen transform -> clip space (y is flipped like in the regular path)
            mvp[0] = 256.f * m[0] * 2.f / logicalW;
            mvp[1] = -256.f * m[1] * 2.f / logicalH;
            mvp[4] = 192.f * m[2] * 2.f / logicalW;
            mvp[5] = -192.f * m[3] * 2.f / logicalH;
            mvp[12] = m[4] * 2.f / logicalW - 1.f;
            mvp[13] = 1.f - m[5] * 2.f / logicalH;

            outSizeW = (float)outW;
            outSizeH = (float)outH;
        }
        else
        {
            const Target& out = slot.Targets[i + 1];
            glBindFramebuffer(GL_FRAMEBUFFER, out.FBO);
            glViewport(0, 0, out.W, out.H);

            // unit quad -> whole target
            mvp[0] = 2.f;
            mvp[5] = 2.f;
            mvp[12] = -1.f;
            mvp[13] = -1.f;

            outSizeW = (float)out.W;
            outSizeH = (float)out.H;
        }

        glUseProgram(pass.Program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.Tex);
        GLint filter = pass.Linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

        if (pass.TextureLoc >= 0)       glUniform1i(pass.TextureLoc, 0);
        if (pass.MVPLoc >= 0)           glUniformMatrix4fv(pass.MVPLoc, 1, GL_FALSE, mvp);
        if (pass.TextureSizeLoc >= 0)   glUniform2f(pass.TextureSizeLoc, (float)in.W, (float)in.H);
        if (pass.InputSizeLoc >= 0)     glUniform2f(pass.InputSizeLoc, (float)in.W, (float)in.H);
        if (pass.OutputSizeLoc >= 0)    glUniform2f(pass.OutputSizeLoc, outSizeW, outSizeH);
        if (pass.FrameCountLoc >= 0)    glUniform1i(pass.FrameCountLoc, (GLint)frameCount);
        if (pass.FrameDirectionLoc >= 0) glUniform1i(pass.FrameDirectionLoc, 1);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

}
