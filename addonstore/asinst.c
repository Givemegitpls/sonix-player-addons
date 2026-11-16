#include "asinst.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aszip.h"
#include "common/cardpace.h"

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"

#define README_MAX (256 * 1024)

static void set_error(char *error, size_t size, const char *text) {
	if (error && size) {
		snprintf(error, size, "%s", text && text[0] ? text : "failed");
	}
}

// The asset name comes from an API response and is used as a file name in the
// cache. It is the only string from the network that lands in a path.
static bool asset_ok(const char *asset) {
	if (!asset || !asset[0] || strchr(asset, '/') || strchr(asset, '\\') || strstr(asset, "..")) {
		return false;
	}
	return true;
}

// What an update is allowed to replace in a folder that is already on the
// card: the code, the tile and the addon's own instructions and artwork. Every
// other name is the card's -- settings.ini (which some releases ship filled
// in), token.txt, sessions, caches, saves, a database, a file somebody put
// there -- and is written only when it is not there yet. That is the rule that
// makes an update safe: a release may add a file, it may not take one over.
static bool update_replaces(const char *name) {
	return strcmp(name, "addon.so") == 0 || strcmp(name, "icon.png") == 0 || strcmp(name, "README.txt") == 0 ||
		   strncmp(name, "assets/", 7) == 0;
}

static bool update_keeps(const char *name, const char *target) {
	if (update_replaces(name)) {
		return false;
	}
	char path[1900];
	snprintf(path, sizeof(path), "%s/%s", target, name);
	return access(path, F_OK) == 0;
}

static bool mkdir_p(const char *path) {
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = buf + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
				return false;
			}
			*p = '/';
		}
	}
	return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

static bool remove_tree(const char *path) {
	struct stat st;
	if (lstat(path, &st) != 0) {
		return true;
	}
	if (!S_ISDIR(st.st_mode)) {
		return unlink(path) == 0;
	}
	DIR *dir = opendir(path);
	if (!dir) {
		return false;
	}
	bool ok = true;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		char child[1024];
		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		if (!remove_tree(child)) {
			ok = false;
		}
	}
	closedir(dir);
	if (ok && rmdir(path) != 0) {
		ok = false;
	}
	return ok;
}

// `suffix` is "" for a fresh install and ".asnew" for the staging half of an
// update: the file lands beside its destination and a rename puts it in place.
static bool write_entry(const aszip_t *z, int index, const char *root, const char *suffix, cardpace_t *pace,
						long *written, char *error, size_t errsz) {
	const aszip_entry_t *e = &z->entry[index];
	char path[1600];
	snprintf(path, sizeof(path), "%s/%s%s", root, e->name, suffix);

	char dir[1600];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (!mkdir_p(dir)) {
			set_error(error, errsz, "cannot create a directory on the card");
			return false;
		}
	}
	size_t len = 0;
	if (e->uncomp_size == 0) {
		FILE *empty = fopen(path, "wb");
		if (!empty) {
			set_error(error, errsz, "cannot create a file on the card");
			return false;
		}
		if (fclose(empty) != 0) {
			unlink(path);
			set_error(error, errsz, "cannot create a file on the card");
			return false;
		}
		return true;
	}
	uint8_t *data = aszip_read(z, index, &len);
	if (!data) {
		set_error(error, errsz, "the archive entry cannot be read (corrupt zip?)");
		return false;
	}
	FILE *f = fopen(path, "wb");
	bool ok = f != NULL;
	if (ok && fwrite(data, 1, len, f) != len) {
		set_error(error, errsz, "write to the card failed (is it full?)");
		ok = false;
	}
	if (ok) {
		*written += (long)len;
		cardpace_note(pace, *written);
	}
	if (f && fclose(f) != 0) {
		ok = false;
	}
	free(data);
	if (!ok && path[0]) {
		unlink(path);
	}
	return ok;
}

// The same, out of the archive rather than a directory: an update keeps the
// manifest already on the card, so the only copy of the release's version is
// the one in the zip.
static void read_version_zip(const aszip_t *z, char *out, size_t size) {
	out[0] = '\0';
	int index = aszip_find(z, "addon.json");
	if (index < 0) {
		return;
	}
	size_t len = 0;
	uint8_t *data = aszip_read(z, index, &len);
	if (!data) {
		return;
	}
	char text[4096];
	if (len >= sizeof(text)) {
		len = sizeof(text) - 1;
	}
	memcpy(text, data, len);
	text[len] = '\0';
	free(data);
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return;
	}
	json_obj_str(&doc, json_root(&doc), "version", out, size);
	json_free(&doc);
}

