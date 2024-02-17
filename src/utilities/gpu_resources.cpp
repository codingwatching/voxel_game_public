#include "gpu_resources.hpp"

#include <application/input.inl>
#include <application/globals.inl>
#include <application/settings.inl>

#include <minizip/unzip.h>
#include <FreeImage.h>

#include <random>

void GpuResources::create(daxa::Device &device) {
    value_noise_image = device.create_image({
        .dimensions = 2,
        .format = daxa::Format::R8_UNORM,
        .size = {256, 256, 1},
        .array_layer_count = 256,
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "value_noise_image",
    });
    blue_noise_vec2_image = device.create_image({
        .dimensions = 3,
        .format = daxa::Format::R8G8B8A8_UNORM,
        .size = {128, 128, 64},
        .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
        .name = "blue_noise_vec2_image",
    });
    input_buffer = device.create_buffer({
        .size = sizeof(GpuInput),
        .name = "input_buffer",
    });
    output_buffer = device.create_buffer({
        .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
        .name = "output_buffer",
    });
    staging_output_buffer = device.create_buffer({
        .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "staging_output_buffer",
    });
    globals_buffer = device.create_buffer({
        .size = sizeof(GpuGlobals),
        .name = "globals_buffer",
    });
    sampler_nnc = device.create_sampler({
        .magnification_filter = daxa::Filter::NEAREST,
        .minification_filter = daxa::Filter::NEAREST,
        .max_lod = 0.0f,
    });
    sampler_lnc = device.create_sampler({
        .magnification_filter = daxa::Filter::LINEAR,
        .minification_filter = daxa::Filter::NEAREST,
        .max_lod = 0.0f,
    });
    sampler_llc = device.create_sampler({
        .magnification_filter = daxa::Filter::LINEAR,
        .minification_filter = daxa::Filter::LINEAR,
        .max_lod = 0.0f,
    });
    sampler_llr = device.create_sampler({
        .magnification_filter = daxa::Filter::LINEAR,
        .minification_filter = daxa::Filter::LINEAR,
        .address_mode_u = daxa::SamplerAddressMode::REPEAT,
        .address_mode_v = daxa::SamplerAddressMode::REPEAT,
        .address_mode_w = daxa::SamplerAddressMode::REPEAT,
        .max_lod = 0.0f,
    });

    task_input_buffer.set_buffers({.buffers = std::array{input_buffer}});
    task_output_buffer.set_buffers({.buffers = std::array{output_buffer}});
    task_staging_output_buffer.set_buffers({.buffers = std::array{staging_output_buffer}});
    task_globals_buffer.set_buffers({.buffers = std::array{globals_buffer}});

    task_value_noise_image.set_images({.images = std::array{value_noise_image}});
    task_blue_noise_vec2_image.set_images({.images = std::array{blue_noise_vec2_image}});

    {
        daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });
        temp_task_graph.use_persistent_image(task_blue_noise_vec2_image);
        temp_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_blue_noise_vec2_image),
            },
            .task = [this](daxa::TaskInterface const &ti) {
                auto staging_buffer = ti.device.create_buffer({
                    .size = static_cast<daxa_u32>(128 * 128 * 4 * 64 * 1),
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "staging_buffer",
                });
                auto *buffer_ptr = ti.device.get_host_address_as<uint8_t>(staging_buffer).value();
                auto *stbn_zip = unzOpen("assets/STBN.zip");
                for (auto i = 0; i < 64; ++i) {
                    [[maybe_unused]] int err = 0;
                    daxa_i32 size_x = 0;
                    daxa_i32 size_y = 0;
                    auto load_image = [&](char const *path, uint8_t *buffer_out_ptr) {
                        err = unzLocateFile(stbn_zip, path, 1);
                        assert(err == UNZ_OK);
                        auto file_info = unz_file_info{};
                        err = unzGetCurrentFileInfo(stbn_zip, &file_info, nullptr, 0, nullptr, 0, nullptr, 0);
                        assert(err == UNZ_OK);
                        auto file_data = std::vector<uint8_t>{};
                        file_data.resize(file_info.uncompressed_size);
                        err = unzOpenCurrentFile(stbn_zip);
                        assert(err == UNZ_OK);
                        err = unzReadCurrentFile(stbn_zip, file_data.data(), static_cast<uint32_t>(file_data.size()));
                        assert(err == file_data.size());

                        auto fi_mem = FreeImage_OpenMemory(file_data.data(), static_cast<DWORD>(file_data.size()));
                        auto fi_file_desc = FreeImage_GetFileTypeFromMemory(fi_mem, 0);
                        FIBITMAP *fi_bitmap = FreeImage_LoadFromMemory(fi_file_desc, fi_mem);
                        FreeImage_CloseMemory(fi_mem);
                        size_x = static_cast<int32_t>(FreeImage_GetWidth(fi_bitmap));
                        size_y = static_cast<int32_t>(FreeImage_GetHeight(fi_bitmap));
                        auto *temp_data = FreeImage_GetBits(fi_bitmap);
                        assert(temp_data != nullptr && "Failed to load image");
                        auto pixel_size = FreeImage_GetBPP(fi_bitmap);
                        if (pixel_size != 32) {
                            auto *temp = FreeImage_ConvertTo32Bits(fi_bitmap);
                            FreeImage_Unload(fi_bitmap);
                            fi_bitmap = temp;
                        }

                        if (temp_data != nullptr) {
                            assert(size_x == 128 && size_y == 128);
                            std::copy(temp_data + 0, temp_data + 128 * 128 * 4, buffer_out_ptr);
                        }
                        FreeImage_Unload(fi_bitmap);
                    };
                    auto vec2_name = std::string{"STBN/stbn_vec2_2Dx1D_128x128x64_"} + std::to_string(i) + ".png";
                    load_image(vec2_name.c_str(), buffer_ptr + (128 * 128 * 4) * i + (128 * 128 * 4 * 64) * 0);
                }

                ti.recorder.pipeline_barrier({
                    .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                });
                ti.recorder.destroy_buffer_deferred(staging_buffer);
                ti.recorder.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = (size_t{128} * 128 * 4 * 64) * 0,
                    .image = task_blue_noise_vec2_image.get_state().images[0],
                    .image_extent = {128, 128, 64},
                });
            },
            .name = "upload_blue_noise",
        });
        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    }

    {
        daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });

        auto texture_path = "assets/debug.png";
        auto fi_file_desc = FreeImage_GetFileType(texture_path, 0);
        FIBITMAP *fi_bitmap = FreeImage_Load(fi_file_desc, texture_path);
        auto size_x = static_cast<uint32_t>(FreeImage_GetWidth(fi_bitmap));
        auto size_y = static_cast<uint32_t>(FreeImage_GetHeight(fi_bitmap));
        auto *temp_data = FreeImage_GetBits(fi_bitmap);
        assert(temp_data != nullptr && "Failed to load image");
        auto pixel_size = FreeImage_GetBPP(fi_bitmap);
        if (pixel_size != 32) {
            auto *temp = FreeImage_ConvertTo32Bits(fi_bitmap);
            FreeImage_Unload(fi_bitmap);
            fi_bitmap = temp;
        }
        auto size = static_cast<daxa_u32>(size_x) * static_cast<daxa_u32>(size_y) * 4 * 1;

        debug_texture = device.create_image({
            .dimensions = 2,
            .format = daxa::Format::R8G8B8A8_UNORM,
            .size = {static_cast<daxa_u32>(size_x), static_cast<daxa_u32>(size_y), 1},
            .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "debug_texture",
        });

        task_debug_texture.set_images({.images = std::array{debug_texture}});
        temp_task_graph.use_persistent_image(task_debug_texture);
        temp_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_debug_texture),
            },
            .task = [&, this](daxa::TaskInterface const &ti) {
                auto staging_buffer = ti.device.create_buffer({
                    .size = size,
                    .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                    .name = "staging_buffer",
                });
                auto *buffer_ptr = ti.device.get_host_address_as<uint8_t>(staging_buffer).value();
                std::copy(temp_data + 0, temp_data + size, buffer_ptr);
                FreeImage_Unload(fi_bitmap);

                ti.recorder.pipeline_barrier({
                    .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                });
                ti.recorder.destroy_buffer_deferred(staging_buffer);
                ti.recorder.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .image = task_debug_texture.get_state().images[0],
                    .image_extent = {static_cast<daxa_u32>(size_x), static_cast<daxa_u32>(size_y), 1},
                });
            },
            .name = "upload_debug_texture",
        });
        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    }

    if (false) {
        daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });

        auto texture_path = "C:/Users/gabe/Downloads/Rugged Terrain with Rocky Peaks/Rugged Terrain with Rocky Peaks Height Map EXR.exr";
        auto fi_file_desc = FreeImage_GetFileType(texture_path, 0);
        FIBITMAP *fi_bitmap = FreeImage_Load(fi_file_desc, texture_path);
        auto size_x = static_cast<uint32_t>(FreeImage_GetWidth(fi_bitmap));
        auto size_y = static_cast<uint32_t>(FreeImage_GetHeight(fi_bitmap));
        auto *temp_data = FreeImage_GetBits(fi_bitmap);
        assert(temp_data != nullptr && "Failed to load image");
        // auto pixel_size = FreeImage_GetBPP(fi_bitmap);
        // if (pixel_size != 32) {
        //     auto *temp = FreeImage_ConvertTo32Bits(fi_bitmap);
        //     FreeImage_Unload(fi_bitmap);
        //     fi_bitmap = temp;
        // }
        auto size = static_cast<daxa_u32>(size_x) * static_cast<daxa_u32>(size_y) * 1 * 4;

        test_texture = device.create_image({
            .dimensions = 2,
            .format = daxa::Format::R32_SFLOAT,
            .size = {static_cast<daxa_u32>(size_x), static_cast<daxa_u32>(size_y), 1},
            .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "test_texture",
        });

        auto texture_path2 = "C:/Users/gabe/Downloads/Rugged Terrain with Rocky Peaks/Rugged Terrain with Rocky Peaks Diffuse EXR.exr";
        auto fi_file_desc2 = FreeImage_GetFileType(texture_path2, 0);
        FIBITMAP *fi_bitmap2 = FreeImage_Load(fi_file_desc2, texture_path2);
        auto size_x2 = static_cast<uint32_t>(FreeImage_GetWidth(fi_bitmap2));
        auto size_y2 = static_cast<uint32_t>(FreeImage_GetHeight(fi_bitmap2));
        {
            auto *temp = FreeImage_ConvertToRGBAF(fi_bitmap2);
            FreeImage_Unload(fi_bitmap2);
            fi_bitmap2 = temp;
        }
        auto *temp_data2 = FreeImage_GetBits(fi_bitmap2);
        assert(temp_data2 != nullptr && "Failed to load image");
        auto size2 = static_cast<daxa_u32>(size_x2) * static_cast<daxa_u32>(size_y2) * 4 * 4;

        test_texture2 = device.create_image({
            .dimensions = 2,
            .format = daxa::Format::R32G32B32A32_SFLOAT,
            .size = {static_cast<daxa_u32>(size_x2), static_cast<daxa_u32>(size_y2), 1},
            .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_SAMPLED,
            .name = "test_texture",
        });

        task_test_texture.set_images({.images = std::array{test_texture}});
        task_test_texture2.set_images({.images = std::array{test_texture2}});
        temp_task_graph.use_persistent_image(task_test_texture);
        temp_task_graph.use_persistent_image(task_test_texture2);
        temp_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_test_texture),
                daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_test_texture2),
            },
            .task = [&, this](daxa::TaskInterface const &ti) {
                {
                    auto staging_buffer = ti.device.create_buffer({
                        .size = size,
                        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                        .name = "staging_buffer",
                    });
                    auto *buffer_ptr = ti.device.get_host_address_as<uint8_t>(staging_buffer).value();
                    std::copy(temp_data + 0, temp_data + size, buffer_ptr);
                    FreeImage_Unload(fi_bitmap);
                    ti.recorder.destroy_buffer_deferred(staging_buffer);
                    ti.recorder.copy_buffer_to_image({
                        .buffer = staging_buffer,
                        .image = task_test_texture.get_state().images[0],
                        .image_extent = {static_cast<daxa_u32>(size_x), static_cast<daxa_u32>(size_y), 1},
                    });
                }
                {
                    auto staging_buffer = ti.device.create_buffer({
                        .size = size2,
                        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                        .name = "staging_buffer",
                    });
                    auto *buffer_ptr = ti.device.get_host_address_as<uint8_t>(staging_buffer).value();
                    std::copy(temp_data2 + 0, temp_data2 + size2, buffer_ptr);
                    FreeImage_Unload(fi_bitmap2);
                    ti.recorder.destroy_buffer_deferred(staging_buffer);
                    ti.recorder.copy_buffer_to_image({
                        .buffer = staging_buffer,
                        .image = task_test_texture2.get_state().images[0],
                        .image_extent = {static_cast<daxa_u32>(size_x2), static_cast<daxa_u32>(size_y2), 1},
                    });
                }
            },
            .name = "upload_test_texture",
        });
        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    } else {
        task_test_texture.set_images({.images = std::array{debug_texture}});
        task_test_texture2.set_images({.images = std::array{debug_texture}});
    }
}

