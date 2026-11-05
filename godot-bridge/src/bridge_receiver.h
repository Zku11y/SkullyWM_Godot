#pragma once
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BridgeReceiver : public godot::Node {
	GDCLASS(BridgeReceiver, godot::Node)
	std::thread worker;
	std::atomic<bool> cancelled{false};
	std::mutex mutex;
	std::string status = "Disconnected";
	std::vector<uint8_t> pixels;
	uint32_t width = 0, height = 0;
	void receive_one(int server, const std::string &path);
	void set_status(const std::string &value);
protected:
	static void _bind_methods();
public:
	~BridgeReceiver() override;
	bool start_listening(const godot::String &path);
	void stop();
	godot::String get_status();
	godot::Dictionary take_frame();
};
