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

#ifndef SHADERCHAIN_H
#define SHADERCHAIN_H

#include <string>
#include <vector>

#include "types.h"
#include "PlatformOGL.h"

namespace melonDS
{

// Post-processing shader chain applied when drawing the DS screens.
//
// A chain is described by a preset file (TOML) listing 1 to kMaxPasses passes:
//
//   name = "Sharp bilinear"
//   [[pass]]
//   shader = "sharp-bilinear.glsl"   # relative to the preset
//   scale_type = "viewport"          # "source" (default) or "viewport"
//   scale = 1                        # integer multiplier for "source"
//   filter = "linear"                # "nearest" (default) or "linear"
//
// Shaders are single files with VERTEX and FRAGMENT sections (libretro GLSL
// convention). The loader prepends the #version line and #define VERTEX or
// FRAGMENT. Uniforms provided: MVPMatrix, Texture, TextureSize, InputSize,
// OutputSize, FrameCount, FrameDirection. Attributes: VertexCoord, TexCoord.
//
// Shader sources must stick to the subset common to GLSL 1.50 and GLSL ES 3.00
// so the same files can be used with WebGL2.
//
// All methods must be called with the OpenGL context current.
class ShaderChain
{
public:
    static constexpr int kMaxPasses = 8;

    ShaderChain() = default;
    ~ShaderChain();

    ShaderChain(const ShaderChain&) = delete;
    ShaderChain& operator=(const ShaderChain&) = delete;

    // Loads a preset from the shaders directory (name is relative to it) and
    // compiles all its passes. On failure nothing is kept and false is returned.
    bool Load(const std::string& presetName);
    void Unload();

    bool IsLoaded() const { return Loaded; }
    const std::string& GetName() const { return Name; }
    const std::string& GetError() const { return Error; }

    // Draws one screen through the chain onto the current framebuffer (0).
    //  * srcTex, srcW, srcH: GL_TEXTURE_2D_ARRAY holding both screens and the size of one layer
    //  * layer: 0 = top, 1 = bottom
    //  * slot: index of the screen transform being drawn (each slot has its own buffers)
    //  * matrix: the 2x3 screen transform, as used by the frontend
    //  * logicalW/H: surface size divided by surface scale; scale: surface scale
    // Returns false if something went wrong; the caller must then unload the chain
    // and use its regular drawing path.
    bool Draw(GLuint srcTex, int srcW, int srcH, int layer, int slot, const float* matrix,
              float logicalW, float logicalH, float scale, u32 frameCount);

private:
    struct Pass
    {
        GLuint Program = 0;
        GLint MVPLoc = -1, TextureLoc = -1, TextureSizeLoc = -1, InputSizeLoc = -1;
        GLint OutputSizeLoc = -1, FrameCountLoc = -1, FrameDirectionLoc = -1;
        bool ViewportScale = false;
        int Scale = 1;
        bool Linear = false;
    };

    struct Target
    {
        GLuint Tex = 0;
        GLuint FBO = 0;
        int W = 0, H = 0;
    };

    // per screen transform slot: copy of the source layer + one target per pass but the last
    struct Slot
    {
        std::vector<Target> Targets;
    };

    bool Loaded = false;
    std::string Name;
    std::string Error;

    std::vector<Pass> Passes;
    std::vector<Slot> Slots;

    GLuint CopyProgram = 0;
    GLint CopyLayerLoc = -1;
    GLuint VertexBuffer = 0, VertexArray = 0;

    bool CompileCopyProgram();
    bool EnsureTarget(Target& target, int w, int h);
    void FreeTarget(Target& target);
};

}

#endif // SHADERCHAIN_H
