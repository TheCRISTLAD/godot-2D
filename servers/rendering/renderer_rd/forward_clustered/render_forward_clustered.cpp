/**************************************************************************/
/*  render_forward_clustered.cpp                                          */
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

#include "render_forward_clustered.h"
#include "core/config/project_settings.h"
#include "core/object/worker_thread_pool.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_default.h"

using namespace RendererSceneRenderImplementation;

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_specular() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR)) {
		RD::DataFormat format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

		usage_bits |= RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_SPECULAR, format, usage_bits);
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::ensure_normal_roughness_texture() {
	ERR_FAIL_NULL(render_buffers);

	if (!render_buffers->has_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_ROUGHNESS)) {
		RD::DataFormat format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

		usage_bits |= RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;

		render_buffers->create_texture(RB_SCOPE_FORWARD_CLUSTERED, RB_TEX_ROUGHNESS, format, usage_bits);
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::free_data() {
	// JIC, should already have been cleared
	if (render_buffers) {
		render_buffers->clear_context(RB_SCOPE_FORWARD_CLUSTERED);
		render_buffers->clear_context(RB_SCOPE_SSDS);
		render_buffers->clear_context(RB_SCOPE_SSIL);
		render_buffers->clear_context(RB_SCOPE_SSAO);
		render_buffers->clear_context(RB_SCOPE_SSR);
	}

	if (cluster_builder) {
		memdelete(cluster_builder);
		cluster_builder = nullptr;
	}

	if (!render_sdfgi_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_sdfgi_uniform_set)) {
		RD::get_singleton()->free(render_sdfgi_uniform_set);
	}
}

void RenderForwardClustered::RenderBufferDataForwardClustered::configure(RenderSceneBuffersRD *p_render_buffers) {
	if (render_buffers) {
		// JIC
		free_data();
	}

	render_buffers = p_render_buffers;
	ERR_FAIL_NULL(render_buffers);

	if (cluster_builder == nullptr) {
		cluster_builder = memnew(ClusterBuilderRD);
	}
	cluster_builder->set_shared(RenderForwardClustered::get_singleton()->get_cluster_builder_shared());

	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	cluster_builder->setup(p_render_buffers->get_internal_size(), p_render_buffers->get_max_cluster_elements(), p_render_buffers->get_depth_texture(), sampler, p_render_buffers->get_internal_texture());
}

void RenderForwardClustered::setup_render_buffer_data(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBufferDataForwardClustered> data;
	data.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, data);
}

bool RenderForwardClustered::free(RID p_rid) {
	if (RendererSceneRenderRD::free(p_rid)) {
		return true;
	}
	return false;
}

/// RENDERING ///

template <RenderForwardClustered::PassMode p_pass_mode, uint32_t p_color_pass_flags>
void RenderForwardClustered::_render_list_template(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
}

