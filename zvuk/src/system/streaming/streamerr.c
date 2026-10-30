#include "streamerr.h"

#include <stdio.h>
#include <string.h>

// See streamerr.h. The addon's copy of the vocabulary, with the sentences
// written here: an addon's strings are its own (docs/ADDONS.md, "Languages"),
// and these are shown inside the addon, not in the player's interface.

static __thread stream_err_t hint;

// The class the last describe() worked out, for a caller that wants it later
// without repeating the log line.
static __thread stream_err_t last_class;

static const char *sentence(stream_err_t cls) {
	switch (cls) {
	case STREAM_ERR_NO_WIFI:
		return "no network connection";
	case STREAM_ERR_NO_RESPONSE:
		return "the server did not answer";
	case STREAM_ERR_SIGN_IN:
		return "the session was refused: sign in again";
	case STREAM_ERR_NO_SUBSCRIPTION:
		return "the subscription does not cover this";
	case STREAM_ERR_KEYS:
		return "the service's keys are missing or out of date";
	case STREAM_ERR_SERVER:
		return "the service is failing: try again later";
	case STREAM_ERR_NOT_FOUND:
		return "not found on the service";
	case STREAM_ERR_NONE:
	case STREAM_ERR_OTHER:
	default:
		return "the request failed";
	}
}

static const char *class_name(stream_err_t cls) {
	switch (cls) {
	case STREAM_ERR_NO_WIFI:
		return "no_wifi";
	case STREAM_ERR_NO_RESPONSE:
		return "no_response";
	case STREAM_ERR_SIGN_IN:
		return "sign_in";
	case STREAM_ERR_NO_SUBSCRIPTION:
		return "no_subscription";
	case STREAM_ERR_KEYS:
		return "keys";
	case STREAM_ERR_SERVER:
		return "server";
	case STREAM_ERR_NOT_FOUND:
		return "not_found";
	case STREAM_ERR_OTHER:
		return "other";
	case STREAM_ERR_NONE:
	default:
		return "none";
	}
}

void streamerr_begin(void) { hint = STREAM_ERR_NONE; }

void streamerr_hint(stream_err_t cls) {
	if (cls != STREAM_ERR_NONE) {
		hint = cls;
	}
}

stream_err_t streamerr_classify(void) { return hint != STREAM_ERR_NONE ? hint : STREAM_ERR_OTHER; }

stream_err_t streamerr_describe(const char *who, const char *detail, char *out, size_t size) {
	stream_err_t cls = streamerr_classify();
	if (cls == STREAM_ERR_NONE) {
		cls = STREAM_ERR_OTHER;
	}
	if (out && size) {
		// The service's own sentence says it best when nobody classified the
		// failure; otherwise the class is what the user can act on.
		const char *text = cls == STREAM_ERR_OTHER && detail && detail[0] ? detail : sentence(cls);
		snprintf(out, size, "%s", text);
	}
	fprintf(stderr, "%s: %s (%s)\n", who ? who : "stream", class_name(cls), detail && detail[0] ? detail : "-");
	hint = STREAM_ERR_NONE;
	last_class = cls;
	return cls;
}
