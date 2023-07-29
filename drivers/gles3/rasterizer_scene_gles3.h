/**************************************************************************/
/*  rasterizer_scene_gles3.h                                              */
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

#ifndef RASTERIZER_SCENE_GLES3_H
#define RASTERIZER_SCENE_GLES3_H

#ifdef GLES3_ENABLED

#include "core/math/projection.h"
#include "core/templates/paged_allocator.h"
#include "core/templates/rid_owner.h"
#include "core/templates/self_list.h"
#include "drivers/gles3/shaders/cubemap_filter.glsl.gen.h"
#include "drivers/gles3/shaders/sky.glsl.gen.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering_server.h"
#include "shader_gles3.h"
#include "storage/light_storage.h"
#include "storage/material_storage.h"
#include "storage/render_scene_buffers_gles3.h"
#include "storage/utilities.h"

enum RenderListType {
	RENDER_LIST_OPAQUE, //used for opaque objects
	RENDER_LIST_ALPHA, //used for transparent objects
	RENDER_LIST_SECONDARY, //used for shadows and other objects
	RENDER_LIST_MAX
};

enum PassMode {
	PASS_MODE_COLOR,
	PASS_MODE_COLOR_TRANSPARENT,
	PASS_MODE_COLOR_ADDITIVE,
	PASS_MODE_SHADOW,
	PASS_MODE_DEPTH,
};

// These should share as much as possible with SkyUniform Location
enum SceneUniformLocation {
	SCENE_TONEMAP_UNIFORM_LOCATION,
	SCENE_GLOBALS_UNIFORM_LOCATION,
	SCENE_DATA_UNIFORM_LOCATION,
	SCENE_MATERIAL_UNIFORM_LOCATION,
	SCENE_EMPTY, // Unused, put here to avoid conflicts with SKY_DIRECTIONAL_LIGHT_UNIFORM_LOCATION.
	SCENE_OMNILIGHT_UNIFORM_LOCATION,
	SCENE_SPOTLIGHT_UNIFORM_LOCATION,
	SCENE_DIRECTIONAL_LIGHT_UNIFORM_LOCATION,
	SCENE_MULTIVIEW_UNIFORM_LOCATION,
};

enum SkyUniformLocation {
	SKY_TONEMAP_UNIFORM_LOCATION,
	SKY_GLOBALS_UNIFORM_LOCATION,
	SKY_EMPTY, // Unused, put here to avoid conflicts with SCENE_DATA_UNIFORM_LOCATION.
	SKY_MATERIAL_UNIFORM_LOCATION,
	SKY_DIRECTIONAL_LIGHT_UNIFORM_LOCATION,
	SKY_MULTIVIEW_UNIFORM_LOCATION,
};

struct RenderDataGLES3 {
	Ref<RenderSceneBuffersGLES3> render_buffers;
	bool transparent_bg = false;

	Transform3D cam_transform;
	Transform3D inv_cam_transform;
	Projection cam_projection;
	bool cam_orthogonal = false;
	uint32_t camera_visible_layers = 0xFFFFFFFF;

	// For stereo rendering
	uint32_t view_count = 1;
	Vector3 view_eye_offset[RendererSceneRender::MAX_RENDER_VIEWS];
	Projection view_projection[RendererSceneRender::MAX_RENDER_VIEWS];

	float z_near = 0.0;
	float z_far = 0.0;

	const PagedArray<RenderGeometryInstance *> *instances = nullptr;
	const PagedArray<RID> *lights = nullptr;
	const PagedArray<RID> *reflection_probes = nullptr;
	RID environment;
	RID reflection_probe;
	int reflection_probe_pass = 0;

	float lod_distance_multiplier = 0.0;

	uint32_t directional_light_count = 0;
	uint32_t spot_light_count = 0;
	uint32_t omni_light_count = 0;

	RenderingMethod::RenderInfo *render_info = nullptr;
};

class RasterizerCanvasGLES3;

class RasterizerSceneGLES3 : public RendererSceneRender {
private:
	static RasterizerSceneGLES3 *singleton;
	RS::ViewportDebugDraw debug_draw = RS::VIEWPORT_DEBUG_DRAW_DISABLED;
	uint64_t scene_pass = 0;

	template <class T>
	struct InstanceSort {
		float depth;
		T *instance = nullptr;
		bool operator<(const InstanceSort &p_sort) const {
			return depth < p_sort.depth;
		}
	};

	struct SceneGlobals {
		RID shader_default_version;
		RID default_material;
		RID default_shader;
		RID cubemap_filter_shader_version;
	} scene_globals;