// What the release has to be before anything is written over a working addon:
// a 32-bit little-endian MIPS ELF (this device's dlopen would refuse anything
// else, and by then the old library would be gone), and a manifest speaking the
// ABI this player was built against. A repository that publishes a source zip
// or an addon for another board is refused here, with the reason, instead of
// producing a tile that cannot start.
static bool payload_ok(const aszip_t *z, char *error, size_t errsz) {
	int bin = aszip_find(z, "addon.so");
	size_t len = 0;
	uint8_t *data = aszip_read(z, bin, &len);
	bool elf = data && len >= 20 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F' &&
			   data[4] == 1 && data[5] == 1 && data[18] == 8 && data[19] == 0;
	free(data);
	if (!elf) {
		set_error(error, errsz, "addon.so is not a MIPS32 little-endian library");
		return false;
	}

	int man = aszip_find(z, "addon.json");
	len = 0;
	data = aszip_read(z, man, &len);
	if (!data) {
		set_error(error, errsz, "addon.json cannot be read");
		return false;
	}
	char text[4096];
	if (len >= sizeof(text)) {
		len = sizeof(text) - 1;
	}
	memcpy(text, data, len);
	text[len] = '\0';
	free(data);
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		set_error(error, errsz, "addon.json is not JSON");
		return false;
	}
	long api = json_obj_long(&doc, json_root(&doc), "api", 0);
	json_free(&doc);
	// SONIX_ADDON_API: the number in src/addon/addon.h this addon was built
	// against. A release built for another API is refused by the loader
	// anyway; doing it here keeps a working tile in place.
	if (api != 1) {
		set_error(error, errsz, "the release speaks another addon API than this player");
		return false;
	}
	return true;
}

// The version string inside an unpacked manifest, so the record says what was
// installed and the page can show it without a second scan.
static void read_version(const char *dir, char *out, size_t size) {
	out[0] = '\0';
	char path[1600];
	snprintf(path, sizeof(path), "%s/addon.json", dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char text[4096];
	size_t len = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[len] = '\0';
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return;
	}
	json_obj_str(&doc, json_root(&doc), "version", out, size);
	json_free(&doc);
}

