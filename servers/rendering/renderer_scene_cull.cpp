/**************************************************************************/
/*  renderer_scene_cull.cpp                                               */
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

#include "renderer_scene_cull.h"

#include "core/config/project_settings.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "rendering_server_default.h"

#include <new>

/* CAMERA API */

RID RendererSceneCull::camera_allocate() {
	return camera_owner.allocate_rid();
}
void RendererSceneCull::camera_initialize(RID p_rid) {
	camera_owner.initialize_rid(p_rid);
}

void RendererSceneCull::camera_set_perspective(RID p_camera, float p_fovy_degrees, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);
	camera->type = Camera::PERSPECTIVE;
	camera->fov = p_fovy_degrees;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_orthogonal(RID p_camera, float p_size, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);
	camera->type = Camera::ORTHOGONAL;
	camera->size = p_size;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_frustum(RID p_camera, float p_size, Vector2 p_offset, float p_z_near, float p_z_far) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);
	camera->type = Camera::FRUSTUM;
	camera->size = p_size;
	camera->offset = p_offset;
	camera->znear = p_z_near;
	camera->zfar = p_z_far;
}

void RendererSceneCull::camera_set_transform(RID p_camera, const Transform3D &p_transform) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);
	camera->transform = p_transform.orthonormalized();
}

void RendererSceneCull::camera_set_cull_mask(RID p_camera, uint32_t p_layers) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);

	camera->visible_layers = p_layers;
}

void RendererSceneCull::camera_set_use_vertical_aspect(RID p_camera, bool p_enable) {
	Camera *camera = camera_owner.get_or_null(p_camera);
	ERR_FAIL_COND(!camera);
	camera->vaspect = p_enable;
}

bool RendererSceneCull::is_camera(RID p_camera) const {
	return camera_owner.owns(p_camera);
}

/* OCCLUDER API */

RID RendererSceneCull::occluder_allocate() {
	return RendererSceneOcclusionCull::get_singleton()->occluder_allocate();
}

void RendererSceneCull::occluder_initialize(RID p_rid) {
	RendererSceneOcclusionCull::get_singleton()->occluder_initialize(p_rid);
}

void RendererSceneCull::occluder_set_mesh(RID p_occluder, const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices) {
	RendererSceneOcclusionCull::get_singleton()->occluder_set_mesh(p_occluder, p_vertices, p_indices);
}

/* SCENARIO API */

void RendererSceneCull::_instance_pair(Instance *p_A, Instance *p_B) {
	RendererSceneCull *self = (RendererSceneCull *)singleton;
	Instance *A = p_A;
	Instance *B = p_B;

	//instance indices are designed so greater always contains lesser
	if (A->base_type > B->base_type) {
		SWAP(A, B); //lesser always first
	}

	if (B->base_type == RS::INSTANCE_LIGHT && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightData *light = static_cast<InstanceLightData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->lights.insert(B);
		light->geometries.insert(A);

		if (geom->can_cast_shadows) {
			light->shadow_dirty = true;
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_LIGHTING_DIRTY;
		}

		if (light->uses_projector) {
			geom->projector_count++;
			if (geom->projector_count == 1) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

		if (light->uses_softshadow) {
			geom->softshadow_count++;
			if (geom->softshadow_count == 1) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

	} else if (self->geometry_instance_pair_mask & (1 << RS::INSTANCE_DECAL) && B->base_type == RS::INSTANCE_DECAL && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->decals.insert(B);
		decal->geometries.insert(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_DECAL_DIRTY;
		}

	} else if (B->base_type == RS::INSTANCE_LIGHTMAP && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		if (A->dynamic_gi) {
			geom->lightmap_captures.insert(B);
			lightmap_data->geometries.insert(A);

			if (A->scenario && A->array_index >= 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_LIGHTMAP_CAPTURE;
			}
			((RendererSceneCull *)self)->_instance_queue_update(A, false, false); //need to update capture
		}

	} else if (B->base_type == RS::INSTANCE_PARTICLES_COLLISION && A->base_type == RS::INSTANCE_PARTICLES) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(B->base_data);
		RSG::particles_storage->particles_add_collision(A->base, collision->instance);
	}
}

void RendererSceneCull::_instance_unpair(Instance *p_A, Instance *p_B) {
	RendererSceneCull *self = (RendererSceneCull *)singleton;
	Instance *A = p_A;
	Instance *B = p_B;

	//instance indices are designed so greater always contains lesser
	if (A->base_type > B->base_type) {
		SWAP(A, B); //lesser always first
	}

	if (B->base_type == RS::INSTANCE_LIGHT && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightData *light = static_cast<InstanceLightData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->lights.erase(B);
		light->geometries.erase(A);

		if (geom->can_cast_shadows) {
			light->shadow_dirty = true;
		}

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_LIGHTING_DIRTY;
		}

		if (light->uses_projector) {
#ifdef DEBUG_ENABLED
			if (geom->projector_count == 0) {
				ERR_PRINT("geom->projector_count==0 - BUG!");
			}
#endif
			geom->projector_count--;
			if (geom->projector_count == 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

		if (light->uses_softshadow) {
#ifdef DEBUG_ENABLED
			if (geom->softshadow_count == 0) {
				ERR_PRINT("geom->softshadow_count==0 - BUG!");
			}
#endif
			geom->softshadow_count--;
			if (geom->softshadow_count == 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags |= InstanceData::FLAG_GEOM_PROJECTOR_SOFTSHADOW_DIRTY;
			}
		}

	} else if (self->geometry_instance_pair_mask & (1 << RS::INSTANCE_DECAL) && B->base_type == RS::INSTANCE_DECAL && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);

		geom->decals.erase(B);
		decal->geometries.erase(A);

		if (A->scenario && A->array_index >= 0) {
			InstanceData &idata = A->scenario->instance_data[A->array_index];
			idata.flags |= InstanceData::FLAG_GEOM_DECAL_DIRTY;
		}

	} else if (B->base_type == RS::INSTANCE_LIGHTMAP && ((1 << A->base_type) & RS::INSTANCE_GEOMETRY_MASK)) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(B->base_data);
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(A->base_data);
		if (A->dynamic_gi) {
			geom->lightmap_captures.erase(B);

			if (geom->lightmap_captures.is_empty() && A->scenario && A->array_index >= 0) {
				InstanceData &idata = A->scenario->instance_data[A->array_index];
				idata.flags &= ~uint32_t(InstanceData::FLAG_LIGHTMAP_CAPTURE);
			}

			lightmap_data->geometries.erase(A);
			((RendererSceneCull *)self)->_instance_queue_update(A, false, false); //need to update capture
		}

	} else if (B->base_type == RS::INSTANCE_PARTICLES_COLLISION && A->base_type == RS::INSTANCE_PARTICLES) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(B->base_data);
		RSG::particles_storage->particles_remove_collision(A->base, collision->instance);
	}
}

/* INSTANCING API */

void RendererSceneCull::_instance_queue_update(Instance *p_instance, bool p_update_aabb, bool p_update_dependencies) {
	if (p_update_aabb) {
		p_instance->update_aabb = true;
	}
	if (p_update_dependencies) {
		p_instance->update_dependencies = true;
	}

	if (p_instance->update_item.in_list()) {
		return;
	}

	_instance_update_list.add(&p_instance->update_item);
}

RID RendererSceneCull::instance_allocate() {
	return instance_owner.allocate_rid();
}
void RendererSceneCull::instance_initialize(RID p_rid) {
	instance_owner.initialize_rid(p_rid);
	Instance *instance = instance_owner.get_or_null(p_rid);
	instance->self = p_rid;
}

void RendererSceneCull::_instance_update_mesh_instance(Instance *p_instance) {
	bool needs_instance = RSG::mesh_storage->mesh_needs_instance(p_instance->base, p_instance->skeleton.is_valid());
	if (needs_instance != p_instance->mesh_instance.is_valid()) {
		if (needs_instance) {
			p_instance->mesh_instance = RSG::mesh_storage->mesh_instance_create(p_instance->base);

		} else {
			RSG::mesh_storage->mesh_instance_free(p_instance->mesh_instance);
			p_instance->mesh_instance = RID();
		}

		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		geom->geometry_instance->set_mesh_instance(p_instance->mesh_instance);

		if (p_instance->scenario && p_instance->array_index >= 0) {
			InstanceData &idata = p_instance->scenario->instance_data[p_instance->array_index];
			if (p_instance->mesh_instance.is_valid()) {
				idata.flags |= InstanceData::FLAG_USES_MESH_INSTANCE;
			} else {
				idata.flags &= ~uint32_t(InstanceData::FLAG_USES_MESH_INSTANCE);
			}
		}
	}

	if (p_instance->mesh_instance.is_valid()) {
		RSG::mesh_storage->mesh_instance_set_skeleton(p_instance->mesh_instance, p_instance->skeleton);
	}
}

void RendererSceneCull::instance_set_layer_mask(RID p_instance, uint32_t p_mask) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->layer_mask == p_mask) {
		return;
	}

	instance->layer_mask = p_mask;
	if (instance->scenario && instance->array_index >= 0) {
		instance->scenario->instance_data[instance->array_index].layer_mask = p_mask;
	}

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_layer_mask(p_mask);

		if (geom->can_cast_shadows) {
			for (HashSet<RendererSceneCull::Instance *>::Iterator I = geom->lights.begin(); I != geom->lights.end(); ++I) {
				InstanceLightData *light = static_cast<InstanceLightData *>((*I)->base_data);
				light->shadow_dirty = true;
			}
		}
	}
}

void RendererSceneCull::instance_set_pivot_data(RID p_instance, float p_sorting_offset, bool p_use_aabb_center) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->sorting_offset = p_sorting_offset;
	instance->use_aabb_center = p_use_aabb_center;

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_pivot_data(p_sorting_offset, p_use_aabb_center);
	} else if (instance->base_type == RS::INSTANCE_DECAL && instance->base_data) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(instance->base_data);
		RSG::texture_storage->decal_instance_set_sorting_offset(decal->instance, instance->sorting_offset);
	}
}

void RendererSceneCull::instance_geometry_set_transparency(RID p_instance, float p_transparency) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->transparency = p_transparency;

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_transparency(p_transparency);
	}
}

void RendererSceneCull::instance_set_transform(RID p_instance, const Transform3D &p_transform) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->transform == p_transform) {
		return; //must be checked to avoid worst evil
	}

#ifdef DEBUG_ENABLED

	for (int i = 0; i < 4; i++) {
		const Vector3 &v = i < 3 ? p_transform.basis.rows[i] : p_transform.origin;
		ERR_FAIL_COND(!v.is_finite());
	}

#endif
	instance->transform = p_transform;
	_instance_queue_update(instance, true);
}

void RendererSceneCull::instance_attach_object_instance_id(RID p_instance, ObjectID p_id) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->object_id = p_id;
}