void RenderForwardClustered::_render_list(RenderingDevice::DrawListID p_draw_list, RenderingDevice::FramebufferFormatID p_framebuffer_Format, RenderListParameters *p_params, uint32_t p_from_element, uint32_t p_to_element) {
	//use template for faster performance (pass mode comparisons are inlined)

	switch (p_params->pass_mode) {
#define VALID_FLAG_COMBINATION(f)                                                                                             \
	case f: {                                                                                                                 \
		_render_list_template<PASS_MODE_COLOR, f>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element); \
	} break;

		case PASS_MODE_COLOR: {
			switch (p_params->color_pass_flags) {
				VALID_FLAG_COMBINATION(0);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_TRANSPARENT | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_SEPARATE_SPECULAR | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MULTIVIEW | COLOR_PASS_FLAG_MOTION_VECTORS);
				VALID_FLAG_COMBINATION(COLOR_PASS_FLAG_MOTION_VECTORS);
				default: {
					ERR_FAIL_MSG("Invalid color pass flag combination " + itos(p_params->color_pass_flags));
				}
			}

		} break;
		case PASS_MODE_SHADOW: {
			_render_list_template<PASS_MODE_SHADOW>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SHADOW_DP: {
			_render_list_template<PASS_MODE_SHADOW_DP>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH: {
			_render_list_template<PASS_MODE_DEPTH>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI: {
			_render_list_template<PASS_MODE_DEPTH_NORMAL_ROUGHNESS_VOXEL_GI>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_DEPTH_MATERIAL: {
			_render_list_template<PASS_MODE_DEPTH_MATERIAL>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
		case PASS_MODE_SDF: {
			_render_list_template<PASS_MODE_SDF>(p_draw_list, p_framebuffer_Format, p_params, p_from_element, p_to_element);
		} break;
	}
}

void RenderForwardClustered::_render_list_thread_function(uint32_t p_thread, RenderListParameters *p_params) {
	uint32_t render_total = p_params->element_count;
	uint32_t total_threads = WorkerThreadPool::get_singleton()->get_thread_count();
	uint32_t render_from = p_thread * render_total / total_threads;
	uint32_t render_to = (p_thread + 1 == total_threads) ? render_total : ((p_thread + 1) * render_total / total_threads);
	_render_list(thread_draw_lists[p_thread], p_params->framebuffer_format, p_params, render_from, render_to);
}

void RenderForwardClustered::_render_list_with_threads(RenderListParameters *p_params, RID p_framebuffer, RD::InitialAction p_initial_color_action, RD::FinalAction p_final_color_action, RD::InitialAction p_initial_depth_action, RD::FinalAction p_final_depth_action, const Vector<Color> &p_clear_color_values, float p_clear_depth, uint32_t p_clear_stencil, const Rect2 &p_region, const Vector<RID> &p_storage_textures) {
	RD::FramebufferFormatID fb_format = RD::get_singleton()->framebuffer_get_format(p_framebuffer);
	p_params->framebuffer_format = fb_format;

	if ((uint32_t)p_params->element_count > render_list_thread_threshold && false) { // secondary command buffers need more testing at this time
		//multi threaded
		thread_draw_lists.resize(WorkerThreadPool::get_singleton()->get_thread_count());
		RD::get_singleton()->draw_list_begin_split(p_framebuffer, thread_draw_lists.size(), thread_draw_lists.ptr(), p_initial_color_action, p_final_color_action, p_initial_depth_action, p_final_depth_action, p_clear_color_values, p_clear_depth, p_clear_stencil, p_region, p_storage_textures);
		WorkerThreadPool::GroupID group_task = WorkerThreadPool::get_singleton()->add_template_group_task(this, &RenderForwardClustered::_render_list_thread_function, p_params, thread_draw_lists.size(), -1, true, SNAME("ForwardClusteredRenderList"));
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group_task);
		RD::get_singleton()->draw_list_end(p_params->barrier);
	} else {
		//single threaded
		RD::DrawListID draw_list = RD::get_singleton()->draw_list_begin(p_framebuffer, p_initial_color_action, p_final_color_action, p_initial_depth_action, p_final_depth_action, p_clear_color_values, p_clear_depth, p_clear_stencil, p_region, p_storage_textures);
		_render_list(draw_list, fb_format, p_params, 0, p_params->element_count);
		RD::get_singleton()->draw_list_end(p_params->barrier);
	}
}

void RenderForwardClustered::_update_instance_data_buffer(RenderListType p_render_list) {
	if (scene_state.instance_data[p_render_list].size() > 0) {
		if (scene_state.instance_buffer[p_render_list] == RID() || scene_state.instance_buffer_size[p_render_list] < scene_state.instance_data[p_render_list].size()) {
			if (scene_state.instance_buffer[p_render_list] != RID()) {
				RD::get_singleton()->free(scene_state.instance_buffer[p_render_list]);
			}
			uint32_t new_size = nearest_power_of_2_templated(MAX(uint64_t(INSTANCE_DATA_BUFFER_MIN_SIZE), scene_state.instance_data[p_render_list].size()));
			scene_state.instance_buffer[p_render_list] = RD::get_singleton()->storage_buffer_create(new_size * sizeof(SceneState::InstanceData));
			scene_state.instance_buffer_size[p_render_list] = new_size;
		}
		RD::get_singleton()->buffer_update(scene_state.instance_buffer[p_render_list], 0, sizeof(SceneState::InstanceData) * scene_state.instance_data[p_render_list].size(), scene_state.instance_data[p_render_list].ptr(), RD::BARRIER_MASK_RASTER);
	}
}
void RenderForwardClustered::_fill_instance_data(RenderListType p_render_list, int *p_render_info, uint32_t p_offset, int32_t p_max_elements, bool p_update_buffer) {
	RenderList *rl = &render_list[p_render_list];
	uint32_t element_total = p_max_elements >= 0 ? uint32_t(p_max_elements) : rl->elements.size();

	scene_state.instance_data[p_render_list].resize(p_offset + element_total);
	rl->element_info.resize(p_offset + element_total);

	if (p_render_info) {
		p_render_info[RS::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += element_total;
	}
	uint64_t frame = RSG::rasterizer->get_frame_number();
	uint32_t repeats = 0;
	GeometryInstanceSurfaceDataCache *prev_surface = nullptr;
	for (uint32_t i = 0; i < element_total; i++) {
		GeometryInstanceSurfaceDataCache *surface = rl->elements[i + p_offset];
		GeometryInstanceForwardClustered *inst = surface->owner;

		SceneState::InstanceData &instance_data = scene_state.instance_data[p_render_list][i + p_offset];

		if (inst->prev_transform_dirty && frame > inst->prev_transform_change_frame + 1 && inst->prev_transform_change_frame) {
			inst->prev_transform = inst->transform;
			inst->prev_transform_dirty = false;
		}

		if (inst->store_transform_cache) {
			RendererRD::MaterialStorage::store_transform(inst->transform, instance_data.transform);
			RendererRD::MaterialStorage::store_transform(inst->prev_transform, instance_data.prev_transform);

#ifdef REAL_T_IS_DOUBLE
			// Split the origin into two components, the float approximation and the missing precision
			// In the shader we will combine these back together to restore the lost precision.
			RendererRD::MaterialStorage::split_double(inst->transform.origin.x, &instance_data.transform[12], &instance_data.transform[3]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.y, &instance_data.transform[13], &instance_data.transform[7]);
			RendererRD::MaterialStorage::split_double(inst->transform.origin.z, &instance_data.transform[14], &instance_data.transform[11]);
#endif
		} else {
			RendererRD::MaterialStorage::store_transform(Transform3D(), instance_data.transform);
			RendererRD::MaterialStorage::store_transform(Transform3D(), instance_data.prev_transform);
		}

		instance_data.flags = inst->flags_cache;
		instance_data.gi_offset = inst->gi_offset_cache;
		instance_data.layer_mask = inst->layer_mask;
		instance_data.instance_uniforms_ofs = uint32_t(inst->shader_uniforms_offset);
		instance_data.lightmap_uv_scale[0] = inst->lightmap_uv_scale.position.x;
		instance_data.lightmap_uv_scale[1] = inst->lightmap_uv_scale.position.y;
		instance_data.lightmap_uv_scale[2] = inst->lightmap_uv_scale.size.x;
		instance_data.lightmap_uv_scale[3] = inst->lightmap_uv_scale.size.y;

		bool cant_repeat = instance_data.flags & INSTANCE_DATA_FLAG_MULTIMESH || inst->mesh_instance.is_valid();

		if (prev_surface != nullptr && !cant_repeat && prev_surface->sort.sort_key1 == surface->sort.sort_key1 && prev_surface->sort.sort_key2 == surface->sort.sort_key2 && inst->mirror == prev_surface->owner->mirror && repeats < RenderElementInfo::MAX_REPEATS) {
			//this element is the same as the previous one, count repeats to draw it using instancing
			repeats++;
		} else {
			if (repeats > 0) {
				for (uint32_t j = 1; j <= repeats; j++) {
					rl->element_info[p_offset + i - j].repeat = j;
				}
			}
			repeats = 1;
			if (p_render_info) {
				p_render_info[RS::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME]++;
			}
		}

		RenderElementInfo &element_info = rl->element_info[p_offset + i];

		element_info.lod_index = surface->sort.lod_index;
		element_info.uses_forward_gi = surface->sort.uses_forward_gi;
		element_info.uses_lightmap = surface->sort.uses_lightmap;
		element_info.uses_softshadow = surface->sort.uses_softshadow;
		element_info.uses_projector = surface->sort.uses_projector;

		if (cant_repeat) {
			prev_surface = nullptr;
		} else {
			prev_surface = surface;
		}
	}

	if (repeats > 0) {
		for (uint32_t j = 1; j <= repeats; j++) {
			rl->element_info[p_offset + element_total - j].repeat = j;
		}
	}

	if (p_update_buffer) {
		_update_instance_data_buffer(p_render_list);
	}
}

_FORCE_INLINE_ static uint32_t _indices_to_primitives(RS::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RS::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RS::PRIMITIVE_MAX] = { 0, 0, 1, 0, 1 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

/* Debug */

void RenderForwardClustered::_debug_draw_cluster(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers.is_valid() && current_cluster_builder != nullptr) {
		RS::ViewportDebugDraw dd = get_debug_draw_mode();

		if (dd == RS::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS || dd == RS::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS || dd == RS::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS) {
			ClusterBuilderRD::ElementType elem_type = ClusterBuilderRD::ELEMENT_TYPE_MAX;
			switch (dd) {
				case RS::VIEWPORT_DEBUG_DRAW_CLUSTER_OMNI_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_OMNI_LIGHT;
					break;
				case RS::VIEWPORT_DEBUG_DRAW_CLUSTER_SPOT_LIGHTS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_SPOT_LIGHT;
					break;
				case RS::VIEWPORT_DEBUG_DRAW_CLUSTER_DECALS:
					elem_type = ClusterBuilderRD::ELEMENT_TYPE_DECAL;
					break;
				default: {
				}
			}
			current_cluster_builder->debug(elem_type);
		}
	}
}

/* Lighting */

// void RenderForwardClustered::setup_added_reflection_probe(const Transform3D &p_transform, const Vector3 &p_half_size) {
// 	if (current_cluster_builder != nullptr) {
// 		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_REFLECTION_PROBE, p_transform, p_half_size);
// 	}
// }

void RenderForwardClustered::setup_added_light(const RS::LightType p_type, const Transform3D &p_transform, float p_radius, float p_spot_aperture) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_light(p_type == RS::LIGHT_SPOT ? ClusterBuilderRD::LIGHT_TYPE_SPOT : ClusterBuilderRD::LIGHT_TYPE_OMNI, p_transform, p_radius, p_spot_aperture);
	}
}

void RenderForwardClustered::setup_added_decal(const Transform3D &p_transform, const Vector3 &p_half_size) {
	if (current_cluster_builder != nullptr) {
		current_cluster_builder->add_box(ClusterBuilderRD::BOX_TYPE_DECAL, p_transform, p_half_size);
	}
}

void RenderForwardClustered::_render_shadow_pass(RID p_light, RID p_shadow_atlas, int p_pass, const PagedArray<RenderGeometryInstance *> &p_instances, const Plane &p_camera_plane, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, bool p_open_pass, bool p_close_pass, bool p_clear_region, RenderingMethod::RenderInfo *p_render_info) {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	ERR_FAIL_COND(!light_storage->owns_light_instance(p_light));

	RID base = light_storage->light_instance_get_base_light(p_light);

	Rect2i atlas_rect;
	uint32_t atlas_size = 1;
	RID atlas_fb;

	bool reverse_cull_face = light_storage->light_get_reverse_cull_face_mode(base);
	bool using_dual_paraboloid = false;
	bool using_dual_paraboloid_flip = false;
	Vector2i dual_paraboloid_offset;
	RID render_fb;
	RID render_texture;
	float zfar;

	bool use_pancake = false;
	bool render_cubemap = false;
	bool finalize_cubemap = false;

	bool flip_y = false;

	Projection light_projection;
	Transform3D light_transform;

	if (light_storage->light_get_type(base) == RS::LIGHT_DIRECTIONAL) {
		//set pssm stuff
		uint64_t last_scene_shadow_pass = light_storage->light_instance_get_shadow_pass(p_light);
		if (last_scene_shadow_pass != get_scene_pass()) {
			light_storage->light_instance_set_directional_rect(p_light, light_storage->get_directional_shadow_rect());
			light_storage->directional_shadow_increase_current_light();
			light_storage->light_instance_set_shadow_pass(p_light, get_scene_pass());
		}

		use_pancake = light_storage->light_get_param(base, RS::LIGHT_PARAM_SHADOW_PANCAKE_SIZE) > 0;
		light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
		light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);

		atlas_rect = light_storage->light_instance_get_directional_rect(p_light);

		if (light_storage->light_directional_get_shadow_mode(base) == RS::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS) {
			atlas_rect.size.width /= 2;
			atlas_rect.size.height /= 2;

			if (p_pass == 1) {
				atlas_rect.position.x += atlas_rect.size.width;
			} else if (p_pass == 2) {
				atlas_rect.position.y += atlas_rect.size.height;
			} else if (p_pass == 3) {
				atlas_rect.position += atlas_rect.size;
			}
		} else if (light_storage->light_directional_get_shadow_mode(base) == RS::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS) {
			atlas_rect.size.height /= 2;

			if (p_pass == 0) {
			} else {
				atlas_rect.position.y += atlas_rect.size.height;
			}
		}

		float directional_shadow_size = light_storage->directional_shadow_get_size();
		Rect2 atlas_rect_norm = atlas_rect;
		atlas_rect_norm.position /= directional_shadow_size;
		atlas_rect_norm.size /= directional_shadow_size;
		light_storage->light_instance_set_directional_shadow_atlas_rect(p_light, p_pass, atlas_rect_norm);

		zfar = RSG::light_storage->light_get_param(base, RS::LIGHT_PARAM_RANGE);

		render_fb = light_storage->direction_shadow_get_fb();
		render_texture = RID();
		flip_y = true;

	} else {
		//set from shadow atlas

		ERR_FAIL_COND(!light_storage->owns_shadow_atlas(p_shadow_atlas));
		ERR_FAIL_COND(!light_storage->shadow_atlas_owns_light_instance(p_shadow_atlas, p_light));

		RSG::light_storage->shadow_atlas_update(p_shadow_atlas);

		uint32_t key = light_storage->shadow_atlas_get_light_instance_key(p_shadow_atlas, p_light);

		uint32_t quadrant = (key >> RendererRD::LightStorage::QUADRANT_SHIFT) & 0x3;
		uint32_t shadow = key & RendererRD::LightStorage::SHADOW_INDEX_MASK;
		uint32_t subdivision = light_storage->shadow_atlas_get_quadrant_subdivision(p_shadow_atlas, quadrant);

		ERR_FAIL_INDEX((int)shadow, light_storage->shadow_atlas_get_quadrant_shadow_size(p_shadow_atlas, quadrant));

		uint32_t shadow_atlas_size = light_storage->shadow_atlas_get_size(p_shadow_atlas);
		uint32_t quadrant_size = shadow_atlas_size >> 1;

		atlas_rect.position.x = (quadrant & 1) * quadrant_size;
		atlas_rect.position.y = (quadrant >> 1) * quadrant_size;

		uint32_t shadow_size = (quadrant_size / subdivision);
		atlas_rect.position.x += (shadow % subdivision) * shadow_size;
		atlas_rect.position.y += (shadow / subdivision) * shadow_size;

		atlas_rect.size.width = shadow_size;
		atlas_rect.size.height = shadow_size;

		zfar = light_storage->light_get_param(base, RS::LIGHT_PARAM_RANGE);

		if (light_storage->light_get_type(base) == RS::LIGHT_OMNI) {
			bool wrap = (shadow + 1) % subdivision == 0;
			dual_paraboloid_offset = wrap ? Vector2i(1 - subdivision, 1) : Vector2i(1, 0);

			if (light_storage->light_omni_get_shadow_mode(base) == RS::LIGHT_OMNI_SHADOW_CUBE) {
				render_texture = light_storage->get_cubemap(shadow_size / 2);
				render_fb = light_storage->get_cubemap_fb(shadow_size / 2, p_pass);

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, p_pass);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, p_pass);
				render_cubemap = true;
				finalize_cubemap = p_pass == 5;
				atlas_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

				atlas_size = shadow_atlas_size;

				if (p_pass == 0) {
					_render_shadow_begin();
				}

			} else {
				atlas_rect.position.x += 1;
				atlas_rect.position.y += 1;
				atlas_rect.size.x -= 2;
				atlas_rect.size.y -= 2;

				atlas_rect.position += p_pass * atlas_rect.size * dual_paraboloid_offset;

				light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
				light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

				using_dual_paraboloid = true;
				using_dual_paraboloid_flip = p_pass == 1;
				render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);
				flip_y = true;
			}

		} else if (light_storage->light_get_type(base) == RS::LIGHT_SPOT) {
			light_projection = light_storage->light_instance_get_shadow_camera(p_light, 0);
			light_transform = light_storage->light_instance_get_shadow_transform(p_light, 0);

			render_fb = light_storage->shadow_atlas_get_fb(p_shadow_atlas);

			flip_y = true;
		}
	}

	if (render_cubemap) {
		//rendering to cubemap
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, false, false, use_pancake, p_camera_plane, p_lod_distance_multiplier, p_screen_mesh_lod_threshold, Rect2(), false, true, true, true, p_render_info);
		if (finalize_cubemap) {
			_render_shadow_process();
			_render_shadow_end();
			//reblit
			Rect2 atlas_rect_norm = atlas_rect;
			atlas_rect_norm.position /= float(atlas_size);
			atlas_rect_norm.size /= float(atlas_size);
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), light_projection.get_z_far(), false);
			atlas_rect_norm.position += Vector2(dual_paraboloid_offset) * atlas_rect_norm.size;
			copy_effects->copy_cubemap_to_dp(render_texture, atlas_fb, atlas_rect_norm, atlas_rect.size, light_projection.get_z_near(), light_projection.get_z_far(), true);

			//restore transform so it can be properly used
			light_storage->light_instance_set_shadow_transform(p_light, Projection(), light_storage->light_instance_get_base_transform(p_light), zfar, 0, 0, 0);
		}

	} else {
		//render shadow
		_render_shadow_append(render_fb, p_instances, light_projection, light_transform, zfar, 0, 0, reverse_cull_face, using_dual_paraboloid, using_dual_paraboloid_flip, use_pancake, p_camera_plane, p_lod_distance_multiplier, 1.0, atlas_rect, flip_y, p_clear_region, p_open_pass, p_close_pass, p_render_info);
	}
}

