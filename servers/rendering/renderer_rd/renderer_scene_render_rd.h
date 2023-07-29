/**************************************************************************/
/*  renderer_scene_render_rd.h                                            */
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

#ifndef RENDERER_SCENE_RENDER_RD_H
#define RENDERER_SCENE_RENDER_RD_H

#include "core/templates/local_vector.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/cluster_builder_rd.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/effects/luminance.h"
#include "servers/rendering/renderer_rd/effects/tone_mapper.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_method.h"

// For RenderDataRD, possibly inherited from RefCounted and add proper getters for our implementation classes

struct RenderDataRD {
	Ref<RenderSceneBuffersRD> render_buffers;
	RenderSceneDataRD *scene_data;

	const PagedArray<RenderGeometryInstance *> *instances = nullptr;
	const PagedArray<RID> *lights = nullptr;
	const PagedArray<RID> *decals = nullptr;
	const PagedArray<RID> *lightmaps = nullptr;
	RID environment;
	RID shadow_atlas;
	RID occluder_debug_tex;

	RID cluster_buffer;
	uint32_t cluster_size = 0;
	uint32_t cluster_max_elements = 0;

	uint32_t directional_light_count = 0;
	bool directional_light_soft_shadows = false;

	RenderingMethod::RenderInfo *render_info = nullptr;

	/* Shadow data */
	const RendererSceneRender::RenderShadowData *render_shadows = nullptr;
	int render_shadow_count = 0;

	LocalVector<int> cube_shadows;
	LocalVector<int> shadows;
	LocalVector<int> directional_shadows;

	/* GI info */
	const RendererSceneRender::RenderSDFGIData *render_sdfgi_regions = nullptr;
	int render_sdfgi_region_count = 0;
	const RendererSceneRender::RenderSDFGIUpdateData *sdfgi_update_data = nullptr;

	uint32_t voxel_gi_count = 0;
};

class RendererSceneRenderRD : public RendererSceneRender {
protected:
	RendererRD::ForwardIDStorage *forward_id_storage = nullptr;
	RendererRD::CopyEffects *copy_effects = nullptr;
	RendererRD::Luminance *luminance = nullptr;
	RendererRD::ToneMapper *tone_mapper = nullptr;
	double time = 0.0;
	double time_step = 0.0;

	/* ENVIRONMENT */

	bool glow_bicubic_upscale = false;

	bool use_physical_light_units = false;

	////////////////////////////////

	virtual RendererRD::ForwardIDStorage *create_forward_id_storage() { return memnew(RendererRD::ForwardIDStorage); };

	virtual void setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) = 0;

	// virtual void _render_scene(RenderDataRD *p_render_data, const Color &p_default_color) = 0;
	virtual void _render_buffers_debug_draw(Ref<RenderSceneBuffersRD> p_render_buffers, RID p_shadow_atlas, RID p_occlusion_buffer);

	virtual void _render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region, float p_exposure_normalization) = 0;
	virtual void _render_uv2(const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) = 0;

	virtual RID _render_buffers_get_normal_texture(Ref<RenderSceneBuffersRD> p_render_buffers) = 0;
	virtual RID _render_buffers_get_velocity_texture(Ref<RenderSceneBuffersRD> p_render_buffers) = 0;

	bool _needs_post_prepass_render(RenderDataRD *p_render_data, bool p_use_gi);
	void _post_prepass_render(RenderDataRD *p_render_data, bool p_use_gi);
	void _pre_resolve_render(RenderDataRD *p_render_data, bool p_use_gi);

	void _render_buffers_copy_screen_texture(const RenderDataRD *p_render_data);
	void _render_buffers_copy_depth_texture(const RenderDataRD *p_render_data);
	void _render_buffers_post_process_and_tonemap(const RenderDataRD *p_render_data);
	void _disable_clear_request(const RenderDataRD *p_render_data);

	// needed for a single argument calls (material and uv2)
	PagedArrayPool<RenderGeometryInstance *> cull_argument_pool;
	PagedArray<RenderGeometryInstance *> cull_argument; //need this to exist

	virtual void _update_shader_quality_settings() {}

