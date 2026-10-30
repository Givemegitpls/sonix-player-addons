#include "yandexcompat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/shell/gui.h"
#include "src/system/device/system.h"
#include "src/system/playback/playlist.h"

// See yandexcompat.h: the player functions rekkod's fork had and this one does
// not, with the substitution each one costs. The bodies are zvukcompat.c's: the
// two services were written against the same fork.

#define YANDEXCOMPAT_PATH_MAX 512

int yandex_playlist_replace_path(const char *old_path, const char *new_path) {
	if (!old_path || !old_path[0] || !new_path || !new_path[0] || strcmp(old_path, new_path) == 0) {
		return 0;
	}
	int count = playlist_count();
	if (count <= 0) {
		return 0;
	}
	char **paths = calloc((size_t)count, sizeof(*paths));
	if (!paths) {
		return 0;
	}
	int changed = 0, kept = 0;
	for (int i = 0; i < count; i++) {
		char path[YANDEXCOMPAT_PATH_MAX];
		if (!playlist_path_at(i, path, sizeof(path)) || !path[0]) {
			continue;
		}
		const char *wanted = strcmp(path, old_path) == 0 ? new_path : path;
		paths[kept] = strdup(wanted);
		if (!paths[kept]) {
			break;
		}
		changed += wanted == new_path;
		kept++;
	}
	// The queue is rebuilt only when something changed and the whole list is
	// here: playlist_load_paths() copies the paths, so they are freed below.
	// A rebuild that could not be made leaves the old names, and the caller
	// finds the file under its guessed name (yandexplay.c says so).
	bool rebuilt = changed > 0 && kept == count;
	if (rebuilt) {
		playlist_load_paths((const char *const *)paths, kept, playlist_current_index());
	}
	for (int i = 0; i < kept; i++) {
		free(paths[i]);
	}
	free(paths);
	return rebuilt ? changed : 0;
}

void yandex_device_state_note_metadata_available(const char *path) {
	// Nothing to do: see yandexcompat.h. The log line is here so that a report
	// of "the tags of a streaming track are empty" has a place to start.
	if (path && path[0]) {
		printf("yandex: %s has its tags now (the player reads them when it loads a track)\n", path);
	}
}

unsigned yandex_sleeptimer_expiration_serial(void) { return 0; }

int yandex_storage_pin_card(char *out, size_t size) {
	if (!out || size == 0) {
		errno = EINVAL;
		return -1;
	}
	out[0] = '\0';
	const char *root = storage_sd_root();
	if (!root || !root[0]) {
		errno = ENODEV;
		return -1;
	}
	if (strlen(root) >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	snprintf(out, size, "%s", root);
	return fd;
}

void yandex_http_set_thread_timeout_cap(int secs) { (void)secs; }

void yandex_gui_post_or_free(void (*cb)(void *), void *user) {
	if (!gui_post(cb, user)) {
		free(user);
	}
}

bool yandex_mp4flac_payload(mp4flac_t *m, const uint8_t *data, size_t size) {
	if (!m || !m->out || (!data && size > 0)) {
		return false;
	}
	if (size == 0) {
		return true;
	}
	if (fwrite(data, 1, size, m->out) != size) {
		return false;
	}
	m->written += (long)size;
	return true;
}

uint64_t yandex_mp4flac_total_samples(const mp4flac_t *m) {
	(void)m;
	return 0;
}

bool yandex_mp4flac_patch_total_samples(const char *path, uint64_t samples) {
	if (!path || !samples || samples >= (1ULL << 36)) {
		return false;
	}
	FILE *f = fopen(path, "r+b");
	if (!f) {
		return false;
	}
	// "fLaC", the STREAMINFO block header, then its 34 bytes: the 36-bit total
	// sits in the low nibble of byte 13 and all of bytes 14-17.
	uint8_t head[8 + 18];
	bool ok = fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "fLaC", 4) == 0 && (head[4] & 0x7f) == 0;
	uint8_t *si = head + 8;
	ok = ok && (si[13] & 0x0f) == 0 && !si[14] && !si[15] && !si[16] && !si[17];
	if (ok) {
		uint8_t patch[5] = {(uint8_t)((si[13] & 0xf0) | ((samples >> 32) & 0x0f)), (uint8_t)(samples >> 24),
							(uint8_t)(samples >> 16), (uint8_t)(samples >> 8), (uint8_t)samples};
		ok = fseek(f, 8 + 13, SEEK_SET) == 0 && fwrite(patch, 1, sizeof(patch), f) == sizeof(patch);
	}
	return fclose(f) == 0 && ok;
}
