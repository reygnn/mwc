/*
 * mwmrc.h - for unit tests that read mwmrc: a temporary XDG_CONFIG_HOME with a real
 * file in it, and what mwc logs as an error, through wlroots' log callback.
 *
 *   mwmrc_init(); load("Mwm*terminal: mwt\n"); log_lines("missing"); mwmrc_done();
 */
#ifndef MWC_UNIT_MWMRC_H
#define MWC_UNIT_MWMRC_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

static char mwmrc_dir[] = "/tmp/mwc-unit.XXXXXX", rc[256], rc_sub[200];
static char logged[8192];

static void log_to_buffer(enum wlr_log_importance importance, const char *fmt,
	va_list args)
{
	size_t n = strlen(logged);
	if (importance == WLR_ERROR && n < sizeof(logged) - 2) {
		vsnprintf(logged + n, sizeof(logged) - n - 1, fmt, args);
		strcat(logged, "\n");
	}
}

static void mwmrc_init(void)
{
	wlr_log_init(WLR_ERROR, log_to_buffer);
	if (!mkdtemp(mwmrc_dir))
		exit(2);
	snprintf(rc_sub, sizeof(rc_sub), "%s/mwc", mwmrc_dir);
	snprintf(rc, sizeof(rc), "%s/mwmrc", rc_sub);
	if (mkdir(rc_sub, 0700))
		exit(2);
	setenv("XDG_CONFIG_HOME", mwmrc_dir, 1);
}

static void mwmrc_done(void)
{
	unlink(rc);
	rmdir(rc_sub);
	rmdir(mwmrc_dir);
}

/* Writes text as the whole mwmrc and loads it; the log starts empty. */
static void load(const char *text)
{
	FILE *f = fopen(rc, "w");
	if (!f || fputs(text, f) < 0 || fclose(f)) {
		perror(rc);
		exit(2);
	}
	logged[0] = '\0';
	config_load();
}

/* How often what was said; "" for all the lines. */
static int log_lines(const char *what)
{
	int n = 0;
	const char *find = *what ? what : "\n";
	for (const char *s = logged; (s = strstr(s, find)); s += strlen(find))
		n++;
	return n;
}

#endif