	/* LIGHT INSTANCE */

	struct LightData {
		float position[3];
		float inv_radius;

		float direction[3]; // Only used by SpotLight
		float size;

		float color[3];
		float attenuation;

		float inv_spot_attenuation;
		float cos_spot_angle;
		float specular_amount;
		float shadow_opacity;
	};
	static_assert(sizeof(LightData) % 16 == 0, "LightData size must be a multiple of 16 bytes");

	struct DirectionalLightData {
		float direction[3];
		float energy;

		float color[3];
		float size;

		uint32_t enabled; // For use by SkyShaders
		float pad[2];
		float specular;
	};
	static_assert(sizeof(DirectionalLightData) % 16 == 0, "DirectionalLightData size must be a multiple of 16 bytes");

	class GeometryInstanceGLES3;

	// Cached data for drawing surfaces
	struct GeometryInstanceSurface {
		enum {
			FLAG_PASS_DEPTH = 1,
			FLAG_PASS_OPAQUE = 2,
			FLAG_PASS_ALPHA = 4,
			FLAG_PASS_SHADOW = 8,
			FLAG_USES_SHARED_SHADOW_MATERIAL = 128,
			FLAG_USES_SCREEN_TEXTURE = 2048,
			FLAG_USES_DEPTH_TEXTURE = 4096,
			FLAG_USES_NORMAL_TEXTURE = 8192,
			FLAG_USES_DOUBLE_SIDED_SHADOWS = 16384,
		};

		union {
			struct {
				uint64_t lod_index : 8;
				uint64_t surface_index : 8;
				uint64_t geometry_id : 32;
				uint64_t material_id_low : 16;

				uint64_t material_id_hi : 16;
				uint64_t shader_id : 32;
				uint64_t uses_softshadow : 1;
				uint64_t uses_projector : 1;
				uint64_t uses_forward_gi : 1;
				uint64_t uses_lightmap : 1;
				uint64_t depth_layer : 4;
				uint64_t priority : 8;
			};
			struct {
				uint64_t sort_key1;
				uint64_t sort_key2;
			};
		} sort;

		RS::PrimitiveType primitive = RS::PRIMITIVE_MAX;
		uint32_t flags = 0;
		uint32_t surface_index = 0;
		uint32_t lod_index = 0;
		uint32_t index_count = 0;

		void *surface = nullptr;
		GLES3::SceneShaderData *shader = nullptr;
		GLES3::SceneMaterialData *material = nullptr;

		void *surface_shadow = nullptr;
		GLES3::SceneShaderData *shader_shadow = nullptr;
		GLES3::SceneMaterialData *material_shadow = nullptr;

		GeometryInstanceSurface *next = nullptr;
		GeometryInstanceGLES3 *owner = nullptr;
	};

	class GeometryInstanceGLES3 : public RenderGeometryInstanceBase {
	public:
		//used during rendering
		bool store_transform_cache = true;

		int32_t instance_count = 0;

		bool can_sdfgi = false;
		bool using_projectors = false;
		bool using_softshadows = false;

		uint32_t omni_light_count = 0;
		LocalVector<RID> omni_lights;
		uint32_t spot_light_count = 0;
		LocalVector<RID> spot_lights;
		LocalVector<uint32_t> omni_light_gl_cache;
		LocalVector<uint32_t> spot_light_gl_cache;

		//used during setup
		GeometryInstanceSurface *surface_caches = nullptr;
		SelfList<GeometryInstanceGLES3> dirty_list_element;

		GeometryInstanceGLES3() :
				dirty_list_element(this) {}

		virtual void _mark_dirty() override;
		virtual void set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) override;
		virtual void set_lightmap_capture(const Color *p_sh9) override;

		virtual void pair_light_instances(const RID *p_light_instances, uint32_t p_light_instance_count) override;
		// virtual void pair_reflection_probe_instances(const RID *p_reflection_probe_instances, uint32_t p_reflection_probe_instance_count) override {}
		virtual void pair_decal_instances(const RID *p_decal_instances, uint32_t p_decal_instance_count) override {}