void GpuResources::destroy(daxa::Device &device) const {
    device.destroy_image(value_noise_image);
    device.destroy_image(blue_noise_vec2_image);
    if (!debug_texture.is_empty()) {
        device.destroy_image(debug_texture);
    }
    if (!test_texture.is_empty()) {
        device.destroy_image(test_texture);
    }
    if (!test_texture2.is_empty()) {
        device.destroy_image(test_texture2);
    }
    device.destroy_buffer(input_buffer);
    device.destroy_buffer(output_buffer);
    device.destroy_buffer(staging_output_buffer);
    device.destroy_buffer(globals_buffer);
    device.destroy_sampler(sampler_nnc);
    device.destroy_sampler(sampler_lnc);
    device.destroy_sampler(sampler_llc);
    device.destroy_sampler(sampler_llr);
}

void GpuResources::use_resources(RecordContext &record_ctx) {
    record_ctx.task_graph.use_persistent_image(task_value_noise_image);
    record_ctx.task_graph.use_persistent_image(task_blue_noise_vec2_image);
    record_ctx.task_graph.use_persistent_image(task_debug_texture);
    record_ctx.task_graph.use_persistent_image(task_test_texture);
    record_ctx.task_graph.use_persistent_image(task_test_texture2);

    record_ctx.task_graph.use_persistent_buffer(task_input_buffer);
    record_ctx.task_graph.use_persistent_buffer(task_output_buffer);
    record_ctx.task_graph.use_persistent_buffer(task_staging_output_buffer);
    record_ctx.task_graph.use_persistent_buffer(task_globals_buffer);

    record_ctx.task_blue_noise_vec2_image = task_blue_noise_vec2_image;
    record_ctx.task_debug_texture = task_debug_texture;
    record_ctx.task_test_texture = task_test_texture;
    record_ctx.task_test_texture2 = task_test_texture2;
    record_ctx.task_input_buffer = task_input_buffer;
    record_ctx.task_globals_buffer = task_globals_buffer;
}

