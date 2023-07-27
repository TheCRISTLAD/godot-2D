/**************************************************************************/
/*  renderer_scene_render_rd.cpp                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "renderer_scene_render_rd.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/storage/camera_attributes_storage.h"

void get_vogel_disk(float *r_kernel, int p_sample_count) {
	const float golden_angle = 2.4;

	for (int i = 0; i < p_sample_count; i++) {
		float r = Math::sqrt(float(i) + 0.5) / Math::sqrt(float(p_sample_count));
		float theta = float(i) * golden_angle;

		r_kernel[i * 4] = Math::cos(theta) * r;
		r_kernel[i * 4 + 1] = Math::sin(theta) * r;
	}
}

/* REFLECTION PROBE */

RID RendererSceneRenderRD::reflection_probe_create_framebuffer(RID p_color, RID p_depth) {
	Vector<RID> fb;
	fb.push_back(p_color);
	fb.push_back(p_depth);
	return RD::get_singleton()->framebuffer_create(fb);
}

////////////////////////////////
Ref<RenderSceneBuffers> RendererSceneRenderRD::render_buffers_create() {
	Ref<RenderSceneBuffersRD> rb;
	rb.instantiate();

	rb->set_can_be_storage(_render_buffers_can_be_storage());
	rb->set_max_cluster_elements(max_cluster_elements);
	rb->set_base_data_format(_render_buffers_get_color_format());

	setup_render_buffer_data(rb);

	return rb;
}

void RendererSceneRenderRD::_render_buffers_copy_screen_texture(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	if (!rb->has_internal_texture()) {
		// We're likely rendering reflection probes where we can't use our backbuffers.
		return;
	}

	RD::get_singleton()->draw_command_begin_label("Copy screen texture");

	rb->allocate_blur_textures();

	bool can_use_storage = _render_buffers_can_be_storage();
	Size2i size = rb->get_internal_size();

	for (uint32_t v = 0; v < rb->get_view_count(); v++) {
		RID texture = rb->get_internal_texture(v);
		int mipmaps = int(rb->get_texture_format(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0).mipmaps);
		RID dest = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0, v, 0);

		if (can_use_storage) {
			copy_effects->copy_to_rect(texture, dest, Rect2i(0, 0, size.x, size.y));
		} else {
			RID fb = FramebufferCacheRD::get_singleton()->get_cache(dest);
			copy_effects->copy_to_fb_rect(texture, fb, Rect2i(0, 0, size.x, size.y));
		}

		for (int i = 1; i < mipmaps; i++) {
			RID source = dest;
			dest = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0, v, i);
			Size2i msize = rb->get_texture_slice_size(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0, i);

			if (can_use_storage) {
				copy_effects->make_mipmap(source, dest, msize);
			} else {
				copy_effects->make_mipmap_raster(source, dest, msize);
			}
		}
	}

	RD::get_singleton()->draw_command_end_label();
}

void RendererSceneRenderRD::_render_buffers_copy_depth_texture(const RenderDataRD *p_render_data) {
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	if (!rb->has_depth_texture()) {
		// We're likely rendering reflection probes where we can't use our backbuffers.
		return;
	}

	RD::get_singleton()->draw_command_begin_label("Copy depth texture");

	// note, this only creates our back depth texture if we haven't already created it.
	uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT;
	usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	usage_bits |= RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT; // set this as color attachment because we're copying data into it, it's not actually used as a depth buffer

	rb->create_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH, RD::DATA_FORMAT_R32_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1);

	bool can_use_storage = _render_buffers_can_be_storage();
	Size2i size = rb->get_internal_size();
	for (uint32_t v = 0; v < p_render_data->scene_data->view_count; v++) {
		RID depth_texture = rb->get_depth_texture(v);
		RID depth_back_texture = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH, v, 0);

		if (can_use_storage) {
			copy_effects->copy_to_rect(depth_texture, depth_back_texture, Rect2i(0, 0, size.x, size.y));
		} else {
			RID depth_back_fb = FramebufferCacheRD::get_singleton()->get_cache(depth_back_texture);
			copy_effects->copy_to_fb_rect(depth_texture, depth_back_fb, Rect2i(0, 0, size.x, size.y));
		}
	}

	RD::get_singleton()->draw_command_end_label();
}