void RendererSceneCull::instance_set_blend_shape_weight(RID p_instance, int p_shape, float p_weight) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->update_item.in_list()) {
		_update_dirty_instance(instance);
	}

	if (instance->mesh_instance.is_valid()) {
		RSG::mesh_storage->mesh_instance_set_blend_shape_weight(instance->mesh_instance, p_shape, p_weight);
	}
}

void RendererSceneCull::instance_set_surface_override_material(RID p_instance, int p_surface, RID p_material) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->base_type == RS::INSTANCE_MESH) {
		//may not have been updated yet, may also have not been set yet. When updated will be correcte, worst case
		instance->materials.resize(MAX(p_surface + 1, RSG::mesh_storage->mesh_get_surface_count(instance->base)));
	}

	ERR_FAIL_INDEX(p_surface, instance->materials.size());

	instance->materials.write[p_surface] = p_material;

	_instance_queue_update(instance, false, true);
}

void RendererSceneCull::instance_set_visible(RID p_instance, bool p_visible) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->visible == p_visible) {
		return;
	}

	instance->visible = p_visible;

	if (p_visible) {
		if (instance->scenario != nullptr) {
			_instance_queue_update(instance, true, false);
		}
	} else if (instance->indexer_id.is_valid()) {
		_unpair_instance(instance);
	}

	if (instance->base_type == RS::INSTANCE_LIGHT) {
		InstanceLightData *light = static_cast<InstanceLightData *>(instance->base_data);
		if (instance->scenario && RSG::light_storage->light_get_type(instance->base) != RS::LIGHT_DIRECTIONAL && light->bake_mode == RS::LIGHT_BAKE_DYNAMIC) {
			if (p_visible) {
				instance->scenario->dynamic_lights.push_back(light->instance);
			} else {
				instance->scenario->dynamic_lights.erase(light->instance);
			}
		}
	}

	if (instance->base_type == RS::INSTANCE_PARTICLES_COLLISION) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(instance->base_data);
		RSG::particles_storage->particles_collision_instance_set_active(collision->instance, p_visible);
	}

	if (instance->base_type == RS::INSTANCE_OCCLUDER) {
		if (instance->scenario) {
			RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(instance->scenario->self, p_instance, instance->base, instance->transform, p_visible);
		}
	}
}

inline bool is_geometry_instance(RenderingServer::InstanceType p_type) {
	return p_type == RS::INSTANCE_MESH || p_type == RS::INSTANCE_MULTIMESH || p_type == RS::INSTANCE_PARTICLES;
}

void RendererSceneCull::instance_attach_skeleton(RID p_instance, RID p_skeleton) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->skeleton == p_skeleton) {
		return;
	}

	instance->skeleton = p_skeleton;

	if (p_skeleton.is_valid()) {
		//update the dependency now, so if cleared, we remove it
		RSG::mesh_storage->skeleton_update_dependency(p_skeleton, &instance->dependency_tracker);
	}

	_instance_queue_update(instance, true, true);

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		_instance_update_mesh_instance(instance);

		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_skeleton(p_skeleton);
	}
}

void RendererSceneCull::instance_set_extra_visibility_margin(RID p_instance, real_t p_margin) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->extra_margin = p_margin;
	_instance_queue_update(instance, true, false);
}

void RendererSceneCull::instance_set_ignore_culling(RID p_instance, bool p_enabled) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);
	instance->ignore_all_culling = p_enabled;

	if (instance->scenario && instance->array_index >= 0) {
		InstanceData &idata = instance->scenario->instance_data[instance->array_index];
		if (instance->ignore_all_culling) {
			idata.flags |= InstanceData::FLAG_IGNORE_ALL_CULLING;
		} else {
			idata.flags &= ~uint32_t(InstanceData::FLAG_IGNORE_ALL_CULLING);
		}
	}
}

Vector<ObjectID> RendererSceneCull::instances_cull_aabb(const AABB &p_aabb, RID p_scenario) const {
	Vector<ObjectID> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_COND_V(!scenario, instances);

	const_cast<RendererSceneCull *>(this)->update_dirty_instances(); // check dirty instances before culling

	struct CullAABB {
		Vector<ObjectID> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (!p_instance->object_id.is_null()) {
				instances.push_back(p_instance->object_id);
			}
			return false;
		}
	};

	CullAABB cull_aabb;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].aabb_query(p_aabb, cull_aabb);
	scenario->indexers[Scenario::INDEXER_VOLUMES].aabb_query(p_aabb, cull_aabb);
	return cull_aabb.instances;
}

Vector<ObjectID> RendererSceneCull::instances_cull_ray(const Vector3 &p_from, const Vector3 &p_to, RID p_scenario) const {
	Vector<ObjectID> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_COND_V(!scenario, instances);
	const_cast<RendererSceneCull *>(this)->update_dirty_instances(); // check dirty instances before culling

	struct CullRay {
		Vector<ObjectID> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (!p_instance->object_id.is_null()) {
				instances.push_back(p_instance->object_id);
			}
			return false;
		}
	};

	CullRay cull_ray;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].ray_query(p_from, p_to, cull_ray);
	scenario->indexers[Scenario::INDEXER_VOLUMES].ray_query(p_from, p_to, cull_ray);
	return cull_ray.instances;
}

Vector<ObjectID> RendererSceneCull::instances_cull_convex(const Vector<Plane> &p_convex, RID p_scenario) const {
	Vector<ObjectID> instances;
	Scenario *scenario = scenario_owner.get_or_null(p_scenario);
	ERR_FAIL_COND_V(!scenario, instances);
	const_cast<RendererSceneCull *>(this)->update_dirty_instances(); // check dirty instances before culling

	Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(&p_convex[0], p_convex.size());

	struct CullConvex {
		Vector<ObjectID> instances;
		_FORCE_INLINE_ bool operator()(void *p_data) {
			Instance *p_instance = (Instance *)p_data;
			if (!p_instance->object_id.is_null()) {
				instances.push_back(p_instance->object_id);
			}
			return false;
		}
	};

	CullConvex cull_convex;
	scenario->indexers[Scenario::INDEXER_GEOMETRY].convex_query(p_convex.ptr(), p_convex.size(), points.ptr(), points.size(), cull_convex);
	scenario->indexers[Scenario::INDEXER_VOLUMES].convex_query(p_convex.ptr(), p_convex.size(), points.ptr(), points.size(), cull_convex);
	return cull_convex.instances;
}

void RendererSceneCull::instance_geometry_set_flag(RID p_instance, RS::InstanceFlags p_flags, bool p_enabled) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	//ERR_FAIL_COND(((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK));

	switch (p_flags) {
		case RS::INSTANCE_FLAG_USE_BAKED_LIGHT: {
			instance->baked_light = p_enabled;

			if (instance->scenario && instance->array_index >= 0) {
				InstanceData &idata = instance->scenario->instance_data[instance->array_index];
				if (instance->baked_light) {
					idata.flags |= InstanceData::FLAG_USES_BAKED_LIGHT;
				} else {
					idata.flags &= ~uint32_t(InstanceData::FLAG_USES_BAKED_LIGHT);
				}
			}

			if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
				ERR_FAIL_NULL(geom->geometry_instance);
				geom->geometry_instance->set_use_baked_light(p_enabled);
			}

		} break;
		case RS::INSTANCE_FLAG_USE_DYNAMIC_GI: {
			if (p_enabled == instance->dynamic_gi) {
				//bye, redundant
				return;
			}

			if (instance->indexer_id.is_valid()) {
				_unpair_instance(instance);
				_instance_queue_update(instance, true, true);
			}

			//once out of octree, can be changed
			instance->dynamic_gi = p_enabled;

			if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
				ERR_FAIL_NULL(geom->geometry_instance);
				geom->geometry_instance->set_use_dynamic_gi(p_enabled);
			}

		} break;
		case RS::INSTANCE_FLAG_DRAW_NEXT_FRAME_IF_VISIBLE: {
			instance->redraw_if_visible = p_enabled;

			if (instance->scenario && instance->array_index >= 0) {
				InstanceData &idata = instance->scenario->instance_data[instance->array_index];
				if (instance->redraw_if_visible) {
					idata.flags |= InstanceData::FLAG_REDRAW_IF_VISIBLE;
				} else {
					idata.flags &= ~uint32_t(InstanceData::FLAG_REDRAW_IF_VISIBLE);
				}
			}

		} break;
		case RS::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING: {
			instance->ignore_occlusion_culling = p_enabled;

			if (instance->scenario && instance->array_index >= 0) {
				InstanceData &idata = instance->scenario->instance_data[instance->array_index];
				if (instance->ignore_occlusion_culling) {
					idata.flags |= InstanceData::FLAG_IGNORE_OCCLUSION_CULLING;
				} else {
					idata.flags &= ~uint32_t(InstanceData::FLAG_IGNORE_OCCLUSION_CULLING);
				}
			}
		} break;
		default: {
		}
	}
}

void RendererSceneCull::instance_geometry_set_cast_shadows_setting(RID p_instance, RS::ShadowCastingSetting p_shadow_casting_setting) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->cast_shadows = p_shadow_casting_setting;

	if (instance->scenario && instance->array_index >= 0) {
		InstanceData &idata = instance->scenario->instance_data[instance->array_index];

		if (instance->cast_shadows != RS::SHADOW_CASTING_SETTING_OFF) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS;
		} else {
			idata.flags &= ~uint32_t(InstanceData::FLAG_CAST_SHADOWS);
		}

		if (instance->cast_shadows == RS::SHADOW_CASTING_SETTING_SHADOWS_ONLY) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS_ONLY;
		} else {
			idata.flags &= ~uint32_t(InstanceData::FLAG_CAST_SHADOWS_ONLY);
		}
	}

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);

		geom->geometry_instance->set_cast_double_sided_shadows(instance->cast_shadows == RS::SHADOW_CASTING_SETTING_DOUBLE_SIDED);
	}

	_instance_queue_update(instance, false, true);
}

void RendererSceneCull::instance_geometry_set_material_override(RID p_instance, RID p_material) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->material_override = p_material;
	_instance_queue_update(instance, false, true);

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_material_override(p_material);
	}
}

void RendererSceneCull::instance_geometry_set_material_overlay(RID p_instance, RID p_material) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->material_overlay = p_material;
	_instance_queue_update(instance, false, true);

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_material_overlay(p_material);
	}
}

void RendererSceneCull::instance_geometry_set_visibility_range(RID p_instance, float p_min, float p_max, float p_min_margin, float p_max_margin, RS::VisibilityRangeFadeMode p_fade_mode) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->visibility_range_begin = p_min;
	instance->visibility_range_end = p_max;
	instance->visibility_range_begin_margin = p_min_margin;
	instance->visibility_range_end_margin = p_max_margin;
	instance->visibility_range_fade_mode = p_fade_mode;

	_update_instance_visibility_dependencies(instance);

	if (instance->scenario && instance->visibility_index != -1) {
		InstanceVisibilityData &vd = instance->scenario->instance_visibility[instance->visibility_index];
		vd.range_begin = instance->visibility_range_begin;
		vd.range_end = instance->visibility_range_end;
		vd.range_begin_margin = instance->visibility_range_begin_margin;
		vd.range_end_margin = instance->visibility_range_end_margin;
		vd.fade_mode = p_fade_mode;
	}
}

