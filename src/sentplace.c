/*
 * sentplace.c: where an X11 window's frame goes for each size it was sent lately.
 * X configures carry no serial, so a commit says only which size the window drew,
 * not which configure that answers. It may be one sent before the last: the place
 * that went with its size is where its frame belongs until the next one.
 */
#include <string.h>
#include "mwc.h"

void sent_places_clear(struct sent_places *s)
{
	s->n = 0;
}

void sent_places_add(struct sent_places *s, int x, int y, int w, int h)
{
	/* One place per size, the newest: the frame belongs where X was told last. */
	for (int i = 0; i < s->n; i++) {
		if (s->p[i].w == w && s->p[i].h == h) {
			memmove(&s->p[i], &s->p[i + 1], (s->n - i - 1) * sizeof(s->p[0]));
			s->n--;
			break;
		}
	}
	if (s->n == SENT_PLACES) {
		memmove(&s->p[0], &s->p[1], (s->n - 1) * sizeof(s->p[0]));
		s->n--;
	}
	s->p[s->n++] = (struct sent_place){ x, y, w, h };
}

bool sent_places_find(const struct sent_places *s, int w, int h, int *x, int *y)
{
	for (int i = 0; i < s->n; i++) {
		if (s->p[i].w == w && s->p[i].h == h) {
			*x = s->p[i].x;
			*y = s->p[i].y;
			return true;
		}
	}
	return false;
}

void sent_places_move(struct sent_places *s, int dx, int dy)
{
	for (int i = 0; i < s->n; i++) {
		s->p[i].x += dx;
		s->p[i].y += dy;
	}
}
