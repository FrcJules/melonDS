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

#ifndef OPENGLSUPPORT_H
#define OPENGLSUPPORT_H

#include <stdio.h>
#include <string.h>

#include "Platform.h"
#include "PlatformOGL.h"

namespace melonDS::OpenGL
{

void LoadShaderCache();
void SaveShaderCache();

struct AttributeTarget
{
    const char* Name;
    u32 Location;
};


bool CompileVertexFragmentProgram(GLuint& result,
    const std::string& vs, const std::string& fs,
    const std::string& name,
    const std::initializer_list<AttributeTarget>& vertexInAttrs,
    const std::initializer_list<AttributeTarget>& fragmentOutAttrs);

bool CompileComputeProgram(GLuint& result, const std::string& source, const std::string& name);

// Per-attachment colour masks (glColorMaski). Desktop OpenGL 3.0 always has them; OpenGL ES 3.0 / WebGL2 only with
// the OES_draw_buffers_indexed extension. The frontend clears this flag before creating the renderer when the extension
// is missing: the renderers then get the same output with glDrawBuffers (an attachment is written or not) and the
// global glColorMask instead (see ColorMaskAll and the sites that write two attachments with different masks).
extern bool IndexedColorMask;

// Same mask on every attachment. With per-attachment masks only attachment 0 is set (the framebuffers drawn next have a
// single attachment, or set the other masks themselves), as the renderers always did; without them the global mask
// covers all the attachments, which is equivalent for those same reasons.
inline void ColorMaskAll(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    if (IndexedColorMask) glColorMaski(0, r, g, b, a);
    else                  glColorMask(r, g, b, a);
}

}

#endif // OPENGLSUPPORT_H