bool as_install(const as_model_t *m, const as_release_t *rel, as_progress_t *pg, char *version, size_t vsz,
				char *error, size_t errsz) {
	if (version && vsz) {
		version[0] = '\0';
	}
	if (!rel->id[0] || !rel->url[0] || !asset_ok(rel->asset)) {
		set_error(error, errsz, "the release has no usable file to install");
		return false;
	}
	if (!mkdir_p(m->cache_dir)) {
		set_error(error, errsz, "cannot use the store's cache folder on the card");
		return false;
	}

	char part[1600];
	snprintf(part, sizeof(part), "%s/%s.part", m->cache_dir, rel->asset);
	if (!as_download(rel->url, part, rel->size, rel->digest, pg, error, errsz)) {
		return false;
	}

	aszip_t z;
	if (!aszip_open(&z, part)) {
		unlink(part);
		set_error(error, errsz, "the downloaded file is not a zip");
		return false;
	}
	bool ok = z.bad == 0 && aszip_find(&z, "addon.json") >= 0 && aszip_find(&z, "addon.so") >= 0;
	if (!ok) {
		aszip_close(&z);
		unlink(part);
		set_error(error, errsz, "the zip is not an addon: addon.json and addon.so must be at its root");
		return false;
	}
	if (!payload_ok(&z, error, errsz)) {
		aszip_close(&z);
		unlink(part);
		return false;
	}

	char target[1600], tmp[1200], old[1200];
	snprintf(target, sizeof(target), "%s/%s", m->addons_dir, rel->id);
	snprintf(tmp, sizeof(tmp), "%s/.addonstore-new-%s", m->addons_dir, rel->id);
	snprintf(old, sizeof(old), "%s/.addonstore-old-%s", m->addons_dir, rel->id);

	// An update, not a first install: the folder is there and it is an addon.
	// It is updated in place -- the files the release carries are replaced one
	// at a time, and everything else in the folder stays exactly where it is:
	// addon.json, settings.ini, session tokens, caches, saves, a README the
	// user edited. Replacing the folder wholesale would take all of that with
	// the old version.
	bool update = access(target, F_OK) == 0;

	if (update) {
		char release_version[AS_VERSION_MAX] = "";
		read_version_zip(&z, release_version, sizeof(release_version));
		if (version && vsz) {
			snprintf(version, vsz, "%s", release_version);
		}
		cardpace_t pace;
		cardpace_reset(&pace);
		long written = 0;

		// Half of a previous attempt (a battery that went, a card that was
		// pulled) leaves .asnew files; they are ours and never wanted.
		for (int i = 0; i < z.count; i++) {
			char staged[1900];
			snprintf(staged, sizeof(staged), "%s/%s.asnew", target, z.entry[i].name);
			unlink(staged);
		}
		// addon.so is written last: while the assets change, the code that is
		// running is still the old one, and the switch of the library is the
		// last thing an update does.
		for (int stage = 0; stage < 2 && ok; stage++) {
			bool library = stage == 1;
			for (int i = 0; i < z.count && ok; i++) {
				const char *name = z.entry[i].name;
				if (strcmp(name, "addon.so") == 0 ? !library : library) {
					continue;
				}
				if (!library && update_keeps(name, target)) {
					continue;
				}
				ok = write_entry(&z, i, target, ".asnew", &pace, &written, error, errsz);
			}
			// Nothing is replaced until every file of this half is staged:
			// a full card fails here with the addon still intact.
			for (int i = 0; ok && i < z.count; i++) {
				const char *name = z.entry[i].name;
				if (strcmp(name, "addon.so") == 0 ? !library : library) {
					continue;
				}
				if (!library && update_keeps(name, target)) {
					continue;
				}
				char staged[1900], final[1900];
				snprintf(staged, sizeof(staged), "%s/%s.asnew", target, name);
				snprintf(final, sizeof(final), "%s/%s", target, name);
				if (rename(staged, final) != 0) {
					set_error(error, errsz, "cannot put a new file in place");
					ok = false;
				}
			}
		}
		if (!ok) {
			// Anything staged but not renamed goes; what was replaced stays,
			// which is why addon.so was last.
			for (int i = 0; i < z.count; i++) {
				char staged[1900];
				snprintf(staged, sizeof(staged), "%s/%s.asnew", target, z.entry[i].name);
				unlink(staged);
			}
			aszip_close(&z);
			unlink(part);
			return false;
		}
		aszip_close(&z);
		unlink(part);
		remove_tree(old);
		printf("addonstore: %s updated in place (manifest and settings kept)\n", rel->id);
		return true;
	}

	remove_tree(tmp);
	remove_tree(old);
	if (!mkdir_p(tmp)) {
		aszip_close(&z);
		unlink(part);
		set_error(error, errsz, "cannot create the addon's folder on the card");
		return false;
	}

	cardpace_t pace;
	cardpace_reset(&pace);
	long written = 0;
	for (int i = 0; i < z.count && ok; i++) {
		ok = write_entry(&z, i, tmp, "", &pace, &written, error, errsz);
	}
	aszip_close(&z);
	unlink(part);
	if (!ok) {
		remove_tree(tmp);
		return false;
	}
	char tmp_version[AS_VERSION_MAX];
	read_version(tmp, tmp_version, sizeof(tmp_version));
	if (version && vsz) {
		snprintf(version, vsz, "%s", tmp_version);
	}

	// The switch. Anything unexpected puts the previous folder back: the user
	// is left with what they had, never with neither.
	bool had_old = false;
	if (access(target, F_OK) == 0) {
		if (rename(target, old) == 0) {
			had_old = true;
		} else {
			remove_tree(tmp);
			set_error(error, errsz, "cannot move the installed version aside");
			return false;
		}
	}
	if (rename(tmp, target) != 0) {
		if (had_old) {
			rename(old, target);
		}
		remove_tree(tmp);
		set_error(error, errsz, "cannot put the new version in place");
		return false;
	}
	if (had_old) {
		remove_tree(old);
	}
	return true;
}

bool as_remove(const as_model_t *m, const char *id, char *error, size_t errsz) {
	if (!id || !id[0]) {
		set_error(error, errsz, "nothing to remove");
		return false;
	}
	// The store's own folder holds the library this page runs from.
	const char *own = strrchr(m->addon_dir, '/');
	own = own ? own + 1 : m->addon_dir;
	if (strcmp(own, id) == 0) {
		set_error(error, errsz, "the store cannot remove itself");
		return false;
	}
	char path[1600];
	as_addon_path(m, id, path, sizeof(path));
	if (access(path, F_OK) != 0) {
		return true;
	}
	if (!remove_tree(path)) {
		set_error(error, errsz, "some files could not be removed (is the card read-only?)");
		return false;
	}
	return true;
}

bool as_readme_local(const as_model_t *m, const char *id, char *out, size_t size) {
	char dir[1600];
	as_addon_path(m, id, dir, sizeof(dir));
	const char *names[] = {"README.txt", "readme.txt", "README.md", "README"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(out, size, "%s/%s", dir, names[i]);
		if (access(out, R_OK) == 0) {
			return true;
		}
	}
	out[0] = '\0';
	return false;
}

