#pragma once
#include "../../dmabuf-proof/protocol.h"
#include <atomic>
#include <condition_variable>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <memory>
#include <mutex>
#include <thread>

class BridgeDMABufReceiver : public godot::RefCounted {
	GDCLASS(BridgeDMABufReceiver, godot::RefCounted)
	struct Slot {
		proof::Frame frame;
		int fd = -1;
		~Slot();
	};
	std::thread worker;
	std::atomic<bool> cancelled{false};
	std::mutex mutex;
	std::condition_variable condition;
	std::unique_ptr<Slot> pending;
	bool completed = false, succeeded = false;
	int release_fd = -1;
	uint64_t copy_ticket = 0;
	proof::Frame copying_frame;
	std::string status = "Disconnected";
	bool compositor_mode = false;
	std::array<uint8_t, GPU_HELLO_BYTES> hello{};
	void receive(int server, const std::string &path);
	void complete_copy(bool success, const std::string &message, int release = -1);

  protected:
	static void _bind_methods();

  public:
	~BridgeDMABufReceiver() override;
	godot::Dictionary configure_compositor(uint64_t physical);
	bool start(const godot::String &path);
	void stop();
	bool has_frame();
	godot::String get_status();
	// Render-thread only. Returns a Godot-owned texture RID after GPU completion.
	godot::Dictionary copy_next(godot::Object *rd, uint64_t physical, uint64_t logical);
	// Render-thread only: adopt imports into engine retirement, then poll without
	// waiting.
	godot::Dictionary submit_next(godot::Object *rd, uint64_t physical, uint64_t logical);
	godot::Dictionary poll_copy(godot::Object *rd);
	void discard_copy(godot::Object *rd);
};
