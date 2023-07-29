/**************************************************************************/
/*  rasterizer_scene_gles3.cpp                                            */
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

#include "rasterizer_scene_gles3.h"
#include "core/config/project_settings.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/rendering_server_globals.h"
#include "storage/config.h"
#include "storage/mesh_storage.h"
#include "storage/particles_storage.h"
#include "storage/texture_storage.h"

#ifdef GLES3_ENABLED

RasterizerSceneGLES3 *RasterizerSceneGLES3::singleton = nullptr;

RenderGeometryInstance *RasterizerSceneGLES3::geometry_instance_create(RID p_base) {
	RS::InstanceType type = RSG::utilities->get_base_type(p_base);
	ERR_FAIL_COND_V(!((1 << type) & RS::INSTANCE_GEOMETRY_MASK), nullptr);

	GeometryInstanceGLES3 *ginstance = geometry_instance_alloc.alloc();
	ginstance->data = memnew(GeometryInstanceGLES3::Data);

	ginstance->data->base = p_base;
	ginstance->data->base_type = type;
	ginstance->data->dependency_tracker.userdata = ginstance;
	ginstance->data->dependency_tracker.changed_callback = _geometry_instance_dependency_changed;
	ginstance->data->dependency_tracker.deleted_callback = _geometry_instance_dependency_deleted;

	ginstance->_mark_dirty();

	return ginstance;
}

uint32_t RasterizerSceneGLES3::geometry_instance_get_pair_mask() {
	return (1 << RS::INSTANCE_LIGHT);
}

void RasterizerSceneGLES3::GeometryInstanceGLES3::pair_light_instances(const RID *p_light_instances, uint32_t p_light_instance_count) {
	GLES3::Config *config = GLES3::Config::get_singleton();

	omni_light_count = 0;
	spot_light_count = 0;
	omni_lights.clear();
	spot_lights.clear();

	for (uint32_t i = 0; i < p_light_instance_count; i++) {
		RS::LightType type = GLES3::LightStorage::get_singleton()->light_instance_get_type(p_light_instances[i]);
		switch (type) {
			case RS::LIGHT_OMNI: {
				if (omni_light_count < (uint32_t)config->max_lights_per_object) {
					omni_lights.push_back(p_light_instances[i]);
					omni_light_count++;
				}
			} break;
			case RS::LIGHT_SPOT: {
				if (spot_light_count < (uint32_t)config->max_lights_per_object) {
					spot_lights.push_back(p_light_instances[i]);
					spot_light_count++;
				}
			} break;
			default:
				break;
		}
	}
}

void RasterizerSceneGLES3::geometry_instance_free(RenderGeometryInstance *p_geometry_instance) {
	GeometryInstanceGLES3 *ginstance = static_cast<GeometryInstanceGLES3 *>(p_geometry_instance);
	ERR_FAIL_COND(!ginstance);
	GeometryInstanceSurface *surf = ginstance->surface_caches;
	while (surf) {
		GeometryInstanceSurface *next = surf->next;
		geometry_instance_surface_alloc.free(surf);
		surf = next;
	}
	memdelete(ginstance->data);
	geometry_instance_alloc.free(ginstance);
}

void RasterizerSceneGLES3::GeometryInstanceGLES3::_mark_dirty() {
	if (dirty_list_element.in_list()) {
		return;
	}

	//clear surface caches
	GeometryInstanceSurface *surf = surface_caches;

	while (surf) {
		GeometryInstanceSurface *next = surf->next;
		RasterizerSceneGLES3::get_singleton()->geometry_instance_surface_alloc.free(surf);
		surf = next;
	}

	surface_caches = nullptr;

	RasterizerSceneGLES3::get_singleton()->geometry_instance_dirty_list.add(&dirty_list_element);
}

void RasterizerSceneGLES3::GeometryInstanceGLES3::set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) {
}

void RasterizerSceneGLES3::GeometryInstanceGLES3::set_lightmap_capture(const Color *p_sh9) {
}

void RasterizerSceneGLES3::_update_dirty_geometry_instances() {
	while (geometry_instance_dirty_list.first()) {
		_geometry_instance_update(geometry_instance_dirty_list.first()->self());
	}
}

void RasterizerSceneGLES3::_geometry_instance_dependency_changed(Dependency::DependencyChangedNotification p_notification, DependencyTracker *p_tracker) {
	switch (p_notification) {
		case Dependency::DEPENDENCY_CHANGED_MATERIAL:
		case Dependency::DEPENDENCY_CHANGED_MESH:
		case Dependency::DEPENDENCY_CHANGED_PARTICLES:
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH:
		case Dependency::DEPENDENCY_CHANGED_SKELETON_DATA: {
			static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
			static_cast<GeometryInstanceGLES3 *>(p_tracker->userdata)->data->dirty_dependencies = true;
		} break;
		case Dependency::DEPENDENCY_CHANGED_MULTIMESH_VISIBLE_INSTANCES: {
			GeometryInstanceGLES3 *ginstance = static_cast<GeometryInstanceGLES3 *>(p_tracker->userdata);
			if (ginstance->data->base_type == RS::INSTANCE_MULTIMESH) {
				ginstance->instance_count = GLES3::MeshStorage::get_singleton()->multimesh_get_instances_to_draw(ginstance->data->base);
			}
		} break;
		default: {
			//rest of notifications of no interest
		} break;
	}
}

void RasterizerSceneGLES3::_geometry_instance_dependency_deleted(const RID &p_dependency, DependencyTracker *p_tracker) {
	static_cast<RenderGeometryInstance *>(p_tracker->userdata)->_mark_dirty();
	static_cast<GeometryInstanceGLES3 *>(p_tracker->userdata)->data->dirty_dependencies = true;
}

