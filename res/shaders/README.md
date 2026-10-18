# Screen shaders

Post-processing shaders for the OpenGL display. Copy the files you want into a
`shaders` folder next to `melonDS.toml` (the melonDS configuration folder), then
pick a preset in **View > Screen shader**. If a shader fails to load or draw,
melonDS shows a message and goes back to the normal display.
This needs the OpenGL display (OpenGL renderer, or "Use OpenGL for display" with the software renderer).

## Presets

A preset is a `.toml` file with one to eight passes:

```toml
name = "Example"

[[pass]]
shader = "file.glsl"        # relative to the preset
scale_type = "viewport"     # "source" (default): input size x scale, "viewport": size of the screen in the window
scale = 1                   # integer, only used with "source"
filter = "linear"           # "nearest" (default) or "linear": how this pass samples its input
```

The last pass is drawn to the window; its `scale` and `scale_type` only affect the `OutputSize` it receives.

## Shaders

One file with a `VERTEX` and a `FRAGMENT` section (same convention as libretro GLSL shaders).
melonDS adds the `#version` line and `#define VERTEX` or `#define FRAGMENT`.
Stick to the subset common to GLSL 1.50 and GLSL ES 3.00 (`in`/`out`, `texture()`) so shaders also work in WebGL2.

* attributes: `VertexCoord`, `TexCoord`
* uniforms: `MVPMatrix`, `Texture`, `TextureSize`, `InputSize`, `OutputSize`, `FrameCount`, `FrameDirection`
* fragment output: `FragColor`

Shaders that use `texture2D`, `varying`, or their own `#version` line need to be ported first.

## Included files

| File | Licence |
|---|---|
| `sharp-bilinear.glsl` | Public domain (Themaister), taken from libretro/glsl-shaders |
| `passthrough.glsl`, presets | GPL-3.0-or-later (melonDS team) |
