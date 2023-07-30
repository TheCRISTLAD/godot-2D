/**************************************************************************/
/*  scene_shader_forward_clustered.cpp                                    */
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

#include "scene_shader_forward_clustered.h"
#include "core/config/project_settings.h"
#include "core/math/math_defs.h"
#include "render_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

using namespace RendererSceneRenderImplementation;

void SceneShaderForwardClustered::ShaderData::set_code(const String &p_code) {
}

bool SceneShaderForwardClustered::ShaderData::is_animated() const {
	return false;
}

bool SceneShaderForwardClustered::ShaderData::casts_shadows() const {
	return false;
}

RS::ShaderNativeSourceCode SceneShaderForwardClustered::ShaderData::get_native_source_code() const {
	SceneShaderForwardClustered *shader_singleton = (SceneShaderForwardClustered *)SceneShaderForwardClustered::singleton;

	return shader_singleton->shader.version_get_native_source_code(version);
}

SceneShaderForwardClustered::ShaderData::ShaderData() :
		shader_list_element(this) {
}

SceneShaderForwardClustered::ShaderData::~ShaderData() {
}

RendererRD::MaterialStorage::ShaderData *SceneShaderForwardClustered::_create_shader_func() {
	ShaderData *shader_data = memnew(ShaderData);
	singleton->shader_list.add(&shader_data->shader_list_element);
	return shader_data;
}

void SceneShaderForwardClustered::MaterialData::set_render_priority(int p_priority) {
}

void SceneShaderForwardClustered::MaterialData::set_next_pass(RID p_pass) {
}

bool SceneShaderForwardClustered::MaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	return false;
}

SceneShaderForwardClustered::MaterialData::~MaterialData() {
}

RendererRD::MaterialStorage::MaterialData *SceneShaderForwardClustered::_create_material_func(ShaderData *p_shader) {
	MaterialData *material_data = memnew(MaterialData);
	material_data->shader_data = p_shader;
	//update will happen later anyway so do nothing.
	return material_data;
}

SceneShaderForwardClustered *SceneShaderForwardClustered::singleton = nullptr;

SceneShaderForwardClustered::SceneShaderForwardClustered() {
}

SceneShaderForwardClustered::~SceneShaderForwardClustered() {
}

void SceneShaderForwardClustered::init(const String p_defines) {
}

void SceneShaderForwardClustered::set_default_specialization_constants(const Vector<RD::PipelineSpecializationConstant> &p_constants) {
}