void RendererSceneRenderRD::_render_buffers_post_process_and_tonemap(const RenderDataRD *p_render_data) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	ERR_FAIL_NULL(p_render_data);

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND(rb.is_null());

	ERR_FAIL_COND_MSG(p_render_data->reflection_probe.is_valid(), "Post processes should not be applied on reflection probes.");

	// Glow, auto exposure and DoF (if enabled).

	Size2i internal_size = rb->get_internal_size();
	Size2i target_size = rb->get_target_size();

	bool can_use_effects = target_size.x >= 8 && target_size.y >= 8; // FIXME I think this should check internal size, we do all our post processing at this size...
	bool can_use_storage = _render_buffers_can_be_storage();

	RID render_target = rb->get_render_target();
	RID internal_texture = rb->get_internal_texture();

	if (can_use_effects && RSG::camera_attributes->camera_attributes_uses_dof(p_render_data->camera_attributes)) {
		RENDER_TIMESTAMP("Depth of Field");
		RD::get_singleton()->draw_command_begin_label("DOF");

		rb->allocate_blur_textures();

		RendererRD::BokehDOF::BokehBuffers buffers;

		// Textures we use
		buffers.base_texture_size = rb->get_internal_size();
		buffers.secondary_texture = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0, 0, 0);
		buffers.half_texture[0] = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BLUR_1, 0, 0);
		buffers.half_texture[1] = rb->get_texture_slice(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0, 0, 1);

		if (can_use_storage) {
			for (uint32_t i = 0; i < rb->get_view_count(); i++) {
				buffers.base_texture = rb->get_internal_texture(i);
				buffers.depth_texture = rb->get_depth_texture(i);

				// In stereo p_render_data->z_near and p_render_data->z_far can be offset for our combined frustrum
				float z_near = p_render_data->scene_data->view_projection[i].get_z_near();
				float z_far = p_render_data->scene_data->view_projection[i].get_z_far();
				bokeh_dof->bokeh_dof_compute(buffers, p_render_data->camera_attributes, z_near, z_far, p_render_data->scene_data->cam_orthogonal);
			};
		} else {
			// Set framebuffers.
			buffers.secondary_fb = rb->weight_buffers[1].fb;
			buffers.half_fb[0] = rb->weight_buffers[2].fb;
			buffers.half_fb[1] = rb->weight_buffers[3].fb;
			buffers.weight_texture[0] = rb->weight_buffers[0].weight;
			buffers.weight_texture[1] = rb->weight_buffers[1].weight;
			buffers.weight_texture[2] = rb->weight_buffers[2].weight;
			buffers.weight_texture[3] = rb->weight_buffers[3].weight;

			// Set weight buffers.
			buffers.base_weight_fb = rb->weight_buffers[0].fb;

			for (uint32_t i = 0; i < rb->get_view_count(); i++) {
				buffers.base_texture = rb->get_internal_texture(i);
				buffers.depth_texture = rb->get_depth_texture(i);
				buffers.base_fb = FramebufferCacheRD::get_singleton()->get_cache(buffers.base_texture); // TODO move this into bokeh_dof_raster, we can do this internally

				// In stereo p_render_data->z_near and p_render_data->z_far can be offset for our combined frustrum
				float z_near = p_render_data->scene_data->view_projection[i].get_z_near();
				float z_far = p_render_data->scene_data->view_projection[i].get_z_far();
				bokeh_dof->bokeh_dof_raster(buffers, p_render_data->camera_attributes, z_near, z_far, p_render_data->scene_data->cam_orthogonal);
			}
		}
		RD::get_singleton()->draw_command_end_label();
	}

	float auto_exposure_scale = 1.0;

	if (can_use_effects && RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes)) {
		RENDER_TIMESTAMP("Auto exposure");

		RD::get_singleton()->draw_command_begin_label("Auto exposure");

		Ref<RendererRD::Luminance::LuminanceBuffers> luminance_buffers = luminance->get_luminance_buffers(rb);

		uint64_t auto_exposure_version = RSG::camera_attributes->camera_attributes_get_auto_exposure_version(p_render_data->camera_attributes);
		bool set_immediate = auto_exposure_version != rb->get_auto_exposure_version();
		rb->set_auto_exposure_version(auto_exposure_version);

		double step = RSG::camera_attributes->camera_attributes_get_auto_exposure_adjust_speed(p_render_data->camera_attributes) * time_step;
		float auto_exposure_min_sensitivity = RSG::camera_attributes->camera_attributes_get_auto_exposure_min_sensitivity(p_render_data->camera_attributes);
		float auto_exposure_max_sensitivity = RSG::camera_attributes->camera_attributes_get_auto_exposure_max_sensitivity(p_render_data->camera_attributes);
		luminance->luminance_reduction(internal_texture, internal_size, luminance_buffers, auto_exposure_min_sensitivity, auto_exposure_max_sensitivity, step, set_immediate);

		// Swap final reduce with prev luminance.

		auto_exposure_scale = RSG::camera_attributes->camera_attributes_get_auto_exposure_scale(p_render_data->camera_attributes);

		RenderingServerDefault::redraw_request(); // Redraw all the time if auto exposure rendering is on.
		RD::get_singleton()->draw_command_end_label();
	}

	int max_glow_level = -1;

	{
		RENDER_TIMESTAMP("Tonemap");
		RD::get_singleton()->draw_command_begin_label("Tonemap");

		RendererRD::ToneMapper::TonemapSettings tonemap;

		tonemap.exposure_texture = luminance->get_current_luminance_buffer(rb);
		if (can_use_effects && RSG::camera_attributes->camera_attributes_uses_auto_exposure(p_render_data->camera_attributes) && tonemap.exposure_texture.is_valid()) {
			tonemap.use_auto_exposure = true;
			tonemap.auto_exposure_scale = auto_exposure_scale;
		} else {
			tonemap.exposure_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
		}

		tonemap.glow_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		tonemap.glow_map = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

		if (rb->get_screen_space_aa() == RS::VIEWPORT_SCREEN_SPACE_AA_FXAA) {
			tonemap.use_fxaa = true;
		}

		tonemap.use_debanding = rb->get_use_debanding();
		tonemap.texture_size = Vector2i(rb->get_internal_size().x, rb->get_internal_size().y);

		tonemap.use_color_correction = false;
		tonemap.use_1d_color_correction = false;
		tonemap.color_correction_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);

		tonemap.luminance_multiplier = _render_buffers_get_luminance_multiplier();
		tonemap.view_count = rb->get_view_count();

		RID dest_fb;

		// If we do a bilinear upscale we just render into our render target and our shader will upscale automatically.
		// Target size in this case is lying as we never get our real target size communicated.
		// Bit nasty but...
		dest_fb = texture_storage->render_target_get_rd_framebuffer(render_target);

		tone_mapper->tonemapper(internal_texture, dest_fb, tonemap);

		RD::get_singleton()->draw_command_end_label();
	}

	texture_storage->render_target_disable_clear_request(render_target);
}

