#include <shared/shared.inl>

#include <utils/trace.glsl>
#include <utils/sky.glsl>
#include <utils/downscale.glsl>

#define SETTINGS deref(settings)
#define INPUT deref(gpu_input)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    u32vec2 pixel_i = gl_GlobalInvocationID.xy;
    u32vec2 offset = get_downscale_offset(gpu_input);

    f32vec2 pixel_p = f32vec2(pixel_i * SHADING_SCL + offset) + 0.5;
    f32vec2 frame_dim = INPUT.frame_dim;
    f32vec2 inv_frame_dim = f32vec2(1.0, 1.0) / frame_dim;
    f32 aspect = frame_dim.x * inv_frame_dim.y;

    f32vec2 uv = pixel_p * inv_frame_dim;
    uv = (uv - 0.5) * f32vec2(aspect, 1.0) * 2.0;

    f32vec2 blue_noise = texelFetch(daxa_texture3D(blue_noise_vec2), ivec3(pixel_i, INPUT.frame_index) & ivec3(127, 127, 63), 0).xy - 0.5;

    f32 depth = texelFetch(daxa_texture2D(depth_image), i32vec2(pixel_i), 0).r;
    u32vec4 g_buffer_value = texelFetch(daxa_utexture2D(g_buffer_image_id), i32vec2(pixel_i * SHADING_SCL + offset), 0);
    f32vec3 nrm = u16_to_nrm(g_buffer_value.y);

    if (depth == MAX_DIST || dot(nrm, nrm) == 0.0) {
        imageStore(daxa_image2D(indirect_diffuse_image_id), i32vec2(pixel_i), f32vec4(0, 0, 0, 0));
        return;
    }

    f32vec3 cam_pos = create_view_pos(globals);
    f32vec3 cam_dir = create_view_dir(globals, uv);
    u32vec3 chunk_n = u32vec3(1u << SETTINGS.log2_chunks_per_axis);
    f32vec3 ray_pos = cam_pos + cam_dir * (depth - 0.01 / VOXEL_SCL) + nrm * 0.01 / VOXEL_SCL;

    mat3 tbn = tbn_from_normal(SUN_DIR);
    f32vec3 ray_dir = tbn * normalize(vec3(rand_circle_pt(blue_noise * 0.5 + 0.5) * 0.03, 1));
    // f32vec3 ray_dir = SUN_DIR;

    VoxelTraceResult trace_result = trace_hierarchy_traversal(VoxelTraceInfo(voxel_malloc_page_allocator, voxel_chunks, chunk_n, ray_dir, MAX_STEPS, MAX_DIST, 0.0, true), ray_pos);

    f32vec3 col = SUN_COL * f32(trace_result.dist == MAX_DIST);

    imageStore(daxa_image2D(indirect_diffuse_image_id), i32vec2(pixel_i), f32vec4(col, 0));
}
#undef INPUT
#undef SETTINGS
