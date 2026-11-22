/*
 * activation.c: which tokens take the focus, which only mark a desktop, for windows on
 * the current desktop and on another one.
 */
#include "../../src/mwc.h"
#include "unit.h"

/* A good token for a mapped window on the current desktop; each case changes one thing. */
static struct activation_request good(void)
{
	return (struct activation_request){
		.from_input = true, .source_had_focus = true, .focus_kept = true,
		.target_mapped = true,
	};
}

TEST(focus_from_input)
{
	struct activation_request r = good();
	CHECK_INT(activation_decide(&r), ACTIVATION_FOCUS);
	r.target_off_desktop = true;
	CHECK_INT(activation_decide(&r), ACTIVATION_SWITCH);
}

/* Each thing a good token needs, missing: no focus, the desktop of one elsewhere marked. */
TEST(no_focus_otherwise)
{
	for (int i = 0; i < 3; i++) {
		for (int off = 0; off < 2; off++) {
			struct activation_request r = good();
			r.target_off_desktop = off;
			if (i == 0)
				r.from_input = false;
			else if (i == 1)
				r.source_had_focus = false;
			else
				r.focus_kept = false;
			CHECK_INT(activation_decide(&r), off ? ACTIVATION_MARK : ACTIVATION_NONE);
		}
	}
}

TEST(unmapped_or_docked)
{
	struct activation_request r = good();
	r.target_mapped = false;
	CHECK_INT(activation_decide(&r), ACTIVATION_NONE);
	r.target_off_desktop = true;
	CHECK_INT(activation_decide(&r), ACTIVATION_NONE);
	r = good();
	r.target_docked = true;
	CHECK_INT(activation_decide(&r), ACTIVATION_NONE);
	r.from_input = false;
	CHECK_INT(activation_decide(&r), ACTIVATION_NONE);
}

int main(void)
{
	RUN(focus_from_input);
	RUN(no_focus_otherwise);
	RUN(unmapped_or_docked);
	return unit_done();
}