void RendererSceneCull::instance_set_visibility_parent(RID p_instance, RID p_parent_instance) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	Instance *old_parent = instance->visibility_parent;
	if (old_parent) {
		old_parent->visibility_dependencies.erase(instance);
		instance->visibility_parent = nullptr;
		_update_instance_visibility_depth(old_parent);
	}

	Instance *parent = instance_owner.get_or_null(p_parent_instance);
	ERR_FAIL_COND(p_parent_instance.is_valid() && !parent);

	if (parent) {
		parent->visibility_dependencies.insert(instance);
		instance->visibility_parent = parent;

		bool cycle_detected = _update_instance_visibility_depth(parent);
		if (cycle_detected) {
			ERR_PRINT("Cycle detected in the visibility dependencies tree. The latest change to visibility_parent will have no effect.");
			parent->visibility_dependencies.erase(instance);
			instance->visibility_parent = nullptr;
		}
	}

	_update_instance_visibility_dependencies(instance);
}

bool RendererSceneCull::_update_instance_visibility_depth(Instance *p_instance) {
	bool cycle_detected = false;
	HashSet<Instance *> traversed_nodes;

	{
		Instance *instance = p_instance;
		while (instance) {
			if (!instance->visibility_dependencies.is_empty()) {
				uint32_t depth = 0;
				for (const Instance *E : instance->visibility_dependencies) {
					depth = MAX(depth, E->visibility_dependencies_depth);
				}
				instance->visibility_dependencies_depth = depth + 1;
			} else {
				instance->visibility_dependencies_depth = 0;
			}

			if (instance->scenario && instance->visibility_index != -1) {
				instance->scenario->instance_visibility.move(instance->visibility_index, instance->visibility_dependencies_depth);
			}

			traversed_nodes.insert(instance);

			instance = instance->visibility_parent;
			if (traversed_nodes.has(instance)) {
				cycle_detected = true;
				break;
			}
		}
	}

	return cycle_detected;
}

void RendererSceneCull::_update_instance_visibility_dependencies(Instance *p_instance) {
	bool is_geometry_instance = ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) && p_instance->base_data;
	bool has_visibility_range = p_instance->visibility_range_begin > 0.0 || p_instance->visibility_range_end > 0.0;
	bool needs_visibility_cull = has_visibility_range && is_geometry_instance && p_instance->array_index != -1;

	if (!needs_visibility_cull && p_instance->visibility_index != -1) {
		p_instance->scenario->instance_visibility.remove_at(p_instance->visibility_index);
		p_instance->visibility_index = -1;
	} else if (needs_visibility_cull && p_instance->visibility_index == -1) {
		InstanceVisibilityData vd;
		vd.instance = p_instance;
		vd.range_begin = p_instance->visibility_range_begin;
		vd.range_end = p_instance->visibility_range_end;
		vd.range_begin_margin = p_instance->visibility_range_begin_margin;
		vd.range_end_margin = p_instance->visibility_range_end_margin;
		vd.position = p_instance->transformed_aabb.get_center();
		vd.array_index = p_instance->array_index;
		vd.fade_mode = p_instance->visibility_range_fade_mode;

		p_instance->scenario->instance_visibility.insert(vd, p_instance->visibility_dependencies_depth);
	}

	if (p_instance->scenario && p_instance->array_index != -1) {
		InstanceData &idata = p_instance->scenario->instance_data[p_instance->array_index];
		idata.visibility_index = p_instance->visibility_index;

		if (is_geometry_instance) {
			if (has_visibility_range && p_instance->visibility_range_fade_mode == RS::VISIBILITY_RANGE_FADE_SELF) {
				bool begin_enabled = p_instance->visibility_range_begin > 0.0f;
				float begin_min = p_instance->visibility_range_begin - p_instance->visibility_range_begin_margin;
				float begin_max = p_instance->visibility_range_begin + p_instance->visibility_range_begin_margin;
				bool end_enabled = p_instance->visibility_range_end > 0.0f;
				float end_min = p_instance->visibility_range_end - p_instance->visibility_range_end_margin;
				float end_max = p_instance->visibility_range_end + p_instance->visibility_range_end_margin;
				idata.instance_geometry->set_fade_range(begin_enabled, begin_min, begin_max, end_enabled, end_min, end_max);
			} else {
				idata.instance_geometry->set_fade_range(false, 0.0f, 0.0f, false, 0.0f, 0.0f);
			}
		}

		if ((has_visibility_range || p_instance->visibility_parent) && (p_instance->visibility_index == -1 || p_instance->visibility_dependencies_depth == 0)) {
			idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK;
		} else {
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK;
		}

		if (p_instance->visibility_parent) {
			idata.parent_array_index = p_instance->visibility_parent->array_index;
		} else {
			idata.parent_array_index = -1;
			if (is_geometry_instance) {
				idata.instance_geometry->set_parent_fade_alpha(1.0f);
			}
		}
	}
}

void RendererSceneCull::instance_geometry_set_lightmap(RID p_instance, RID p_lightmap, const Rect2 &p_lightmap_uv_scale, int p_slice_index) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	if (instance->lightmap) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(((Instance *)instance->lightmap)->base_data);
		lightmap_data->users.erase(instance);
		instance->lightmap = nullptr;
	}

	Instance *lightmap_instance = instance_owner.get_or_null(p_lightmap);

	instance->lightmap = lightmap_instance;
	instance->lightmap_uv_scale = p_lightmap_uv_scale;
	instance->lightmap_slice_index = p_slice_index;

	RID lightmap_instance_rid;

	if (lightmap_instance) {
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(lightmap_instance->base_data);
		lightmap_data->users.insert(instance);
		lightmap_instance_rid = lightmap_data->instance;
	}

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_use_lightmap(lightmap_instance_rid, p_lightmap_uv_scale, p_slice_index);
	}
}

void RendererSceneCull::instance_geometry_set_lod_bias(RID p_instance, float p_lod_bias) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	instance->lod_bias = p_lod_bias;

	if ((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK && instance->base_data) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_lod_bias(p_lod_bias);
	}
}

void RendererSceneCull::instance_geometry_set_shader_parameter(RID p_instance, const StringName &p_parameter, const Variant &p_value) {
	Instance *instance = instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND(!instance);

	ERR_FAIL_COND(p_value.get_type() == Variant::OBJECT);

	HashMap<StringName, Instance::InstanceShaderParameter>::Iterator E = instance->instance_shader_uniforms.find(p_parameter);

	if (!E) {
		Instance::InstanceShaderParameter isp;
		isp.index = -1;
		isp.info = PropertyInfo();
		isp.value = p_value;
		instance->instance_shader_uniforms[p_parameter] = isp;
	} else {
		E->value.value = p_value;
		if (E->value.index >= 0 && instance->instance_allocated_shader_uniforms) {
			int flags_count = 0;
			if (E->value.info.hint == PROPERTY_HINT_FLAGS) {
				// A small hack to detect boolean flags count and prevent overhead.
				switch (E->value.info.hint_string.length()) {
					case 3: // "x,y"
						flags_count = 1;
						break;
					case 5: // "x,y,z"
						flags_count = 2;
						break;
					case 7: // "x,y,z,w"
						flags_count = 3;
						break;
				}
			}
			//update directly
			RSG::material_storage->global_shader_parameters_instance_update(p_instance, E->value.index, p_value, flags_count);
		}
	}
}

Variant RendererSceneCull::instance_geometry_get_shader_parameter(RID p_instance, const StringName &p_parameter) const {
	const Instance *instance = const_cast<RendererSceneCull *>(this)->instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND_V(!instance, Variant());

	if (instance->instance_shader_uniforms.has(p_parameter)) {
		return instance->instance_shader_uniforms[p_parameter].value;
	}
	return Variant();
}

Variant RendererSceneCull::instance_geometry_get_shader_parameter_default_value(RID p_instance, const StringName &p_parameter) const {
	const Instance *instance = const_cast<RendererSceneCull *>(this)->instance_owner.get_or_null(p_instance);
	ERR_FAIL_COND_V(!instance, Variant());

	if (instance->instance_shader_uniforms.has(p_parameter)) {
		return instance->instance_shader_uniforms[p_parameter].default_value;
	}
	return Variant();
}

