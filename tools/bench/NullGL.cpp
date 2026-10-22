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

// See NullGL.h. Only the entry points used by the OpenGL renderer (GLES build) are here:
// a missing one shows up as a link error.

#include <string.h>
#include <type_traits>

#include <GLES3/gl3.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2ext.h>

#include "NullGL.h"
#include "xxhash/xxhash.h"

#define NULLGL_FUNCS(X) \
    X(glActiveTexture) X(glAttachShader) X(glBindAttribLocation) X(glBindBuffer) X(glBindBufferBase) \
    X(glBindFramebuffer) X(glBindTexture) X(glBindVertexArray) X(glBlendColor) X(glBlendEquationSeparate) \
    X(glBlendFuncSeparate) X(glBlitFramebuffer) X(glBufferData) X(glBufferSubData) X(glCheckFramebufferStatus) \
    X(glClear) X(glClearColor) X(glClearDepthf) X(glColorMask) X(glColorMaskiOES) X(glCompileShader) \
    X(glCreateProgram) X(glCreateShader) X(glDeleteBuffers) X(glDeleteFramebuffers) X(glDeleteProgram) \
    X(glDeleteShader) X(glDeleteTextures) X(glDeleteVertexArrays) X(glDepthFunc) X(glDepthMask) \
    X(glDepthRangef) X(glDetachShader) X(glDisable) X(glDrawArrays) X(glDrawBuffers) X(glDrawElements) \
    X(glEnable) X(glEnableVertexAttribArray) X(glFramebufferTexture2D) X(glFramebufferTextureLayer) \
    X(glGenBuffers) X(glGenFramebuffers) X(glGenTextures) X(glGenVertexArrays) X(glGetError) \
    X(glGetProgramInfoLog) X(glGetProgramiv) X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetUniformBlockIndex) \
    X(glGetUniformLocation) X(glLineWidth) X(glLinkProgram) X(glReadBuffer) X(glReadPixels) X(glScissor) \
    X(glShaderSource) X(glStencilFunc) X(glStencilMask) X(glStencilOp) X(glTexImage2D) X(glTexImage3D) \
    X(glTexParameteri) X(glTexStorage3D) X(glTexSubImage2D) X(glTexSubImage3D) X(glUniform1i) X(glUniform1ui) \
    X(glUniform2f) X(glUniform4ui) X(glUniformBlockBinding) X(glUseProgram) X(glVertexAttribIPointer) \
    X(glVertexAttribPointer) X(glViewport)

namespace
{
enum Func
{
#define X(name) F_##name,
    NULLGL_FUNCS(X)
#undef X
    F_Count
};

const char* const FuncNames[] =
{
#define X(name) #name,
    NULLGL_FUNCS(X)
#undef X
};

uint64_t Counts[F_Count];
uint64_t UploadBytes = 0;
uint64_t ReadPixelsCalls = 0;
XXH64_state_t* Hash = nullptr;
GLuint NextName = 1;
bool HashOn = false;

void Mix(const void* data, size_t len)
{
    if (!HashOn) return;
    if (!Hash)
    {
        Hash = XXH64_createState();
        XXH64_reset(Hash, 0);
    }
    XXH64_update(Hash, data, len);
}

template <typename T> uint64_t ToU64(T v)
{
    if constexpr (std::is_floating_point_v<T>) { uint32_t u; float f = (float)v; memcpy(&u, &f, 4); return u; }
    else return (uint64_t)v;
}

// counts the call and hashes its scalar arguments
template <typename... T>
void Call(Func f, T... args)
{
    Counts[f]++;
    uint64_t v[1 + sizeof...(T)] = {(uint64_t)f};
    [[maybe_unused]] int i = 1;
    ((v[i++] = ToU64(args)), ...);
    Mix(v, sizeof(v));
}

void Data(const void* p, size_t len)
{
    if (!p || !len) return;
    UploadBytes += len;
    Mix(p, len);
}

size_t PixelSize(GLenum format, GLenum type)
{
    switch (type)
    {
    case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_6_5:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    }

    size_t comp = 4;
    switch (format)
    {
    case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT: comp = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_DEPTH_STENCIL: comp = 2; break;
    case GL_RGB: case GL_RGB_INTEGER: comp = 3; break;
    }

    size_t size = 1;
    switch (type)
    {
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: size = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: size = 4; break;
    }
    return comp * size;
}

void GenNames(GLsizei n, GLuint* out)
{
    for (GLsizei i = 0; i < n; i++)
        out[i] = NextName++;
}
}

namespace NullGL
{
int GetCounters(const char** names, uint64_t* counts, int max)
{
    int n = 0;
    for (int i = 0; i < F_Count && n < max; i++)
    {
        names[n] = FuncNames[i];
        counts[n] = Counts[i];
        n++;
    }
    return n;
}

Stats GetStats()
{
    Stats s {};
    for (int i = 0; i < F_Count; i++)
        s.Calls += Counts[i];
    s.UploadBytes = UploadBytes;
    s.ReadPixels = ReadPixelsCalls;
    return s;
}

void ResetCounters()
{
    memset(Counts, 0, sizeof(Counts));
    UploadBytes = 0;
    ReadPixelsCalls = 0;
}

uint64_t StreamHash()
{
    if (!Hash) return 0;
    return XXH64_digest(Hash);
}

void EnableHash(bool on) { HashOn = on; }

void ResetHash()
{
    if (!Hash) Hash = XXH64_createState();
    XXH64_reset(Hash, 0);
}
}

