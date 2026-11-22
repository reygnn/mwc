/*
 * launch.c: what is said for a program mwc started that failed at once, and when
 * nothing is. Which programs exist is the test's: foot, nice and env do.
 */
#include <string.h>
#include "../../src/mwc.h"
#include "unit.h"

static bool exists(const char *program)
{
	return !strcmp(program, "foot") || !strcmp(program, "nice") || !strcmp(program, "env") ||
		!strcmp(program, "/etc/mwc/autostart");
}

static bool says(const char *what, bool shell, int status, const char *want)
{
	char out[64] = "untouched";
	bool said = launch_failure(what, shell, status, exists, out, sizeof(out));
	if (said && strcmp(out, want))
		fprintf(stderr, "  \"%s\" %d: \"%s\", expected \"%s\"\n", what, status, out, want);
	return said && !strcmp(out, want);
}

TEST(names_and_reasons)
{
	CHECK(says("mfmx", true, 127, "mfmx: not found"));
	CHECK(says("screenShot -c -r", true, 127, "screenShot: not found"));
	CHECK(says("  \tmrun --x", true, 127, "mrun: not found"));
	CHECK(says("/home/u/bin/tool", true, 126, "/home/u/bin/tool: can't be run"));
}

/* The program the command starts, past exec and assignments. */
TEST(past_exec_and_assignments)
{
	CHECK(says("exec mfmx", true, 127, "mfmx: not found"));
	CHECK(says("GDK_BACKEND=wayland LANG=C mfmx -x", true, 127, "mfmx: not found"));
	CHECK(says("exec FOO=1 mfmx", true, 127, "mfmx: not found"));
	/* Not an assignment: = not after a name. */
	CHECK(says("=x mfmx", true, 127, "=x: not found"));
	CHECK(says("1A=x mfmx", true, 127, "1A=x: not found"));
}

/* A program that is there passed on its child's status: the whole command. */
TEST(wrappers)
{
	CHECK(says("foot -e mcc", true, 127, "foot -e mcc: not found"));
	CHECK(says("exec nice -n 5 mcc  ", true, 127, "exec nice -n 5 mcc: not found"));
	CHECK(says("env FOO=1 mcc", true, 126, "env FOO=1 mcc: can't be run"));
}

/* What the shell runs itself: the whole command, as for a wrapper. */
TEST(shell_words)
{
	CHECK(says("cd ~/src && missing-ide", true, 127, "cd ~/src && missing-ide: not found"));
	CHECK(says("(missing-x)", true, 127, "(missing-x): not found"));
	CHECK(says("{ missing-x; }", true, 127, "{ missing-x; }: not found"));
	CHECK(says("exec . ~/env", true, 127, "exec . ~/env: not found"));
}

/* A file run without a shell: its path whole, spaces too; if it is there, something
 * in it failed, and nothing is said. */
TEST(files)
{
	CHECK(says("/home/u/My Config/mwc/autostart", false, 127,
		"/home/u/My Config/mwc/autostart: not found"));
	CHECK(says("exec x", false, 126, "exec x: can't be run"));
	char out[64];
	CHECK(!launch_failure("/etc/mwc/autostart", false, 127, exists, out, sizeof(out)));
}

TEST(nothing_said)
{
	char out[64];
	CHECK(!launch_failure("screenShot -r", true, 1, exists, out, sizeof(out)));
	CHECK(!launch_failure("false", true, 0, exists, out, sizeof(out)));
	CHECK(!launch_failure("x", true, 128 + 15, exists, out, sizeof(out)));
	CHECK(!launch_failure("", true, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure("   ", true, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure("exec", true, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure("A=1 B=2", true, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure("", false, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure(NULL, true, 127, exists, out, sizeof(out)));
	CHECK(!launch_failure("x", true, 127, exists, out, 0));
	CHECK(!launch_failure("x", true, 127, exists, out, 8)); /* no room for a name */
	/* Without a way to look programs up, the program alone. */
	CHECK(launch_failure("foot -e mcc", true, 127, NULL, out, sizeof(out)));
	CHECK(!strcmp(out, "foot: not found"));
}

/* A long name is cut to fit, with an ellipsis, and never inside a UTF-8 character. */
TEST(long_name)
{
	char cmd[200], out[64];
	memset(cmd, 'a', sizeof(cmd) - 1);
	cmd[sizeof(cmd) - 1] = '\0';
	CHECK(launch_failure(cmd, true, 127, exists, out, sizeof(out)));
	CHECK(strlen(out) < sizeof(out));
	const char *tail = "…: not found";
	CHECK(strlen(out) > strlen(tail) && !strcmp(out + strlen(out) - strlen(tail), tail));
	CHECK(out[0] == 'a');
	/* "ä" is two bytes; at every offset the cut leaves whole characters only. */
	for (int lead = 0; lead < 2; lead++) {
		char u[200] = "";
		for (int i = 0; i < lead; i++)
			strcat(u, "a");
		while (strlen(u) < 150)
			strcat(u, "ä");
		CHECK(launch_failure(u, false, 127, exists, out, sizeof(out)));
		size_t keep = strlen(out) - strlen(tail);
		CHECK(keep % 2 == (size_t)lead % 2);
		CHECK(!strncmp(out, u, keep));
	}
}

int main(void)
{
	RUN(names_and_reasons);
	RUN(past_exec_and_assignments);
	RUN(wrappers);
	RUN(shell_words);
	RUN(files);
	RUN(nothing_said);
	RUN(long_name);
	return unit_done();
}