void RendererSceneCull::_update_instance(Instance *p_instance) {
	p_instance->version++;

	if (p_instance->base_type == RS::INSTANCE_LIGHT) {
		InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

		RSG::light_storage->light_instance_set_transform(light->instance, p_instance->transform);
		RSG::light_storage->light_instance_set_aabb(light->instance, p_instance->transform.xform(p_instance->aabb));
		light->shadow_dirty = true;

		RS::LightBakeMode bake_mode = RSG::light_storage->light_get_bake_mode(p_instance->base);
		if (RSG::light_storage->light_get_type(p_instance->base) != RS::LIGHT_DIRECTIONAL && bake_mode != light->bake_mode) {
			if (p_instance->visible && p_instance->scenario && light->bake_mode == RS::LIGHT_BAKE_DYNAMIC) {
				p_instance->scenario->dynamic_lights.erase(light->instance);
			}

			light->bake_mode = bake_mode;

			if (p_instance->visible && p_instance->scenario && light->bake_mode == RS::LIGHT_BAKE_DYNAMIC) {
				p_instance->scenario->dynamic_lights.push_back(light->instance);
			}
		}
	} else if (p_instance->base_type == RS::INSTANCE_DECAL) {
		InstanceDecalData *decal = static_cast<InstanceDecalData *>(p_instance->base_data);

		RSG::texture_storage->decal_instance_set_transform(decal->instance, p_instance->transform);
	} else if (p_instance->base_type == RS::INSTANCE_LIGHTMAP) {
		InstanceLightmapData *lightmap = static_cast<InstanceLightmapData *>(p_instance->base_data);

		RSG::light_storage->lightmap_instance_set_transform(lightmap->instance, p_instance->transform);
	} else if (p_instance->base_type == RS::INSTANCE_PARTICLES) {
		RSG::particles_storage->particles_set_emission_transform(p_instance->base, p_instance->transform);
	} else if (p_instance->base_type == RS::INSTANCE_PARTICLES_COLLISION) {
		InstanceParticlesCollisionData *collision = static_cast<InstanceParticlesCollisionData *>(p_instance->base_data);

		//remove materials no longer used and un-own them
		if (RSG::particles_storage->particles_collision_is_heightfield(p_instance->base)) {
			heightfield_particle_colliders_update_list.insert(p_instance);
		}
		RSG::particles_storage->particles_collision_instance_set_transform(collision->instance, p_instance->transform);
	} else if (p_instance->base_type == RS::INSTANCE_OCCLUDER) {
		if (p_instance->scenario) {
			RendererSceneOcclusionCull::get_singleton()->scenario_set_instance(p_instance->scenario->self, p_instance->self, p_instance->base, p_instance->transform, p_instance->visible);
		}
	}

	if (!p_instance->aabb.has_surface()) {
		return;
	}

	if (p_instance->base_type == RS::INSTANCE_LIGHTMAP) {
		//if this moved, update the captured objects
		InstanceLightmapData *lightmap_data = static_cast<InstanceLightmapData *>(p_instance->base_data);
		//erase dependencies, since no longer a lightmap

		for (Instance *E : lightmap_data->geometries) {
			Instance *geom = E;
			_instance_queue_update(geom, true, false);
		}
	}

	AABB new_aabb;
	new_aabb = p_instance->transform.xform(p_instance->aabb);
	p_instance->transformed_aabb = new_aabb;

	if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		//make sure lights are updated if it casts shadow

		if (geom->can_cast_shadows) {
			for (const Instance *E : geom->lights) {
				InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
				light->shadow_dirty = true;
			}
		}

		if (!p_instance->lightmap && geom->lightmap_captures.size()) {
			//affected by lightmap captures, must update capture info!
			_update_instance_lightmap_captures(p_instance);
		} else {
			if (!p_instance->lightmap_sh.is_empty()) {
				p_instance->lightmap_sh.clear(); //don't need SH
				p_instance->lightmap_target_sh.clear(); //don't need SH
				ERR_FAIL_NULL(geom->geometry_instance);
				geom->geometry_instance->set_lightmap_capture(nullptr);
			}
		}

		ERR_FAIL_NULL(geom->geometry_instance);
		geom->geometry_instance->set_transform(p_instance->transform, p_instance->aabb, p_instance->transformed_aabb);
	}

	// note: we had to remove is equal approx check here, it meant that det == 0.000004 won't work, which is the case for some of our scenes.
	if (p_instance->scenario == nullptr || !p_instance->visible || p_instance->transform.basis.determinant() == 0) {
		p_instance->prev_transformed_aabb = p_instance->transformed_aabb;
		return;
	}

	//quantize to improve moving object performance
	AABB bvh_aabb = p_instance->transformed_aabb;

	if (p_instance->indexer_id.is_valid() && bvh_aabb != p_instance->prev_transformed_aabb) {
		//assume motion, see if bounds need to be quantized
		AABB motion_aabb = bvh_aabb.merge(p_instance->prev_transformed_aabb);
		float motion_longest_axis = motion_aabb.get_longest_axis_size();
		float longest_axis = p_instance->transformed_aabb.get_longest_axis_size();

		if (motion_longest_axis < longest_axis * 2) {
			//moved but not a lot, use motion aabb quantizing
			float quantize_size = Math::pow(2.0, Math::ceil(Math::log(motion_longest_axis) / Math::log(2.0))) * 0.5; //one fifth
			bvh_aabb.quantize(quantize_size);
		}
	}

	if (!p_instance->indexer_id.is_valid()) {
		if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
			p_instance->indexer_id = p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].insert(bvh_aabb, p_instance);
		} else {
			p_instance->indexer_id = p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].insert(bvh_aabb, p_instance);
		}

		p_instance->array_index = p_instance->scenario->instance_data.size();
		InstanceData idata;
		idata.instance = p_instance;
		idata.layer_mask = p_instance->layer_mask;
		idata.flags = p_instance->base_type; //changing it means de-indexing, so this never needs to be changed later
		idata.base_rid = p_instance->base;
		idata.parent_array_index = p_instance->visibility_parent ? p_instance->visibility_parent->array_index : -1;
		idata.visibility_index = p_instance->visibility_index;

		for (Instance *E : p_instance->visibility_dependencies) {
			Instance *dep_instance = E;
			if (dep_instance->array_index != -1) {
				dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = p_instance->array_index;
			}
		}

		switch (p_instance->base_type) {
			case RS::INSTANCE_MESH:
			case RS::INSTANCE_MULTIMESH:
			case RS::INSTANCE_PARTICLES: {
				InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
				idata.instance_geometry = geom->geometry_instance;
			} break;
			case RS::INSTANCE_LIGHT: {
				InstanceLightData *light_data = static_cast<InstanceLightData *>(p_instance->base_data);
				idata.instance_data_rid = light_data->instance.get_id();
				light_data->uses_projector = RSG::light_storage->light_has_projector(p_instance->base);
				light_data->uses_softshadow = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_SIZE) > CMP_EPSILON;

			} break;
			case RS::INSTANCE_DECAL: {
				idata.instance_data_rid = static_cast<InstanceDecalData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RS::INSTANCE_LIGHTMAP: {
				idata.instance_data_rid = static_cast<InstanceLightmapData *>(p_instance->base_data)->instance.get_id();
			} break;
			case RS::INSTANCE_VOXEL_GI: {
				idata.instance_data_rid = static_cast<InstanceVoxelGIData *>(p_instance->base_data)->probe_instance.get_id();
			} break;
			case RS::INSTANCE_VISIBLITY_NOTIFIER: {
				idata.visibility_notifier = static_cast<InstanceVisibilityNotifierData *>(p_instance->base_data);
			} break;
			default: {
			}
		}

		if (p_instance->cast_shadows != RS::SHADOW_CASTING_SETTING_OFF) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS;
		}
		if (p_instance->cast_shadows == RS::SHADOW_CASTING_SETTING_SHADOWS_ONLY) {
			idata.flags |= InstanceData::FLAG_CAST_SHADOWS_ONLY;
		}
		if (p_instance->redraw_if_visible) {
			idata.flags |= InstanceData::FLAG_REDRAW_IF_VISIBLE;
		}
		// dirty flags should not be set here, since no pairing has happened
		if (p_instance->baked_light) {
			idata.flags |= InstanceData::FLAG_USES_BAKED_LIGHT;
		}
		if (p_instance->mesh_instance.is_valid()) {
			idata.flags |= InstanceData::FLAG_USES_MESH_INSTANCE;
		}
		if (p_instance->ignore_occlusion_culling) {
			idata.flags |= InstanceData::FLAG_IGNORE_OCCLUSION_CULLING;
		}
		if (p_instance->ignore_all_culling) {
			idata.flags |= InstanceData::FLAG_IGNORE_ALL_CULLING;
		}

		p_instance->scenario->instance_data.push_back(idata);
		p_instance->scenario->instance_aabbs.push_back(InstanceBounds(p_instance->transformed_aabb));
		_update_instance_visibility_dependencies(p_instance);
	} else {
		if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
			p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].update(p_instance->indexer_id, bvh_aabb);
		} else {
			p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].update(p_instance->indexer_id, bvh_aabb);
		}
		p_instance->scenario->instance_aabbs[p_instance->array_index] = InstanceBounds(p_instance->transformed_aabb);
	}

	if (p_instance->visibility_index != -1) {
		p_instance->scenario->instance_visibility[p_instance->visibility_index].position = p_instance->transformed_aabb.get_center();
	}

	//move instance and repair
	pair_pass++;

	PairInstances pair;

	pair.instance = p_instance;
	pair.pair_allocator = &pair_allocator;
	pair.pair_pass = pair_pass;
	pair.pair_mask = 0;
	pair.cull_mask = 0xFFFFFFFF;

	if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
		pair.pair_mask |= 1 << RS::INSTANCE_LIGHT;
		pair.pair_mask |= 1 << RS::INSTANCE_VOXEL_GI;
		pair.pair_mask |= 1 << RS::INSTANCE_LIGHTMAP;
		if (p_instance->base_type == RS::INSTANCE_PARTICLES) {
			pair.pair_mask |= 1 << RS::INSTANCE_PARTICLES_COLLISION;
		}

		pair.pair_mask |= geometry_instance_pair_mask;

		pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
	} else if (p_instance->base_type == RS::INSTANCE_LIGHT) {
		pair.pair_mask |= RS::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];

		if (RSG::light_storage->light_get_bake_mode(p_instance->base) == RS::LIGHT_BAKE_DYNAMIC) {
			pair.pair_mask |= (1 << RS::INSTANCE_VOXEL_GI);
			pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
		}
		pair.cull_mask = RSG::light_storage->light_get_cull_mask(p_instance->base);
	} else if (geometry_instance_pair_mask & (1 << RS::INSTANCE_DECAL) && (p_instance->base_type == RS::INSTANCE_DECAL)) {
		pair.pair_mask = RS::INSTANCE_GEOMETRY_MASK;
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
		pair.cull_mask = RSG::texture_storage->decal_get_cull_mask(p_instance->base);
	} else if (p_instance->base_type == RS::INSTANCE_PARTICLES_COLLISION) {
		pair.pair_mask = (1 << RS::INSTANCE_PARTICLES);
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
	} else if (p_instance->base_type == RS::INSTANCE_VOXEL_GI) {
		//lights and geometries
		pair.pair_mask = RS::INSTANCE_GEOMETRY_MASK | (1 << RS::INSTANCE_LIGHT);
		pair.bvh = &p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY];
		pair.bvh2 = &p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES];
	}

	pair.pair();

	p_instance->prev_transformed_aabb = p_instance->transformed_aabb;
}

void RendererSceneCull::_unpair_instance(Instance *p_instance) {
	if (!p_instance->indexer_id.is_valid()) {
		return; //nothing to do
	}

	while (p_instance->pairs.first()) {
		InstancePair *pair = p_instance->pairs.first()->self();
		Instance *other_instance = p_instance == pair->a ? pair->b : pair->a;
		_instance_unpair(p_instance, other_instance);
		pair_allocator.free(pair);
	}

	if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
		p_instance->scenario->indexers[Scenario::INDEXER_GEOMETRY].remove(p_instance->indexer_id);
	} else {
		p_instance->scenario->indexers[Scenario::INDEXER_VOLUMES].remove(p_instance->indexer_id);
	}

	p_instance->indexer_id = DynamicBVH::ID();

	//replace this by last
	int32_t swap_with_index = p_instance->scenario->instance_data.size() - 1;
	if (swap_with_index != p_instance->array_index) {
		Instance *swapped_instance = p_instance->scenario->instance_data[swap_with_index].instance;
		swapped_instance->array_index = p_instance->array_index; //swap
		p_instance->scenario->instance_data[p_instance->array_index] = p_instance->scenario->instance_data[swap_with_index];
		p_instance->scenario->instance_aabbs[p_instance->array_index] = p_instance->scenario->instance_aabbs[swap_with_index];

		if (swapped_instance->visibility_index != -1) {
			swapped_instance->scenario->instance_visibility[swapped_instance->visibility_index].array_index = swapped_instance->array_index;
		}

		for (Instance *E : swapped_instance->visibility_dependencies) {
			Instance *dep_instance = E;
			if (dep_instance != p_instance && dep_instance->array_index != -1) {
				dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = swapped_instance->array_index;
			}
		}
	}

	// pop last
	p_instance->scenario->instance_data.pop_back();
	p_instance->scenario->instance_aabbs.pop_back();

	//uninitialize
	p_instance->array_index = -1;
	if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
		// Clear these now because the InstanceData containing the dirty flags is gone
		InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
		ERR_FAIL_NULL(geom->geometry_instance);

		geom->geometry_instance->pair_light_instances(nullptr, 0);
		geom->geometry_instance->pair_decal_instances(nullptr, 0);
	}

	for (Instance *E : p_instance->visibility_dependencies) {
		Instance *dep_instance = E;
		if (dep_instance->array_index != -1) {
			dep_instance->scenario->instance_data[dep_instance->array_index].parent_array_index = -1;
			if ((1 << dep_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
				dep_instance->scenario->instance_data[dep_instance->array_index].instance_geometry->set_parent_fade_alpha(1.0f);
			}
		}
	}

	_update_instance_visibility_dependencies(p_instance);
}

