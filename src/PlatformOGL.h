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
#else
#define MELONDS_RGB5A1_TYPE GL_UNSIGNED_SHORT_1_5_5_5_REV
#define MELONDS_PIX555(ptr, count) (ptr)
#endif

#endif