private:
	RS::ViewportDebugDraw debug_draw = RS::VIEWPORT_DEBUG_DRAW_DISABLED;
	static RendererSceneRenderRD *singleton;

	/* Shadow atlas */
	RS::ShadowQuality shadows_quality = RS::SHADOW_QUALITY_MAX; //So it always updates when first set
	RS::ShadowQuality directional_shadow_quality = RS::SHADOW_QUALITY_MAX;
	float shadows_quality_radius = 1.0;
	float directional_shadow_quality_radius = 1.0;

	float *directional_penumbra_shadow_kernel = nullptr;
	float *directional_soft_shadow_kernel = nullptr;
	float *penumbra_shadow_kernel = nullptr;
	float *soft_shadow_kernel = nullptr;
	int directional_penumbra_shadow_samples = 0;
	int directional_soft_shadow_samples = 0;
	int penumbra_shadow_samples = 0;
	int soft_shadow_samples = 0;
	RS::DecalFilter decals_filter = RS::DECAL_FILTER_LINEAR_MIPMAPS;
	RS::LightProjectorFilter light_projectors_filter = RS::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS;

	/* RENDER BUFFERS */

	/* Light data */

	uint64_t scene_pass = 0;

	uint32_t max_cluster_elements = 512;

	/* Volumetric Fog */

	uint32_t volumetric_fog_size = 128;
	uint32_t volumetric_fog_depth = 128;
	bool volumetric_fog_filter_active = true;

public:
	static RendererSceneRenderRD *get_singleton() { return singleton; }

	/* LIGHTING */

	virtual void setup_added_light(const RS::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture){};
	virtual void setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size){};

	/* ENVIRONMENT API */

	_FORCE_INLINE_ bool is_using_physical_light_units() {
		return use_physical_light_units;
	}

	/* render buffers */

	virtual float _render_buffers_get_luminance_multiplier();
	virtual RD::DataFormat _render_buffers_get_color_format();
	virtual bool _render_buffers_can_be_storage();
	virtual Ref<RenderSceneBuffers> render_buffers_create() override;

	virtual void base_uniforms_changed() = 0;
	virtual void update_uniform_sets(){};

	virtual void render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) override;

	virtual void set_scene_pass(uint64_t p_pass) override {
		scene_pass = p_pass;
	}
	_FORCE_INLINE_ uint64_t get_scene_pass() {
		return scene_pass;
	}

	virtual void decals_set_filter(RS::DecalFilter p_filter) override;
	virtual void light_projectors_set_filter(RS::LightProjectorFilter p_filter) override;

	_FORCE_INLINE_ RS::ShadowQuality shadows_quality_get() const {
		return shadows_quality;
	}
	_FORCE_INLINE_ RS::ShadowQuality directional_shadow_quality_get() const {
		return directional_shadow_quality;
	}
	_FORCE_INLINE_ float shadows_quality_radius_get() const {
		return shadows_quality_radius;
	}
	_FORCE_INLINE_ float directional_shadow_quality_radius_get() const {
		return directional_shadow_quality_radius;
	}

	_FORCE_INLINE_ float *directional_penumbra_shadow_kernel_get() {
		return directional_penumbra_shadow_kernel;
	}
	_FORCE_INLINE_ float *directional_soft_shadow_kernel_get() {
		return directional_soft_shadow_kernel;
	}
	_FORCE_INLINE_ float *penumbra_shadow_kernel_get() {
		return penumbra_shadow_kernel;
	}
	_FORCE_INLINE_ float *soft_shadow_kernel_get() {
		return soft_shadow_kernel;
	}

	_FORCE_INLINE_ int directional_penumbra_shadow_samples_get() const {
		return directional_penumbra_shadow_samples;
	}
	_FORCE_INLINE_ int directional_soft_shadow_samples_get() const {
		return directional_soft_shadow_samples;
	}
	_FORCE_INLINE_ int penumbra_shadow_samples_get() const {
		return penumbra_shadow_samples;
	}
	_FORCE_INLINE_ int soft_shadow_samples_get() const {
		return soft_shadow_samples;
	}

	_FORCE_INLINE_ RS::LightProjectorFilter light_projectors_get_filter() const {
		return light_projectors_filter;
	}
	_FORCE_INLINE_ RS::DecalFilter decals_get_filter() const {
		return decals_filter;
	}

	virtual bool free(RID p_rid) override;

	virtual void update() override;

	virtual void set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) override;
	_FORCE_INLINE_ RS::ViewportDebugDraw get_debug_draw_mode() const {
		return debug_draw;
	}

	virtual void set_time(double p_time, double p_step) override;

	virtual uint32_t get_max_elements() const;

	void init();

	RendererSceneRenderRD();
	~RendererSceneRenderRD();
};

#endif // RENDERER_SCENE_RENDER_RD_H
