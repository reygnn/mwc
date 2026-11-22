/*
 * launch.c - what mwc says when a program it started fails at once: its name and
 * why, from the shell's exit status. input.c starts the program and acts on this.
 * Tested in tests/unit.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "mwc.h"

/* NAME=value, as the shell takes before a command. */
static bool assignment(const char *w, size_t len)
{
	if (len == 0 || !(isalpha((unsigned char)w[0]) || w[0] == '_'))
		return false;
	for (size_t i = 1; i < len; i++) {
		if (w[i] == '=')
			return true;
		if (!isalnum((unsigned char)w[i]) && w[i] != '_')
			return false;
	}
	return false;
}

/* What the shell does itself, never a program to find: then a later one is missing. */
static bool shell_word(const char *w)
{
	static const char *const words[] = {
		"cd", ".", "source", "(", "{", "!", "if", "while", "until", "for", "case",
		"test", "[", "export", "set", "unset", "eval", "command", "builtin", "time",
	};
	for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++)
		if (!strcmp(w, words[i]))
			return true;
	return w[0] == '(' || w[0] == '{';
}

/* At most n bytes of s, not ending inside a UTF-8 character. */
static int utf8_cut(const char *s, int n)
{
	while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return n;
}

/*
 * Only the shell's 127 (not found) and 126 (found, but not to be run): a misspelt name
 * in mwmrc. A program that ends at once on its own with another status, as screenShot
 * does when a selection is cancelled, is not mwc's to report.
 * The name is the program the command starts, past exec and NAME=value. If there is
 * such a program, or the shell runs that word itself (cd, a subshell), something later
 * is missing, as for foot -e with a typo: then the whole command. A file run without a
 * shell is named whole; if it is there, a command in it is what's missing, and that
 * file's name would mislead: nothing is said.
 */
bool launch_failure(const char *what, bool shell, int status,
	bool (*found)(const char *program), char *out, size_t size)
{
	const char *why = status == 127 ? "not found" : status == 126 ? "can't be run" : NULL;
	if (!why || !what || size == 0)
		return false;
	const char *name = what;
	size_t len = strlen(what);
	if (!shell && found && found(what))
		return false;
	if (shell) {
		const char *blank = " \t\n";
		name += strspn(name, blank);
		const char *word = name;
		size_t wlen;
		for (;;) {
			word += strspn(word, blank);
			wlen = strcspn(word, blank);
			if (!(wlen == 4 && !strncmp(word, "exec", 4)) && !assignment(word, wlen))
				break;
			word += wlen;
		}
		if (wlen == 0)
			return false;
		char program[256];
		snprintf(program, sizeof(program), "%.*s", (int)wlen, word);
		if (shell_word(program) || (found && found(program))) {
			len = strlen(name);
			while (len > 0 && strchr(blank, name[len - 1]))
				len--;
		} else {
			name = word;
			len = wlen;
		}
	}
	if (len == 0)
		return false;
	/* The name whole, or cut with an ellipsis: what is left of out after the reason. */
	int room = (int)size - 1 - (int)strlen(why) - 2;
	if (room < 4)
		return false;
	if ((int)len > room)
		snprintf(out, size, "%.*s…: %s", utf8_cut(name, room - 3), name, why);
	else
		snprintf(out, size, "%.*s: %s", (int)len, name, why);
	return true;
}