void RasterizerSceneGLES3::_geometry_instance_add_surface_with_material(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, GLES3::SceneMaterialData *p_material, uint32_t p_material_id, uint32_t p_shader_id, RID p_mesh) {
	// GLES3::MeshStorage *mesh_storage = GLES3::MeshStorage::get_singleton();

	// bool has_read_screen_alpha = p_material->shader_data->uses_screen_texture || p_material->shader_data->uses_depth_texture || p_material->shader_data->uses_normal_texture;
	// bool has_base_alpha = ((p_material->shader_data->uses_alpha && !p_material->shader_data->uses_alpha_clip) || has_read_screen_alpha);
	// bool has_blend_alpha = p_material->shader_data->uses_blend_alpha;
	// bool has_alpha = has_base_alpha || has_blend_alpha;

	// uint32_t flags = 0;

	// if (p_material->shader_data->uses_screen_texture) {
	// 	flags |= GeometryInstanceSurface::FLAG_USES_SCREEN_TEXTURE;
	// }

	// if (p_material->shader_data->uses_depth_texture) {
	// 	flags |= GeometryInstanceSurface::FLAG_USES_DEPTH_TEXTURE;
	// }

	// if (p_material->shader_data->uses_normal_texture) {
	// 	flags |= GeometryInstanceSurface::FLAG_USES_NORMAL_TEXTURE;
	// }

	// if (ginstance->data->cast_double_sided_shadows) {
	// 	flags |= GeometryInstanceSurface::FLAG_USES_DOUBLE_SIDED_SHADOWS;
	// }

	// if (has_alpha || has_read_screen_alpha || p_material->shader_data->depth_draw == GLES3::SceneShaderData::DEPTH_DRAW_DISABLED || p_material->shader_data->depth_test == GLES3::SceneShaderData::DEPTH_TEST_DISABLED) {
	// 	//material is only meant for alpha pass
	// 	flags |= GeometryInstanceSurface::FLAG_PASS_ALPHA;
	// 	if (p_material->shader_data->uses_depth_prepass_alpha && !(p_material->shader_data->depth_draw == GLES3::SceneShaderData::DEPTH_DRAW_DISABLED || p_material->shader_data->depth_test == GLES3::SceneShaderData::DEPTH_TEST_DISABLED)) {
	// 		flags |= GeometryInstanceSurface::FLAG_PASS_DEPTH;
	// 		flags |= GeometryInstanceSurface::FLAG_PASS_SHADOW;
	// 	}
	// } else {
	// 	flags |= GeometryInstanceSurface::FLAG_PASS_OPAQUE;
	// 	flags |= GeometryInstanceSurface::FLAG_PASS_DEPTH;
	// 	flags |= GeometryInstanceSurface::FLAG_PASS_SHADOW;
	// }

	// GLES3::SceneMaterialData *material_shadow = nullptr;
	// void *surface_shadow = nullptr;
	// if (!p_material->shader_data->uses_particle_trails && !p_material->shader_data->writes_modelview_or_projection && !p_material->shader_data->uses_vertex && !p_material->shader_data->uses_discard && !p_material->shader_data->uses_depth_prepass_alpha && !p_material->shader_data->uses_alpha_clip) {
	// 	flags |= GeometryInstanceSurface::FLAG_USES_SHARED_SHADOW_MATERIAL;
	// 	material_shadow = static_cast<GLES3::SceneMaterialData *>(GLES3::MaterialStorage::get_singleton()->material_get_data(scene_globals.default_material, RS::SHADER_SPATIAL));

	// 	RID shadow_mesh = mesh_storage->mesh_get_shadow_mesh(p_mesh);

	// 	if (shadow_mesh.is_valid()) {
	// 		surface_shadow = mesh_storage->mesh_get_surface(shadow_mesh, p_surface);
	// 	}

	// } else {
	// 	material_shadow = p_material;
	// }

	// GeometryInstanceSurface *sdcache = geometry_instance_surface_alloc.alloc();

	// sdcache->flags = flags;

	// sdcache->shader = p_material->shader_data;
	// sdcache->material = p_material;
	// sdcache->surface = mesh_storage->mesh_get_surface(p_mesh, p_surface);
	// sdcache->primitive = mesh_storage->mesh_surface_get_primitive(sdcache->surface);
	// sdcache->surface_index = p_surface;

	// if (ginstance->data->dirty_dependencies) {
	// 	RSG::utilities->base_update_dependency(p_mesh, &ginstance->data->dependency_tracker);
	// }

	// //shadow
	// sdcache->shader_shadow = material_shadow->shader_data;
	// sdcache->material_shadow = material_shadow;

	// sdcache->surface_shadow = surface_shadow ? surface_shadow : sdcache->surface;

	// sdcache->owner = ginstance;

	// sdcache->next = ginstance->surface_caches;
	// ginstance->surface_caches = sdcache;

	// //sortkey

	// sdcache->sort.sort_key1 = 0;
	// sdcache->sort.sort_key2 = 0;

	// sdcache->sort.surface_index = p_surface;
	// sdcache->sort.material_id_low = p_material_id & 0x0000FFFF;
	// sdcache->sort.material_id_hi = p_material_id >> 16;
	// sdcache->sort.shader_id = p_shader_id;
	// sdcache->sort.geometry_id = p_mesh.get_local_index();
	// sdcache->sort.priority = p_material->priority;
}

void RasterizerSceneGLES3::_geometry_instance_add_surface_with_material_chain(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, GLES3::SceneMaterialData *p_material_data, RID p_mat_src, RID p_mesh) {
	// GLES3::SceneMaterialData *material_data = p_material_data;
	// GLES3::MaterialStorage *material_storage = GLES3::MaterialStorage::get_singleton();

	// _geometry_instance_add_surface_with_material(ginstance, p_surface, material_data, p_mat_src.get_local_index(), material_storage->material_get_shader_id(p_mat_src), p_mesh);

	// while (material_data->next_pass.is_valid()) {
	// 	RID next_pass = material_data->next_pass;
	// 	material_data = static_cast<GLES3::SceneMaterialData *>(material_storage->material_get_data(next_pass, RS::SHADER_SPATIAL));
	// 	if (!material_data || !material_data->shader_data->valid) {
	// 		break;
	// 	}
	// 	if (ginstance->data->dirty_dependencies) {
	// 		material_storage->material_update_dependency(next_pass, &ginstance->data->dependency_tracker);
	// 	}
	// 	_geometry_instance_add_surface_with_material(ginstance, p_surface, material_data, next_pass.get_local_index(), material_storage->material_get_shader_id(next_pass), p_mesh);
	// }
}