void RenderForwardClustered::_render_shadow_begin() {
	scene_state.shadow_passes.clear();
	RD::get_singleton()->draw_command_begin_label("Shadow Setup");
	_update_render_base_uniform_set();

	render_list[RENDER_LIST_SECONDARY].clear();
	scene_state.instance_data[RENDER_LIST_SECONDARY].clear();
}

void RenderForwardClustered::_render_shadow_append(RID p_framebuffer, const PagedArray<RenderGeometryInstance *> &p_instances, const Projection &p_projection, const Transform3D &p_transform, float p_zfar, float p_bias, float p_normal_bias, bool p_reverse_cull_face, bool p_use_dp, bool p_use_dp_flip, bool p_use_pancake, const Plane &p_camera_plane, float p_lod_distance_multiplier, float p_screen_mesh_lod_threshold, const Rect2i &p_rect, bool p_flip_y, bool p_clear_region, bool p_begin, bool p_end, RenderingMethod::RenderInfo *p_render_info) {
	uint32_t shadow_pass_index = scene_state.shadow_passes.size();

	SceneState::ShadowPass shadow_pass;

	RenderSceneDataRD scene_data;
	scene_data.cam_projection = p_projection;
	scene_data.cam_transform = p_transform;
	scene_data.view_projection[0] = p_projection;
	scene_data.z_far = p_zfar;
	scene_data.z_near = 0.0;
	scene_data.lod_distance_multiplier = p_lod_distance_multiplier;
	scene_data.dual_paraboloid_side = p_use_dp_flip ? -1 : 1;
	scene_data.opaque_prepass_threshold = 0.1f;
	scene_data.time = time;
	scene_data.time_step = time_step;

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.cluster_size = 1;
	render_data.cluster_max_elements = 32;
	render_data.instances = &p_instances;
	render_data.render_info = p_render_info;

	// if (get_debug_draw_mode() == RS::VIEWPORT_DEBUG_DRAW_DISABLE_LOD) {
	// 	scene_data.screen_mesh_lod_threshold = 0.0;
	// } else {
	// 	scene_data.screen_mesh_lod_threshold = p_screen_mesh_lod_threshold;
	// }

	PassMode pass_mode = p_use_dp ? PASS_MODE_SHADOW_DP : PASS_MODE_SHADOW;

	uint32_t render_list_from = render_list[RENDER_LIST_SECONDARY].elements.size();
	uint32_t render_list_size = render_list[RENDER_LIST_SECONDARY].elements.size() - render_list_from;
	render_list[RENDER_LIST_SECONDARY].sort_by_key_range(render_list_from, render_list_size);
	_fill_instance_data(RENDER_LIST_SECONDARY, p_render_info ? p_render_info->info[RS::VIEWPORT_RENDER_INFO_TYPE_SHADOW] : (int *)nullptr, render_list_from, render_list_size, false);

	{
		//regular forward for now
		bool flip_cull = p_use_dp_flip;
		if (p_flip_y) {
			flip_cull = !flip_cull;
		}

		if (p_reverse_cull_face) {
			flip_cull = !flip_cull;
		}

		shadow_pass.element_from = render_list_from;
		shadow_pass.element_count = render_list_size;
		shadow_pass.flip_cull = flip_cull;
		shadow_pass.pass_mode = pass_mode;

		shadow_pass.rp_uniform_set = RID(); //will be filled later when instance buffer is complete
		shadow_pass.camera_plane = p_camera_plane;
		// shadow_pass.screen_mesh_lod_threshold = scene_data.screen_mesh_lod_threshold;
		shadow_pass.lod_distance_multiplier = scene_data.lod_distance_multiplier;

		shadow_pass.framebuffer = p_framebuffer;
		shadow_pass.initial_depth_action = p_begin ? (p_clear_region ? RD::INITIAL_ACTION_CLEAR_REGION : RD::INITIAL_ACTION_CLEAR) : (p_clear_region ? RD::INITIAL_ACTION_CLEAR_REGION_CONTINUE : RD::INITIAL_ACTION_CONTINUE);
		shadow_pass.final_depth_action = p_end ? RD::FINAL_ACTION_READ : RD::FINAL_ACTION_CONTINUE;
		shadow_pass.rect = p_rect;

		scene_state.shadow_passes.push_back(shadow_pass);
	}
}