void RendererSceneCull::_update_instance_aabb(Instance *p_instance) {
	AABB new_aabb;

	ERR_FAIL_COND(p_instance->base_type != RS::INSTANCE_NONE && !p_instance->base.is_valid());

	switch (p_instance->base_type) {
		case RenderingServer::INSTANCE_NONE: {
			// do nothing
		} break;
		case RenderingServer::INSTANCE_MESH: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::mesh_storage->mesh_get_aabb(p_instance->base, p_instance->skeleton);
			}

		} break;

		case RenderingServer::INSTANCE_MULTIMESH: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::mesh_storage->multimesh_get_aabb(p_instance->base);
			}

		} break;
		case RenderingServer::INSTANCE_PARTICLES: {
			if (p_instance->custom_aabb) {
				new_aabb = *p_instance->custom_aabb;
			} else {
				new_aabb = RSG::particles_storage->particles_get_aabb(p_instance->base);
			}

		} break;
		case RenderingServer::INSTANCE_PARTICLES_COLLISION: {
			new_aabb = RSG::particles_storage->particles_collision_get_aabb(p_instance->base);

		} break;
		case RenderingServer::INSTANCE_VISIBLITY_NOTIFIER: {
			new_aabb = RSG::utilities->visibility_notifier_get_aabb(p_instance->base);
		} break;
		case RenderingServer::INSTANCE_LIGHT: {
			new_aabb = RSG::light_storage->light_get_aabb(p_instance->base);

		} break;
		case RenderingServer::INSTANCE_DECAL: {
			new_aabb = RSG::texture_storage->decal_get_aabb(p_instance->base);

		} break;
		case RenderingServer::INSTANCE_LIGHTMAP: {
			new_aabb = RSG::light_storage->lightmap_get_aabb(p_instance->base);

		} break;
		default: {
		}
	}

	// <Zylann> This is why I didn't re-use Instance::aabb to implement custom AABBs
	if (p_instance->extra_margin) {
		new_aabb.grow_by(p_instance->extra_margin);
	}

	p_instance->aabb = new_aabb;
}

void RendererSceneCull::_update_instance_lightmap_captures(Instance *p_instance) {
	bool first_set = p_instance->lightmap_sh.size() == 0;
	p_instance->lightmap_sh.resize(9); //using SH
	p_instance->lightmap_target_sh.resize(9); //using SH
	Color *instance_sh = p_instance->lightmap_target_sh.ptrw();
	bool inside = false;
	Color accum_sh[9];
	float accum_blend = 0.0;

	InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
	for (Instance *E : geom->lightmap_captures) {
		Instance *lightmap = E;

		bool interior = RSG::light_storage->lightmap_is_interior(lightmap->base);

		if (inside && !interior) {
			continue; //we are inside, ignore exteriors
		}

		Transform3D to_bounds = lightmap->transform.affine_inverse();
		Vector3 center = p_instance->transform.xform(p_instance->aabb.get_center()); //use aabb center

		Vector3 lm_pos = to_bounds.xform(center);

		AABB bounds = RSG::light_storage->lightmap_get_aabb(lightmap->base);
		if (!bounds.has_point(lm_pos)) {
			continue; //not in this lightmap
		}

		Color sh[9];
		RSG::light_storage->lightmap_tap_sh_light(lightmap->base, lm_pos, sh);

		//rotate it
		Basis rot = lightmap->transform.basis.orthonormalized();
		for (int i = 0; i < 3; i++) {
			real_t csh[9];
			for (int j = 0; j < 9; j++) {
				csh[j] = sh[j][i];
			}
			rot.rotate_sh(csh);
			for (int j = 0; j < 9; j++) {
				sh[j][i] = csh[j];
			}
		}

		Vector3 inner_pos = ((lm_pos - bounds.position) / bounds.size) * 2.0 - Vector3(1.0, 1.0, 1.0);

		real_t blend = MAX(inner_pos.x, MAX(inner_pos.y, inner_pos.z));
		//make blend more rounded
		blend = Math::lerp(inner_pos.length(), blend, blend);
		blend *= blend;
		blend = MAX(0.0, 1.0 - blend);

		if (interior && !inside) {
			//do not blend, just replace
			for (int j = 0; j < 9; j++) {
				accum_sh[j] = sh[j] * blend;
			}
			accum_blend = blend;
			inside = true;
		} else {
			for (int j = 0; j < 9; j++) {
				accum_sh[j] += sh[j] * blend;
			}
			accum_blend += blend;
		}
	}

	if (accum_blend > 0.0) {
		for (int j = 0; j < 9; j++) {
			instance_sh[j] = accum_sh[j] / accum_blend;
			if (first_set) {
				p_instance->lightmap_sh.write[j] = instance_sh[j];
			}
		}
	}

	ERR_FAIL_NULL(geom->geometry_instance);
	geom->geometry_instance->set_lightmap_capture(p_instance->lightmap_sh.ptr());
}

void RendererSceneCull::_light_instance_setup_directional_shadow(int p_shadow_index, Instance *p_instance, const Transform3D p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_cam_vaspect) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

	Transform3D light_transform = p_instance->transform;
	light_transform.orthonormalize(); //scale does not count on lights

	real_t max_distance = p_cam_projection.get_z_far();
	real_t shadow_max = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_SHADOW_MAX_DISTANCE);
	if (shadow_max > 0 && !p_cam_orthogonal) { //its impractical (and leads to unwanted behaviors) to set max distance in orthogonal camera
		max_distance = MIN(shadow_max, max_distance);
	}
	max_distance = MAX(max_distance, p_cam_projection.get_z_near() + 0.001);
	real_t min_distance = MIN(p_cam_projection.get_z_near(), max_distance);

	real_t pancake_size = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_SHADOW_PANCAKE_SIZE);

	real_t range = max_distance - min_distance;

	int splits = 0;
	switch (RSG::light_storage->light_directional_get_shadow_mode(p_instance->base)) {
		case RS::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL:
			splits = 1;
			break;
		case RS::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS:
			splits = 2;
			break;
		case RS::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS:
			splits = 4;
			break;
	}

	real_t distances[5];

	distances[0] = min_distance;
	for (int i = 0; i < splits; i++) {
		distances[i + 1] = min_distance + RSG::light_storage->light_get_param(p_instance->base, RS::LightParam(RS::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET + i)) * range;
	};

	distances[splits] = max_distance;

	real_t texture_size = RSG::light_storage->get_directional_light_shadow_size(light->instance);

	bool overlap = RSG::light_storage->light_directional_get_blend_splits(p_instance->base);

	cull.shadow_count = p_shadow_index + 1;
	cull.shadows[p_shadow_index].cascade_count = splits;
	cull.shadows[p_shadow_index].light_instance = light->instance;

	for (int i = 0; i < splits; i++) {
		RENDER_TIMESTAMP("Cull DirectionalLight3D, Split " + itos(i));

		// setup a camera matrix for that range!
		Projection camera_matrix;

		real_t aspect = p_cam_projection.get_aspect();

		if (p_cam_orthogonal) {
			Vector2 vp_he = p_cam_projection.get_viewport_half_extents();

			camera_matrix.set_orthogonal(vp_he.y * 2.0, aspect, distances[(i == 0 || !overlap) ? i : i - 1], distances[i + 1], false);
		} else {
			real_t fov = p_cam_projection.get_fov(); //this is actually yfov, because set aspect tries to keep it
			camera_matrix.set_perspective(fov, aspect, distances[(i == 0 || !overlap) ? i : i - 1], distances[i + 1], true);
		}

		//obtain the frustum endpoints

		Vector3 endpoints[8]; // frustum plane endpoints
		bool res = camera_matrix.get_endpoints(p_cam_transform, endpoints);
		ERR_CONTINUE(!res);

		// obtain the light frustum ranges (given endpoints)

		Transform3D transform = light_transform; //discard scale and stabilize light

		Vector3 x_vec = transform.basis.get_column(Vector3::AXIS_X).normalized();
		Vector3 y_vec = transform.basis.get_column(Vector3::AXIS_Y).normalized();
		Vector3 z_vec = transform.basis.get_column(Vector3::AXIS_Z).normalized();
		//z_vec points against the camera, like in default opengl

		real_t x_min = 0.f, x_max = 0.f;
		real_t y_min = 0.f, y_max = 0.f;
		real_t z_min = 0.f, z_max = 0.f;

		// FIXME: z_max_cam is defined, computed, but not used below when setting up
		// ortho_camera. Commented out for now to fix warnings but should be investigated.
		real_t x_min_cam = 0.f, x_max_cam = 0.f;
		real_t y_min_cam = 0.f, y_max_cam = 0.f;
		real_t z_min_cam = 0.f;
		//real_t z_max_cam = 0.f;

		//real_t bias_scale = 1.0;
		//real_t aspect_bias_scale = 1.0;

		//used for culling

		for (int j = 0; j < 8; j++) {
			real_t d_x = x_vec.dot(endpoints[j]);
			real_t d_y = y_vec.dot(endpoints[j]);
			real_t d_z = z_vec.dot(endpoints[j]);

			if (j == 0 || d_x < x_min) {
				x_min = d_x;
			}
			if (j == 0 || d_x > x_max) {
				x_max = d_x;
			}

			if (j == 0 || d_y < y_min) {
				y_min = d_y;
			}
			if (j == 0 || d_y > y_max) {
				y_max = d_y;
			}

			if (j == 0 || d_z < z_min) {
				z_min = d_z;
			}
			if (j == 0 || d_z > z_max) {
				z_max = d_z;
			}
		}

		real_t radius = 0;
		real_t soft_shadow_expand = 0;
		Vector3 center;

		{
			//camera viewport stuff

			for (int j = 0; j < 8; j++) {
				center += endpoints[j];
			}
			center /= 8.0;

			//center=x_vec*(x_max-x_min)*0.5 + y_vec*(y_max-y_min)*0.5 + z_vec*(z_max-z_min)*0.5;

			for (int j = 0; j < 8; j++) {
				real_t d = center.distance_to(endpoints[j]);
				if (d > radius) {
					radius = d;
				}
			}

			radius *= texture_size / (texture_size - 2.0); //add a texel by each side

			z_min_cam = z_vec.dot(center) - radius;

			{
				float soft_shadow_angle = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_SIZE);

				if (soft_shadow_angle > 0.0) {
					float z_range = (z_vec.dot(center) + radius + pancake_size) - z_min_cam;
					soft_shadow_expand = Math::tan(Math::deg_to_rad(soft_shadow_angle)) * z_range;

					x_max += soft_shadow_expand;
					y_max += soft_shadow_expand;

					x_min -= soft_shadow_expand;
					y_min -= soft_shadow_expand;
				}
			}

			// This trick here is what stabilizes the shadow (make potential jaggies to not move)
			// at the cost of some wasted resolution. Still, the quality increase is very well worth it.
			const real_t unit = (radius + soft_shadow_expand) * 2.0 / texture_size;
			x_max_cam = Math::snapped(x_vec.dot(center) + radius + soft_shadow_expand, unit);
			x_min_cam = Math::snapped(x_vec.dot(center) - radius - soft_shadow_expand, unit);
			y_max_cam = Math::snapped(y_vec.dot(center) + radius + soft_shadow_expand, unit);
			y_min_cam = Math::snapped(y_vec.dot(center) - radius - soft_shadow_expand, unit);
		}

		//now that we know all ranges, we can proceed to make the light frustum planes, for culling octree

		Vector<Plane> light_frustum_planes;
		light_frustum_planes.resize(6);

		//right/left
		light_frustum_planes.write[0] = Plane(x_vec, x_max);
		light_frustum_planes.write[1] = Plane(-x_vec, -x_min);
		//top/bottom
		light_frustum_planes.write[2] = Plane(y_vec, y_max);
		light_frustum_planes.write[3] = Plane(-y_vec, -y_min);
		//near/far
		light_frustum_planes.write[4] = Plane(z_vec, z_max + 1e6);
		light_frustum_planes.write[5] = Plane(-z_vec, -z_min); // z_min is ok, since casters further than far-light plane are not needed

		// a pre pass will need to be needed to determine the actual z-near to be used

		z_max = z_vec.dot(center) + radius + pancake_size;

		{
			Projection ortho_camera;
			real_t half_x = (x_max_cam - x_min_cam) * 0.5;
			real_t half_y = (y_max_cam - y_min_cam) * 0.5;

			ortho_camera.set_orthogonal(-half_x, half_x, -half_y, half_y, 0, (z_max - z_min_cam));

			Vector2 uv_scale(1.0 / (x_max_cam - x_min_cam), 1.0 / (y_max_cam - y_min_cam));

			Transform3D ortho_transform;
			ortho_transform.basis = transform.basis;
			ortho_transform.origin = x_vec * (x_min_cam + half_x) + y_vec * (y_min_cam + half_y) + z_vec * z_max;

			cull.shadows[p_shadow_index].cascades[i].frustum = Frustum(light_frustum_planes);
			cull.shadows[p_shadow_index].cascades[i].projection = ortho_camera;
			cull.shadows[p_shadow_index].cascades[i].transform = ortho_transform;
			cull.shadows[p_shadow_index].cascades[i].zfar = z_max - z_min_cam;
			cull.shadows[p_shadow_index].cascades[i].split = distances[i + 1];
			cull.shadows[p_shadow_index].cascades[i].shadow_texel_size = radius * 2.0 / texture_size;
			cull.shadows[p_shadow_index].cascades[i].bias_scale = (z_max - z_min_cam);
			cull.shadows[p_shadow_index].cascades[i].range_begin = z_max;
			cull.shadows[p_shadow_index].cascades[i].uv_scale = uv_scale;
		}
	}
}