void RasterizerSceneGLES3::_geometry_instance_add_surface(GeometryInstanceGLES3 *ginstance, uint32_t p_surface, RID p_material, RID p_mesh) {
	// GLES3::MaterialStorage *material_storage = GLES3::MaterialStorage::get_singleton();
	// RID m_src;

	// m_src = ginstance->data->material_override.is_valid() ? ginstance->data->material_override : p_material;

	// GLES3::SceneMaterialData *material_data = nullptr;

	// if (m_src.is_valid()) {
	// 	material_data = static_cast<GLES3::SceneMaterialData *>(material_storage->material_get_data(m_src, RS::SHADER_SPATIAL));
	// 	if (!material_data || !material_data->shader_data->valid) {
	// 		material_data = nullptr;
	// 	}
	// }

	// if (material_data) {
	// 	if (ginstance->data->dirty_dependencies) {
	// 		material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
	// 	}
	// } else {
	// 	material_data = static_cast<GLES3::SceneMaterialData *>(material_storage->material_get_data(scene_globals.default_material, RS::SHADER_SPATIAL));
	// 	m_src = scene_globals.default_material;
	// }

	// ERR_FAIL_COND(!material_data);

	// _geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material_data, m_src, p_mesh);

	// if (ginstance->data->material_overlay.is_valid()) {
	// 	m_src = ginstance->data->material_overlay;

	// 	material_data = static_cast<GLES3::SceneMaterialData *>(material_storage->material_get_data(m_src, RS::SHADER_SPATIAL));
	// 	if (material_data && material_data->shader_data->valid) {
	// 		if (ginstance->data->dirty_dependencies) {
	// 			material_storage->material_update_dependency(m_src, &ginstance->data->dependency_tracker);
	// 		}

	// 		_geometry_instance_add_surface_with_material_chain(ginstance, p_surface, material_data, m_src, p_mesh);
	// 	}
	// }
}

void RasterizerSceneGLES3::_geometry_instance_update(RenderGeometryInstance *p_geometry_instance) {
	GLES3::MeshStorage *mesh_storage = GLES3::MeshStorage::get_singleton();
	GLES3::ParticlesStorage *particles_storage = GLES3::ParticlesStorage::get_singleton();

	GeometryInstanceGLES3 *ginstance = static_cast<GeometryInstanceGLES3 *>(p_geometry_instance);

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

			ginstance->instance_count = -1;

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

			ginstance->instance_count = particles_storage->particles_get_amount(ginstance->data->base);
		} break;

		default: {
		}
	}

	bool store_transform = true;
	ginstance->base_flags = 0;

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

	} else if (ginstance->data->base_type == RS::INSTANCE_PARTICLES) {
		ginstance->base_flags |= INSTANCE_DATA_FLAG_PARTICLES;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH;

		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR;
		ginstance->base_flags |= INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA;

		if (!particles_storage->particles_is_using_local_coords(ginstance->data->base)) {
			store_transform = false;
		}

	} else if (ginstance->data->base_type == RS::INSTANCE_MESH) {
		if (mesh_storage->skeleton_is_valid(ginstance->data->skeleton)) {
			if (ginstance->data->dirty_dependencies) {
				mesh_storage->skeleton_update_dependency(ginstance->data->skeleton, &ginstance->data->dependency_tracker);
			}
		}
	}

	ginstance->store_transform_cache = store_transform;

	if (ginstance->data->dirty_dependencies) {
		ginstance->data->dependency_tracker.update_end();
		ginstance->data->dirty_dependencies = false;
	}

	ginstance->dirty_list_element.remove_from_list();
}

// Helper functions for IBL filtering

Vector3 importance_sample_GGX(Vector2 xi, float roughness4) {
	// Compute distribution direction
	float phi = 2.0 * Math_PI * xi.x;
	float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (roughness4 - 1.0) * xi.y));
	float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

	// Convert to spherical direction
	Vector3 half_vector;
	half_vector.x = sin_theta * cos(phi);
	half_vector.y = sin_theta * sin(phi);
	half_vector.z = cos_theta;

	return half_vector;
}

float distribution_GGX(float NdotH, float roughness4) {
	float NdotH2 = NdotH * NdotH;
	float denom = (NdotH2 * (roughness4 - 1.0) + 1.0);
	denom = Math_PI * denom * denom;

	return roughness4 / denom;
}

float radical_inverse_vdC(uint32_t bits) {
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
	bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
	bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
	bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);

	return float(bits) * 2.3283064365386963e-10;
}

Vector2 hammersley(uint32_t i, uint32_t N) {
	return Vector2(float(i) / float(N), radical_inverse_vdC(i));
}

/* ENVIRONMENT API */

_FORCE_INLINE_ static uint32_t _indices_to_primitives(RS::PrimitiveType p_primitive, uint32_t p_indices) {
	static const uint32_t divisor[RS::PRIMITIVE_MAX] = { 1, 2, 1, 3, 1 };
	static const uint32_t subtractor[RS::PRIMITIVE_MAX] = { 0, 0, 1, 0, 1 };
	return (p_indices - subtractor[p_primitive]) / divisor[p_primitive];
}