void RenderForwardClustered::_render_shadow_process() {
}

void RenderForwardClustered::_render_shadow_end(uint32_t p_barrier) {
	RD::get_singleton()->draw_command_begin_label("Shadow Render");

	for (SceneState::ShadowPass &shadow_pass : scene_state.shadow_passes) {
		RenderListParameters render_list_parameters(render_list[RENDER_LIST_SECONDARY].elements.ptr() + shadow_pass.element_from, render_list[RENDER_LIST_SECONDARY].element_info.ptr() + shadow_pass.element_from, shadow_pass.element_count, shadow_pass.flip_cull, shadow_pass.pass_mode, 0, true, false, shadow_pass.rp_uniform_set, false, Vector2(), shadow_pass.lod_distance_multiplier, 1.0, 1, shadow_pass.element_from, RD::BARRIER_MASK_NO_BARRIER);
		_render_list_with_threads(&render_list_parameters, shadow_pass.framebuffer, RD::INITIAL_ACTION_DROP, RD::FINAL_ACTION_DISCARD, shadow_pass.initial_depth_action, shadow_pass.final_depth_action, Vector<Color>(), 1.0, 0, shadow_pass.rect);
	}

	if (p_barrier != RD::BARRIER_MASK_NO_BARRIER) {
		RD::get_singleton()->barrier(RD::BARRIER_MASK_RASTER, p_barrier);
	}
	RD::get_singleton()->draw_command_end_label();
}

