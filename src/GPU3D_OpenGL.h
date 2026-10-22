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

#pragma once

#ifdef OGLRENDERER_ENABLED
#include "GPU3D.h"
#include "OpenGLSupport.h"
#include "GPU3D_TexcacheOpenGL.h"
#include "NonStupidBitfield.h"

namespace melonDS
{
class GLRenderer;

class GLRenderer3D : public Renderer3D
{
public:
    GLRenderer3D(melonDS::GPU3D& gpu3D, GLRenderer& parent) noexcept;
    ~GLRenderer3D() override;
    bool Init() override;
    void Reset() override;

    void SetRenderSettings(int scale, bool betterpolygons) noexcept;
    void SetBetterPolygons(bool betterpolygons) noexcept;
    void SetScaleFactor(int scale) noexcept;
    [[nodiscard]] bool GetBetterPolygons() const noexcept { return BetterPolygons; }
    [[nodiscard]] int GetScaleFactor() const noexcept { return ScaleFactor; }

    void RenderFrame() override;
    u32* GetLine(int line) override;

private:
    GLRenderer& Parent;

    // GL version requirements
    // * texelFetch: 3.0 (GLSL 1.30)     (3.2/1.50 for MS)
    // * UBO: 3.1

    struct RendererPolygon
    {
        Polygon* PolyData;

        u32 NumIndices;
        u32 IndicesOffset;
        GLuint PrimType;

        u32 NumEdgeIndices;
        u32 EdgeIndicesOffset;

        u32 RenderKey;

        GLuint TexID;
        u32 TexRepeat;
    };

    //GLCompositor CurGLCompositor;
    RendererPolygon PolygonList[2048] {};

    bool TexEnable;
    TexcacheOpenGL Texcache;

    bool BuildRenderShader(bool wbuffer);
    bool BuildRenderShaderVariant(bool wbuffer, GLuint& out);
    bool CompileMainProgram(GLuint& prog, const std::string& vs, std::string fs, const char* name,
                            std::initializer_list<OpenGL::AttributeTarget> vertexIn) const;
    void UseRenderShader(bool wbuffer);
    void SetupPolygon(RendererPolygon* rp, Polygon* polygon) const;
    u32* SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32 texlayer, u32* vptr) const;
    void BuildPolygons(RendererPolygon* polygons, int npolys, int captureinfo[16]);
    void SetupPolygonTexture(const RendererPolygon* poly) const;
    int RenderSinglePolygon(int i) const;
    int RenderPolygonBatch(int i) const;
    int RenderPolygonEdgeBatch(int i) const;
    void RenderSceneChunk(int y, int h);

    // GL state set while drawing the polygons (RenderSceneChunk): a call that would set a value that is
    // already set is skipped. Fewer GL calls (each one is a JavaScript call in WebGL), same GL state at
    // every draw. Every value is unknown again at the start of RenderSceneChunk, the only user.
    struct StateCache
    {
        s64 DepthFunc, DepthMask, Blend, ActiveTexture, StencilMask, RenderMode;
        s64 StencilFunc[3], StencilOp[3];
        s64 ColorMask[2];
        s64 GlobalColorMask, DrawBuffers; // without per-attachment colour masks (see CColorMasks)
        GLuint BoundTex[3];             // GL_TEXTURE_2D_ARRAY of texture units 0-2
        int NumWraps;
        GLuint WrapTex[32];             // wrap modes of the array textures used in this chunk
        GLint WrapS[32], WrapT[32];
    };
    mutable StateCache Cache {};
    void CacheReset() const;
    void CDepthFunc(GLenum func) const;
    void CDepthMask(GLboolean mask) const;
    void CBlend(bool enable) const;
    void CStencilFunc(GLenum func, GLint ref, GLuint mask) const;
    void CStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass) const;
    void CStencilMask(GLuint writemask) const;
    void CColorMask(GLuint buf, GLboolean r, GLboolean g, GLboolean b, GLboolean a) const;
    void CColorMasks(GLboolean r0, GLboolean g0, GLboolean b0, GLboolean a0,
                     GLboolean r1, GLboolean g1, GLboolean b1, GLboolean a1) const;
    void CActiveTexture(GLenum unit) const;
    void CBindTex2DArray(GLuint tex) const;
    void CTexWrap(GLint s, GLint t) const;
    void CRenderMode(GLint mode) const;


    enum
    {
        RenderMode_Opaque = 0,
        RenderMode_Translucent,
        RenderMode_ShadowMask,
    };


    GLuint ClearShaderPlain {};
    GLuint ClearShaderBitmap {};

    GLuint RenderShader[2] {};
    GLuint RenderShaderLines[2] {}; // identical copy used for GL_LINES only (see RenderSceneChunk)
    GLint RenderModeULoc = 0;
    GLint AlphaRefULoc = 0;
    GLuint CurShaderID = -1;

    GLuint FinalPassEdgeShader {};
    GLuint FinalPassFogShader {};

    // std140 compliant structure
    struct
    {
        float uScreenSize[2];       // vec2       0 / 2
        u32 uDispCnt;               // int        2 / 1
        u32 __pad0;
        float uToonColors[32][4];   // vec4[32]   4 / 128
        float uEdgeColors[8][4];    // vec4[8]    132 / 32
        float uFogColor[4];         // vec4       164 / 4
        float uFogDensity[34][4];   // float[34]  168 / 136
        u32 uFogOffset;             // int        304 / 1
        u32 uFogShift;              // int        305 / 1
        u32 _pad1[2];               // int        306 / 2
    } ShaderConfig {};

    GLuint ShaderConfigUBO {};
    int NumFinalPolys {}, NumOpaqueFinalPolys {};

    GLuint ClearVertexBufferID = 0, ClearVertexArrayID {};
    GLint ClearUniformLoc[4] {};

    GLint ClearBitmapULoc[2] {};
    GLuint ClearBitmapTex[2];
    u32* ClearBitmap[2];
    u8 ClearBitmapDirty;

    // vertex buffer
    // * XYZW: 4x16bit
    // * RGBA: 4x8bit
    // * ST: 2x16bit
    // * polygon data: 3x32bit (polygon/texture attributes)
    //
    // polygon attributes:
    // * bit4-7, 11, 14-15, 24-29: POLYGON_ATTR
    // * bit16-20: Z shift
    // * bit8: front-facing (?)
    // * bit9: W-buffering (?)

    GLuint VertexBufferID {};
    u32 VertexBuffer[10240 * 7] {};
    u32 NumVertices {};

    GLuint VertexArrayID {};
    GLuint IndexBufferID {};
    u16 IndexBuffer[2048 * 40] {};
    u32 NumIndices {}, NumEdgeIndices {};

    const u32 EdgeIndicesOffset = 2048 * 30;

    int ScaleFactor {};
    bool BetterPolygons {};
    int ScreenW {}, ScreenH {};

    GLuint ColorBufferTex {}, DepthBufferTex {}, AttrBufferTex {};
    // without per-attachment colour masks only: copy of the attribute buffer on colour attachment 2, of which only
    // B (fog flag) is used; the fog pass reads it instead of AttrBufferTex (see CColorMasks)
    GLuint FogAttrBufferTex {};

    GLuint MainFramebuffer {};
};
}
#endif