template <PassMode p_pass_mode>
void RasterizerSceneGLES3::_render_list_template(RenderListParameters *p_params, const RenderDataGLES3 *p_render_data, uint32_t p_from_element, uint32_t p_to_element, bool p_alpha_pass) {
	GLES3::MeshStorage *mesh_storage = GLES3::MeshStorage::get_singleton();
	GLES3::ParticlesStorage *particles_storage = GLES3::ParticlesStorage::get_singleton();
	GLES3::MaterialStorage *material_storage = GLES3::MaterialStorage::get_singleton();

	GLuint prev_vertex_array_gl = 0;
	GLuint prev_index_array_gl = 0;

	GLES3::SceneMaterialData *prev_material_data = nullptr;
	GLES3::SceneShaderData *prev_shader = nullptr;
	GeometryInstanceGLES3 *prev_inst = nullptr;
	SceneShaderGLES3::ShaderVariant prev_variant = SceneShaderGLES3::ShaderVariant::MODE_COLOR;
	SceneShaderGLES3::ShaderVariant shader_variant = SceneShaderGLES3::MODE_COLOR; // Assigned to silence wrong -Wmaybe-initialized
	uint64_t prev_spec_constants = 0;

	// Specializations constants used by all instances in the scene.
	uint64_t base_spec_constants = p_params->spec_constant_base_flags;

	if (p_render_data->view_count > 1) {
		base_spec_constants |= SceneShaderGLES3::USE_MULTIVIEW;
	}

	switch (p_pass_mode) {
		case PASS_MODE_COLOR:
		case PASS_MODE_COLOR_TRANSPARENT: {
		} break;
		case PASS_MODE_COLOR_ADDITIVE: {
			shader_variant = SceneShaderGLES3::MODE_ADDITIVE;
		} break;
		case PASS_MODE_SHADOW:
		case PASS_MODE_DEPTH: {
			shader_variant = SceneShaderGLES3::MODE_DEPTH;
		} break;
	}

	if constexpr (p_pass_mode == PASS_MODE_COLOR || p_pass_mode == PASS_MODE_COLOR_TRANSPARENT) {
		GLES3::TextureStorage *texture_storage = GLES3::TextureStorage::get_singleton();
		GLES3::Config *config = GLES3::Config::get_singleton();
		glActiveTexture(GL_TEXTURE0 + config->max_texture_image_units - 2);
		GLuint texture_to_bind = texture_storage->get_texture(texture_storage->texture_gl_get_default(GLES3::DEFAULT_GL_TEXTURE_CUBEMAP_BLACK))->tex_id;
	}

	bool should_request_redraw = false;
	if constexpr (p_pass_mode != PASS_MODE_DEPTH) {
		// Don't count elements during depth pre-pass to match the RD renderers.
		if (p_render_data->render_info) {
			p_render_data->render_info->info[RS::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RS::VIEWPORT_RENDER_INFO_OBJECTS_IN_FRAME] += p_to_element - p_from_element;
		}
	}

	for (uint32_t i = p_from_element; i < p_to_element; i++) {
		const GeometryInstanceSurface *surf = p_params->elements[i];
		GeometryInstanceGLES3 *inst = surf->owner;

		if (p_pass_mode == PASS_MODE_COLOR && !(surf->flags & GeometryInstanceSurface::FLAG_PASS_OPAQUE)) {
			continue; // Objects with "Depth-prepass" transparency are included in both render lists, but should only be rendered in the transparent pass
		}

		if (inst->instance_count == 0) {
			continue;
		}

		GLES3::SceneShaderData *shader;
		GLES3::SceneMaterialData *material_data;
		void *mesh_surface;

		if constexpr (p_pass_mode == PASS_MODE_SHADOW) {
			shader = surf->shader_shadow;
			material_data = surf->material_shadow;
			mesh_surface = surf->surface_shadow;
		} else {
			shader = surf->shader;
			material_data = surf->material;
			mesh_surface = surf->surface;
		}

		if (!mesh_surface) {
			continue;
		}

		//request a redraw if one of the shaders uses TIME
		if (shader->uses_time) {
			should_request_redraw = true;
		}

		if constexpr (p_pass_mode == PASS_MODE_COLOR_TRANSPARENT) {
			if (scene_state.current_depth_test != shader->depth_test) {
				if (shader->depth_test == GLES3::SceneShaderData::DEPTH_TEST_DISABLED) {
					glDisable(GL_DEPTH_TEST);
				} else {
					glEnable(GL_DEPTH_TEST);
				}
				scene_state.current_depth_test = shader->depth_test;
			}
		}

		if (scene_state.current_depth_draw != shader->depth_draw) {
			switch (shader->depth_draw) {
				case GLES3::SceneShaderData::DEPTH_DRAW_OPAQUE: {
					glDepthMask(p_pass_mode == PASS_MODE_COLOR);
				} break;
				case GLES3::SceneShaderData::DEPTH_DRAW_ALWAYS: {
					glDepthMask(GL_TRUE);
				} break;
				case GLES3::SceneShaderData::DEPTH_DRAW_DISABLED: {
					glDepthMask(GL_FALSE);
				} break;
			}

			scene_state.current_depth_draw = shader->depth_draw;
		}

		if constexpr (p_pass_mode == PASS_MODE_COLOR_TRANSPARENT || p_pass_mode == PASS_MODE_COLOR_ADDITIVE) {
			GLES3::SceneShaderData::BlendMode desired_blend_mode;
			if constexpr (p_pass_mode == PASS_MODE_COLOR_ADDITIVE) {
				desired_blend_mode = GLES3::SceneShaderData::BLEND_MODE_ADD;
			} else {
				desired_blend_mode = shader->blend_mode;
			}

			if (desired_blend_mode != scene_state.current_blend_mode) {
				switch (desired_blend_mode) {
					case GLES3::SceneShaderData::BLEND_MODE_MIX: {
						glBlendEquation(GL_FUNC_ADD);
						if (p_render_data->transparent_bg) {
							glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
						} else {
							glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
						}

					} break;
					case GLES3::SceneShaderData::BLEND_MODE_ADD: {
						glBlendEquation(GL_FUNC_ADD);
						glBlendFunc(p_pass_mode == PASS_MODE_COLOR_TRANSPARENT ? GL_SRC_ALPHA : GL_ONE, GL_ONE);

					} break;
					case GLES3::SceneShaderData::BLEND_MODE_SUB: {
						glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE);

					} break;
					case GLES3::SceneShaderData::BLEND_MODE_MUL: {
						glBlendEquation(GL_FUNC_ADD);
						if (p_render_data->transparent_bg) {
							glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO);
						} else {
							glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
						}

					} break;
					case GLES3::SceneShaderData::BLEND_MODE_ALPHA_TO_COVERAGE: {
						// Do nothing for now.
					} break;
				}
				scene_state.current_blend_mode = desired_blend_mode;
			}
		}

		//find cull variant
		GLES3::SceneShaderData::Cull cull_mode = shader->cull_mode;

		if ((surf->flags & GeometryInstanceSurface::FLAG_USES_DOUBLE_SIDED_SHADOWS)) {
			cull_mode = GLES3::SceneShaderData::CULL_DISABLED;
		} else {
			bool mirror = inst->mirror;
			if (p_params->reverse_cull) {
				mirror = !mirror;
			}
			if (cull_mode == GLES3::SceneShaderData::CULL_FRONT && mirror) {
				cull_mode = GLES3::SceneShaderData::CULL_BACK;
			} else if (cull_mode == GLES3::SceneShaderData::CULL_BACK && mirror) {
				cull_mode = GLES3::SceneShaderData::CULL_FRONT;
			}
		}

		if (scene_state.cull_mode != cull_mode) {
			if (cull_mode == GLES3::SceneShaderData::CULL_DISABLED) {
				glDisable(GL_CULL_FACE);
			} else {
				if (scene_state.cull_mode == GLES3::SceneShaderData::CULL_DISABLED) {
					// Last time was disabled, so enable and set proper face.
					glEnable(GL_CULL_FACE);
				}
				glCullFace(cull_mode == GLES3::SceneShaderData::CULL_FRONT ? GL_FRONT : GL_BACK);
			}
			scene_state.cull_mode = cull_mode;
		}

		RS::PrimitiveType primitive = surf->primitive;
		if (shader->uses_point_size) {
			primitive = RS::PRIMITIVE_POINTS;
		}
		static const GLenum prim[5] = { GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP };
		GLenum primitive_gl = prim[int(primitive)];

		GLuint vertex_array_gl = 0;
		GLuint index_array_gl = 0;

		//skeleton and blend shape
		if (surf->owner->mesh_instance.is_valid()) {
			mesh_storage->mesh_instance_surface_get_vertex_arrays_and_format(surf->owner->mesh_instance, surf->surface_index, shader->vertex_input_mask, vertex_array_gl);
		} else {
			mesh_storage->mesh_surface_get_vertex_arrays_and_format(mesh_surface, shader->vertex_input_mask, vertex_array_gl);
		}

		index_array_gl = mesh_storage->mesh_surface_get_index_buffer(mesh_surface, surf->lod_index);

		if (prev_vertex_array_gl != vertex_array_gl) {
			if (vertex_array_gl != 0) {
				glBindVertexArray(vertex_array_gl);
			}
			prev_vertex_array_gl = vertex_array_gl;

			// Invalidate the previous index array
			prev_index_array_gl = 0;
		}

		bool use_index_buffer = index_array_gl != 0;
		if (prev_index_array_gl != index_array_gl) {
			if (index_array_gl != 0) {
				// Bind index each time so we can use LODs
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_array_gl);
			}
			prev_index_array_gl = index_array_gl;
		}

		Transform3D world_transform;
		if (inst->store_transform_cache) {
			world_transform = inst->transform;
		}

		if (prev_material_data != material_data) {
			material_data->bind_uniforms();
			prev_material_data = material_data;
		}

		SceneShaderGLES3::ShaderVariant instance_variant = shader_variant;
		if (inst->instance_count > 0) {
			// Will need to use instancing to draw (either MultiMesh or Particles).
			instance_variant = SceneShaderGLES3::ShaderVariant(1 + int(shader_variant));
		}

		uint64_t spec_constants = base_spec_constants;

		if (inst->omni_light_count == 0) {
			spec_constants |= SceneShaderGLES3::DISABLE_LIGHT_OMNI;
		}

		if (inst->spot_light_count == 0) {
			spec_constants |= SceneShaderGLES3::DISABLE_LIGHT_SPOT;
		}

		if (prev_shader != shader || prev_variant != instance_variant || spec_constants != prev_spec_constants) {
			bool success = material_storage->shaders.scene_shader.version_bind_shader(shader->version, instance_variant, spec_constants);
			if (!success) {
				continue;
			}

			float opaque_prepass_threshold = 0.0;
			if constexpr (p_pass_mode == PASS_MODE_DEPTH) {
				opaque_prepass_threshold = 0.99;
			} else if constexpr (p_pass_mode == PASS_MODE_SHADOW) {
				opaque_prepass_threshold = 0.1;
			}

			material_storage->shaders.scene_shader.version_set_uniform(SceneShaderGLES3::OPAQUE_PREPASS_THRESHOLD, opaque_prepass_threshold, shader->version, instance_variant, spec_constants);

			prev_shader = shader;
			prev_variant = instance_variant;
			prev_spec_constants = spec_constants;
		}

		if (prev_inst != inst || prev_shader != shader || prev_variant != instance_variant) {
			// Rebind the light indices.
			material_storage->shaders.scene_shader.version_set_uniform(SceneShaderGLES3::OMNI_LIGHT_COUNT, inst->omni_light_count, shader->version, instance_variant, spec_constants);
			material_storage->shaders.scene_shader.version_set_uniform(SceneShaderGLES3::SPOT_LIGHT_COUNT, inst->spot_light_count, shader->version, instance_variant, spec_constants);

			if (inst->omni_light_count) {
				glUniform1uiv(material_storage->shaders.scene_shader.version_get_uniform(SceneShaderGLES3::OMNI_LIGHT_INDICES, shader->version, instance_variant, spec_constants), inst->omni_light_count, inst->omni_light_gl_cache.ptr());
			}

			if (inst->spot_light_count) {
				glUniform1uiv(material_storage->shaders.scene_shader.version_get_uniform(SceneShaderGLES3::SPOT_LIGHT_INDICES, shader->version, instance_variant, spec_constants), inst->spot_light_count, inst->spot_light_gl_cache.ptr());
			}

			prev_inst = inst;
		}

		material_storage->shaders.scene_shader.version_set_uniform(SceneShaderGLES3::WORLD_TRANSFORM, world_transform, shader->version, instance_variant, spec_constants);

		// Can be index count or vertex count
		uint32_t count = 0;
		if (surf->lod_index > 0) {
			count = surf->index_count;
		} else {
			count = mesh_storage->mesh_surface_get_vertices_drawn_count(mesh_surface);
		}
		if constexpr (p_pass_mode != PASS_MODE_DEPTH) {
			// Don't count draw calls during depth pre-pass to match the RD renderers.
			if (p_render_data->render_info) {
				p_render_data->render_info->info[RS::VIEWPORT_RENDER_INFO_TYPE_VISIBLE][RS::VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME]++;
			}
		}

		if (inst->instance_count > 0) {
			// Using MultiMesh or Particles.
			// Bind instance buffers.

			GLuint instance_buffer = 0;
			uint32_t stride = 0;
			if (inst->flags_cache & INSTANCE_DATA_FLAG_PARTICLES) {
				instance_buffer = particles_storage->particles_get_gl_buffer(inst->data->base);
				stride = 16; // 12 bytes for instance transform and 4 bytes for packed color and custom.
			} else {
				instance_buffer = mesh_storage->multimesh_get_gl_buffer(inst->data->base);
				stride = mesh_storage->multimesh_get_stride(inst->data->base);
			}

			if (instance_buffer == 0) {
				// Instance buffer not initialized yet. Skip rendering for now.
				continue;
			}

			glBindBuffer(GL_ARRAY_BUFFER, instance_buffer);

			glEnableVertexAttribArray(12);
			glVertexAttribPointer(12, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), CAST_INT_TO_UCHAR_PTR(0));
			glVertexAttribDivisor(12, 1);
			glEnableVertexAttribArray(13);
			glVertexAttribPointer(13, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), CAST_INT_TO_UCHAR_PTR(sizeof(float) * 4));
			glVertexAttribDivisor(13, 1);
			if (!(inst->flags_cache & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D)) {
				glEnableVertexAttribArray(14);
				glVertexAttribPointer(14, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), CAST_INT_TO_UCHAR_PTR(sizeof(float) * 8));
				glVertexAttribDivisor(14, 1);
			}

			if ((inst->flags_cache & INSTANCE_DATA_FLAG_MULTIMESH_HAS_COLOR) || (inst->flags_cache & INSTANCE_DATA_FLAG_MULTIMESH_HAS_CUSTOM_DATA)) {
				uint32_t color_custom_offset = inst->flags_cache & INSTANCE_DATA_FLAG_MULTIMESH_FORMAT_2D ? 8 : 12;
				glEnableVertexAttribArray(15);
				glVertexAttribIPointer(15, 4, GL_UNSIGNED_INT, stride * sizeof(float), CAST_INT_TO_UCHAR_PTR(color_custom_offset * sizeof(float)));
				glVertexAttribDivisor(15, 1);
			}
			if (use_index_buffer) {
				glDrawElementsInstanced(primitive_gl, count, mesh_storage->mesh_surface_get_index_type(mesh_surface), 0, inst->instance_count);
			} else {
				glDrawArraysInstanced(primitive_gl, 0, count, inst->instance_count);
			}
		} else {
			// Using regular Mesh.
			if (use_index_buffer) {
				glDrawElements(primitive_gl, count, mesh_storage->mesh_surface_get_index_type(mesh_surface), 0);
			} else {
				glDrawArrays(primitive_gl, 0, count);
			}
		}
		if (inst->instance_count > 0) {
			glDisableVertexAttribArray(12);
			glDisableVertexAttribArray(13);
			glDisableVertexAttribArray(14);
			glDisableVertexAttribArray(15);
		}
	}

	// Make the actual redraw request
	if (should_request_redraw) {
		RenderingServerDefault::redraw_request();
	}
}

