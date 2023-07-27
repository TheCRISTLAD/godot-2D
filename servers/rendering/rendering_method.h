/**************************************************************************/
/*  rendering_method.h                                                    */
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

#ifndef RENDERING_METHOD_H
#define RENDERING_METHOD_H

#include "servers/rendering/storage/render_scene_buffers.h"
#include "servers/rendering_server.h"

class RenderingMethod {
public:
	virtual RID camera_allocate() = 0;
	virtual void camera_initialize(RID p_rid) = 0;

	virtual void camera_set_perspective(RID p_camera, float p_fovy_degrees, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_orthogonal(RID p_camera, float p_size, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_frustum(RID p_camera, float p_size, Vector2 p_offset, float p_z_near, float p_z_far) = 0;
	virtual void camera_set_transform(RID p_camera, const Transform3D &p_transform) = 0;
	virtual void camera_set_cull_mask(RID p_camera, uint32_t p_layers) = 0;
	virtual void camera_set_environment(RID p_camera, RID p_env) = 0;
	virtual void camera_set_camera_attributes(RID p_camera, RID p_attributes) = 0;
	virtual void camera_set_use_vertical_aspect(RID p_camera, bool p_enable) = 0;
	virtual bool is_camera(RID p_camera) const = 0;

	virtual RID occluder_allocate() = 0;
	virtual void occluder_initialize(RID p_occluder) = 0;
	virtual void occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) = 0;

	virtual RID instance_allocate() = 0;
	virtual void instance_initialize(RID p_rid) = 0;

	virtual void instance_set_layer_mask(RID p_instance, uint32_t p_mask) = 0;
	virtual void instance_set_pivot_data(RID p_instance, float p_sorting_offset, bool p_use_aabb_center) = 0;
	virtual void instance_set_transform(RID p_instance, const Transform3D &p_transform) = 0;
	virtual void instance_attach_object_instance_id(RID p_instance, ObjectID p_id) = 0;
	virtual void instance_set_blend_shape_weight(RID p_instance, int p_shape, float p_weight) = 0;
	virtual void instance_set_surface_override_material(RID p_instance, int p_surface, RID p_material) = 0;
	virtual void instance_set_visible(RID p_instance, bool p_visible) = 0;
	virtual void instance_geometry_set_transparency(RID p_instance, float p_transparency) = 0;

	virtual void instance_attach_skeleton(RID p_instance, RID p_skeleton) = 0;

	virtual void instance_set_extra_visibility_margin(RID p_instance, real_t p_margin) = 0;
	virtual void instance_set_visibility_parent(RID p_instance, RID p_parent_instance) = 0;

	virtual void instance_set_ignore_culling(RID p_instance, bool p_enabled) = 0;

	// don't use these in a game!
	virtual Vector<ObjectID> instances_cull_aabb(const AABB &p_aabb, RID p_scenario = RID()) const = 0;
	virtual Vector<ObjectID> instances_cull_ray(const Vector3 &p_from, const Vector3 &p_to, RID p_scenario = RID()) const = 0;
	virtual Vector<ObjectID> instances_cull_convex(const Vector<Plane> &p_convex, RID p_scenario = RID()) const = 0;

	virtual void instance_geometry_set_flag(RID p_instance, RS::InstanceFlags p_flags, bool p_enabled) = 0;
	virtual void instance_geometry_set_cast_shadows_setting(RID p_instance, RS::ShadowCastingSetting p_shadow_casting_setting) = 0;
	virtual void instance_geometry_set_material_override(RID p_instance, RID p_material) = 0;
	virtual void instance_geometry_set_material_overlay(RID p_instance, RID p_material) = 0;

	virtual void instance_geometry_set_visibility_range(RID p_instance, float p_min, float p_max, float p_min_margin, float p_max_margin, RS::VisibilityRangeFadeMode p_fade_mode) = 0;
	virtual void instance_geometry_set_lightmap(RID p_instance, RID p_lightmap, const Rect2 &p_lightmap_uv_scale, int p_slice_index) = 0;
	virtual void instance_geometry_set_lod_bias(RID p_instance, float p_lod_bias) = 0;
	virtual void instance_geometry_set_shader_parameter(RID p_instance, const StringName &p_parameter, const Variant &p_value) = 0;
	virtual Variant instance_geometry_get_shader_parameter(RID p_instance, const StringName &p_parameter) const = 0;
	virtual Variant instance_geometry_get_shader_parameter_default_value(RID p_instance, const StringName &p_parameter) const = 0;

	/* ENVIRONMENT API */

	// Glow

	virtual void screen_space_roughness_limiter_set_active(bool p_enable, float p_amount, float p_limit) = 0;
	virtual void sub_surface_scattering_set_quality(RS::SubSurfaceScatteringQuality p_quality) = 0;
	virtual void sub_surface_scattering_set_scale(float p_scale, float p_depth_scale) = 0;

	virtual void positional_soft_shadow_filter_set_quality(RS::ShadowQuality p_quality) = 0;
	virtual void directional_soft_shadow_filter_set_quality(RS::ShadowQuality p_quality) = 0;

	/* Render Buffers */

	virtual Ref<RenderSceneBuffers> render_buffers_create() = 0;

	virtual void set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) = 0;

	struct RenderInfo {
		int info[RS::VIEWPORT_RENDER_INFO_TYPE_MAX][RS::VIEWPORT_RENDER_INFO_MAX] = {};
	};

	virtual void update() = 0;
	virtual void update_visibility_notifiers() = 0;

	virtual void decals_set_filter(RS::DecalFilter p_filter) = 0;
	virtual void light_projectors_set_filter(RS::LightProjectorFilter p_filter) = 0;

	virtual bool free(RID p_rid) = 0;

	RenderingMethod();
	virtual ~RenderingMethod();
};

#endif // RENDERING_METHOD_H
