#ifndef PLATFORMOGL_H
#define PLATFORMOGL_H

// If you don't wanna use glad for your platform,
// define MELONDS_GL_HEADER to the path of some other header
// that pulls in the necessary OpenGL declarations.
// Make sure to include quotes or angle brackets as needed,
// and that all targets get the same MELONDS_GL_HEADER definition.

#ifndef MELONDS_GL_HEADER
#define MELONDS_GL_HEADER "\"frontend/glad/glad.h\""
#endif

#include MELONDS_GL_HEADER

#ifdef MELONDS_GL_PROFILE
// optional per-region timing of the OpenGL renderer (nanoseconds, summed; read and reset by the frontend)
#include <chrono>
extern unsigned long long MelonGLProf[16];
struct MelonGLProfScope
{
    int Index;
    std::chrono::steady_clock::time_point Start;
    explicit MelonGLProfScope(int index) : Index(index), Start(std::chrono::steady_clock::now()) {}
    ~MelonGLProfScope() { MelonGLProf[Index] += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count(); }
};
#define GL_PROF(index) MelonGLProfScope MelonGLProfVar##index(index)
#define GL_COUNT(index) (MelonGLProf[index]++)   // raw counter, not a duration
#else
#define GL_PROF(index)
#define GL_COUNT(index)
#endif

#ifdef MELONDS_GL_SECTION_COUNT
// screen sections composited before VBlank by the GL 2D renderer (games that change display registers mid-frame),
// read and reset by the frontend
extern unsigned MelonGL2DSections;
#define GL_SECTION_COUNT() (MelonGL2DSections++)
#else
#define GL_SECTION_COUNT()
#endif

#ifdef MELONDS_GLES
// OpenGL ES 3.0 / WebGL2 compatibility: the renderers are written for desktop OpenGL 3.1,
// these definitions cover the (few) desktop-only calls they use.
#include <stddef.h>
#include <stdint.h>
#include <vector>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2ext.h>

#define glClearDepth glClearDepthf
#define glDepthRange glDepthRangef

// per-attachment color masks need OES_draw_buffers_indexed (a WebGL2 extension that must be enabled)
#define glColorMaski glColorMaskiOES

// ES has no border color: clamp to the edge; the 2D compositor shader emulates the transparent border itself
#define GL_CLAMP_TO_BORDER GL_CLAMP_TO_EDGE

static inline void glDrawBuffer(GLenum buffer)
{
    glDrawBuffers(1, &buffer);
}

// all the textures attached this way are plain 2D textures
#define glFramebufferTexture(target, attachment, texture, level) \
    glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, texture, level)

// The DS stores colors as ABGR1555 (red in the low bits, GL_UNSIGNED_SHORT_1_5_5_5_REV).
// ES only has the RGBA5551 layout, so 16-bit pixel data is shuffled on the CPU.
#define MELONDS_RGB5A1_TYPE GL_UNSIGNED_SHORT_5_5_5_1

static inline const uint16_t* MelonDS_ToES5551(const uint16_t* src, size_t count)
{
    static std::vector<uint16_t> scratch;
    if (scratch.size() < count) scratch.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        uint16_t v = src[i];
        scratch[i] = ((v & 0x001F) << 11) | ((v & 0x03E0) << 1) | ((v & 0x7C00) >> 9) | (v >> 15);
    }
    return scratch.data();
}

static inline void MelonDS_FromES5551(const uint16_t* src, uint16_t* dst, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        uint16_t v = src[i];
        dst[i] = ((v & 0xF800) >> 11) | ((v & 0x07C0) >> 1) | ((v & 0x003E) << 9) | ((v & 1) << 15);
    }
}

#define MELONDS_PIX555(ptr, count) MelonDS_ToES5551((const uint16_t*)(ptr), (count))

// Palette textures are RGBA8: uploading them as RGBA5551 makes the driver convert every pixel on the CPU (slow path).
// Expanding ABGR1555 to RGBA8 here and uploading plain bytes takes the driver's direct copy path.
static inline void MelonDS_TexSubImagePalette(int y, int width, int height, const uint16_t* src)
{
    static std::vector<uint32_t> expanded;
    size_t count = (size_t)width * height;
    if (expanded.size() < count) expanded.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        uint32_t v = src[i];
        uint32_t r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        expanded[i] = r | (g << 8) | (b << 16) | ((v & 0x8000) ? 0xFF000000u : 0u);   // bytes R, G, B, A
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, expanded.data());
}
#define MELONDS_TEXSUBIMAGE_PALETTE(y, w, h, ptr) MelonDS_TexSubImagePalette((y), (w), (h), (const uint16_t*)(ptr))
#else
#define MELONDS_RGB5A1_TYPE GL_UNSIGNED_SHORT_1_5_5_5_REV
#define MELONDS_PIX555(ptr, count) (ptr)
#define MELONDS_TEXSUBIMAGE_PALETTE(y, w, h, ptr) \
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (y), (w), (h), GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, (ptr))
#endif

#endif