bool RendererSceneCull::_light_instance_update_shadow(Instance *p_instance, const Transform3D p_cam_transform, const Projection &p_cam_projection, bool p_cam_orthogonal, bool p_cam_vaspect, RID p_shadow_atlas, Scenario *p_scenario, float p_screen_mesh_lod_threshold, uint32_t p_visible_layers) {
	InstanceLightData *light = static_cast<InstanceLightData *>(p_instance->base_data);

	Transform3D light_transform = p_instance->transform;
	light_transform.orthonormalize(); //scale does not count on lights

	bool animated_material_found = false;

	switch (RSG::light_storage->light_get_type(p_instance->base)) {
		case RS::LIGHT_DIRECTIONAL: {
		} break;
		case RS::LIGHT_OMNI: {
			RS::LightOmniShadowMode shadow_mode = RSG::light_storage->light_omni_get_shadow_mode(p_instance->base);

			if (shadow_mode == RS::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID || !RSG::light_storage->light_instances_can_render_shadow_cube()) {
				if (max_shadows_used + 2 > MAX_UPDATE_SHADOWS) {
					return true;
				}
				for (int i = 0; i < 2; i++) {
					//using this one ensures that raster deferred will have it
					RENDER_TIMESTAMP("Cull OmniLight3D Shadow Paraboloid, Half " + itos(i));

					real_t radius = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_RANGE);

					real_t z = i == 0 ? -1 : 1;
					Vector<Plane> planes;
					planes.resize(6);
					planes.write[0] = light_transform.xform(Plane(Vector3(0, 0, z), radius));
					planes.write[1] = light_transform.xform(Plane(Vector3(1, 0, z).normalized(), radius));
					planes.write[2] = light_transform.xform(Plane(Vector3(-1, 0, z).normalized(), radius));
					planes.write[3] = light_transform.xform(Plane(Vector3(0, 1, z).normalized(), radius));
					planes.write[4] = light_transform.xform(Plane(Vector3(0, -1, z).normalized(), radius));
					planes.write[5] = light_transform.xform(Plane(Vector3(0, 0, -z), 0));

					instance_shadow_cull_result.clear();

					Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(&planes[0], planes.size());

					struct CullConvex {
						PagedArray<Instance *> *result;
						_FORCE_INLINE_ bool operator()(void *p_data) {
							Instance *p_instance = (Instance *)p_data;
							result->push_back(p_instance);
							return false;
						}
					};

					CullConvex cull_convex;
					cull_convex.result = &instance_shadow_cull_result;

					p_scenario->indexers[Scenario::INDEXER_GEOMETRY].convex_query(planes.ptr(), planes.size(), points.ptr(), points.size(), cull_convex);

					RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[max_shadows_used++];

					for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
						Instance *instance = instance_shadow_cull_result[j];
						if (!instance->visible || !((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask)) {
							continue;
						} else {
							if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
								animated_material_found = true;
							}

							if (instance->mesh_instance.is_valid()) {
								RSG::mesh_storage->mesh_instance_check_for_update(instance->mesh_instance);
							}
						}

						shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
					}

					RSG::mesh_storage->update_mesh_instances();

					RSG::light_storage->light_instance_set_shadow_transform(light->instance, Projection(), light_transform, radius, 0, i, 0);
					shadow_data.light = light->instance;
					shadow_data.pass = i;
				}
			} else { //shadow cube

				if (max_shadows_used + 6 > MAX_UPDATE_SHADOWS) {
					return true;
				}

				real_t radius = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_RANGE);
				Projection cm;
				cm.set_perspective(90, 1, radius * 0.005f, radius);

				for (int i = 0; i < 6; i++) {
					RENDER_TIMESTAMP("Cull OmniLight3D Shadow Cube, Side " + itos(i));
					//using this one ensures that raster deferred will have it

					static const Vector3 view_normals[6] = {
						Vector3(+1, 0, 0),
						Vector3(-1, 0, 0),
						Vector3(0, -1, 0),
						Vector3(0, +1, 0),
						Vector3(0, 0, +1),
						Vector3(0, 0, -1)
					};
					static const Vector3 view_up[6] = {
						Vector3(0, -1, 0),
						Vector3(0, -1, 0),
						Vector3(0, 0, -1),
						Vector3(0, 0, +1),
						Vector3(0, -1, 0),
						Vector3(0, -1, 0)
					};

					Transform3D xform = light_transform * Transform3D().looking_at(view_normals[i], view_up[i]);

					Vector<Plane> planes = cm.get_projection_planes(xform);

					instance_shadow_cull_result.clear();

					Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(&planes[0], planes.size());

					struct CullConvex {
						PagedArray<Instance *> *result;
						_FORCE_INLINE_ bool operator()(void *p_data) {
							Instance *p_instance = (Instance *)p_data;
							result->push_back(p_instance);
							return false;
						}
					};

					CullConvex cull_convex;
					cull_convex.result = &instance_shadow_cull_result;

					p_scenario->indexers[Scenario::INDEXER_GEOMETRY].convex_query(planes.ptr(), planes.size(), points.ptr(), points.size(), cull_convex);

					RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[max_shadows_used++];

					for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
						Instance *instance = instance_shadow_cull_result[j];
						if (!instance->visible || !((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask)) {
							continue;
						} else {
							if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
								animated_material_found = true;
							}
							if (instance->mesh_instance.is_valid()) {
								RSG::mesh_storage->mesh_instance_check_for_update(instance->mesh_instance);
							}
						}

						shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
					}

					RSG::mesh_storage->update_mesh_instances();
					RSG::light_storage->light_instance_set_shadow_transform(light->instance, cm, xform, radius, 0, i, 0);

					shadow_data.light = light->instance;
					shadow_data.pass = i;
				}

				//restore the regular DP matrix
				//RSG::light_storage->light_instance_set_shadow_transform(light->instance, Projection(), light_transform, radius, 0, 0, 0);
			}

		} break;
		case RS::LIGHT_SPOT: {
			RENDER_TIMESTAMP("Cull SpotLight3D Shadow");

			if (max_shadows_used + 1 > MAX_UPDATE_SHADOWS) {
				return true;
			}

			real_t radius = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_RANGE);
			real_t angle = RSG::light_storage->light_get_param(p_instance->base, RS::LIGHT_PARAM_SPOT_ANGLE);

			Projection cm;
			cm.set_perspective(angle * 2.0, 1.0, 0.005f * radius, radius);

			Vector<Plane> planes = cm.get_projection_planes(light_transform);

			instance_shadow_cull_result.clear();

			Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(&planes[0], planes.size());

			struct CullConvex {
				PagedArray<Instance *> *result;
				_FORCE_INLINE_ bool operator()(void *p_data) {
					Instance *p_instance = (Instance *)p_data;
					result->push_back(p_instance);
					return false;
				}
			};

			CullConvex cull_convex;
			cull_convex.result = &instance_shadow_cull_result;

			p_scenario->indexers[Scenario::INDEXER_GEOMETRY].convex_query(planes.ptr(), planes.size(), points.ptr(), points.size(), cull_convex);

			RendererSceneRender::RenderShadowData &shadow_data = render_shadow_data[max_shadows_used++];

			for (int j = 0; j < (int)instance_shadow_cull_result.size(); j++) {
				Instance *instance = instance_shadow_cull_result[j];
				if (!instance->visible || !((1 << instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) || !static_cast<InstanceGeometryData *>(instance->base_data)->can_cast_shadows || !(p_visible_layers & instance->layer_mask)) {
					continue;
				} else {
					if (static_cast<InstanceGeometryData *>(instance->base_data)->material_is_animated) {
						animated_material_found = true;
					}

					if (instance->mesh_instance.is_valid()) {
						RSG::mesh_storage->mesh_instance_check_for_update(instance->mesh_instance);
					}
				}
				shadow_data.instances.push_back(static_cast<InstanceGeometryData *>(instance->base_data)->geometry_instance);
			}

			RSG::mesh_storage->update_mesh_instances();

			RSG::light_storage->light_instance_set_shadow_transform(light->instance, cm, light_transform, radius, 0, 0, 0);
			shadow_data.light = light->instance;
			shadow_data.pass = 0;

		} break;
	}

	return animated_material_found;
}