void RasterizerSceneGLES3::render_material(const Transform3D &p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, const PagedArray<RenderGeometryInstance *> &p_instances, RID p_framebuffer, const Rect2i &p_region) {
}

void RasterizerSceneGLES3::set_time(double p_time, double p_step) {
	time = p_time;
	time_step = p_step;
}

void RasterizerSceneGLES3::set_debug_draw_mode(RS::ViewportDebugDraw p_debug_draw) {
	debug_draw = p_debug_draw;
}

Ref<RenderSceneBuffers> RasterizerSceneGLES3::render_buffers_create() {
	Ref<RenderSceneBuffersGLES3> rb;
	rb.instantiate();
	return rb;
}

void RasterizerSceneGLES3::_render_buffers_debug_draw(Ref<RenderSceneBuffersGLES3> p_render_buffers, RID p_shadow_atlas, RID p_occlusion_buffer) {
}

bool RasterizerSceneGLES3::free(RID p_rid) {
	if (GLES3::LightStorage::get_singleton()->owns_light_instance(p_rid)) {
		GLES3::LightStorage::get_singleton()->light_instance_free(p_rid);
	}
	return true;
}

void RasterizerSceneGLES3::update() {
}

void RasterizerSceneGLES3::decals_set_filter(RS::DecalFilter p_filter) {
}