		virtual void set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) override {}
	};

	enum {
		INSTANCE_DATA_FLAGS_NON_UNIFORM_SCALE = 1 << 4,
		INSTANCE_DATA_FLAG_USE_GI_BUFFERS = 1 << 5,
		INSTANCE_DATA_FLAG_USE_LIGHTMAP_CAPTURE = 1 << 7,
		INSTANCE_DATA_FLAG_USE_LIGHTMAP = 1 << 8,
		INSTANCE_DATA_FLAG_USE_SH_LIGHTMAP = 1 << 9,
		INSTANCE_DATA_FLAG_USE_VOXEL_GI = 1 << 10,
		INSTANCE_DATA_FLAG_PARTICLES = 1 << 11,
		INSTANCE_DATA_FLAG_MULTIMESH = 1 << 12,
		INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D = 1 << 13,
		INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR = 1 << 14,
		INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA = 1 << 15,
	};

	static void _geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker);
	static void _geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker);

	SelfList<GeometryInstanceGLES3>::List geometry_instance_dirty_list;

	// Use PagedAllocator instead of RID to maximize performance
	PagedAllocator<GeometryInstanceGLES3> geometry_instance_alloc;
	PagedAllocator<GeometryInstanceSurface> geometry_instance_surface_alloc;

	void _geometry_instance_add_surface_with_material(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, GLES3::SceneMaterialData *p_material, uint32_t p_material_id, uint32_t p_shader_id, RID p_mesh);
	void _geometry_instance_add_surface_with_material_chain(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, GLES3::SceneMaterialData *p_material, RID p_mat_src, RID p_mesh);
	void _geometry_instance_add_surface(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, RID p_material, RID p_mesh);
	void _geometry_instance_update(RenderGeometryInstance *p_geometry_instance);
	void _update_dirty_geometry_instances();

	struct SceneState {
		struct UBO {
			float projection_matrix[16];
			float inv_projection_matrix[16];
			float inv_view_matrix[16];
			float view_matrix[16];

			float viewport_size[2];
			float screen_pixel_size[2];

			float ambient_light_color_energy[4];

			float ambient_color_sky_mix;
			uint32_t material_uv2_mode;
			float emissive_exposure_normalization;
			uint32_t use_ambient_light = 0;

			uint32_t use_ambient_cubemap = 0;
			uint32_t use_reflection_cubemap = 0;
			float fog_aerial_perspective;
			float time;

			float radiance_inverse_xform[12];

			uint32_t directional_light_count;
			float z_far;
			float z_near;
			float IBL_exposure_normalization;

			uint32_t fog_enabled;
			float fog_density;
			float fog_height;
			float fog_height_density;

			float fog_light_color[3];
			float fog_sun_scatter;
			uint32_t camera_visible_layers;
			uint32_t pad1;
			uint32_t pad2;
			uint32_t pad3;
		};
		static_assert(sizeof(UBO) % 16 == 0, "Scene UBO size must be a multiple of 16 bytes");

		struct MultiviewUBO {
			float projection_matrix_view[RendererSceneRender::MAX_RENDER_VIEWS][16];
			float inv_projection_matrix_view[RendererSceneRender::MAX_RENDER_VIEWS][16];
			float eye_offset[RendererSceneRender::MAX_RENDER_VIEWS][4];
		};
		static_assert(sizeof(MultiviewUBO) % 16 == 0, "Multiview UBO size must be a multiple of 16 bytes");

		struct TonemapUBO {
			float exposure = 1.0;
			float white = 1.0;
			int32_t tonemapper = 0;
			int32_t pad = 0;
		};
		static_assert(sizeof(TonemapUBO) % 16 == 0, "Tonemap UBO size must be a multiple of 16 bytes");

		UBO ubo;
		GLuint ubo_buffer = 0;
		MultiviewUBO multiview_ubo;
		GLuint multiview_buffer = 0;
		GLuint tonemap_buffer = 0;

		bool used_depth_prepass = false;

		GLES3::SceneShaderData::BlendMode current_blend_mode = GLES3::SceneShaderData::BLEND_MODE_MIX;
		GLES3::SceneShaderData::DepthDraw current_depth_draw = GLES3::SceneShaderData::DEPTH_DRAW_OPAQUE;
		GLES3::SceneShaderData::DepthTest current_depth_test = GLES3::SceneShaderData::DEPTH_TEST_DISABLED;
		GLES3::SceneShaderData::Cull cull_mode = GLES3::SceneShaderData::CULL_BACK;

		bool texscreen_copied = false;
		bool used_screen_texture = false;
		bool used_normal_texture = false;
		bool used_depth_texture = false;

		LightData *omni_lights = nullptr;
		LightData *spot_lights = nullptr;

		InstanceSort<GLES3::LightInstance> *omni_light_sort;
		InstanceSort<GLES3::LightInstance> *spot_light_sort;
		GLuint omni_light_buffer = 0;
		GLuint spot_light_buffer = 0;
		uint32_t omni_light_count = 0;
		uint32_t spot_light_count = 0;

		DirectionalLightData *directional_lights = nullptr;
		GLuint directional_light_buffer = 0;
	} scene_state;

	struct RenderListParameters {
		GeometryInstanceSurface **elements = nullptr;
		int element_count = 0;
		bool reverse_cull = false;
		uint64_t spec_constant_base_flags = 0;
		bool force_wireframe = false;

		RenderListParameters(GeometryInstanceSurface **p_elements, int p_element_count, bool p_reverse_cull, uint64_t p_spec_constant_base_flags, bool p_force_wireframe = false) {
			elements = p_elements;
			element_count = p_element_count;
			reverse_cull = p_reverse_cull;
			spec_constant_base_flags = p_spec_constant_base_flags;
			force_wireframe = p_force_wireframe;
		}
	};

	struct RenderList {
		LocalVector<GeometryInstanceSurface *> elements;

		void clear() {
			elements.clear();
		}

		//should eventually be replaced by radix

		struct SortByKey {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurface *A, const GeometryInstanceSurface *B) const {
				return (A->sort.sort_key2 == B->sort.sort_key2) ? (A->sort.sort_key1 < B->sort.sort_key1) : (A->sort.sort_key2 < B->sort.sort_key2);
			}
		};

		void sort_by_key() {
			SortArray<GeometryInstanceSurface *, SortByKey> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		void sort_by_key_range(uint32_t p_from, uint32_t p_size) {
			SortArray<GeometryInstanceSurface *, SortByKey> sorter;
			sorter.sort(elements.ptr() + p_from, p_size);
		}

		struct SortByDepth {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurface *A, const GeometryInstanceSurface *B) const {
				return (A->owner->depth < B->owner->depth);
			}
		};

		void sort_by_depth() { //used for shadows

			SortArray<GeometryInstanceSurface *, SortByDepth> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		struct SortByReverseDepthAndPriority {
			_FORCE_INLINE_ bool operator()(const GeometryInstanceSurface *A, const GeometryInstanceSurface *B) const {
				return (A->sort.priority == B->sort.priority) ? (A->owner->depth > B->owner->depth) : (A->sort.priority < B->sort.priority);
			}
		};

		void sort_by_reverse_depth_and_priority() { //used for alpha

			SortArray<GeometryInstanceSurface *, SortByReverseDepthAndPriority> sorter;
			sorter.sort(elements.ptr(), elements.size());
		}

		_FORCE_INLINE_ void add_element(GeometryInstanceSurface *p_element) {
			elements.push_back(p_element);
		}
	};

	RenderList render_list[RENDER_LIST_MAX];

	template <PassMode p_pass_mode>
	_FORCE_INLINE_ void _render_list_template(RenderListParameters *p_params, const RenderDataGLES3 *p_render_data, uint32_t p_from_element, uint32_t p_to_element, bool p_alpha_pass = false);