void RendererSceneCull::_visibility_cull_threaded(uint32_t p_thread, VisibilityCullData *cull_data) {
	uint32_t total_threads = WorkerThreadPool::get_singleton()->get_thread_count();
	uint32_t bin_from = p_thread * cull_data->cull_count / total_threads;
	uint32_t bin_to = (p_thread + 1 == total_threads) ? cull_data->cull_count : ((p_thread + 1) * cull_data->cull_count / total_threads);

	_visibility_cull(*cull_data, cull_data->cull_offset + bin_from, cull_data->cull_offset + bin_to);
}

void RendererSceneCull::_visibility_cull(const VisibilityCullData &cull_data, uint64_t p_from, uint64_t p_to) {
	Scenario *scenario = cull_data.scenario;
	for (unsigned int i = p_from; i < p_to; i++) {
		InstanceVisibilityData &vd = scenario->instance_visibility[i];
		InstanceData &idata = scenario->instance_data[vd.array_index];

		if (idata.parent_array_index >= 0) {
			uint32_t parent_flags = scenario->instance_data[idata.parent_array_index].flags;

			if ((parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN) || !(parent_flags & (InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE | InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN))) {
				idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
				idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
				idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
				continue;
			}
		}

		int range_check = _visibility_range_check<true>(vd, cull_data.camera_position, cull_data.viewport_mask);

		if (range_check == -1) {
			idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
		} else if (range_check == 1) {
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
			idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
		} else {
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN;
			idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE;
			if (range_check == 2) {
				idata.flags |= InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
			} else {
				idata.flags &= ~InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN;
			}
		}
	}
}

template <bool p_fade_check>
int RendererSceneCull::_visibility_range_check(InstanceVisibilityData &r_vis_data, const Vector3 &p_camera_pos, uint64_t p_viewport_mask) {
	float dist = p_camera_pos.distance_to(r_vis_data.position);
	const RS::VisibilityRangeFadeMode &fade_mode = r_vis_data.fade_mode;

	float begin_offset = -r_vis_data.range_begin_margin;
	float end_offset = r_vis_data.range_end_margin;

	if (fade_mode == RS::VISIBILITY_RANGE_FADE_DISABLED && !(p_viewport_mask & r_vis_data.viewport_state)) {
		begin_offset = -begin_offset;
		end_offset = -end_offset;
	}

	if (r_vis_data.range_end > 0.0f && dist > r_vis_data.range_end + end_offset) {
		r_vis_data.viewport_state &= ~p_viewport_mask;
		return -1;
	} else if (r_vis_data.range_begin > 0.0f && dist < r_vis_data.range_begin + begin_offset) {
		r_vis_data.viewport_state &= ~p_viewport_mask;
		return 1;
	} else {
		r_vis_data.viewport_state |= p_viewport_mask;
		if (p_fade_check) {
			if (fade_mode != RS::VISIBILITY_RANGE_FADE_DISABLED) {
				r_vis_data.children_fade_alpha = 1.0f;
				if (r_vis_data.range_end > 0.0f && dist > r_vis_data.range_end - end_offset) {
					if (fade_mode == RS::VISIBILITY_RANGE_FADE_DEPENDENCIES) {
						r_vis_data.children_fade_alpha = MIN(1.0f, (dist - (r_vis_data.range_end - end_offset)) / (2.0f * r_vis_data.range_end_margin));
					}
					return 2;
				} else if (r_vis_data.range_begin > 0.0f && dist < r_vis_data.range_begin - begin_offset) {
					if (fade_mode == RS::VISIBILITY_RANGE_FADE_DEPENDENCIES) {
						r_vis_data.children_fade_alpha = MIN(1.0f, 1.0 - (dist - (r_vis_data.range_begin + begin_offset)) / (2.0f * r_vis_data.range_begin_margin));
					}
					return 2;
				}
			}
		}
		return 0;
	}
}

bool RendererSceneCull::_visibility_parent_check(const CullData &p_cull_data, const InstanceData &p_instance_data) {
	if (p_instance_data.parent_array_index == -1) {
		return true;
	}
	const uint32_t &parent_flags = p_cull_data.scenario->instance_data[p_instance_data.parent_array_index].flags;
	return ((parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_NEEDS_CHECK) == InstanceData::FLAG_VISIBILITY_DEPENDENCY_HIDDEN_CLOSE_RANGE) || (parent_flags & InstanceData::FLAG_VISIBILITY_DEPENDENCY_FADE_CHILDREN);
}

void RendererSceneCull::_update_instance_shader_uniforms_from_material(HashMap<StringName, Instance::InstanceShaderParameter> &isparams, const HashMap<StringName, Instance::InstanceShaderParameter> &existing_isparams, RID p_material) {
	List<RendererMaterialStorage::InstanceShaderParam> plist;
	RSG::material_storage->material_get_instance_shader_parameters(p_material, &plist);
	for (const RendererMaterialStorage::InstanceShaderParam &E : plist) {
		StringName name = E.info.name;
		if (isparams.has(name)) {
			if (isparams[name].info.type != E.info.type) {
				WARN_PRINT("More than one material in instance export the same instance shader uniform '" + E.info.name + "', but they do it with different data types. Only the first one (in order) will display correctly.");
			}
			if (isparams[name].index != E.index) {
				WARN_PRINT("More than one material in instance export the same instance shader uniform '" + E.info.name + "', but they do it with different indices. Only the first one (in order) will display correctly.");
			}
			continue; //first one found always has priority
		}

		Instance::InstanceShaderParameter isp;
		isp.index = E.index;
		isp.info = E.info;
		isp.default_value = E.default_value;
		if (existing_isparams.has(name)) {
			isp.value = existing_isparams[name].value;
		} else {
			isp.value = E.default_value;
		}
		isparams[name] = isp;
	}
}

