#include <renderer/kajiya/inc/samplers.hlsl>
#include <renderer/kajiya/inc/uv.hlsl>
#include <renderer/kajiya/inc/color.hlsl>
#include <renderer/kajiya/inc/image.hlsl>
#include <renderer/kajiya/inc/frame_constants.hlsl>
#include <renderer/kajiya/inc/hash.hlsl>
#include <renderer/kajiya/inc/unjitter_taa.hlsl>
#include <renderer/kajiya/inc/soft_color_clamp.hlsl>
#include "taa_common.hlsl"

[[vk::binding(0)]] Texture2D<vec4> input_tex;
[[vk::binding(1)]] Texture2D<float> depth_tex;
[[vk::binding(2)]] RWTexture2D<float> output_tex;
[[vk::binding(3)]] cbuffer _ {
    vec4 input_tex_size;
};

struct InputRemap {
    static InputRemap create() {
        InputRemap res;
        return res;
    }

    vec4 remap(vec4 v) {
        return vec4(sRGB_to_YCbCr(decode_rgb(v.rgb)), 1);
    }
};

[numthreads(8, 8, 1)]
void main(uvec2 px: SV_DispatchThreadID) {
    const vec3 input = sRGB_to_YCbCr(decode_rgb(input_tex[px].rgb));
    
    vec3 iex = 0;
    vec3 iex2 = 0;
    float iwsum = 0;
    {
        int k = 1;
        for (int y = -k; y <= k; ++y) {
            for (int x = -k; x <= k; ++x) {
                vec3 s = sRGB_to_YCbCr(decode_rgb(input_tex[px + ivec2(x, y)].rgb));
                float w = 1;
                iwsum += w;
                iex += s * w;
                iex2 += s * s * w;
            }
        }
    }

    iex /= iwsum;
    iex2 /= iwsum;

    vec3 ivar = max(0, iex2 - iex * iex);
    //output_tex[px] = sqrt(ivar.x) / max(1e-5, iex.x);
    output_tex[px] = ivar.x;
}
