#ifndef STREAMERR_H
#define STREAMERR_H

#include <stdbool.h>
#include <stddef.h>

// Why a streaming request failed, in words a person can act on.
//
// The addon's own copy of the vocabulary rekkod's streamerr.h defined. That
// module is not carried: it classifies from http.h's per-request outcome
// (http_last_outcome), and this player's http.h does not record one -- see the
// port's notes. What is kept is what the service modules actually use:
//
//   * the classes, because a service knows better than a status code what went
//     wrong (a 200 with an empty answer, a 418 from an anti-bot filter, a
//     token the service refused);
//   * streamerr_begin()/streamerr_hint(), called from inside the request;
//   * streamerr_describe(), which turns the hint and the service's own words
//     into one sentence and one log line.
//
// The sentence goes where a page shows it (gui_notify_popup) or into the
// listener's `text`. Nothing here touches the interface: it runs on the
// worker, like the requests themselves.
//
// Thread contract: per thread, like http_last_error(). begin() before a job,
// hint() from inside it, describe() after a failure, all on the same thread.

// How long a request the interface waits on may take. The player's http.h
// takes the timeout per call, so this is only the number the caller passes.
#define STREAM_UI_TIMEOUT_SECS 8

typedef enum {
	STREAM_ERR_NONE = 0,
	STREAM_ERR_NO_WIFI,			// no network at all
	STREAM_ERR_NO_RESPONSE,		// the server could not be reached, or went quiet
	STREAM_ERR_SIGN_IN,			// the session was refused: sign in again
	STREAM_ERR_NO_SUBSCRIPTION, // the account's plan does not cover it
	STREAM_ERR_KEYS,			// the application's keys are missing or out of date
	STREAM_ERR_SERVER,			// the service is failing (5xx, or asking us to slow down)
	STREAM_ERR_NOT_FOUND,		// what was asked for is not there (any more)
	STREAM_ERR_OTHER,			// anything else; the service's own words are kept
} stream_err_t;

// A job is starting: forget the hint, so a failure before any request is made
// is not read as an older request's.
void streamerr_begin(void);

// From inside a service: it knows better than the status code. The last hint
// before streamerr_describe() wins.
void streamerr_hint(stream_err_t cls);

// The class of the failure that just happened on this thread, without logging
// or clearing anything.
stream_err_t streamerr_classify(void);

// The failure that just happened on this thread: classifies it, logs one line
// ("zvuk: no_response: <detail>"), clears the hint and writes the sentence to
// show into `out`. `detail` is the service's own message; it is used as the
// sentence for STREAM_ERR_OTHER and kept for the log in every other case.
stream_err_t streamerr_describe(const char *who, const char *detail, char *out, size_t size);

#endif /* STREAMERR_H */
