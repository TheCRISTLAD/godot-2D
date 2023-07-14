// material_editor_plugin.h

#ifndef MATERIAL_EDITOR_PLUGIN_H
#define MATERIAL_EDITOR_PLUGIN_H

#include "editor/editor_inspector.h"
#include "editor/editor_plugin.h"
#include "editor/plugins/editor_resource_conversion_plugin.h"
#include "scene/resources/material.h"

class ColorRect;
class HBoxContainer;
class SubViewport;
class SubViewportContainer;
class TextureButton;

class MaterialEditor : public Control {
	GDCLASS(MaterialEditor, Control);

	SubViewportContainer *vc_2d = nullptr;
	SubViewport *viewport_2d = nullptr;
	HBoxContainer *layout_2d = nullptr;
	ColorRect *rect_instance = nullptr;

	Ref<Material> material;

	struct ThemeCache {
		Ref<Texture2D> checkerboard;
	} theme_cache;

protected:
	virtual void _update_theme_item_cache() override;
	void _notification(int p_what);

public:
	void edit(Ref<Material> p_material);
	MaterialEditor();
};

class EditorInspectorPluginMaterial : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginMaterial, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
};

class MaterialEditorPlugin : public EditorPlugin {
	GDCLASS(MaterialEditorPlugin, EditorPlugin);

public:
	virtual String get_name() const override { return "Material"; }

	MaterialEditorPlugin();
};

class ParticleProcessMaterialConversionPlugin : public EditorResourceConversionPlugin {
	GDCLASS(ParticleProcessMaterialConversionPlugin, EditorResourceConversionPlugin);

public:
	virtual String converts_to() const override;
	virtual bool handles(const Ref<Resource> &p_resource) const override;
	virtual Ref<Resource> convert(const Ref<Resource> &p_resource) const override;
};

class CanvasItemMaterialConversionPlugin : public EditorResourceConversionPlugin {
	GDCLASS(CanvasItemMaterialConversionPlugin, EditorResourceConversionPlugin);

public:
	virtual String converts_to() const override;
	virtual bool handles(const Ref<Resource> &p_resource) const override;
	virtual Ref<Resource> convert(const Ref<Resource> &p_resource) const override;
};

#endif // MATERIAL_EDITOR_PLUGIN_H
