#include "session_process.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <godot_cpp/core/class_db.hpp>
#include <map>
#include <memory>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;
using namespace godot;
namespace {
struct Reaper {
	std::thread worker;
	std::shared_ptr<std::atomic<bool>> done;
};
// Main-thread only. Join before library termination, so no thread executes
// extension code after unload. Finished entries are collected during polling.
std::vector<Reaper> reapers;
void collect_reapers(bool all = false) {
	for (auto item = reapers.begin(); item != reapers.end();) {
		if (all || item->done->load()) {
			item->worker.join();
			item = reapers.erase(item);
		} else ++item;
	}
}
std::string utf8(const String &value) { return value.utf8().get_data(); }
std::string quote(const std::string &value) {
	std::string result = "'";
	for (char c : value)
		result += c == '\'' ? "'\\''" : std::string(1, c);
	return result + "'";
}
bool valid_string(const String &value) {
	for (int64_t i = 0; i < value.length(); i++)
		if (value[i] == 0) return false;
	return true;
}
} // namespace

void BridgeSessionProcess::_bind_methods() {
	ClassDB::bind_method(D_METHOD("prepare", "supervisor", "dwl", "arguments", "app_id", "fps"), &BridgeSessionProcess::prepare);
	ClassDB::bind_method(D_METHOD("start"), &BridgeSessionProcess::start);
	ClassDB::bind_method(D_METHOD("stop"), &BridgeSessionProcess::stop);
	ClassDB::bind_method(D_METHOD("poll"), &BridgeSessionProcess::poll);
}
bool BridgeSessionProcess::prepare(const String &host, const String &dwl,
		const PackedStringArray &arguments, const String &selector, int rate) {
	poll();
	if (supervisor_pid > 0) {
		error = "Previous session is still stopping";
		return false;
	}
	error.clear();
	state = "error";
	if (!valid_string(host) || !valid_string(dwl) || !valid_string(selector) ||
		selector.is_empty() || selector.length() > 255 || selector == "Godot" ||
		selector == "org.godotengine.Godot" || arguments.is_empty() ||
		arguments.size() > 64 || arguments[0].is_empty() || rate < 1 || rate > 60) {
		error = "Provide a native app ID, 1–60 FPS and 1–64 application arguments";
		return false;
	}
	supervisor = utf8(host);
	compositor = utf8(dwl);
	if (supervisor.empty() || supervisor[0] != '/' || access(supervisor.c_str(), X_OK) ||
		compositor.empty() || compositor[0] != '/' || access(compositor.c_str(), X_OK)) {
		error = "Build bridge_session_host and dwl; executable paths must be absolute";
		return false;
	}
	command = "exec";
	for (int64_t i = 0; i < arguments.size(); i++) {
		if (!valid_string(arguments[i])) {
			error = "Application arguments cannot contain NUL";
			return false;
		}
		command += " " + quote(utf8(arguments[i]));
	}
	if (command.size() > 32768) {
		error = "Application arguments exceed 32 KiB";
		return false;
	}
	char pattern[] = "/tmp/dwl-godot-session-XXXXXX";
	char *created = mkdtemp(pattern);
	if (!created) {
		error = std::strerror(errno);
		return false;
	}
	directory = created;
	app_id = utf8(selector);
	fps = rate;
	compositor_pid = 0;
	exit_code = -1;
	state = "prepared";
	return true;
}
bool BridgeSessionProcess::start() {
	if (state != "prepared") {
		error = "Prepare the session before starting it";
		return false;
	}
	std::map<std::string, std::string> env;
	for (char **entry = environ; *entry; entry++) {
		std::string value = *entry;
		auto equal = value.find('=');
		auto key = value.substr(0, equal);
		if (key.rfind("DWL_BRIDGE_", 0) && key.rfind("WLR_", 0) && key != "WAYLAND_SOCKET")
			env[key] = value.substr(equal + 1);
	}
	env["WLR_BACKENDS"] = "wayland";
	env["WLR_RENDERER"] = "gles2";
	env["DWL_BRIDGE_GPU_SOCKET"] = directory + "/gpu.sock";
	env["DWL_BRIDGE_GPU_APP_ID"] = app_id;
	env["DWL_BRIDGE_FPS"] = std::to_string(fps);
	std::vector<std::string> entries;
	for (const auto &item : env)
		entries.push_back(item.first + "=" + item.second);
	std::vector<char *> envp;
	for (auto &item : entries)
		envp.push_back(item.data());
	envp.push_back(nullptr);
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair)) {
		error = std::strerror(errno);
		state = "error";
		return false;
	}
	// Duplicate above the reserved descriptors before constructing spawn actions.
	int child = fcntl(pair[1], F_DUPFD_CLOEXEC, 10);
	close(pair[1]);
	if (child < 0) {
		close(pair[0]);
		error = std::strerror(errno);
		state = "error";
		return false;
	}
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	posix_spawn_file_actions_init(&actions);
	posix_spawnattr_init(&attr);
	int result = posix_spawn_file_actions_adddup2(&actions, child, 3);
	if (!result) result = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
	if (!result) result = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
	if (!result) result = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	if (!result) result = posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	if (!result) result = posix_spawn_file_actions_addopen(&actions, 2, (directory + "/dwl.log").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	sigset_t empty, defaults;
	sigemptyset(&empty);
	sigemptyset(&defaults);
	for (int signal : {SIGTERM, SIGINT, SIGCHLD, SIGPIPE}) sigaddset(&defaults, signal);
	if (!result) result = posix_spawnattr_setsigmask(&attr, &empty);
	if (!result) result = posix_spawnattr_setsigdefault(&attr, &defaults);
	if (!result) result = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	char *argv[] = {supervisor.data(), compositor.data(), command.data(), nullptr};
	if (!result) result = posix_spawn(&supervisor_pid, supervisor.c_str(), &actions, &attr, argv, envp.data());
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close(child);
	if (result) {
		close(pair[0]);
		supervisor_pid = -1;
		error = std::strerror(result);
		state = "error";
		return false;
	}
	lease = pair[0];
	state = "running";
	return true;
}
void BridgeSessionProcess::stop() {
	if (lease >= 0) close(lease);
	lease = -1;
	state = supervisor_pid > 0 ? "stopping" : "stopped";
}
Dictionary BridgeSessionProcess::poll() {
	collect_reapers();
	if (lease >= 0) {
		char packet[96];
		ssize_t size;
		while ((size = recv(lease, packet, sizeof(packet) - 1, MSG_DONTWAIT)) > 0) {
			packet[size] = 0;
			int value;
			if (sscanf(packet, "DWS1 STARTED %d", &value) == 1) compositor_pid = value;
			else if (sscanf(packet, "DWS1 EXIT %d", &value) == 1) {
				exit_code = value;
				error = "Nested compositor exited (" + std::to_string(value) + "); inspect dwl.log";
				state = "error";
			} else if (sscanf(packet, "DWS1 ERROR %d", &value) == 1) {
				error = std::strerror(value);
				state = "error";
			}
		}
	}
	if (supervisor_pid > 0) {
		int status;
		pid_t result = waitpid(supervisor_pid, &status, WNOHANG);
		if (result == supervisor_pid || (result < 0 && errno == ECHILD)) {
			if (result > 0) exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
			if (state == "running") {
				error = "Nested session ended; inspect dwl.log";
				state = "error";
			} else if (state == "stopping") state = "stopped";
			supervisor_pid = -1;
			if (lease >= 0) close(lease);
			lease = -1;
		}
	}
	Dictionary info;
	info["state"] = String(state.c_str());
	info["error"] = String(error.c_str());
	info["runtime_directory"] = String(directory.c_str());
	info["socket_path"] = String((directory + "/gpu.sock").c_str());
	info["supervisor_pid"] = supervisor_pid;
	info["dwl_pid"] = compositor_pid;
	info["exit_code"] = exit_code;
	return info;
}
BridgeSessionProcess::~BridgeSessionProcess() {
	stop();
	if (supervisor_pid > 0) {
		// Normal stop polls/reaps before release. Unexpected scene removal must
		// neither block the main thread nor leave a zombie in a living engine.
		pid_t child = supervisor_pid;
		auto done = std::make_shared<std::atomic<bool>>(false);
		reapers.push_back({std::thread([child, done] {
			while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
			done->store(true);
		}), done});
	}
}
void BridgeSessionProcess::finish_reapers() { collect_reapers(true); }