void RendererSceneRenderRD::_disable_clear_request(const RenderDataRD *p_render_data) {
	ERR_FAIL_COND(p_render_data->render_buffers.is_null());

	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	texture_storage->render_target_disable_clear_request(p_render_data->render_buffers->get_render_target());
}

void RendererSceneRenderRD::_render_buffers_debug_draw(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_shadow_atlas, RID p_occlusion_buffer) {
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	ERR_FAIL_COND(p_render_buffers.is_null());

	RID render_target = p_render_buffers->get_render_target();

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_SHADOW_ATLAS) {
		if (p_shadow_atlas.is_valid()) {
			RID shadow_atlas_texture = RendererRD::LightStorage::get_singleton()->shadow_atlas_get_texture(p_shadow_atlas);

			if (shadow_atlas_texture.is_null()) {
				shadow_atlas_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
			}

			Size2 rtsize = texture_storage->render_target_get_size(render_target);
			copy_effects->copy_to_fb_rect(shadow_atlas_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Vector2(), rtsize / 2), false, true);
		}
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_DIRECTIONAL_SHADOW_ATLAS) {
		if (RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture().is_valid()) {
			RID shadow_atlas_texture = RendererRD::LightStorage::get_singleton()->directional_shadow_get_texture();
			Size2i rtsize = texture_storage->render_target_get_size(render_target);

			copy_effects->copy_to_fb_rect(shadow_atlas_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Vector2(), rtsize / 2), false, true);
		}
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_DECAL_ATLAS) {
		RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture();

		if (decal_atlas.is_valid()) {
			Size2i rtsize = texture_storage->render_target_get_size(render_target);

			copy_effects->copy_to_fb_rect(decal_atlas, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Vector2(), rtsize / 2), false, false, true);
		}
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_SCENE_LUMINANCE) {
		RID luminance_texture = luminance->get_current_luminance_buffer(p_render_buffers);
		if (luminance_texture.is_valid()) {
			Size2i rtsize = texture_storage->render_target_get_size(render_target);

			copy_effects->copy_to_fb_rect(luminance_texture, texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize / 8), false, true);
		}
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER && _render_buffers_get_normal_texture(p_render_buffers).is_valid()) {
		Size2 rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(_render_buffers_get_normal_texture(p_render_buffers), texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_OCCLUDERS) {
		if (p_occlusion_buffer.is_valid()) {
			Size2i rtsize = texture_storage->render_target_get_size(render_target);
			copy_effects->copy_to_fb_rect(texture_storage->texture_get_rd_texture(p_occlusion_buffer), texture_storage->render_target_get_rd_framebuffer(render_target), Rect2i(Vector2(), rtsize), true, false);
		}
	}

	if (debug_draw == RS::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS && _render_buffers_get_velocity_texture(p_render_buffers).is_valid()) {
		Size2i rtsize = texture_storage->render_target_get_size(render_target);
		copy_effects->copy_to_fb_rect(_render_buffers_get_velocity_texture(p_render_buffers), texture_storage->render_target_get_rd_framebuffer(render_target), Rect2(Vector2(), rtsize), false, false);
	}
}

