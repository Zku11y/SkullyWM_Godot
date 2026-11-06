#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <string>
#include <sys/types.h>

// Main-thread process controller; image transport remains in DMABufReceiver.
class BridgeSessionProcess : public godot::RefCounted {
	GDCLASS(BridgeSessionProcess, godot::RefCounted)
	int lease = -1, compositor_pid = 0, exit_code = -1;
	pid_t supervisor_pid = -1;
	std::string directory, supervisor, compositor, command, app_id;
	std::string state = "stopped", error;
	int fps = 10;
  protected:
	static void _bind_methods();
  public:
	static void finish_reapers();
	~BridgeSessionProcess() override;
	bool prepare(const godot::String &host, const godot::String &dwl,
		const godot::PackedStringArray &arguments, const godot::String &selector, int rate);
	bool start();
	void stop();
	godot::Dictionary poll();
};
