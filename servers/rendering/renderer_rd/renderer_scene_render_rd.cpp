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

void get_vogel_disk(float *r_kernel, int p_sample_count) {
	const float golden_angle = 2.4;

	for (int i = 0; i < p_sample_count; i++) {
		float r = Math::sqrt(float(i) + 0.5) / Math::sqrt(float(p_sample_count));
		float theta = float(i) * golden_angle;

		r_kernel[i * 4] = Math::cos(theta) * r;
		r_kernel[i * 4 + 1] = Math::sin(theta) * r;
	}
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

	// ERR_FAIL_COND_MSG(p_render_data->reflection_probe.is_valid(), "Post processes should not be applied on reflection probes.");

	// Glow, auto exposure and DoF (if enabled).

	Size2i internal_size = rb->get_internal_size();
	Size2i target_size = rb->get_target_size();

	bool can_use_effects = target_size.x >= 8 && target_size.y >= 8; // FIXME I think this should check internal size, we do all our post processing at this size...
	bool can_use_storage = _render_buffers_can_be_storage();

	RID render_target = rb->get_render_target();
	RID internal_texture = rb->get_internal_texture();

	float auto_exposure_scale = 1.0;

	int max_glow_level = -1;

	{
		RENDER_TIMESTAMP("Tonemap");
		RD::get_singleton()->draw_command_begin_label("Tonemap");

		RendererRD::ToneMapper::TonemapSettings tonemap;

		tonemap.exposure_texture = luminance->get_current_luminance_buffer(rb);

		tonemap.exposure_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

		tonemap.glow_texture = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		tonemap.glow_map = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

		// if (rb->get_screen_space_aa() == RS::VIEWPORT_SCREEN_SPACE_AA_FXAA) {
		// 	tonemap.use_fxaa = true;
		// }

		tonemap.use_debanding = false;
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

float RendererSceneRenderRD::_render_buffers_get_luminance_multiplier() {
	return 1.0;
}

RD::DataFormat RendererSceneRenderRD::_render_buffers_get_color_format() {
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

bool RendererSceneRenderRD::_render_buffers_can_be_storage() {
	return true;
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

bool RendererSceneRenderRD::free(RID p_rid) {
	return true;
}

void RendererSceneRenderRD::set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) {
	debug_draw = p_debug_draw;
}

void RendererSceneRenderRD::update() {
}

void RendererSceneRenderRD::set_time(double p_time, double p_step) {
	time = p_time;
	time_step = p_step;
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

	/* GI */

	{ //decals
		RendererRD::TextureStorage::get_singleton()->set_max_decals(max_cluster_elements);
	}

	{ //lights
	}

	use_physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");

	glow_bicubic_upscale = int(GLOBAL_GET("rendering/environment/glow/upscale_mode")) > 0;

	directional_penumbra_shadow_kernel = memnew_arr(float, 128);
	directional_soft_shadow_kernel = memnew_arr(float, 128);
	penumbra_shadow_kernel = memnew_arr(float, 128);
	soft_shadow_kernel = memnew_arr(float, 128);

	decals_set_filter(RS::DecalFilter(int(GLOBAL_GET("rendering/textures/decals/filter"))));
	light_projectors_set_filter(RS::LightProjectorFilter(int(GLOBAL_GET("rendering/textures/light_projectors/filter"))));

	cull_argument.set_page_pool(&cull_argument_pool);

	bool can_use_storage = _render_buffers_can_be_storage();
	copy_effects = memnew(RendererRD::CopyEffects(!can_use_storage));
	luminance = memnew(RendererRD::Luminance(!can_use_storage));
	tone_mapper = memnew(RendererRD::ToneMapper);
}

RendererSceneRenderRD::~RendererSceneRenderRD() {
	if (forward_id_storage) {
		memdelete(forward_id_storage);
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

	memdelete_arr(directional_penumbra_shadow_kernel);
	memdelete_arr(directional_soft_shadow_kernel);
	memdelete_arr(penumbra_shadow_kernel);
	memdelete_arr(soft_shadow_kernel);

	RSG::light_storage->directional_shadow_atlas_set_size(0);
	cull_argument.reset(); //avoid exit error
}
