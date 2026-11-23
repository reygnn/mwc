/*
 * spawn.c - the programs mwc starts: the terminal, the run dialog, the screen saver,
 * the root menu's and the autostart file. Each runs in a session of its own; a watcher
 * tells mwc within a second if it couldn't be started, and launch.c says what to make
 * of that.
 */
#define _GNU_SOURCE /* pidfd_open, close_range, pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/pidfd.h>
#include <sys/wait.h>
#include <wlr/util/log.h>
#include "mwc.h"

/* For the toast about a program that couldn't be started; set by spawn_init(). */
static struct server *spawn_server;

/* A program's watcher, until it reports or the second is over. */
struct launch_watch {
	struct wl_list link;
	struct wl_event_source *source;
	int fd;
	char *what;
	bool shell; /* what is a command for /bin/sh -c, not a file */
};
static struct wl_list launch_watches = { &launch_watches, &launch_watches };
static int launches; /* started with a watcher, for the tests */

static void launch_watch_free(struct launch_watch *w)
{
	wl_list_remove(&w->link);
	wl_event_source_remove(w->source);
	close(w->fd);
	free(w->what);
	free(w);
}

/* A program by name in PATH, or by its path. */
static bool program_found(const char *program)
{
	return strchr(program, '/') ? access(program, X_OK) == 0 : in_path(program);
}

static int launch_reported(int fd, uint32_t mask, void *data)
{
	struct launch_watch *w = data;
	int status;
	char text[64];
	bool got = read(fd, &status, sizeof(status)) == (ssize_t)sizeof(status);
	if (got && launch_failure(w->what, w->shell, status, program_found, text,
			sizeof(text))) {
		wlr_log(WLR_ERROR, "\"%s\": %s", w->what, text);
		toast_error(spawn_server, text, 3000);
	}
	launch_watch_free(w);
	return 0;
}

void spawn_fini(void)
{
	struct launch_watch *w, *tmp;
	wl_list_for_each_safe(w, tmp, &launch_watches, link)
		launch_watch_free(w);
}

/* The program's process; its exit status 127 if it can't be. */
static void launch_exec(const char *const argv[])
{
	execv(argv[0], (char *const *)argv);
	/* A script without a #! line, as a shell would run it. */
	if (errno == ENOEXEC) {
		const char *sh[] = { "/bin/sh", argv[0], NULL };
		execv(sh[0], (char *const *)sh);
	}
	fprintf(stderr, "mwc: cannot run %s: %s\n", argv[0], strerror(errno));
	_exit(127);
}

int spawn_watching(int *started)
{
	*started = launches;
	return wl_list_length(&launch_watches);
}

/* The watcher's: the program's exit status if it ends within a second. */
static bool launch_wait(pid_t program, int *code)
{
	int status;
	pid_t got = 0;
	int pidfd = pidfd_open(program, 0);
	if (pidfd >= 0) {
		struct pollfd p = { .fd = pidfd, .events = POLLIN };
		if (poll(&p, 1, 1000) == 1)
			got = waitpid(program, &status, WNOHANG);
		close(pidfd);
	} else {
		/* No pidfd (a kernel before 5.3, a seccomp profile): looked at 50 times. */
		const struct timespec step = { 0, 20 * 1000 * 1000 };
		for (int i = 0; i < 50 && got == 0; i++) {
			got = waitpid(program, &status, WNOHANG);
			if (got == 0)
				nanosleep(&step, NULL);
		}
	}
	if (got != program || !WIFEXITED(status))
		return false;
	*code = WEXITSTATUS(status);
	return true;
}

/*
 * Double fork, so mwc never collects zombies, with a watcher between: it waits a second
 * for the program and writes its exit status to a pipe mwc reads, if it ended by then.
 * After that the program goes on without it. what names it for launch_failure().
 */
static void spawn_argv(const char *const argv[], const char *what, bool shell)
{
	int fds[2] = { -1, -1 };
	/* Close-on-exec: the program must not keep the watcher's end open. Without the
	 * pipe the program starts all the same, unwatched. */
	bool watched = spawn_server && pipe2(fds, O_CLOEXEC) == 0;
	pid_t pid = fork();
	if (pid == 0) {
		/* wl_event_loop_add_signal() blocks our handled signals, and
		 * programs would inherit that and ignore SIGTERM or SIGHUP. */
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		/* An ignored signal stays ignored across exec; SIGCHLD ignored would take
		 * the program's status from its watcher. */
		signal(SIGPIPE, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);
		setsid();
		if (fork() != 0)
			_exit(0);
		if (!watched)
			launch_exec(argv);
		/* The watcher never execs: mwc's sockets, the DRM device and clipboard
		 * pipes would stay open in it. Only the pipe stays, as 3, closed for the
		 * program at its exec, and the standard ones; before the program comes. */
		close(fds[0]);
		int out = dup2(fds[1], 3);
		if (out < 0 || fcntl(out, F_SETFD, FD_CLOEXEC) != 0)
			_exit(1);
		close_range(4, ~0U, 0);
		pid_t program = fork();
		if (program == 0)
			launch_exec(argv);
		int code;
		if (program > 0 && launch_wait(program, &code) &&
				write(out, &code, sizeof(code)) < 0)
			_exit(1);
		_exit(0);
	}
	if (watched)
		close(fds[1]);
	if (pid > 0) {
		int status;
		waitpid(pid, &status, 0);
	}
	if (!watched)
		return;
	struct launch_watch *w = calloc(1, sizeof(*w));
	if (w && pid > 0 && (w->what = strdup(what))) {
		w->fd = fds[0];
		w->shell = shell;
		launches++;
		w->source = wl_event_loop_add_fd(spawn_server->event_loop, fds[0],
			WL_EVENT_READABLE | WL_EVENT_HANGUP, launch_reported, w);
		if (w->source) {
			wl_list_insert(&launch_watches, &w->link);
			return;
		}
	}
	if (w)
		free(w->what);
	free(w);
	close(fds[0]);
}

void spawn(const char *cmd)
{
	if (!cmd || !*cmd)
		return;
	const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
	spawn_argv(argv, cmd, true);
}

void spawn_file(const char *path)
{
	const char *argv[] = { path, NULL };
	spawn_argv(argv, path, false);
}

void spawn_init(struct server *server)
{
	spawn_server = server;
}