extern "C" {

void glActiveTexture(GLenum t) { Call(F_glActiveTexture, t); }
void glAttachShader(GLuint p, GLuint s) { Call(F_glAttachShader, p, s); }
void glBindAttribLocation(GLuint p, GLuint i, const GLchar* name) { Call(F_glBindAttribLocation, p, i); Mix(name, strlen(name)); }
void glBindBuffer(GLenum t, GLuint b) { Call(F_glBindBuffer, t, b); }
void glBindBufferBase(GLenum t, GLuint i, GLuint b) { Call(F_glBindBufferBase, t, i, b); }
void glBindFramebuffer(GLenum t, GLuint f) { Call(F_glBindFramebuffer, t, f); }
void glBindTexture(GLenum t, GLuint tex) { Call(F_glBindTexture, t, tex); }
void glBindVertexArray(GLuint a) { Call(F_glBindVertexArray, a); }
void glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { Call(F_glBlendColor, r, g, b, a); }
void glBlendEquationSeparate(GLenum a, GLenum b) { Call(F_glBlendEquationSeparate, a, b); }
void glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d) { Call(F_glBlendFuncSeparate, a, b, c, d); }
void glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{ Call(F_glBlitFramebuffer, a, b, c, d, e, f, g, h, m, fl); }
void glBufferData(GLenum t, GLsizeiptr size, const void* data, GLenum usage) { Call(F_glBufferData, t, size, usage); Data(data, size); }
void glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void* data) { Call(F_glBufferSubData, t, off, size); Data(data, size); }
GLenum glCheckFramebufferStatus(GLenum t) { Call(F_glCheckFramebufferStatus, t); return GL_FRAMEBUFFER_COMPLETE; }
void glClear(GLbitfield m) { Call(F_glClear, m); }
void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { Call(F_glClearColor, r, g, b, a); }
void glClearDepthf(GLfloat d) { Call(F_glClearDepthf, d); }
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { Call(F_glColorMask, r, g, b, a); }
void glColorMaskiOES(GLuint i, GLboolean r, GLboolean g, GLboolean b, GLboolean a) { Call(F_glColorMaskiOES, i, r, g, b, a); }
void glCompileShader(GLuint s) { Call(F_glCompileShader, s); }
GLuint glCreateProgram() { Call(F_glCreateProgram); return NextName++; }
GLuint glCreateShader(GLenum t) { Call(F_glCreateShader, t); return NextName++; }
void glDeleteBuffers(GLsizei n, const GLuint* b) { Call(F_glDeleteBuffers, n); }
void glDeleteFramebuffers(GLsizei n, const GLuint* f) { Call(F_glDeleteFramebuffers, n); }
void glDeleteProgram(GLuint p) { Call(F_glDeleteProgram, p); }
void glDeleteShader(GLuint s) { Call(F_glDeleteShader, s); }
void glDeleteTextures(GLsizei n, const GLuint* t) { Call(F_glDeleteTextures, n); }
void glDeleteVertexArrays(GLsizei n, const GLuint* a) { Call(F_glDeleteVertexArrays, n); }
void glDepthFunc(GLenum f) { Call(F_glDepthFunc, f); }
void glDepthMask(GLboolean m) { Call(F_glDepthMask, m); }
void glDepthRangef(GLfloat n, GLfloat f) { Call(F_glDepthRangef, n, f); }
void glDetachShader(GLuint p, GLuint s) { Call(F_glDetachShader, p, s); }
void glDisable(GLenum c) { Call(F_glDisable, c); }
void glDrawArrays(GLenum m, GLint first, GLsizei count) { Call(F_glDrawArrays, m, first, count); }
void glDrawBuffers(GLsizei n, const GLenum* bufs) { Call(F_glDrawBuffers, n); Mix(bufs, n * sizeof(GLenum)); }
void glDrawElements(GLenum m, GLsizei count, GLenum type, const void* idx) { Call(F_glDrawElements, m, count, type, (uintptr_t)idx); }
void glEnable(GLenum c) { Call(F_glEnable, c); }
void glEnableVertexAttribArray(GLuint i) { Call(F_glEnableVertexAttribArray, i); }
void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { Call(F_glFramebufferTexture2D, t, a, tt, tex, l); }
void glFramebufferTextureLayer(GLenum t, GLenum a, GLuint tex, GLint l, GLint layer) { Call(F_glFramebufferTextureLayer, t, a, tex, l, layer); }
void glGenBuffers(GLsizei n, GLuint* b) { Call(F_glGenBuffers, n); GenNames(n, b); }
void glGenFramebuffers(GLsizei n, GLuint* f) { Call(F_glGenFramebuffers, n); GenNames(n, f); }
void glGenTextures(GLsizei n, GLuint* t) { Call(F_glGenTextures, n); GenNames(n, t); }
void glGenVertexArrays(GLsizei n, GLuint* a) { Call(F_glGenVertexArrays, n); GenNames(n, a); }
GLenum glGetError() { Call(F_glGetError); return GL_NO_ERROR; }
void glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei* len, GLchar* log) { Call(F_glGetProgramInfoLog, p); if (len) *len = 0; if (max > 0) log[0] = 0; }
void glGetProgramiv(GLuint p, GLenum name, GLint* v) { Call(F_glGetProgramiv, p, name); *v = (name == GL_LINK_STATUS) ? GL_TRUE : 0; }
void glGetShaderInfoLog(GLuint s, GLsizei max, GLsizei* len, GLchar* log) { Call(F_glGetShaderInfoLog, s); if (len) *len = 0; if (max > 0) log[0] = 0; }
void glGetShaderiv(GLuint s, GLenum name, GLint* v) { Call(F_glGetShaderiv, s, name); *v = (name == GL_COMPILE_STATUS) ? GL_TRUE : 0; }
GLuint glGetUniformBlockIndex(GLuint p, const GLchar* name) { Call(F_glGetUniformBlockIndex, p); Mix(name, strlen(name)); return 0; }
GLint glGetUniformLocation(GLuint p, const GLchar* name) { Call(F_glGetUniformLocation, p); Mix(name, strlen(name)); return (GLint)(NextName++); }
void glLineWidth(GLfloat w) { Call(F_glLineWidth, w); }
void glLinkProgram(GLuint p) { Call(F_glLinkProgram, p); }
void glReadBuffer(GLenum b) { Call(F_glReadBuffer, b); }
void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* out)
{
    // no GPU: the pixels read back are zeros (deterministic)
    Call(F_glReadPixels, x, y, w, h, format, type);
    ReadPixelsCalls++;
    memset(out, 0, (size_t)w * h * PixelSize(format, type));
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { Call(F_glScissor, x, y, w, h); }
void glShaderSource(GLuint s, GLsizei n, const GLchar* const* src, const GLint* len)
{
    Call(F_glShaderSource, s, n);
    for (GLsizei i = 0; i < n; i++)
        Mix(src[i], (len && len[i] >= 0) ? (size_t)len[i] : strlen(src[i]));
}
void glStencilFunc(GLenum f, GLint ref, GLuint mask) { Call(F_glStencilFunc, f, ref, mask); }
void glStencilMask(GLuint m) { Call(F_glStencilMask, m); }
void glStencilOp(GLenum a, GLenum b, GLenum c) { Call(F_glStencilOp, a, b, c); }
void glTexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void* data)
{ Call(F_glTexImage2D, t, l, ifmt, w, h, b, format, type); Data(data, (size_t)w * h * PixelSize(format, type)); }
void glTexImage3D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum format, GLenum type, const void* data)
{ Call(F_glTexImage3D, t, l, ifmt, w, h, d, b, format, type); Data(data, (size_t)w * h * d * PixelSize(format, type)); }
void glTexParameteri(GLenum t, GLenum name, GLint v) { Call(F_glTexParameteri, t, name, v); }
void glTexStorage3D(GLenum t, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { Call(F_glTexStorage3D, t, levels, ifmt, w, h, d); }
void glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* data)
{ Call(F_glTexSubImage2D, t, l, x, y, w, h, format, type); Data(data, (size_t)w * h * PixelSize(format, type)); }
void glTexSubImage3D(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void* data)
{ Call(F_glTexSubImage3D, t, l, x, y, z, w, h, d, format, type); Data(data, (size_t)w * h * d * PixelSize(format, type)); }
void glUniform1i(GLint loc, GLint v) { Call(F_glUniform1i, loc, v); }
void glUniform1ui(GLint loc, GLuint v) { Call(F_glUniform1ui, loc, v); }
void glUniform2f(GLint loc, GLfloat a, GLfloat b) { Call(F_glUniform2f, loc, a, b); }
void glUniform4ui(GLint loc, GLuint a, GLuint b, GLuint c, GLuint d) { Call(F_glUniform4ui, loc, a, b, c, d); }
void glUniformBlockBinding(GLuint p, GLuint i, GLuint b) { Call(F_glUniformBlockBinding, p, i, b); }
void glUseProgram(GLuint p) { Call(F_glUseProgram, p); }
void glVertexAttribIPointer(GLuint i, GLint size, GLenum type, GLsizei stride, const void* p)
{ Call(F_glVertexAttribIPointer, i, size, type, stride, (uintptr_t)p); }
void glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p)
{ Call(F_glVertexAttribPointer, i, size, type, norm, stride, (uintptr_t)p); }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { Call(F_glViewport, x, y, w, h); }

}

// Zero-filled heap: a few renderer buffers are uploaded before every byte of them has been written
// (unused parts of uniform blocks). With a zeroed heap the command-stream hash is deterministic.
#include <new>
#include <stdlib.h>
void* operator new(size_t size) { void* p = calloc(1, size ? size : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