void GpuResources::update_seeded_value_noise(daxa::Device &device, uint64_t seed) {
    daxa::TaskGraph temp_task_graph = daxa::TaskGraph({
        .device = device,
        .name = "temp_task_graph",
    });
    temp_task_graph.use_persistent_image(task_value_noise_image);
    temp_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, daxa::ImageViewType::REGULAR_2D, task_value_noise_image.view().view({.layer_count = 256})),
        },
        .task = [this, seed](daxa::TaskInterface const &ti) {
            auto staging_buffer = ti.device.create_buffer({
                .size = static_cast<daxa_u32>(256 * 256 * 256 * 1),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "staging_buffer",
            });
            auto *buffer_ptr = ti.device.get_host_address_as<uint8_t>(staging_buffer).value();
            std::mt19937_64 rng(seed);
            std::uniform_int_distribution<std::mt19937::result_type> dist(0, 255);
            for (daxa_u32 i = 0; i < (256 * 256 * 256 * 1); ++i) {
                buffer_ptr[i] = dist(rng) & 0xff;
            }
            ti.recorder.pipeline_barrier({
                .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
            });
            ti.recorder.destroy_buffer_deferred(staging_buffer);
            for (daxa_u32 i = 0; i < 256; ++i) {
                ti.recorder.copy_buffer_to_image({
                    .buffer = staging_buffer,
                    .buffer_offset = 256 * 256 * i,
                    .image = task_value_noise_image.get_state().images[0],
                    .image_slice{
                        .base_array_layer = i,
                        .layer_count = 1,
                    },
                    .image_extent = {256, 256, 1},
                });
            }
        },
        .name = "upload_value_noise",
    });
    temp_task_graph.submit({});
    temp_task_graph.complete({});
    temp_task_graph.execute({});
}