void RasterizerSceneGLES3::light_projectors_set_filter(RS::LightProjectorFilter p_filter) {
}

RasterizerSceneGLES3::RasterizerSceneGLES3() {
	singleton = this;

	GLES3::MaterialStorage *material_storage = GLES3::MaterialStorage::get_singleton();
	GLES3::Config *config = GLES3::Config::get_singleton();

	// Quality settings.
	use_physical_light_units = GLOBAL_GET("rendering/lights_and_shadows/use_physical_light_units");

	{
		// Setup Lights

		config->max_renderable_lights = MIN(config->max_renderable_lights, config->max_uniform_buffer_size / (int)sizeof(RasterizerSceneGLES3::LightData));
		config->max_lights_per_object = MIN(config->max_lights_per_object, config->max_renderable_lights);

		uint32_t light_buffer_size = config->max_renderable_lights * sizeof(LightData);
		scene_state.omni_lights = memnew_arr(LightData, config->max_renderable_lights);
		scene_state.omni_light_sort = memnew_arr(InstanceSort<GLES3::LightInstance>, config->max_renderable_lights);
		glGenBuffers(1, &scene_state.omni_light_buffer);
		glBindBuffer(GL_UNIFORM_BUFFER, scene_state.omni_light_buffer);
		GLES3::Utilities::get_singleton()->buffer_allocate_data(GL_UNIFORM_BUFFER, scene_state.omni_light_buffer, light_buffer_size, nullptr, GL_STREAM_DRAW, "OmniLight UBO");

		scene_state.spot_lights = memnew_arr(LightData, config->max_renderable_lights);
		scene_state.spot_light_sort = memnew_arr(InstanceSort<GLES3::LightInstance>, config->max_renderable_lights);
		glGenBuffers(1, &scene_state.spot_light_buffer);
		glBindBuffer(GL_UNIFORM_BUFFER, scene_state.spot_light_buffer);
		GLES3::Utilities::get_singleton()->buffer_allocate_data(GL_UNIFORM_BUFFER, scene_state.spot_light_buffer, light_buffer_size, nullptr, GL_STREAM_DRAW, "SpotLight UBO");

		uint32_t directional_light_buffer_size = MAX_DIRECTIONAL_LIGHTS * sizeof(DirectionalLightData);
		scene_state.directional_lights = memnew_arr(DirectionalLightData, MAX_DIRECTIONAL_LIGHTS);
		glGenBuffers(1, &scene_state.directional_light_buffer);
		glBindBuffer(GL_UNIFORM_BUFFER, scene_state.directional_light_buffer);
		GLES3::Utilities::get_singleton()->buffer_allocate_data(GL_UNIFORM_BUFFER, scene_state.directional_light_buffer, directional_light_buffer_size, nullptr, GL_STREAM_DRAW, "DirectionalLight UBO");

		glBindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	// {
	// 	sky_globals.max_directional_lights = 4;
	// 	uint32_t directional_light_buffer_size = sky_globals.max_directional_lights * sizeof(DirectionalLightData);
	// 	sky_globals.directional_lights = memnew_arr(DirectionalLightData, sky_globals.max_directional_lights);
	// 	sky_globals.last_frame_directional_lights = memnew_arr(DirectionalLightData, sky_globals.max_directional_lights);
	// 	sky_globals.last_frame_directional_light_count = sky_globals.max_directional_lights + 1;
	// 	glGenBuffers(1, &sky_globals.directional_light_buffer);
	// 	glBindBuffer(GL_UNIFORM_BUFFER, sky_globals.directional_light_buffer);
	// 	GLES3::Utilities::get_singleton()->buffer_allocate_data(GL_UNIFORM_BUFFER, sky_globals.directional_light_buffer, directional_light_buffer_size, nullptr, GL_STREAM_DRAW, "Sky DirectionalLight UBO");

	// 	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	// }

	{
		String global_defines;
		global_defines += "#define MAX_GLOBAL_SHADER_UNIFORMS 256\n"; // TODO: this is arbitrary for now
		global_defines += "\n#define MAX_LIGHT_DATA_STRUCTS " + itos(config->max_renderable_lights) + "\n";
		global_defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "\n";
		global_defines += "\n#define MAX_FORWARD_LIGHTS uint(" + itos(config->max_lights_per_object) + ")\n";
		material_storage->shaders.scene_shader.initialize(global_defines);
		scene_globals.shader_default_version = material_storage->shaders.scene_shader.version_create();
		material_storage->shaders.scene_shader.version_bind_shader(scene_globals.shader_default_version, SceneShaderGLES3::MODE_COLOR);
	}

	// 	{
	// 		//default material and shader
	// 		scene_globals.default_shader = material_storage->shader_allocate();
	// 		material_storage->shader_initialize(scene_globals.default_shader);
	// 		material_storage->shader_set_code(scene_globals.default_shader, R"(
	// // Default 3D material shader.

	// shader_type spatial;

	// void vertex() {
	// 	ROUGHNESS = 0.8;
	// }

	// void fragment() {
	// 	ALBEDO = vec3(0.6);
	// 	ROUGHNESS = 0.8;
	// 	METALLIC = 0.2;
	// }
	// )");
	// 		scene_globals.default_material = material_storage->material_allocate();
	// 		material_storage->material_initialize(scene_globals.default_material);
	// 		material_storage->material_set_shader(scene_globals.default_material, scene_globals.default_shader);
	// 	}

	// {
	// 	// Initialize Sky stuff
	// 	sky_globals.roughness_layers = GLOBAL_GET("rendering/reflections/sky_reflections/roughness_layers");
	// 	sky_globals.ggx_samples = GLOBAL_GET("rendering/reflections/sky_reflections/ggx_samples");

	// 	String global_defines;
	// 	global_defines += "#define MAX_GLOBAL_SHADER_UNIFORMS 256\n"; // TODO: this is arbitrary for now
	// 	global_defines += "\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS " + itos(sky_globals.max_directional_lights) + "\n";
	// 	material_storage->shaders.sky_shader.initialize(global_defines);
	// 	sky_globals.shader_default_version = material_storage->shaders.sky_shader.version_create();
	// }

	// {
	// 	String global_defines;
	// 	global_defines += "\n#define MAX_SAMPLE_COUNT " + itos(sky_globals.ggx_samples) + "\n";
	// 	material_storage->shaders.cubemap_filter_shader.initialize(global_defines);
	// 	scene_globals.cubemap_filter_shader_version = material_storage->shaders.cubemap_filter_shader.version_create();
	// }

	// 	{
	// 		sky_globals.default_shader = material_storage->shader_allocate();

	// 		material_storage->shader_initialize(sky_globals.default_shader);

	// 		material_storage->shader_set_code(sky_globals.default_shader, R"(
	// // Default sky shader.

	// shader_type sky;

	// void sky() {
	// 	COLOR = vec3(0.0);
	// }
	// )");
	// 		sky_globals.default_material = material_storage->material_allocate();
	// 		material_storage->material_initialize(sky_globals.default_material);

	// 		material_storage->material_set_shader(sky_globals.default_material, sky_globals.default_shader);
	// 	}
	// 	{
	// 		sky_globals.fog_shader = material_storage->shader_allocate();
	// 		material_storage->shader_initialize(sky_globals.fog_shader);

	// 		material_storage->shader_set_code(sky_globals.fog_shader, R"(
	// // Default clear color sky shader.

	// shader_type sky;

	// uniform vec4 clear_color;

	// void sky() {
	// 	COLOR = clear_color.rgb;
	// }
	// )");
	// 		sky_globals.fog_material = material_storage->material_allocate();
	// 		material_storage->material_initialize(sky_globals.fog_material);

	// 		material_storage->material_set_shader(sky_globals.fog_material, sky_globals.fog_shader);
	// 	}

	// 	{
	// 		glGenVertexArrays(1, &sky_globals.screen_triangle_array);
	// 		glBindVertexArray(sky_globals.screen_triangle_array);
	// 		glGenBuffers(1, &sky_globals.screen_triangle);
	// 		glBindBuffer(GL_ARRAY_BUFFER, sky_globals.screen_triangle);

	// 		const float qv[6] = {
	// 			-1.0f,
	// 			-1.0f,
	// 			3.0f,
	// 			-1.0f,
	// 			-1.0f,
	// 			3.0f,
	// 		};

	// 		GLES3::Utilities::get_singleton()->buffer_allocate_data(GL_ARRAY_BUFFER, sky_globals.screen_triangle, sizeof(float) * 6, qv, GL_STATIC_DRAW, "Screen triangle vertex buffer");

	// 		glVertexAttribPointer(RS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
	// 		glEnableVertexAttribArray(RS::ARRAY_VERTEX);
	// 		glBindVertexArray(0);
	// 		glBindBuffer(GL_ARRAY_BUFFER, 0); //unbind
	// 	}

#ifdef GLES_OVER_GL
	glEnable(_EXT_TEXTURE_CUBE_MAP_SEAMLESS);
#endif

	// MultiMesh may read from color when color is disabled, so make sure that the color defaults to white instead of black;
	glVertexAttrib4f(RS::ARRAY_COLOR, 1.0, 1.0, 1.0, 1.0);
}

RasterizerSceneGLES3::~RasterizerSceneGLES3() {
	GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.directional_light_buffer);
	GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.omni_light_buffer);
	GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.spot_light_buffer);
	memdelete_arr(scene_state.directional_lights);
	memdelete_arr(scene_state.omni_lights);
	memdelete_arr(scene_state.spot_lights);
	memdelete_arr(scene_state.omni_light_sort);
	memdelete_arr(scene_state.spot_light_sort);

	// Scene Shader
	GLES3::MaterialStorage::get_singleton()->shaders.scene_shader.version_free(scene_globals.shader_default_version);
	GLES3::MaterialStorage::get_singleton()->shaders.cubemap_filter_shader.version_free(scene_globals.cubemap_filter_shader_version);
	RSG::material_storage->material_free(scene_globals.default_material);
	RSG::material_storage->shader_free(scene_globals.default_shader);

	// Sky Shader
	// GLES3::MaterialStorage::get_singleton()->shaders.sky_shader.version_free(sky_globals.shader_default_version);
	// RSG::material_storage->material_free(sky_globals.default_material);
	// RSG::material_storage->shader_free(sky_globals.default_shader);
	// RSG::material_storage->material_free(sky_globals.fog_material);
	// RSG::material_storage->shader_free(sky_globals.fog_shader);
	// GLES3::Utilities::get_singleton()->buffer_free_data(sky_globals.screen_triangle);
	// glDeleteVertexArrays(1, &sky_globals.screen_triangle_array);
	// glDeleteTextures(1, &sky_globals.radical_inverse_vdc_cache_tex);
	// GLES3::Utilities::get_singleton()->buffer_free_data(sky_globals.directional_light_buffer);
	// memdelete_arr(sky_globals.directional_lights);
	// memdelete_arr(sky_globals.last_frame_directional_lights);

	// UBOs
	if (scene_state.ubo_buffer != 0) {
		GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.ubo_buffer);
	}

	if (scene_state.multiview_buffer != 0) {
		GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.multiview_buffer);
	}

	if (scene_state.tonemap_buffer != 0) {
		GLES3::Utilities::get_singleton()->buffer_free_data(scene_state.tonemap_buffer);
	}

	singleton = nullptr;
}

#endif // GLES3_ENABLED