void RenderForwardClustered::base_uniforms_changed() {
	if (!render_base_uniform_set.is_null() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
		RD::get_singleton()->free(render_base_uniform_set);
	}
	render_base_uniform_set = RID();
}

void RenderForwardClustered::_update_render_base_uniform_set() {
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	if (render_base_uniform_set.is_null() || !RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set) || (lightmap_texture_array_version != light_storage->lightmap_array_get_version()) || base_uniform_set_updated) {
		base_uniform_set_updated = false;

		if (render_base_uniform_set.is_valid() && RD::get_singleton()->uniform_set_is_valid(render_base_uniform_set)) {
			RD::get_singleton()->free(render_base_uniform_set);
		}

		lightmap_texture_array_version = light_storage->lightmap_array_get_version();

		Vector<RD::Uniform> uniforms;

		{
			Vector<RID> ids;
			ids.resize(12);
			RID *ids_ptr = ids.ptrw();
			ids_ptr[0] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[1] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[2] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[3] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[4] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[5] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
			ids_ptr[6] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
			ids_ptr[7] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
			ids_ptr[8] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
			ids_ptr[9] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
			ids_ptr[10] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
			ids_ptr[11] = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);

			RD::Uniform u(RD::UNIFORM_TYPE_SAMPLER, 1, ids);

			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 2;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			u.append_id(scene_shader.shadow_sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 3;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			RID sampler;
			switch (decals_get_filter()) {
				case RS::DECAL_FILTER_NEAREST: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::DECAL_FILTER_LINEAR: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::DECAL_FILTER_NEAREST_MIPMAPS: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::DECAL_FILTER_LINEAR_MIPMAPS: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
			}

			u.append_id(sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 4;
			u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
			RID sampler;
			switch (light_projectors_get_filter()) {
				case RS::LIGHT_PROJECTOR_FILTER_NEAREST: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::LIGHT_PROJECTOR_FILTER_LINEAR: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
				case RS::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC: {
					sampler = material_storage->sampler_rd_get_custom(RS::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC, RS::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
				} break;
			}

			u.append_id(sampler);
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.binding = 5;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_omni_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 6;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_spot_light_buffer());
			uniforms.push_back(u);
		}

		// {
		// 	RD::Uniform u;
		// 	u.binding = 7;
		// 	u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		// 	u.append_id(RendererRD::LightStorage::get_singleton()->get_reflection_probe_buffer());
		// 	uniforms.push_back(u);
		// }
		{
			RD::Uniform u;
			u.binding = 8;
			u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
			u.append_id(RendererRD::LightStorage::get_singleton()->get_directional_light_buffer());
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 9;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 10;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(scene_state.lightmap_capture_buffer);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 11;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 12;
			u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
			RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
			u.append_id(decal_atlas);
			uniforms.push_back(u);
		}
		{
			RD::Uniform u;
			u.binding = 13;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.append_id(RendererRD::TextureStorage::get_singleton()->get_decal_buffer());
			uniforms.push_back(u);
		}

		{
			RD::Uniform u;
			u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
			u.binding = 14;
			u.append_id(RendererRD::MaterialStorage::get_singleton()->global_shader_uniforms_get_storage_buffer());
			uniforms.push_back(u);
		}

		// {
		// 	RD::Uniform u;
		// 	u.uniform_type = RD::UNIFORM_TYPE_UNIFORM_BUFFER;
		// 	u.binding = 15;
		// 	u.append_id(sdfgi_get_ubo());
		// 	uniforms.push_back(u);
		// }

		render_base_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, scene_shader.default_shader_rd, SCENE_UNIFORM_SET);
	}
}

