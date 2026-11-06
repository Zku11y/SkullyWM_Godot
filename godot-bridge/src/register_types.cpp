#include "bridge_receiver.h"
#include "vulkan_probe.h"
#include "dmabuf_receiver.h"
#include "session_process.h"
#include <godot_cpp/godot.hpp>

using namespace godot;
static void initialize(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ClassDB::register_class<BridgeReceiver>();
		ClassDB::register_class<BridgeVulkanProbe>();
		ClassDB::register_class<BridgeDMABufReceiver>();
		ClassDB::register_class<BridgeSessionProcess>();
	}
}
static void terminate(ModuleInitializationLevel level) {
	if (level == MODULE_INITIALIZATION_LEVEL_SCENE)
		BridgeSessionProcess::finish_reapers();
}

extern "C" GDExtensionBool GDE_EXPORT bridge_library_init(
		GDExtensionInterfaceGetProcAddress get_proc_address,
		GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
	GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
	init.register_initializer(initialize);
	init.register_terminator(terminate);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
