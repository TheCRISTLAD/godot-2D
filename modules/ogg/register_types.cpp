// register_types.cpp
// MIT LICENSE -> GODOT ENGINE

#include "register_types.h"

#include "ogg_packet_sequence.h"

void initialize_ogg_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(OggPacketSequence);
	GDREGISTER_CLASS(OggPacketSequencePlayback);
}

void uninitialize_ogg_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