RenderForwardClustered *RenderForwardClustered::singleton = nullptr;

void RenderForwardClustered::GeometryInstanceForwardClustered::_mark_dirty() {
	if (dirty_list_element.in_list()) {
		return;
	}

	//clear surface caches
	GeometryInstanceSurfaceDataCache *surf = surface_caches;

	while (surf) {
		GeometryInstanceSurfaceDataCache *next = surf->next;
		RenderForwardClustered::get_singleton()->geometry_instance_surface_alloc.free(surf);
		surf = next;
	}

	surface_caches = nullptr;

	RenderForwardClustered::get_singleton()->geometry_instance_dirty_list.add(&dirty_list_element);
}

void RenderForwardClustered::_geometry_instance_add_surface(GeometryInstanceForwardClustered *ginstance, uint32_t p_surface, RID p_material, RID p_mesh) {
}

void RenderForwardClustered::_geometry_instance_update(RenderGeometryInstance *p_geometry_instance) {
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_geometry_instance);

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_begin();
	}

	//add geometry for drawing
	switch (ginstance->data->base_type) {
		case RS::INSTANCE_MESH: {
			const RID *materials = nullptr;
			uint32_t surface_count;
			RID mesh = ginstance->data->base;

			materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
			if (materials) {
				//if no materials, no surfaces.
				const RID *inst_materials = ginstance->data->surface_materials.ptr();
				uint32_t surf_mat_count = ginstance->data->surface_materials.size();

				for (uint32_t j = 0; j < surface_count; j++) {
					RID material = (j < surf_mat_count && inst_materials[j].is_valid()) ? inst_materials[j] : materials[j];
					_geometry_instance_add_surface(ginstance, j, material, mesh);
				}
			}

			ginstance->instance_count = 1;

		} break;

		case RS::INSTANCE_MULTIMESH: {
			RID mesh = mesh_storage->multimesh_get_mesh(ginstance->data->base);
			if (mesh.is_valid()) {
				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t j = 0; j < surface_count; j++) {
						_geometry_instance_add_surface(ginstance, j, materials[j], mesh);
					}
				}

				ginstance->instance_count = mesh_storage->multimesh_get_instances_to_draw(ginstance->data->base);
			}

		} break;
