// A host-side check for the store's own logic: releases parsing, install,
// readme extraction, records, removal. The network is stubbed -- one canned
// release list and one zip served from a local file -- so the code under test is
// the real asdata/asnet/asinst/aszip plus the player's json and sha256.
//
//   cc -O1 -Wall -I. -I../sonix-player/sonix-player -o /tmp/test_store \
//      tools/test_store.c asdata.c asnet.c asinst.c aszip.c \
//      common/cardpace.c src/system/core/json.c src/system/core/sha256.c \
//      tools/stub_net.c
//
// (from the addonstore/ directory; see tools/run_store_test.sh). An optional
// third argument is a second zip of the same addon: with it the harness checks
// that an update replaces addon.so and leaves addon.json and settings alone.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../asdata.h"
#include "../asinst.h"
#include "../asnet.h"
#include "../aszip.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

static void write_file(const char *path, const void *data, size_t len) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fwrite(data, 1, len, f);
	fclose(f);
}

static long file_size(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static char *slurp(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)size + 1);
	if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf) {
		buf[size] = '\0';
		*len = (size_t)size;
	}
	return buf;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s CARD_ROOT ZIP [ZIP_V2]\n", argv[0]);
		return 2;
	}
	const char *card = argv[1];
	const char *zip = argv[2];
	const char *zip_v2 = argc > 3 ? argv[3] : NULL;
	const char *zip_bad_so = argc > 4 ? argv[4] : NULL;
	const char *zip_bad_api = argc > 5 ? argv[5] : NULL;

	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/Addons/addonstore/cache'", card, card);
	if (system(cmd) != 0) {
		return 2;
	}

	char addon_dir[512];
	snprintf(addon_dir, sizeof(addon_dir), "%s/Addons/addonstore", card);
	setenv("AS_TEST_ZIP", zip, 1);

	as_model_t model;
	char cache[512];
	snprintf(cache, sizeof(cache), "%s/Addons/addonstore/cache", card);
	as_model_init(&model, addon_dir, card, cache);

	check(as_repos_load(&model), "repos.txt is written with the default");
	check(access(addon_dir, F_OK) != 0 || model.repo_count == 1, "one repository from the default");
	as_cache_load(&model);
	check(model.release_count == 0, "the release cache starts empty");
	as_records_load(&model);
	as_scan_installed(&model);
	check(model.installed_count == 0, "nothing installed on a fresh card");

	// --- what a manifest says, out of its text ---
	char ttl[AS_TITLE_MAX], sec[16], ver[AS_VERSION_MAX];
	as_manifest_fields("{\"title\":\"Dino\",\"section\":\"more\",\"version\":\"1.2\",\"titles\":{\"English\":\"Dino!\"}}",
					   ttl, sizeof(ttl), sec, sizeof(sec), ver, sizeof(ver));
	check(strcmp(sec, "more") == 0 && strcmp(ver, "1.2") == 0 && strcmp(ttl, "Dino!") == 0,
		  "manifest fields: section, version, localized title");
	as_manifest_fields("{\"title\":\"X\",\"section\":\"nonsense\"}", ttl, sizeof(ttl), sec, sizeof(sec), ver,
					   sizeof(ver));
	check(sec[0] == '\0' && strcmp(ttl, "X") == 0, "an unknown section is left empty");

	// --- the release list, through the stubbed API ---
	as_release_t found[AS_MAX];
	int found_count = 0;
	char error[AS_ERROR_MAX] = "";
	as_progress_t progress = {0};
	check(as_fetch_releases("testowner/testrepo", found, AS_MAX, &found_count, error, sizeof(error), &progress),
		  "the release list is fetched");
	const char *real_json = getenv("AS_TEST_JSON");
	if (real_json && real_json[0]) {
		// A response captured from the real API: the shape is the point, not
		// the names. Every entry must have an id, a size and a download URL.
		bool shaped = found_count > 0;
		for (int i = 0; i < found_count; i++) {
			if (!found[i].id[0] || found[i].size <= 0 || !found[i].url[0]) {
				shaped = false;
			}
		}
		check(shaped, "every asset in the real response parsed");
		printf("  %d assets, first %s (%s, %ld bytes)\n", found_count, found[0].id, found[0].tag, found[0].size);
		printf("%d failure(s)\n", failures);
		return failures ? 1 : 0;
	}
	check(found_count == 1 && strcmp(found[0].id, "dino") == 0, "one asset, id dino");
	check(found[0].size == file_size(zip), "the size is the asset's");
	check(as_read_manifest(&model, &found[0], &progress, error, sizeof(error)) &&
			  strcmp(found[0].section, "more") == 0 && found[0].title[0] != '\0',
		  "a release's manifest is read without installing it");

	// --- install ---
	char version[AS_VERSION_MAX] = "";
	check(as_install(&model, &found[0], &progress, version, sizeof(version), error, sizeof(error)),
		  "install succeeds");
	char path[700];
	snprintf(path, sizeof(path), "%s/Addons/dino/addon.so", card);
	check(access(path, R_OK) == 0, "addon.so landed in Addons/dino");
	snprintf(path, sizeof(path), "%s/Addons/dino/README.txt", card);
	check(access(path, R_OK) == 0, "README.txt landed in Addons/dino");
	check(version[0] != '\0', "the installed manifest names a version");
	check(access(addon_dir, F_OK) == 0, "the store's own folder is untouched");

	as_scan_installed(&model);
	check(model.installed_count == 1 && strcmp(model.installed[0].id, "dino") == 0,
		  "the installed scan sees it");

	// --- the record, and update detection ---
	as_record_t rec;
	memset(&rec, 0, sizeof(rec));
	snprintf(rec.id, sizeof(rec.id), "%s", found[0].id);
	snprintf(rec.repo, sizeof(rec.repo), "%s", found[0].repo);
	snprintf(rec.asset, sizeof(rec.asset), "%s", found[0].asset);
	snprintf(rec.digest, sizeof(rec.digest), "%s", found[0].digest);
	snprintf(rec.version, sizeof(rec.version), "%s", version);
	rec.size = found[0].size;
	check(as_record_put(&model, &rec), "the record is written");
	check(!as_update_available(&model, "dino"), "no update while the asset is the same");

	as_records_load(&model);
	check(model.record_count == 1 && strcmp(model.record[0].id, "dino") == 0, "the record is read back");

	model.release_count = 0;
	model.release[model.release_count++] = found[0];
	check(as_installed_matches(&model, "dino"), "the installed release matches the record");
	model.release[0].digest[0] = model.release[0].digest[0] == 's' ? 'x' : 's';
	check(as_update_available(&model, "dino"), "a different asset digest is an update");
	model.release[0] = found[0];
	check(!as_update_available(&model, "dino"), "the same release is not");

	// Put by hand, with no record at all: the library itself still says
	// whether the card is current.
	check(as_record_drop(&model, "dino"), "the record can be dropped");
	check(as_installed_matches(&model, "dino") && !as_update_available(&model, "dino"),
		  "a hand-installed addon is up to date without a record");
	model.release[0].so_sha256[0] = model.release[0].so_sha256[0] == 'a' ? 'b' : 'a';
	check(as_update_available(&model, "dino"), "and is offered an update when the library differs");
	model.release[0] = found[0];
	check(!as_update_available(&model, "addonstore"), "nothing installed is never an update");

	// --- readme out of the release ---
	check(as_readme_fetch(&model, &found[0], "dino", &progress, error, sizeof(error)), "readme fetch works");
	char cached[700];
	check(as_readme_cached(&model, "dino", cached, sizeof(cached)), "the readme is in the cache");
	size_t len = 0;
	char *text = slurp(cached, &len);
	check(text && len > 0, "the cached readme has text");
	free(text);

	// --- an update keeps the manifest and everything not in the archive ---
	if (zip_v2) {
		snprintf(path, sizeof(path), "%s/Addons/dino/settings.ini", card);
		write_file(path, "quality = 5\n", 12);
		snprintf(path, sizeof(path), "%s/Addons/dino/addon.json", card);
		const char *custom_manifest = "{\"api\":1,\"title\":\"Custom\",\"version\":\"custom\"}\n";
		write_file(path, custom_manifest, strlen(custom_manifest));
		snprintf(path, sizeof(path), "%s/Addons/dino/cache/note.txt", card);
		snprintf(cmd, sizeof(cmd), "mkdir -p '%s/Addons/dino/cache'", card);
		if (system(cmd) != 0) {
			return 2;
		}
		write_file(path, "keep me\n", 8);
		snprintf(path, sizeof(path), "%s/Addons/dino/addon.so", card);
		long so_before = file_size(path);
		snprintf(path, sizeof(path), "%s/Addons/dino/README.txt", card);
		long readme_before = file_size(path);

		const char *real_json_old = getenv("AS_TEST_JSON");
		(void)real_json_old;
		setenv("AS_TEST_ZIP", zip_v2, 1);
		as_release_t v2 = found[0];
		v2.size = file_size(zip_v2);
		snprintf(v2.digest, sizeof(v2.digest), "%s", "");
		char error2[AS_ERROR_MAX] = "";
		as_progress_t progress2 = {0};
		check(as_install(&model, &v2, &progress2, version, sizeof(version), error2, sizeof(error2)),
			  "update succeeds");
		setenv("AS_TEST_ZIP", zip, 1);

		snprintf(path, sizeof(path), "%s/Addons/dino/settings.ini", card);
		size_t len2 = 0;
		char *settings = slurp(path, &len2);
		check(settings && strcmp(settings, "quality = 5\n") == 0, "settings.ini survives an update");
		free(settings);
		snprintf(path, sizeof(path), "%s/Addons/dino/addon.json", card);
		char *manifest = slurp(path, &len2);
		check(manifest && strstr(manifest, "Custom") != NULL, "addon.json survives an update");
		free(manifest);
		snprintf(path, sizeof(path), "%s/Addons/dino/cache/note.txt", card);
		check(access(path, R_OK) == 0, "a cache file nobody's archive mentions survives");
		snprintf(path, sizeof(path), "%s/Addons/dino/settings.ini", card);
		char *settings_after = slurp(path, &len2);
		check(settings_after && strcmp(settings_after, "quality = 5\n") == 0,
			  "a settings.ini shipped in the release does not overwrite the card's");
		free(settings_after);
		snprintf(path, sizeof(path), "%s/Addons/dino/newfile.txt", card);
		check(access(path, R_OK) == 0, "a file the release adds is written");
		snprintf(path, sizeof(path), "%s/Addons/dino/addon.so", card);
		check(file_size(path) != so_before, "addon.so was replaced");
		snprintf(path, sizeof(path), "%s/Addons/dino/README.txt", card);
		check(file_size(path) != readme_before, "README.txt was replaced");
	}

	// --- a release that cannot work is refused before anything is replaced ---
	if (zip_bad_so || zip_bad_api) {
		char path_so[700];
		snprintf(path_so, sizeof(path_so), "%s/Addons/dino/addon.so", card);
		long before = file_size(path_so);
		as_release_t bad = found[0];
		char bad_error[AS_ERROR_MAX] = "";
		snprintf(bad.digest, sizeof(bad.digest), "%s", "");
		if (zip_bad_so) {
			setenv("AS_TEST_ZIP", zip_bad_so, 1);
			bad.size = file_size(zip_bad_so);
			check(!as_install(&model, &bad, &progress, version, sizeof(version), bad_error, sizeof(bad_error)),
				  "a non-MIPS addon.so is refused");
		}
		if (zip_bad_api) {
			setenv("AS_TEST_ZIP", zip_bad_api, 1);
			bad.size = file_size(zip_bad_api);
			bad_error[0] = '\0';
			check(!as_install(&model, &bad, &progress, version, sizeof(version), bad_error, sizeof(bad_error)),
				  "an addon for another API is refused");
		}
		setenv("AS_TEST_ZIP", zip, 1);
		check(file_size(path_so) == before, "the installed addon.so is untouched after a refused release");
	}

	// --- remove ---
	check(!as_remove(&model, "addonstore", error, sizeof(error)), "the store refuses to remove itself");
	check(as_remove(&model, "dino", error, sizeof(error)), "remove works");
	check(access(path, F_OK) != 0, "the folder is gone");
	as_scan_installed(&model);
	check(model.installed_count == 0, "the scan is empty again");

	printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
