#ifndef ADDON_CARDPACE_H
#define ADDON_CARDPACE_H

// How fast a download may fill the card.
//
// The card this device writes through times out (jzmmc, 3 s) when the page
// cache is pushed at it faster than it retires the pages: the download thread
// then sits in balance_dirty_pages_ratelimited, every fsync in the player (the
// log, the library journal) waits behind it, and the playback thread waits on a
// page that is being written back -- the whole process in D state, which reads
// as a freeze. A 25-45 MB lossless track arriving at network speed is 2-4 MB/s
// of dirty pages; the rate below is what the card has been seen to take.
//
// Playback is a track ahead, so a slower download costs nothing anybody hears:
// a 40 MB track at 1 MB/s is 40 s, and the track it precedes plays for minutes.
// The number is a property of the board, not of the card: 9 MB/s write measured
// on the device with dd conv=fsync, and 1 MB/s leaves room for the log, the
// library journal and the playback reads.
//
// One of these per download, next to the download's own counters:
//
//   cardpace_reset(&d->pace);              // before the first write
//   ...
//   cardpace_note(&d->pace, d->done);      // after every write to the track
//
// Cover images (a few tens of KB) are not worth a clock of their own.
#define CARDPACE_BYTES_PER_SEC (1024L * 1024L)

typedef struct {
	long started_ms; // when the download first noted a write, for the log line
	long at_ms;	 // the clock the rate is measured from; 0 = not started yet
} cardpace_t;

// Zeroes the clock: the first cardpace_note() after this starts the rate.
void cardpace_reset(cardpace_t *p);

// Holds a download to the card's rate: `written` is the whole file so far, so
// the sleep is whatever is left of the time that much data should have taken.
// The first note starts the clock and is not delayed. Any thread.
void cardpace_note(cardpace_t *p, long written);

// Milliseconds since the first note, for the "took N s, M KB/s" line at the end
// of a download. 0 when nothing was written.
long cardpace_elapsed_ms(const cardpace_t *p);

#endif