#if 0
		case RS::INSTANCE_IMMEDIATE: {
			RasterizerStorageGLES3::Immediate *immediate = storage->immediate_owner.get_or_null(inst->base);
			ERR_CONTINUE(!immediate);

			_add_geometry(immediate, inst, nullptr, -1, p_depth_pass, p_shadow_pass);

		} break;
#endif
		case RS::INSTANCE_PARTICLES: {
			int draw_passes = particles_storage->particles_get_draw_passes(ginstance->data->base);

			for (int j = 0; j < draw_passes; j++) {
				RID mesh = particles_storage->particles_get_draw_pass_mesh(ginstance->data->base, j);
				if (!mesh.is_valid()) {
					continue;
				}

				const RID *materials = nullptr;
				uint32_t surface_count;

				materials = mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				if (materials) {
					for (uint32_t k = 0; k < surface_count; k++) {
						_geometry_instance_add_surface(ginstance, k, materials[k], mesh);
					}
				}
			}

			ginstance->instance_count = particles_storage->particles_get_amount(ginstance->data->base, ginstance->trail_steps);

		} break;

		default: {
		}
	}

	//Fill push constant

	ginstance->base_flags = 0;

	bool store_transform = true;

	if (ginstance->data->base_type == RS::INSTANCE_MULTIMESH) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;
		if (mesh_storage->multimesh_get_transform_format(ginstance->data->base) == RS::MULTIMESH_TRANSFORM_2D) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D;
		}
		if (mesh_storage->multimesh_uses_colors(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		}
		if (mesh_storage->multimesh_uses_custom_data(ginstance->data->base)) {
			ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;
		}

		ginstance->transforms_uniform_set = mesh_storage->multimesh_get_3d_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

	} else if (ginstance->data->base_type == RS::INSTANCE_PARTICLES) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_PARTICLES;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;

		//for particles, stride is the trail size
		ginstance->base_flags |= (ginstance->trail_steps << INSTANCE_DATA_FLAGS_PARTICLE_TRAIL_SHIFT);

		if (!particles_storage->particles_is_using_local_coords(ginstance->data->base)) {
			store_transform = false;
		}
		ginstance->transforms_uniform_set = particles_storage->particles_get_instance_buffer_uniform_set(ginstance->data->base, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);

		if (particles_storage->particles_get_frame_counter(ginstance->data->base) == 0) {
			// Particles haven't been cleared or updated, update once now to ensure they are ready to render.
			particles_storage->update_particles();
		}
	} else if (ginstance->data->base_type == RS::INSTANCE_MESH) {
		if (mesh_storage->skeleton_is_valid(ginstance->data->skeleton)) {
			ginstance->transforms_uniform_set = mesh_storage->skeleton_get_3d_uniform_set(ginstance->data->skeleton, scene_shader.default_shader_rd, TRANSFORMS_UNIFORM_SET);
			if (ginstance->data->dirty_dependencies) {
				mesh_storage->skeleton_update_dependency(ginstance->data->skeleton, &ginstance->data->dependency_tracker);
			}
		} else {
			ginstance->transforms_uniform_set = RID();
		}
	}

	ginstance->store_transform_cache = store_transform;
	ginstance->can_sdfgi = false;

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_end();
		ginstance->data->dirty_dependencies = false;
	}

	ginstance->dirty_list_element.remove_from_list();
}

void RenderForwardClustered::_update_dirty_geometry_instances() {
	while (geometry_instance_dirty_list.first()) {
		_geometry_instance_update(geometry_instance_dirty_list.first()->self());
	}
}

void RenderForwardClustered::_geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker) {
	switch (p_notification) {
		case Dependency::DEPENDENCY_CHANGED_MATERIAL:
		case Dependency::DEPENDENCY_CHANGED_MESH:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH:
		case Dependency::DEPENDENCY_CHANGED_SKELETON_DATA: {
			static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
			static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
		} break;
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES: {
			GeometryInstanceForwardClustered *ginstance = static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata);
			if (ginstance->data->base_type == RS::INSTANCE_MULTIMESH) {
				ginstance->instance_count = RendererRD::MeshStorage::get_singleton()->multimesh_get_instances_to_draw(ginstance->data->base);
			}
		} break;
		default: {
			//rest of notifications of no interest
		} break;
	}
}
void RenderForwardClustered::_geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker) {
	static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
	static_cast<GeometryInstanceForwardClustered *>(p_tracker->userdata)->data->dirty_dependencies = true;
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabbb) {
	uint64_t frame = RSG::rasterizer->get_frame_number();
	if (frame != prev_transform_change_frame) {
		prev_transform = transform;
		prev_transform_change_frame = frame;
		prev_transform_dirty = true;
	}

	RenderGeometryInstanceBase::set_transform(p_transform, p_aabb, p_transformed_aabbb);
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) {
	lightmap_instance = p_lightmap_instance;
	lightmap_uv_scale = p_lightmap_uv_scale;
	lightmap_slice_index = p_lightmap_slice_index;

	_mark_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_lightmap_capture(const Color *p_sh9) {
	if (p_sh9) {
		if (lightmap_sh == nullptr) {
			lightmap_sh = RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.alloc();
		}

		memcpy(lightmap_sh->sh, p_sh9, sizeof(Color) * 9);
	} else {
		if (lightmap_sh != nullptr) {
			RenderForwardClustered::get_singleton()->geometry_instance_lightmap_sh.free(lightmap_sh);
			lightmap_sh = nullptr;
		}
	}
	_mark_dirty();
}

void RenderForwardClustered::GeometryInstanceForwardClustered::set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) {
	using_projectors = p_projector;
	using_softshadows = p_softshadow;
	_mark_dirty();
}

