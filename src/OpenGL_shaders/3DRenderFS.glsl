#version 140

uniform usampler2DArray CurTexture;
uniform sampler2DArray Capture128Texture;
uniform sampler2DArray Capture256Texture;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

uniform int uRenderMode; // 0=opaque 1=translucent 2=shadowmask
uniform int uAlphaRef;   // DISP3DCNT alpha test (0 = off); like the software renderer: discard if alpha5 <= ref

smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in ivec3 fPolygonAttr;

#ifdef WBuffer
smooth in float fZ;
#endif

out vec4 oColor;
out vec4 oAttr;
#ifdef FogAttachment
// without per-attachment colour masks the fog flag (oAttr.b) is also written to its own attachment (see GPU3D_OpenGL.cpp)
out vec4 oFogAttr;
#endif

// 6-bit expansion of a 5-bit colour component, as the console and the software renderer do: (c << 1), plus 1 if non-zero
ivec3 To6(ivec3 c5)
{
    return (c5 << 1) + ivec3(notEqual(c5, ivec3(0)));
}

vec4 FinalColor()
{
    // The console samples a texture at the left/top edge of each pixel, GL at its centre: shift the sample by half a pixel
    // (exact at render scale 1; at higher scales the shift is below what is visible). Derivatives are taken outside any branch.
    vec2 dtx = dFdx(fTexcoord);
    vec2 dty = dFdy(fTexcoord);
    const float texbias = -0.5;

    // From here on the blending arithmetic is the software renderer's, in integers (6-bit colours, 5-bit alpha):
    // float math followed by the final truncation made every textured pixel up to 4/255 too dark.
    ivec3 vc = ivec3(fColor.rgb * 255.0 + 0.5) >> 2;
    int polyalpha = int(fColor.a * 31.0 + 0.5);
    int blendmode = (fPolygonAttr.x >> 4) & 0x3;
    bool highlight = (uDispCnt & (1<<1)) != 0;

    if (blendmode == 2)
    {
        if (!highlight)
        {
            // toon: the vertex colour is replaced by the toon colour
            vc = To6(ivec3(uToonColors[vc.r >> 1].rgb * 31.0 + 0.5));
        }
        else
        {
            // highlight: all vertex colour components are set to the red component
            vc = vc.rrr;
        }
    }

    ivec3 rgb;
    int alpha;

    if (fPolygonAttr.y == 0xFFFF)
    {
        // no texture
        rgb = vc;
        alpha = polyalpha;
    }
    else
    {
        vec3 texcoord = vec3(fTexcoord + (dtx + dty) * texbias, fPolygonAttr.y);
        ivec4 tc;
        if (fPolygonAttr.z == 0)
            tc = ivec4(texture(CurTexture, texcoord));          // already 6-bit colour, 5-bit alpha
        else if (fPolygonAttr.z == 1)
            tc = ivec4(texture(Capture128Texture, texcoord) * vec4(63,63,63,31) + 0.5);
        else
            tc = ivec4(texture(Capture256Texture, texcoord) * vec4(63,63,63,31) + 0.5);

        if ((blendmode & 1) != 0)
        {
            // decal
            if (tc.a == 0)       rgb = vc;
            else if (tc.a == 31) rgb = tc.rgb;
            else                 rgb = (tc.rgb * tc.a + vc * (31 - tc.a)) >> 5;
            alpha = polyalpha;
        }
        else
        {
            // modulate
            rgb = ((tc.rgb + 1) * (vc + 1) - 1) >> 6;
            alpha = ((tc.a + 1) * (polyalpha + 1) - 1) >> 5;
        }
    }

    if (blendmode == 2 && highlight)
    {
        // highlight: the toon colour is added to the final colour
        rgb = min(rgb + To6(ivec3(uToonColors[vc.r >> 1].rgb * 31.0 + 0.5)), ivec3(63));
    }

    // 6-bit value v is stored as (4v+2)/255 so that the final pass, which reads 8-bit values and drops two bits, gets v back
    return vec4(vec3(rgb * 4 + 2) / 255.0, float(alpha) / 31.0);
}

void main()
{
    if (uRenderMode == 2)
    {
        oColor = vec4(0,0,0,1);
    }
    else
    {
        vec4 col = FinalColor();
        if (int(col.a * 31.0 + 0.5) <= uAlphaRef) discard;
        if (uRenderMode == 0)
        {
            // opaque pixels
            if (col.a < 30.5/31.0) discard;

            oAttr.r = float((fPolygonAttr.x >> 24) & 0x3F) / 63.0;
            oAttr.g = 0.0;
            oAttr.b = float((fPolygonAttr.x >> 15) & 0x1);
            oAttr.a = 1.0;
        }
        else
        {
            // translucent pixels
            if (col.a < 0.5/31.0) discard;
            if (col.a >= 30.5/31.0) discard;

            oAttr.b = 0.0;
            oAttr.a = 1.0;
        }

        oColor = col;
    }

#ifdef FogAttachment
    oFogAttr = oAttr;
#endif

#ifdef WBuffer
    gl_FragDepth = fZ;
#endif
}