float RendererSceneRenderRD::_render_buffers_get_luminance_multiplier() {
	return 1.0;
}

RD::DataFormat RendererSceneRenderRD::_render_buffers_get_color_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

bool RendererSceneRenderRD::_render_buffers_can_be_storage() {
	return true;
}

void RendererSceneRenderRD::positional_soft_shadow_filter_set_quality(RS::ShadowQuality p_quality) {
	ERR_FAIL_INDEX_MSG(p_quality, RS::SHADOW_QUALITY_MAX, "Shadow quality too high, please see RenderingServer's ShadowQuality enum");

	if (shadows_quality != p_quality) {
		shadows_quality = p_quality;

		switch (shadows_quality) {
			case RS::SHADOW_QUALITY_HARD: {
				penumbra_shadow_samples = 4;
				soft_shadow_samples = 0;
				shadows_quality_radius = 1.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_VERY_LOW: {
				penumbra_shadow_samples = 4;
				soft_shadow_samples = 1;
				shadows_quality_radius = 1.5;
			} break;
			case RS::SHADOW_QUALITY_SOFT_LOW: {
				penumbra_shadow_samples = 8;
				soft_shadow_samples = 4;
				shadows_quality_radius = 2.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_MEDIUM: {
				penumbra_shadow_samples = 12;
				soft_shadow_samples = 8;
				shadows_quality_radius = 2.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_HIGH: {
				penumbra_shadow_samples = 24;
				soft_shadow_samples = 16;
				shadows_quality_radius = 3.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_ULTRA: {
				penumbra_shadow_samples = 32;
				soft_shadow_samples = 32;
				shadows_quality_radius = 4.0;
			} break;
			case RS::SHADOW_QUALITY_MAX:
				break;
		}
		get_vogel_disk(penumbra_shadow_kernel, penumbra_shadow_samples);
		get_vogel_disk(soft_shadow_kernel, soft_shadow_samples);
	}

	_update_shader_quality_settings();
}

void RendererSceneRenderRD::directional_soft_shadow_filter_set_quality(RS::ShadowQuality p_quality) {
	ERR_FAIL_INDEX_MSG(p_quality, RS::SHADOW_QUALITY_MAX, "Shadow quality too high, please see RenderingServer's ShadowQuality enum");

	if (directional_shadow_quality != p_quality) {
		directional_shadow_quality = p_quality;

		switch (directional_shadow_quality) {
			case RS::SHADOW_QUALITY_HARD: {
				directional_penumbra_shadow_samples = 4;
				directional_soft_shadow_samples = 0;
				directional_shadow_quality_radius = 1.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_VERY_LOW: {
				directional_penumbra_shadow_samples = 4;
				directional_soft_shadow_samples = 1;
				directional_shadow_quality_radius = 1.5;
			} break;
			case RS::SHADOW_QUALITY_SOFT_LOW: {
				directional_penumbra_shadow_samples = 8;
				directional_soft_shadow_samples = 4;
				directional_shadow_quality_radius = 2.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_MEDIUM: {
				directional_penumbra_shadow_samples = 12;
				directional_soft_shadow_samples = 8;
				directional_shadow_quality_radius = 2.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_HIGH: {
				directional_penumbra_shadow_samples = 24;
				directional_soft_shadow_samples = 16;
				directional_shadow_quality_radius = 3.0;
			} break;
			case RS::SHADOW_QUALITY_SOFT_ULTRA: {
				directional_penumbra_shadow_samples = 32;
				directional_soft_shadow_samples = 32;
				directional_shadow_quality_radius = 4.0;
			} break;
			case RS::SHADOW_QUALITY_MAX:
				break;
		}
		get_vogel_disk(directional_penumbra_shadow_kernel, directional_penumbra_shadow_samples);
		get_vogel_disk(directional_soft_shadow_kernel, directional_soft_shadow_samples);
	}

	_update_shader_quality_settings();
}

void RendererSceneRenderRD::decals_set_filter(RenderingServer::DecalFilter p_filter) {
	if (decals_filter == p_filter) {
		return;
	}
	decals_filter = p_filter;
	_update_shader_quality_settings();
}
void RendererSceneRenderRD::light_projectors_set_filter(RenderingServer::LightProjectorFilter p_filter) {
	if (light_projectors_filter == p_filter) {
		return;
	}
	light_projectors_filter = p_filter;
	_update_shader_quality_settings();
}

int RendererSceneRenderRD::get_roughness_layers() const {
	return sky.roughness_layers;
}

bool RendererSceneRenderRD::is_using_radiance_cubemap_array() const {
	return sky.sky_use_cubemap_array;
}

bool RendererSceneRenderRD::_needs_post_prepass_render(RenderDataRD *p_render_data, bool p_use_gi) {
	return false;
}

void RendererSceneRenderRD::_post_prepass_render(RenderDataRD *p_render_data, bool p_use_gi) {
}

void RendererSceneRenderRD::_pre_resolve_render(RenderDataRD *p_render_data, bool p_use_gi) {
	if (p_render_data->render_buffers.is_valid()) {
		if (p_use_gi) {
			RD::get_singleton()->compute_list_end();
		}
	}
}

// void RendererSceneRenderRD::render_scene(const Ref<RenderSceneBuffers> &p_render_buffers, const CameraData *p_camera_data, const CameraData *p_prev_camera_data, const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const PagedArray<RID> &p_reflection_probes, const PagedArray<RID> &p_voxel_gi_instances, const PagedArray<RID> &p_decals, const PagedArray<RID> &p_lightmaps, const PagedArray<RID> &p_fog_volumes, RID p_environment, RID p_camera_attributes, RID p_shadow_atlas, RID p_occluder_debug_tex, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass, float p_screen_mesh_lod_threshold, const RenderShadowData *p_render_shadows, int p_render_shadow_count, const RenderSDFGIData *p_render_sdfgi_regions, int p_render_sdfgi_region_count, const RenderSDFGIUpdateData *p_sdfgi_update_data, RenderingMethod::RenderInfo *r_render_info) {
// 	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
// 	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

// 	// getting this here now so we can direct call a bunch of things more easily
// 	ERR_FAIL_COND(p_render_buffers.is_null());
// 	Ref<RenderSceneBuffersRD> rb = p_render_buffers;
// 	ERR_FAIL_COND(rb.is_null());

// 	// setup scene data
// 	RenderSceneDataRD scene_data;
// 	{
// 		// Our first camera is used by default
// 		scene_data.cam_transform = p_camera_data->main_transform;
// 		scene_data.cam_projection = p_camera_data->main_projection;
// 		scene_data.cam_orthogonal = p_camera_data->is_orthogonal;
// 		scene_data.camera_visible_layers = p_camera_data->visible_layers;
// 		scene_data.taa_jitter = p_camera_data->taa_jitter;

// 		scene_data.view_count = p_camera_data->view_count;
// 		for (uint32_t v = 0; v < p_camera_data->view_count; v++) {
// 			scene_data.view_eye_offset[v] = p_camera_data->view_offset[v].origin;
// 			scene_data.view_projection[v] = p_camera_data->view_projection[v];
// 		}

// 		scene_data.prev_cam_transform = p_prev_camera_data->main_transform;
// 		scene_data.prev_cam_projection = p_prev_camera_data->main_projection;
// 		scene_data.prev_taa_jitter = p_prev_camera_data->taa_jitter;

// 		for (uint32_t v = 0; v < p_camera_data->view_count; v++) {
// 			scene_data.prev_view_projection[v] = p_prev_camera_data->view_projection[v];
// 		}

// 		scene_data.z_near = p_camera_data->main_projection.get_z_near();
// 		scene_data.z_far = p_camera_data->main_projection.get_z_far();

// 		// this should be the same for all cameras..
// 		const float lod_distance_multiplier = p_camera_data->main_projection.get_lod_multiplier();

// 		// Also, take into account resolution scaling for the multiplier, since we have more leeway with quality
// 		// degradation visibility. Conversely, allow upwards scaling, too, for increased mesh detail at high res.
// 		const float scaling_3d_scale = GLOBAL_GET("rendering/scaling_3d/scale");
// 		scene_data.lod_distance_multiplier = lod_distance_multiplier * (1.0 / scaling_3d_scale);

// 		if (get_debug_draw_mode() == RS::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) {
// 			scene_data.screen_mesh_lod_threshold = 0.0;
// 		} else {
// 			scene_data.screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
// 		}

// 		if (p_shadow_atlas.is_valid()) {
// 			int shadow_atlas_size = light_storage->shadow_atlas_get_size(p_shadow_atlas);
// 			scene_data.shadow_atlas_pixel_size.x = 1.0 / shadow_atlas_size;
// 			scene_data.shadow_atlas_pixel_size.y = 1.0 / shadow_atlas_size;
// 		}
// 		{
// 			int directional_shadow_size = light_storage->directional_shadow_get_size();
// 			scene_data.directional_shadow_pixel_size.x = 1.0 / directional_shadow_size;
// 			scene_data.directional_shadow_pixel_size.y = 1.0 / directional_shadow_size;
// 		}

// 		scene_data.time = time;
// 		scene_data.time_step = time_step;
// 	}

// 	//assign render data
// 	RenderDataRD render_data;
// 	{
// 		render_data.render_buffers = rb;
// 		render_data.scene_data = &scene_data;

// 		render_data.instances = &p_instances;
// 		render_data.lights = &p_lights;
// 		render_data.reflection_probes = &p_reflection_probes;
// 		render_data.voxel_gi_instances = &p_voxel_gi_instances;
// 		render_data.decals = &p_decals;
// 		render_data.lightmaps = &p_lightmaps;
// 		render_data.fog_volumes = &p_fog_volumes;
// 		render_data.environment = p_environment;
// 		render_data.camera_attributes = p_camera_attributes;
// 		render_data.shadow_atlas = p_shadow_atlas;
// 		render_data.occluder_debug_tex = p_occluder_debug_tex;
// 		render_data.reflection_atlas = p_reflection_atlas;
// 		render_data.reflection_probe = p_reflection_probe;
// 		render_data.reflection_probe_pass = p_reflection_probe_pass;

// 		render_data.render_shadows = p_render_shadows;
// 		render_data.render_shadow_count = p_render_shadow_count;
// 		render_data.render_sdfgi_regions = p_render_sdfgi_regions;
// 		render_data.render_sdfgi_region_count = p_render_sdfgi_region_count;
// 		render_data.sdfgi_update_data = p_sdfgi_update_data;

// 		render_data.render_info = r_render_info;
// 	}

// 	PagedArray<RID> empty;

// 	if (get_debug_draw_mode() == RS::VIEWPORT_DEBUG_DRAW_UNSHADED) {
// 		render_data.lights = &empty;
// 		render_data.reflection_probes = &empty;
// 		render_data.voxel_gi_instances = &empty;
// 	}

// 	Color clear_color;
// 	if (p_render_buffers.is_valid() && p_reflection_probe.is_null()) {
// 		clear_color = texture_storage->render_target_get_clear_request_color(rb->get_render_target());
// 	} else {
// 		clear_color = RSG::texture_storage->get_default_clear_color();
// 	}

// 	//calls _pre_opaque_render between depth pre-pass and opaque pass
// 	_render_scene(&render_data, clear_color);
// }

void RendererSceneRenderRD::render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) {
	_render_material(p_cam_transform, p_cam_projection, p_cam_orthogonal, p_instances, p_framebuffer, p_region, 1.0);
}

bool RendererSceneRenderRD::free(RID p_rid) {
	if (RSG::camera_attributes->owns_camera_attributes(p_rid)) {
		RSG::camera_attributes->camera_attributes_free(p_rid);
	} else if (sky.sky_owner.owns(p_rid)) {
		sky.update_dirty_skys();
		sky.free_sky(p_rid);
	} else {
		return false;
	}

	return true;
}

void RendererSceneRenderRD::set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) {
	debug_draw = p_debug_draw;
}

void RendererSceneRenderRD::update() {
	sky.update_dirty_skys();
}

void RendererSceneRenderRD::set_time(double p_time, double p_step) {
	time = p_time;
	time_step = p_step;
}

void RendererSceneRenderRD::screen_space_roughness_limiter_set_active(bool p_enable, float p_amount, float p_limit) {
	screen_space_roughness_limiter = p_enable;
	screen_space_roughness_limiter_amount = p_amount;
	screen_space_roughness_limiter_limit = p_limit;
}

bool RendererSceneRenderRD::screen_space_roughness_limiter_is_active() const {
	return screen_space_roughness_limiter;
}

float RendererSceneRenderRD::screen_space_roughness_limiter_get_amount() const {
	return screen_space_roughness_limiter_amount;
}

float RendererSceneRenderRD::screen_space_roughness_limiter_get_limit() const {
	return screen_space_roughness_limiter_limit;
}

RendererSceneRenderRD *RendererSceneRenderRD::singleton = nullptr;

uint32_t RendererSceneRenderRD::get_max_elements() const {
	return GLOBAL_GET("rendering/limits/cluster_builder/max_clustered_elements");
}

RendererSceneRenderRD::RendererSceneRenderRD() {
	singleton = this;
}

void RendererSceneRenderRD::init() {
	max_cluster_elements = get_max_elements();
	RendererRD::LightStorage::get_singleton()->set_max_cluster_elements(max_cluster_elements);

	/* Forward ID */
	forward_id_storage = create_forward_id_storage();

	/* SKY SHADER */

	sky.init();

	/* GI */

	{ //decals
		RendererRD::TextureStorage::get_singleton()->set_max_decals(max_cluster_elements);
	}

	{ //lights
	}

	RSG::camera_attributes->camera_attributes_set_dof_blur_bokeh_shape(RS::DOFBokehShape(int(GLOBAL_GET("rendering/camera/depth_of_field/depth_of_field_bokeh_shape"))));
	RSG::camera_attributes->camera_attributes_set_dof_blur_quality(RS::DOFBlurQuality(int(GLOBAL_GET("rendering/camera/depth_of_field/depth_of_field_bokeh_quality"))), GLOBAL_GET("rendering/camera/depth_of_field/depth_of_field_use_jitter"));
	use_physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");

	screen_space_roughness_limiter = GLOBAL_GET("rendering/anti_aliasing/screen_space_roughness_limiter/enabled");
	screen_space_roughness_limiter_amount = GLOBAL_GET("rendering/anti_aliasing/screen_space_roughness_limiter/amount");
	screen_space_roughness_limiter_limit = GLOBAL_GET("rendering/anti_aliasing/screen_space_roughness_limiter/limit");
	glow_bicubic_upscale = int(GLOBAL_GET("rendering/environment/glow/upscale_mode")) > 0;

	directional_penumbra_shadow_kernel = memnew_arr(float, 128);
	directional_soft_shadow_kernel = memnew_arr(float, 128);
	penumbra_shadow_kernel = memnew_arr(float, 128);
	soft_shadow_kernel = memnew_arr(float, 128);
	positional_soft_shadow_filter_set_quality(RS::ShadowQuality(int(GLOBAL_GET("rendering/lights_and_shadows/positional_shadow/soft_shadow_filter_quality"))));
	directional_soft_shadow_filter_set_quality(RS::ShadowQuality(int(GLOBAL_GET("rendering/lights_and_shadows/directional_shadow/soft_shadow_filter_quality"))));

	decals_set_filter(RS::DecalFilter(int(GLOBAL_GET("rendering/textures/decals/filter"))));
	light_projectors_set_filter(RS::LightProjectorFilter(int(GLOBAL_GET("rendering/textures/light_projectors/filter"))));

	cull_argument.set_page_pool(&cull_argument_pool);

	bool can_use_storage = _render_buffers_can_be_storage();
	bokeh_dof = memnew(RendererRD::BokehDOF(!can_use_storage));
	copy_effects = memnew(RendererRD::CopyEffects(!can_use_storage));
	luminance = memnew(RendererRD::Luminance(!can_use_storage));
	tone_mapper = memnew(RendererRD::ToneMapper);
}

RendererSceneRenderRD::~RendererSceneRenderRD() {
	if (forward_id_storage) {
		memdelete(forward_id_storage);
	}

	if (bokeh_dof) {
		memdelete(bokeh_dof);
	}
	if (copy_effects) {
		memdelete(copy_effects);
	}
	if (luminance) {
		memdelete(luminance);
	}
	if (tone_mapper) {
		memdelete(tone_mapper);
	}

	if (sky.sky_scene_state.uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(sky.sky_scene_state.uniform_set)) {
		RD::get_singleton()->free(sky.sky_scene_state.uniform_set);
	}

	memdelete_arr(directional_penumbra_shadow_kernel);
	memdelete_arr(directional_soft_shadow_kernel);
	memdelete_arr(penumbra_shadow_kernel);
	memdelete_arr(soft_shadow_kernel);

	RSG::light_storage->directional_shadow_atlas_set_size(0);
	cull_argument.reset(); //avoid exit error
}