void RenderForwardClustered::_update_shader_quality_settings() {
	Vector<RD::PipelineSpecializationConstant> spec_constants;

	RD::PipelineSpecializationConstant sc;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;

	sc.constant_id = SPEC_CONSTANT_SOFT_SHADOW_SAMPLES;
	sc.int_value = soft_shadow_samples_get();

	spec_constants.push_back(sc);

	sc.constant_id = SPEC_CONSTANT_PENUMBRA_SHADOW_SAMPLES;
	sc.int_value = penumbra_shadow_samples_get();

	spec_constants.push_back(sc);

	sc.constant_id = SPEC_CONSTANT_DIRECTIONAL_SOFT_SHADOW_SAMPLES;
	sc.int_value = directional_soft_shadow_samples_get();

	spec_constants.push_back(sc);

	sc.constant_id = SPEC_CONSTANT_DIRECTIONAL_PENUMBRA_SHADOW_SAMPLES;
	sc.int_value = directional_penumbra_shadow_samples_get();

	spec_constants.push_back(sc);

	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
	sc.constant_id = SPEC_CONSTANT_DECAL_FILTER;
	sc.bool_value = decals_get_filter() == RS::DECAL_FILTER_NEAREST_MIPMAPS ||
			decals_get_filter() == RS::DECAL_FILTER_LINEAR_MIPMAPS ||
			decals_get_filter() == RS::DECAL_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			decals_get_filter() == RS::DECAL_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	spec_constants.push_back(sc);

	sc.constant_id = SPEC_CONSTANT_PROJECTOR_FILTER;
	sc.bool_value = light_projectors_get_filter() == RS::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS ||
			light_projectors_get_filter() == RS::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS ||
			light_projectors_get_filter() == RS::LIGHT_PROJECTOR_FILTER_NEAREST_MIPMAPS_ANISOTROPIC ||
			light_projectors_get_filter() == RS::LIGHT_PROJECTOR_FILTER_LINEAR_MIPMAPS_ANISOTROPIC;

	spec_constants.push_back(sc);

	scene_shader.set_default_specialization_constants(spec_constants);

	base_uniforms_changed(); //also need this
}

RenderForwardClustered::RenderForwardClustered() {
	singleton = this;

	/* SCENE SHADER */

	{
		String defines;
		defines += "\n#define MAX_ROUGHNESS_LOD " + itos(5 - 1) + ".0\n";
		// if (is_using_radiance_cubemap_array()) {
		// 	defines += "\n#define USE_RADIANCE_CUBEMAP_ARRAY \n";
		// }
		defines += "\n#define SDFGI_OCT_SIZE " + itos(5 - 1) + "\n";
		defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "\n";

		{
			//lightmaps
			scene_state.max_lightmaps = MAX_LIGHTMAPS;
			defines += "\n#define MAX_LIGHTMAP_TEXTURES " + itos(scene_state.max_lightmaps) + "\n";
			defines += "\n#define MAX_LIGHTMAPS " + itos(scene_state.max_lightmaps) + "\n";

			scene_state.lightmap_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapData) * scene_state.max_lightmaps);
		}
		{
			//captures
			scene_state.max_lightmap_captures = 2048;
			scene_state.lightmap_captures = memnew_arr(LightmapCaptureData, scene_state.max_lightmap_captures);
			scene_state.lightmap_capture_buffer = RD::get_singleton()->storage_buffer_create(sizeof(LightmapCaptureData) * scene_state.max_lightmap_captures);
		}
		{
			defines += "\n#define MATERIAL_UNIFORM_SET " + itos(MATERIAL_UNIFORM_SET) + "\n";
		}
#ifdef REAL_T_IS_DOUBLE
		{
			defines += "\n#define USE_DOUBLE_PRECISION \n";
		}
#endif

		scene_shader.init(defines);
	}

	/* shadow sampler */
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_LESS;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}

	render_list_thread_threshold = GLOBAL_GET("rendering/limits/forward_renderer/threaded_render_minimum_instances");

	_update_shader_quality_settings();

	resolve_effects = memnew(RendererRD::Resolve());
	taa = memnew(RendererRD::TAA);
	ss_effects = memnew(RendererRD::SSEffects);
}

RenderForwardClustered::~RenderForwardClustered() {
	if (ss_effects != nullptr) {
		memdelete(ss_effects);
		ss_effects = nullptr;
	}

	if (taa != nullptr) {
		memdelete(taa);
		taa = nullptr;
	}

	if (resolve_effects != nullptr) {
		memdelete(resolve_effects);
		resolve_effects = nullptr;
	}

	RD::get_singleton()->free(shadow_sampler);
	RSG::light_storage->directional_shadow_atlas_set_size(0);

	{
		for (const RID &rid : scene_state.uniform_buffers) {
			RD::get_singleton()->free(rid);
		}
		for (const RID &rid : scene_state.implementation_uniform_buffers) {
			RD::get_singleton()->free(rid);
		}
		RD::get_singleton()->free(scene_state.lightmap_buffer);
		RD::get_singleton()->free(scene_state.lightmap_capture_buffer);
		for (uint32_t i = 0; i < RENDER_LIST_MAX; i++) {
			if (scene_state.instance_buffer[i] != RID()) {
				RD::get_singleton()->free(scene_state.instance_buffer[i]);
			}
		}
		memdelete_arr(scene_state.lightmap_captures);
	}

	while (sdfgi_framebuffer_size_cache.begin()) {
		RD::get_singleton()->free(sdfgi_framebuffer_size_cache.begin()->value);
		sdfgi_framebuffer_size_cache.remove(sdfgi_framebuffer_size_cache.begin());
	}
}
