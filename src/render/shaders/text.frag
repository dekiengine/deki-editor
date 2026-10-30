#version 450 core
// Subpixel text for GPUs drawn through SDL's GPU API, which has no
// dual-source blending: one pass per colour channel, each an ordinary alpha
// blend that writes only its channel, with that channel's coverage as alpha.
// Built three times, CHANNEL=0, 1 and 2. A pass keeps ImGui's drawing order,
// so a shape drawn over text in the same draw call still covers it.
// tools/build-text-shaders.sh compiles it into TextShaders.h.

layout(location = 0) out vec4 fColor;

layout(set = 2, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in struct
{
    vec4 Color;
    vec2 UV;
} In;

// Coverage exponents for black and for white text: light text on the dark
// theme is thickened a little, as DirectWrite and Chromium do.
const float kGammaDark = 1.0;
const float kGammaLight = 0.8;

void main()
{
    // Subpixel glyphs keep straight colour with alpha = the largest channel;
    // everything else in the atlas is white, so its coverage is its alpha.
    vec4 t = texture(sTexture, In.UV.st);
    vec3 coverage = t.rgb * t.a;
    float luminance = dot(In.Color.rgb, vec3(0.2126, 0.7152, 0.0722));
    coverage = pow(coverage, vec3(mix(kGammaDark, kGammaLight, luminance))) * In.Color.a;
    fColor = vec4(In.Color.rgb, coverage[CHANNEL]);
}
