/* Linux-only session guardian. FD 3 is a private lease to the Godot process.
 * It is never inherited by dwl/applications. EOF also handles editor SIGKILL.
 * This single-threaded subreaper owns only descendants it launched itself. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void
terminate(int signo)
{
	(void)signo;
	stopping = 1;
}

static long long
milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void
report(const char *event, int value)
{
	char packet[96];
	int size = snprintf(packet, sizeof(packet), "DWS1 %s %d", event, value);
	/* Status is advisory and bounded. A full/dead peer never blocks cleanup. */
	(void)send(3, packet, (size_t)size, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static int
signal_children(int signo)
{
	char name[96];
	FILE *file;
	long child;
	snprintf(name, sizeof(name), "/proc/self/task/%ld/children", (long)getpid());
	file = fopen(name, "r");
	if (!file)
		return -1;
	/* This is a snapshot, not a complete census. Re-scan until waitpid says
	 * ECHILD; exiting parents can expose new adopted descendants each pass.
	 * Do not reap while signalling: direct-child PIDs cannot be recycled. */
	while (fscanf(file, "%ld", &child) == 1) {
		if (child > 1)
			kill((pid_t)child, signo);
	}
	fclose(file);
	return 0;
}

int
main(int argc, char **argv)
{
	struct sigaction action = {0};
	struct pollfd lease = {.fd = 3, .events = POLLIN};
	pid_t compositor, reaped;
	int status, exit_code = 0, have_children = 1;
	long long began;
	if (argc != 3)
		return 2;
	if (fcntl(3, F_SETFD, FD_CLOEXEC) < 0 ||
			prctl(PR_SET_CHILD_SUBREAPER, 1L) < 0 || signal_children(0) < 0) {
		report("ERROR", errno);
		return 2;
	}
	action.sa_handler = terminate;
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	/* Godot can die before the helper finishes exec/startup. */
	if (poll(&lease, 1, 0) > 0)
		return 0;
	compositor = fork();
	if (compositor < 0) {
		report("ERROR", errno);
		return 2;
	}
	if (!compositor) {
		/* No Godot descriptors or lease reach this exec. dwl creates its own
		 * session for the startup program; subreaping covers that too. */
		close(3);
		action.sa_handler = SIG_DFL;
		sigaction(SIGTERM, &action, NULL);
		sigaction(SIGINT, &action, NULL);
		execl(argv[1], argv[1], "-s", argv[2], (char *)NULL);
		perror("bridge session: exec dwl");
		_exit(127);
	}
	report("STARTED", compositor);
	while (!stopping) {
		int result = poll(&lease, 1, 50);
		if (result > 0 || (result < 0 && errno != EINTR))
			break;
		reaped = waitpid(compositor, &status, WNOHANG);
		if (reaped == compositor) {
			exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
			report("EXIT", exit_code);
			break;
		}
	}
	/* Do not trust stale process groups or scan unrelated desktop processes.
	 * Killing a parent adopts its remaining children; repeat and reap them.
	 * An uninterruptible kernel task can delay cleanup, but never the UI loop. */
	began = milliseconds();
	while (have_children) {
		int signo = milliseconds() - began < 1500 ? SIGTERM : SIGKILL;
		if (signal_children(signo) < 0) {
			perror("bridge session: enumerate owned children");
			return 2;
		}
		do {
			reaped = waitpid(-1, &status, WNOHANG);
		} while (reaped > 0 || (reaped < 0 && errno == EINTR));
		have_children = !(reaped < 0 && errno == ECHILD);
		if (have_children)
			poll(NULL, 0, 20);
	}
	/* Private diagnostic directory is retained; stale transport sockets aren't. */
	unlink("gpu.sock");
	report("STOPPED", exit_code);
	return exit_code;
}
