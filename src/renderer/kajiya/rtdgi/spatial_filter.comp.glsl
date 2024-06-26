#include <renderer/kajiya/rtdgi.inl>

#include <utilities/gpu/math.glsl>
#include <renderer/kajiya/inc/color.glsl>
#include <renderer/kajiya/inc/camera.glsl>
// #include <renderer/kajiya/inc/frame_constants.glsl>
// #include <renderer/kajiya/inc/quasi_random.glsl>
// #include <utilities/gpu/uv.glsl>
// #include <utilities/gpu/hash.glsl>
#include <renderer/kajiya/inc/safety.glsl>

DAXA_DECL_PUSH_CONSTANT(RtdgiSpatialFilterComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex input_tex = push.uses.input_tex;
daxa_ImageViewIndex depth_tex = push.uses.depth_tex;
daxa_ImageViewIndex ssao_tex = push.uses.ssao_tex;
daxa_ImageViewIndex geometric_normal_tex = push.uses.geometric_normal_tex;
daxa_ImageViewIndex output_tex = push.uses.output_tex;

#define USE_SSAO_STEERING 1
#define USE_DYNAMIC_KERNEL_RADIUS 0

float max_3(float x, float y, float z) { return max(x, max(y, z)); }

// Bias towards dimmer input -- we don't care about energy loss here
// since this does not feed into subsequent passes, but want to minimize noise.
//
// https://gpuopen.com/learn/optimized-reversible-tonemapper-for-resolve/
vec3 crunch(vec3 v) {
    return v;
    // return v * rcp(max_3(v.r, v.g, v.b) + 1.0);
}
vec3 uncrunch(vec3 v) {
    return v;
    // return v * rcp(1.0 - max_3(v.r, v.g, v.b));
}

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    uvec2 px = gl_GlobalInvocationID.xy;
#if 0
    safeImageStore(output_tex, ivec2(px), safeTexelFetch(input_tex, ivec2(px), 0));
    return;
#endif

    vec4 sum = vec4(0);

    const vec4 input_value = safeTexelFetch(input_tex, ivec2(px), 0);

    const float center_validity = input_value.a;
    const float center_depth = safeTexelFetch(depth_tex, ivec2(px), 0).r;
    const float center_ssao = safeTexelFetch(ssao_tex, ivec2(px), 0).r;
    const vec3 center_value = input_value.rgb;
    const vec3 center_normal_vs = safeTexelFetch(geometric_normal_tex, ivec2(px), 0).xyz * 2.0 - 1.0;

    const vec2 uv = get_uv(px, push.output_tex_size);
    const ViewRayContext view_ray_context = vrc_from_uv_and_depth(gpu_input, uv, center_depth);

    if (center_validity == 1) {
        safeImageStore(output_tex, ivec2(px), vec4(center_value, 1.0));
        return;
    }

    const float ang_off = (deref(gpu_input).frame_index * 23) % 32 * M_TAU + interleaved_gradient_noise(px) * M_PI;

    const uint MAX_SAMPLE_COUNT = 8;
    const float MAX_RADIUS_PX = sqrt(mix(16.0 * 16.0, 2.0 * 2.0, center_validity));

    // Feeds into the `pow` to remap sample index to radius.
    // At 0.5 (sqrt), it's proper circle sampling, with higher values becoming conical.
    // Must be constants, so the `pow` can be const-folded.
    const float KERNEL_SHARPNESS = 0.666;

    const uint sample_count = clamp(uint(exp2(4.0 * square(1.0 - center_validity))), 2, MAX_SAMPLE_COUNT);

    {
        sum += vec4(crunch(center_value), 1);

        const float RADIUS_SAMPLE_MULT = MAX_RADIUS_PX / pow(float(MAX_SAMPLE_COUNT - 1), KERNEL_SHARPNESS);

        // Note: faster on RTX2080 than a dynamic loop
        for (uint sample_i = 1; sample_i < MAX_SAMPLE_COUNT; ++sample_i) {
            const float ang = (sample_i + ang_off) * GOLDEN_ANGLE;

            float radius = pow(float(sample_i), KERNEL_SHARPNESS) * RADIUS_SAMPLE_MULT;
            vec2 sample_offset = vec2(cos(ang), sin(ang)) * radius;
            const ivec2 sample_px = ivec2(vec2(px) + sample_offset);

            const float sample_depth = safeTexelFetch(depth_tex, ivec2(sample_px), 0).r;
            const vec3 sample_val = safeTexelFetch(input_tex, ivec2(sample_px), 0).rgb;
            const float sample_ssao = safeTexelFetch(ssao_tex, ivec2(sample_px), 0).r;
            const vec3 sample_normal_vs = safeTexelFetch(geometric_normal_tex, ivec2(sample_px), 0).xyz * 2.0 - 1.0;

            if (sample_depth != 0 && sample_i < sample_count) {
                float wt = 1;

#if PER_VOXEL_NORMALS
                const vec2 sample_uv = get_uv(sample_px, push.output_tex_size);
                const ViewRayContext sample_ray_ctx = vrc_from_uv_and_depth(gpu_input, sample_uv, sample_depth);
                if (ray_hit_ws(sample_ray_ctx) != ray_hit_ws(view_ray_context)) {
                    // NOTE(grundlett): Cut-off all samples that don't hit the same voxel
                    wt = 0;
                }
#else
                // wt *= pow(saturate(dot(center_normal_vs, sample_normal_vs)), 20);
                wt *= exp2(-100.0 * abs(center_normal_vs.z * (center_depth / sample_depth - 1.0)));

#if USE_SSAO_STEERING
                wt *= exp2(-20.0 * abs(sample_ssao - center_ssao));
#endif
#endif

                sum += vec4(crunch(sample_val), 1.0) * wt;
            }
        }
    }

    float norm_factor = 1.0 / max(1e-5, sum.a);
    vec3 filtered = uncrunch(sum.rgb * norm_factor);

    safeImageStore(output_tex, ivec2(px), vec4(filtered, 1.0));
}
