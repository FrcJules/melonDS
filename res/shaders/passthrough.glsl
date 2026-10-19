/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS, licensed under the GNU GPL version 3 or later.

    Plain copy of the input. Minimal example of the shader format
    (see README.md): single file, VERTEX and FRAGMENT sections.
*/

#if defined(VERTEX)

in vec4 VertexCoord;
in vec4 TexCoord;

uniform mat4 MVPMatrix;

out vec2 vTexCoord;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    vTexCoord = TexCoord.xy;
}

#elif defined(FRAGMENT)

uniform sampler2D Texture;

in vec2 vTexCoord;
out vec4 FragColor;

void main()
{
    FragColor = vec4(texture(Texture, vTexCoord).rgb, 1.0);
}

#endif