bool as_readme_cached(const as_model_t *m, const char *id, char *out, size_t size) {
	snprintf(out, size, "%s/readme/%s.txt", m->cache_dir, id);
	return access(out, R_OK) == 0;
}

bool as_read_manifest(const as_model_t *m, as_release_t *rel, as_progress_t *pg, char *error, size_t errsz) {
	if (!rel->url[0] || !asset_ok(rel->asset)) {
		set_error(error, errsz, "the release names an unusable file");
		return false;
	}
	if (!mkdir_p(m->cache_dir)) {
		set_error(error, errsz, "cannot use the store's cache folder on the card");
		return false;
	}
	char part[1600];
	snprintf(part, sizeof(part), "%s/%s.manifest.part", m->cache_dir, rel->asset);
	if (!as_download(rel->url, part, rel->size, rel->digest, pg, error, errsz)) {
		return false;
	}
	aszip_t z;
	if (!aszip_open(&z, part)) {
		unlink(part);
		set_error(error, errsz, "the downloaded file is not a zip");
		return false;
	}
	int index = aszip_find(&z, "addon.json");
	size_t len = 0;
	uint8_t *data = index >= 0 ? aszip_read(&z, index, &len) : NULL;
	if (!data) {
		aszip_close(&z);
		unlink(part);
		set_error(error, errsz, "the release has no addon.json");
		return false;
	}
	char text[4096];
	if (len >= sizeof(text)) {
		len = sizeof(text) - 1;
	}
	memcpy(text, data, len);
	text[len] = '\0';
	free(data);
	as_manifest_fields(text, rel->title, sizeof(rel->title), rel->section, sizeof(rel->section), NULL, 0);

	// The library's fingerprint too: the zip and the installed folder are not
	// the same file, so the asset's digest cannot say whether a hand-copied
	// addon is this release. This does, without another download.
	index = aszip_find(&z, "addon.so");
	len = 0;
	data = index >= 0 ? aszip_read(&z, index, &len) : NULL;
	if (data) {
		sha256_ctx sha;
		sha256_init(&sha);
		sha256_update(&sha, data, len);
		sha256_final_hex(&sha, rel->so_sha256);
		rel->so_size = (long)len;
		free(data);
	}
	aszip_close(&z);
	unlink(part);
	return true;
}

bool as_readme_fetch(const as_model_t *m, const as_release_t *rel, const char *id, as_progress_t *pg, char *error,
					 size_t errsz) {
	if (!asset_ok(rel->asset)) {
		set_error(error, errsz, "the release names an unusable file");
		return false;
	}
	if (!mkdir_p(m->cache_dir)) {
		set_error(error, errsz, "cannot use the store's cache folder on the card");
		return false;
	}
	char part[1600];
	snprintf(part, sizeof(part), "%s/%s.part", m->cache_dir, rel->asset);
	if (!as_download(rel->url, part, rel->size, rel->digest, pg, error, errsz)) {
		return false;
	}
	aszip_t z;
	if (!aszip_open(&z, part)) {
		unlink(part);
		set_error(error, errsz, "the downloaded file is not a zip");
		return false;
	}
	int index = aszip_find(&z, "README.txt");
	for (int i = 0; index < 0 && i < z.count; i++) {
		if (strcasecmp(z.entry[i].name, "README.txt") == 0 || strcasecmp(z.entry[i].name, "README") == 0) {
			index = i;
		}
	}
	if (index < 0) {
		aszip_close(&z);
		unlink(part);
		set_error(error, errsz, "the release has no README");
		return false;
	}
	size_t len = 0;
	uint8_t *data = aszip_read(&z, index, &len);
	aszip_close(&z);
	unlink(part);
	if (!data) {
		set_error(error, errsz, "the README cannot be read out of the zip");
		return false;
	}
	if (len > README_MAX) {
		len = README_MAX;
	}

	char dir[1600];
	snprintf(dir, sizeof(dir), "%s/readme", m->cache_dir);
	char path[1900];
	snprintf(path, sizeof(path), "%s/%s.txt", dir, id);
	bool ok = mkdir_p(dir);
	FILE *f = ok ? fopen(path, "wb") : NULL;
	if (f) {
		ok = fwrite(data, 1, len, f) == len && fclose(f) == 0;
		if (!ok) {
			unlink(path);
		}
	} else {
		ok = false;
	}
	free(data);
	if (!ok) {
		set_error(error, errsz, "cannot write the README to the card");
	}
	return ok;
}
