// noise_editor_plugin.h
// MIT LICENSE -> GODOT ENGINE

#ifndef NOISE_EDITOR_PLUGIN_H
#define NOISE_EDITOR_PLUGIN_H

#ifdef TOOLS_ENABLED

#include "editor/editor_plugin.h"

class NoiseEditorPlugin : public EditorPlugin {
	GDCLASS(NoiseEditorPlugin, EditorPlugin)

public:
	String get_name() const override;

	NoiseEditorPlugin();
};

#endif // TOOLS_ENABLED

#endif // NOISE_EDITOR_PLUGIN_H