void RendererSceneCull::_update_dirty_instance(Instance *p_instance) {
	if (p_instance->update_aabb) {
		_update_instance_aabb(p_instance);
	}

	if (p_instance->update_dependencies) {
		p_instance->dependency_tracker.update_begin();

		if (p_instance->base.is_valid()) {
			RSG::utilities->base_update_dependency(p_instance->base, &p_instance->dependency_tracker);
		}

		if (p_instance->material_override.is_valid()) {
			RSG::material_storage->material_update_dependency(p_instance->material_override, &p_instance->dependency_tracker);
		}

		if (p_instance->material_overlay.is_valid()) {
			RSG::material_storage->material_update_dependency(p_instance->material_overlay, &p_instance->dependency_tracker);
		}

		if (p_instance->base_type == RS::INSTANCE_MESH) {
			//remove materials no longer used and un-own them

			int new_mat_count = RSG::mesh_storage->mesh_get_surface_count(p_instance->base);
			p_instance->materials.resize(new_mat_count);

			_instance_update_mesh_instance(p_instance);
		}

		if (p_instance->base_type == RS::INSTANCE_PARTICLES) {
			// update the process material dependency

			RID particle_material = RSG::particles_storage->particles_get_process_material(p_instance->base);
			if (particle_material.is_valid()) {
				RSG::material_storage->material_update_dependency(particle_material, &p_instance->dependency_tracker);
			}
		}

		if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
			InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);

			bool can_cast_shadows = true;
			bool is_animated = false;
			HashMap<StringName, Instance::InstanceShaderParameter> isparams;

			if (p_instance->cast_shadows == RS::SHADOW_CASTING_SETTING_OFF) {
				can_cast_shadows = false;
			}

			if (p_instance->material_override.is_valid()) {
				if (!RSG::material_storage->material_casts_shadows(p_instance->material_override)) {
					can_cast_shadows = false;
				}
				is_animated = RSG::material_storage->material_is_animated(p_instance->material_override);
				_update_instance_shader_uniforms_from_material(isparams, p_instance->instance_shader_uniforms, p_instance->material_override);
			} else {
				if (p_instance->base_type == RS::INSTANCE_MESH) {
					RID mesh = p_instance->base;

					if (mesh.is_valid()) {
						bool cast_shadows = false;

						for (int i = 0; i < p_instance->materials.size(); i++) {
							RID mat = p_instance->materials[i].is_valid() ? p_instance->materials[i] : RSG::mesh_storage->mesh_surface_get_material(mesh, i);

							if (!mat.is_valid()) {
								cast_shadows = true;
							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}

								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								_update_instance_shader_uniforms_from_material(isparams, p_instance->instance_shader_uniforms, mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}

						if (!cast_shadows) {
							can_cast_shadows = false;
						}
					}

				} else if (p_instance->base_type == RS::INSTANCE_MULTIMESH) {
					RID mesh = RSG::mesh_storage->multimesh_get_mesh(p_instance->base);
					if (mesh.is_valid()) {
						bool cast_shadows = false;

						int sc = RSG::mesh_storage->mesh_get_surface_count(mesh);
						for (int i = 0; i < sc; i++) {
							RID mat = RSG::mesh_storage->mesh_surface_get_material(mesh, i);

							if (!mat.is_valid()) {
								cast_shadows = true;

							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}
								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								_update_instance_shader_uniforms_from_material(isparams, p_instance->instance_shader_uniforms, mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}

						if (!cast_shadows) {
							can_cast_shadows = false;
						}

						RSG::utilities->base_update_dependency(mesh, &p_instance->dependency_tracker);
					}
				} else if (p_instance->base_type == RS::INSTANCE_PARTICLES) {
					bool cast_shadows = false;

					int dp = RSG::particles_storage->particles_get_draw_passes(p_instance->base);

					for (int i = 0; i < dp; i++) {
						RID mesh = RSG::particles_storage->particles_get_draw_pass_mesh(p_instance->base, i);
						if (!mesh.is_valid()) {
							continue;
						}

						int sc = RSG::mesh_storage->mesh_get_surface_count(mesh);
						for (int j = 0; j < sc; j++) {
							RID mat = RSG::mesh_storage->mesh_surface_get_material(mesh, j);

							if (!mat.is_valid()) {
								cast_shadows = true;
							} else {
								if (RSG::material_storage->material_casts_shadows(mat)) {
									cast_shadows = true;
								}

								if (RSG::material_storage->material_is_animated(mat)) {
									is_animated = true;
								}

								_update_instance_shader_uniforms_from_material(isparams, p_instance->instance_shader_uniforms, mat);

								RSG::material_storage->material_update_dependency(mat, &p_instance->dependency_tracker);
							}
						}
					}

					if (!cast_shadows) {
						can_cast_shadows = false;
					}
				}
			}

			if (p_instance->material_overlay.is_valid()) {
				can_cast_shadows = can_cast_shadows && RSG::material_storage->material_casts_shadows(p_instance->material_overlay);
				is_animated = is_animated || RSG::material_storage->material_is_animated(p_instance->material_overlay);
				_update_instance_shader_uniforms_from_material(isparams, p_instance->instance_shader_uniforms, p_instance->material_overlay);
			}

			if (can_cast_shadows != geom->can_cast_shadows) {
				//ability to cast shadows change, let lights now
				for (const Instance *E : geom->lights) {
					InstanceLightData *light = static_cast<InstanceLightData *>(E->base_data);
					light->shadow_dirty = true;
				}

				geom->can_cast_shadows = can_cast_shadows;
			}

			geom->material_is_animated = is_animated;
			p_instance->instance_shader_uniforms = isparams;

			if (p_instance->instance_allocated_shader_uniforms != (p_instance->instance_shader_uniforms.size() > 0)) {
				p_instance->instance_allocated_shader_uniforms = (p_instance->instance_shader_uniforms.size() > 0);
				if (p_instance->instance_allocated_shader_uniforms) {
					p_instance->instance_allocated_shader_uniforms_offset = RSG::material_storage->global_shader_parameters_instance_allocate(p_instance->self);
					ERR_FAIL_NULL(geom->geometry_instance);
					geom->geometry_instance->set_instance_shader_uniforms_offset(p_instance->instance_allocated_shader_uniforms_offset);

					for (const KeyValue<StringName, Instance::InstanceShaderParameter> &E : p_instance->instance_shader_uniforms) {
						if (E.value.value.get_type() != Variant::NIL) {
							int flags_count = 0;
							if (E.value.info.hint == PROPERTY_HINT_FLAGS) {
								// A small hack to detect boolean flags count and prevent overhead.
								switch (E.value.info.hint_string.length()) {
									case 3: // "x,y"
										flags_count = 1;
										break;
									case 5: // "x,y,z"
										flags_count = 2;
										break;
									case 7: // "x,y,z,w"
										flags_count = 3;
										break;
								}
							}
							RSG::material_storage->global_shader_parameters_instance_update(p_instance->self, E.value.index, E.value.value, flags_count);
						}
					}
				} else {
					RSG::material_storage->global_shader_parameters_instance_free(p_instance->self);
					p_instance->instance_allocated_shader_uniforms_offset = -1;
					ERR_FAIL_NULL(geom->geometry_instance);
					geom->geometry_instance->set_instance_shader_uniforms_offset(-1);
				}
			}
		}

		if (p_instance->skeleton.is_valid()) {
			RSG::mesh_storage->skeleton_update_dependency(p_instance->skeleton, &p_instance->dependency_tracker);
		}

		p_instance->dependency_tracker.update_end();

		if ((1 << p_instance->base_type) & RS::INSTANCE_GEOMETRY_MASK) {
			InstanceGeometryData *geom = static_cast<InstanceGeometryData *>(p_instance->base_data);
			ERR_FAIL_NULL(geom->geometry_instance);
			geom->geometry_instance->set_surface_materials(p_instance->materials);
		}
	}

	_instance_update_list.remove(&p_instance->update_item);

	_update_instance(p_instance);

	p_instance->update_aabb = false;
	p_instance->update_dependencies = false;
}

void RendererSceneCull::update_dirty_instances() {
	RSG::utilities->update_dirty_resources();

	while (_instance_update_list.first()) {
		_update_dirty_instance(_instance_update_list.first()->self());
	}
}

void RendererSceneCull::update() {
	//optimize bvhs

	uint32_t rid_count = scenario_owner.get_rid_count();
	RID *rids = (RID *)alloca(sizeof(RID) * rid_count);
	scenario_owner.fill_owned_buffer(rids);
	for (uint32_t i = 0; i < rid_count; i++) {
		Scenario *s = scenario_owner.get_or_null(rids[i]);
		s->indexers[Scenario::INDEXER_GEOMETRY].optimize_incremental(indexer_update_iterations);
		s->indexers[Scenario::INDEXER_VOLUMES].optimize_incremental(indexer_update_iterations);
	}
	scene_render->update();
	update_dirty_instances();
}

bool RendererSceneCull::free(RID p_rid) {
	if (p_rid.is_null()) {
		return true;
	}

	if (scene_render->free(p_rid)) {
		return true;
	}

	if (camera_owner.owns(p_rid)) {
		camera_owner.free(p_rid);

	} else if (scenario_owner.owns(p_rid)) {
		Scenario *scenario = scenario_owner.get_or_null(p_rid);

		scenario->instance_aabbs.reset();
		scenario->instance_data.reset();
		scenario->instance_visibility.reset();

		scenario_owner.free(p_rid);
		RendererSceneOcclusionCull::get_singleton()->remove_scenario(p_rid);

	} else if (RendererSceneOcclusionCull::get_singleton()->is_occluder(p_rid)) {
		RendererSceneOcclusionCull::get_singleton()->free_occluder(p_rid);
	} else if (instance_owner.owns(p_rid)) {
		// delete the instance

		update_dirty_instances();

		Instance *instance = instance_owner.get_or_null(p_rid);

		instance_geometry_set_lightmap(p_rid, RID(), Rect2(), 0);
		instance_geometry_set_material_override(p_rid, RID());
		instance_geometry_set_material_overlay(p_rid, RID());
		instance_attach_skeleton(p_rid, RID());

		if (instance->instance_allocated_shader_uniforms) {
			//free the used shader parameters
			RSG::material_storage->global_shader_parameters_instance_free(instance->self);
		}
		update_dirty_instances(); //in case something changed this

		instance_owner.free(p_rid);
	} else {
		return false;
	}

	return true;
}

void RendererSceneCull::update_visibility_notifiers() {
	SelfList<InstanceVisibilityNotifierData> *E = visible_notifier_list.first();
	while (E) {
		SelfList<InstanceVisibilityNotifierData> *N = E->next();

		InstanceVisibilityNotifierData *visibility_notifier = E->self();
		if (visibility_notifier->just_visible) {
			visibility_notifier->just_visible = false;

			RSG::utilities->visibility_notifier_call(visibility_notifier->base, true, RSG::threaded);
		} else {
			if (visibility_notifier->visible_in_frame != RSG::rasterizer->get_frame_number()) {
				visible_notifier_list.remove(E);

				RSG::utilities->visibility_notifier_call(visibility_notifier->base, false, RSG::threaded);
			}
		}

		E = N;
	}
}

/*******************************/
/* Passthrough to Scene Render */
/*******************************/

/* ENVIRONMENT API */

RendererSceneCull *RendererSceneCull::singleton = nullptr;

void RendererSceneCull::set_scene_render(RendererSceneRender *p_scene_render) {
	scene_render = p_scene_render;
	geometry_instance_pair_mask = scene_render->geometry_instance_get_pair_mask();
}

float get_halton_value(int index, int base) {
	float f = 1;
	float r = 0;
	while (index > 0) {
		f = f / static_cast<float>(base);
		r = r + f * (index % base);
		index = index / base;
	}
	return r * 2.0f - 1.0f;
};

RendererSceneCull::RendererSceneCull() {
	render_pass = 1;
	singleton = this;

	instance_cull_result.set_page_pool(&instance_cull_page_pool);
	instance_shadow_cull_result.set_page_pool(&instance_cull_page_pool);

	for (uint32_t i = 0; i < MAX_UPDATE_SHADOWS; i++) {
		render_shadow_data[i].instances.set_page_pool(&geometry_instance_cull_page_pool);
	}
	for (uint32_t i = 0; i < SDFGI_MAX_CASCADES * SDFGI_MAX_REGIONS_PER_CASCADE; i++) {
		render_sdfgi_data[i].instances.set_page_pool(&geometry_instance_cull_page_pool);
	}

	scene_cull_result.init(&rid_cull_page_pool, &geometry_instance_cull_page_pool, &instance_cull_page_pool);
	scene_cull_result_threads.resize(WorkerThreadPool::get_singleton()->get_thread_count());
	for (InstanceCullResult &thread : scene_cull_result_threads) {
		thread.init(&rid_cull_page_pool, &geometry_instance_cull_page_pool, &instance_cull_page_pool);
	}

	indexer_update_iterations = GLOBAL_GET("rendering/limits/spatial_indexer/update_iterations_per_frame");
	thread_cull_threshold = GLOBAL_GET("rendering/limits/spatial_indexer/threaded_cull_minimum_instances");
	thread_cull_threshold = MAX(thread_cull_threshold, (uint32_t)WorkerThreadPool::get_singleton()->get_thread_count()); //make sure there is at least one thread per CPU

	taa_jitter_array.resize(TAA_JITTER_COUNT);
	for (int i = 0; i < TAA_JITTER_COUNT; i++) {
		taa_jitter_array[i].x = get_halton_value(i, 2);
		taa_jitter_array[i].y = get_halton_value(i, 3);
	}

	dummy_occlusion_culling = memnew(RendererSceneOcclusionCull);
}

RendererSceneCull::~RendererSceneCull() {
	instance_cull_result.reset();
	instance_shadow_cull_result.reset();

	for (uint32_t i = 0; i < MAX_UPDATE_SHADOWS; i++) {
		render_shadow_data[i].instances.reset();
	}
	for (uint32_t i = 0; i < SDFGI_MAX_CASCADES * SDFGI_MAX_REGIONS_PER_CASCADE; i++) {
		render_sdfgi_data[i].instances.reset();
	}

	scene_cull_result.reset();
	for (InstanceCullResult &thread : scene_cull_result_threads) {
		thread.reset();
	}
	scene_cull_result_threads.clear();

	if (dummy_occlusion_culling) {
		memdelete(dummy_occlusion_culling);
	}
}