protected:
	double time;
	double time_step = 0;

	void _render_buffers_debug_draw(Ref<RenderSceneBuffersGLES3> p_render_buffers, RID p_shadow_atlas, RID p_occlusion_buffer);

	/* Camera Attributes */

	// struct CameraAttributes {
	// 	float exposure_multiplier = 1.0;
	// 	float exposure_normalization = 1.0;
	// };

	bool use_physical_light_units = false;

public:
	static RasterizerSceneGLES3 *get_singleton() { return singleton; }

	RasterizerCanvasGLES3 *canvas = nullptr;

	RenderGeometryInstance *geometry_instance_create(RID p_base) override;
	void geometry_instance_free(RenderGeometryInstance *p_geometry_instance) override;

	uint32_t geometry_instance_get_pair_mask() override;

	/* ENVIRONMENT API */

	_FORCE_INLINE_ bool is_using_physical_light_units() {
		return use_physical_light_units;
	}

	void render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) override;

	void set_scene_pass(uint64_t p_pass) override {
		scene_pass = p_pass;
	}

	_FORCE_INLINE_ uint64_t get_scene_pass() {
		return scene_pass;
	}

	void set_time(double p_time, double p_step) override;
	void set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) override;
	_FORCE_INLINE_ RS::ViewportDebugDraw get_debug_draw_mode() const {
		return debug_draw;
	}

	Ref<RenderSceneBuffers> render_buffers_create() override;

	bool free(RID p_rid) override;
	void update() override;

	void decals_set_filter(RS::DecalFilter p_filter) override;
	void light_projectors_set_filter(RS::LightProjectorFilter p_filter) override;

	RasterizerSceneGLES3();
	~RasterizerSceneGLES3();
};

#endif // GLES3_ENABLED

#endif // RASTERIZER_SCENE_GLES3_H
