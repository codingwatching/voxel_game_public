#include <shared/shared.inl>

#include <utils/trace.glsl>

#define SETTINGS deref(settings)
#define INPUT deref(gpu_input)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    u32vec2 pixel_i = gl_GlobalInvocationID.xy;

    f32vec2 pixel_p = f32vec2(pixel_i) + 0.5;
    f32vec2 frame_dim = f32vec2(INPUT.frame_dim) / PREPASS_SCL;
    f32vec2 inv_frame_dim = f32vec2(1.0, 1.0) / frame_dim;
    f32 aspect = frame_dim.x * inv_frame_dim.y;

    f32vec2 uv = pixel_p * inv_frame_dim;

    uv = (uv - 0.5) * f32vec2(aspect, 1.0) * 2.0;
    f32vec3 cam_pos = create_view_pos(globals);
    f32vec3 ray_pos = cam_pos;
    f32vec3 ray_dir = create_view_dir(globals, uv);
    u32vec3 chunk_n = u32vec3(1u << SETTINGS.log2_chunks_per_axis);

#if ENABLE_DEPTH_PREPASS
    VoxelTraceResult trace_result = trace_hierarchy_traversal(VoxelTraceInfo(voxel_malloc_page_allocator, voxel_chunks, chunk_n, ray_dir, MAX_STEPS, MAX_DIST, 32.0 / frame_dim.y * deref(globals).player.cam.tan_half_fov, true), ray_pos);
    u32 step_n = trace_result.step_n;
#else
    u32 step_n = 0;
#endif

    f32 depth = length(ray_pos - cam_pos);

    imageStore(daxa_image2D(render_depth_prepass_image), i32vec2(pixel_i), f32vec4(depth, step_n, 0, 0));
}
#undef INPUT
#undef SETTINGS
