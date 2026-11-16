// A host-side check for the store's own logic: release parsing, the catalogue,
// search, repositories, install, readme extraction, records, removal. The
// network is stubbed -- one canned release list, one catalogue and one zip
// served from local files -- so the code under test is the real
// asdata/asnet/asinst/aszip plus the player's json and sha256.
//
//   cc -O1 -Wall -I. -I../sonix-player/sonix-player -o /tmp/test_store
//      tools/test_store.c asdata.c asnet.c asinst.c aszip.c
//      common/cardpace.c src/system/core/json.c src/system/core/sha256.c
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

#include "src/system/core/sha256.h"

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
	char path[700];
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

	// --- the API fallback with one tag per addon ---
	// Releases come newest first; the newest names one addon. Reading only the
	// newest would show one addon, so every release is read and the first
	// occurrence of an id wins.
	{
		const char *two_releases =
			"[{\"tag_name\":\"latest\",\"assets\":[{\"name\":\"dino.zip\",\"size\":44,"
			"\"browser_download_url\":\"https://test.invalid/copy-dino.zip\"},"
			"{\"name\":\"qobuz.zip\",\"size\":55,"
			"\"browser_download_url\":\"https://test.invalid/copy-qobuz.zip\"}]},"
			"{\"tag_name\":\"dino-v2.0\",\"assets\":[{\"name\":\"dino.zip\",\"size\":11,"
			"\"browser_download_url\":\"https://test.invalid/dino.zip\"}]},"
			"{\"tag_name\":\"qobuz-v1.0\",\"assets\":[{\"name\":\"dino.zip\",\"size\":22,"
			"\"browser_download_url\":\"https://test.invalid/old-dino.zip\"},"
			"{\"name\":\"qobuz.zip\",\"size\":33,"
			"\"browser_download_url\":\"https://test.invalid/qobuz.zip\"}]}]";
		snprintf(path, sizeof(path), "%s/two-releases.json", card);
		write_file(path, two_releases, strlen(two_releases));
		setenv("AS_TEST_JSON", path, 1);
		as_release_t many[AS_MAX];
		int many_count = 0;
		char many_error[AS_ERROR_MAX] = "";
		as_progress_t many_progress = {0};
		check(as_fetch_releases("testowner/testrepo", many, AS_MAX, &many_count, many_error, sizeof(many_error),
								&many_progress),
			  "the API fallback is fetched");
		check(many_count == 2 && strcmp(many[0].id, "dino") == 0 && many[0].size == 11 &&
				  strcmp(many[1].id, "qobuz") == 0 && strcmp(many[1].tag, "qobuz-v1.0") == 0,
			  "the newest release wins per addon, the rest are not forgotten, and the `latest` copy is skipped");
		unsetenv("AS_TEST_JSON");
	}

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

	// --- search ---
	check(as_matches("dino", "Dino", ""), "an empty query matches everything");
	check(as_matches("dino", "Dino", "din") && as_matches("dino", "Dino", "DINO"),
		  "search ignores ASCII case, in the title and in the id");
	check(as_matches("dino", "Динозаврик", "дин") && as_matches("dino", "Динозаврик", "ДИН"),
		  "search ignores Russian case too");
	check(as_matches("yandex_music", "Яндекс Музыка", "yandex") && !as_matches("dino", "Dino", "qobuz"),
		  "the id and the title are both searched");

	// --- repositories: what a paste becomes ---
	check(as_repo_valid("owner/name") && !as_repo_valid("owner") && !as_repo_valid("a/b/c") &&
			  !as_repo_valid("owner/na me") && !as_repo_valid("../x"),
		  "only owner/name is a repository");
	check(as_repos_add(&model, "https://github.com/Some/Repo.git") && model.repo_count == 2 &&
			  strcmp(model.repo[1], "Some/Repo") == 0,
		  "a pasted URL becomes owner/name");
	check(as_repos_add(&model, "Some/Repo") && model.repo_count == 2, "the same repository is one line");
	check(!as_repos_add(&model, "not a repo"), "garbage is refused");
	check(as_repos_remove(&model, 1) && model.repo_count == 1, "a repository can be removed");
	size_t repos_len = 0;
	snprintf(path, sizeof(path), "%s/repos.txt", addon_dir);
	char *repos_text = slurp(path, &repos_len);
	check(repos_text && !strstr(repos_text, "Some/Repo"), "the file no longer has it");
	free(repos_text);

	// --- the catalogue: index.json, which is why the page needs no zip ---
	char zip_hex[65], so_hex[65];
	long zip_len = file_size(zip);
	size_t zip_read = 0;
	char *zip_bytes = slurp(zip, &zip_read);
	sha256_ctx zip_sha;
	sha256_init(&zip_sha);
	if (zip_bytes) {
		sha256_update(&zip_sha, (const uint8_t *)zip_bytes, zip_read);
	}
	sha256_final_hex(&zip_sha, zip_hex);
	free(zip_bytes);

	size_t so_len = 0;
	aszip_t probe;
	check(aszip_open(&probe, zip), "the test zip opens");
	int probe_index = aszip_find(&probe, "addon.so");
	uint8_t *probe_so = probe_index >= 0 ? aszip_read(&probe, probe_index, &so_len) : NULL;
	sha256_ctx so_sha;
	sha256_init(&so_sha);
	if (probe_so) {
		sha256_update(&so_sha, probe_so, so_len);
	}
	sha256_final_hex(&so_sha, so_hex);
	free(probe_so);
	aszip_close(&probe);

	char index_text[1200];
	snprintf(index_text, sizeof(index_text),
			 "{\"schema\":1,\"repo\":\"testowner/testrepo\",\"addons\":[{\"id\":\"dino\",\"title\":\"Dino\","
			 "\"titles\":{\"English\":\"Dino!\"},\"section\":\"more\",\"version\":\"2.0\",\"tag\":\"dino-v2.0\","
			 "\"asset\":\"dino.zip\",\"size\":%ld,\"sha256\":\"%s\",\"so_size\":%zu,\"so_sha256\":\"%s\","
			 "\"url\":\"https://test.invalid/dino.zip\"}]}",
			 zip_len, zip_hex, so_len, so_hex);
	as_release_t listed[AS_MAX];
	int listed_count = as_index_parse(index_text, listed, AS_MAX, "testowner/testrepo");
	check(listed_count == 1 && listed[0].listed && strcmp(listed[0].title, "Dino!") == 0 &&
			  strcmp(listed[0].section, "more") == 0 && strcmp(listed[0].version, "2.0") == 0 &&
			  strcmp(listed[0].tag, "dino-v2.0") == 0 && listed[0].size == zip_len &&
			  listed[0].so_size == (long)so_len && strcmp(listed[0].so_sha256, so_hex) == 0 &&
			  strncmp(listed[0].digest, "sha256:", 7) == 0,
		  "the catalogue carries everything a row and an update check need");
	check(as_index_parse("{\"schema\":2,\"addons\":[]}", listed, AS_MAX, "x") == -1,
		  "another schema is not a catalogue");
	check(as_index_parse("not json", listed, AS_MAX, "x") == -1, "and neither is garbage");

	// The same through the network: the catalogue URL is answered with this
	// file, so no API call and no zip download happen at all.
	snprintf(path, sizeof(path), "%s/index.json", cache);
	write_file(path, index_text, strlen(index_text));
	setenv("AS_TEST_INDEX", path, 1);
	as_release_t fetched[AS_MAX];
	int fetched_count = 0;
	error[0] = '\0';
	check(as_fetch_releases("testowner/testrepo", fetched, AS_MAX, &fetched_count, error, sizeof(error), &progress),
		  "the catalogue is fetched");
	check(fetched_count == 1 && fetched[0].listed && strcmp(fetched[0].id, "dino") == 0 &&
			  fetched[0].so_sha256[0] != '\0',
		  "one listed entry, with its own library fingerprint");
	unsetenv("AS_TEST_INDEX");

	// The catalogue file gone -- deleted by hand, or a half-finished upload -- and
	// the release it lives in answering instead: one request, the same archives,
	// and the entry is read from the archive like the API's own (not `listed`).
	char bundle_text[420];
	snprintf(bundle_text, sizeof(bundle_text),
			 "{\"tag_name\":\"latest\",\"assets\":["
			 "{\"name\":\"index.json\",\"size\":12,\"browser_download_url\":\"https://test.invalid/index.json\"},"
			 "{\"name\":\"dino.zip\",\"size\":%ld,\"digest\":\"sha256:%s\","
			 "\"browser_download_url\":\"https://test.invalid/dino.zip\"}]}",
			 zip_len, zip_hex);
	snprintf(path, sizeof(path), "%s/bundle.json", cache);
	write_file(path, bundle_text, strlen(bundle_text));
	setenv("AS_TEST_BUNDLE", path, 1);
	error[0] = '\0';
	check(as_fetch_releases("testowner/testrepo", fetched, AS_MAX, &fetched_count, error, sizeof(error), &progress),
		  "the bundle answers when the catalogue file is missing");
	check(fetched_count == 1 && strcmp(fetched[0].id, "dino") == 0 && strcmp(fetched[0].tag, "latest") == 0 &&
			  fetched[0].size == zip_len && !fetched[0].listed,
		  "one entry out of the release's assets, and index.json is not an addon");
	unsetenv("AS_TEST_BUNDLE");

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
	snprintf(path, sizeof(path), "%s/Addons/dino/addon.so", card);
	check(access(path, R_OK) == 0, "addon.so landed in Addons/dino");
	snprintf(path, sizeof(path), "%s/Addons/dino/README.txt", card);
	check(access(path, R_OK) == 0, "README.txt landed in Addons/dino");
	check(version[0] != '\0', "the installed manifest names a version");
	check(access(addon_dir, F_OK) == 0, "the store's own folder is untouched");

	as_scan_installed(&model);
	check(model.installed_count == 1 && strcmp(model.installed[0].id, "dino") == 0,
		  "the installed scan sees it");

	// --- the fingerprinted library: read once, then answered by stat ---
	char so_hex_file[65], again[65];
	check(as_local_sha(&model, "dino", so_hex_file), "the installed library hashes");
	check(strcmp(so_hex_file, so_hex) == 0, "and it is the fingerprint the catalogue carries");
	snprintf(path, sizeof(path), "%s/local.tsv", cache);
	check(access(path, R_OK) == 0, "the fingerprint is remembered on the card");
	model.local_count = 0;
	as_local_load(&model);
	check(model.local_count == 1 && as_local_sha(&model, "dino", again) && strcmp(again, so_hex_file) == 0,
		  "the remembered fingerprint answers the next time, without hashing the card");

	// A catalogue entry alone -- no record, no manifest download -- is enough
	// to say the card is current: the library is the evidence.
	model.release_count = 0;
	model.release[model.release_count++] = listed[0];
	check(as_installed_matches(&model, "dino") && !as_update_available(&model, "dino"),
		  "a listed release matches the card by its library fingerprint");
	model.release_count = 0;

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

	// The card's addon.json is never rewritten by an update, so the version a
	// release recorded is the one the store shows -- not the stale string in
	// the file.
	char shown_version[AS_VERSION_MAX];
	as_installed_version(&model, "dino", shown_version, sizeof(shown_version));
	check(strcmp(shown_version, version) == 0, "the store shows the recorded version");
	snprintf(rec.version, sizeof(rec.version), "%s", "9.9");
	check(as_record_put(&model, &rec), "the record can be replaced");
	as_installed_version(&model, "dino", shown_version, sizeof(shown_version));
	check(strcmp(shown_version, "9.9") == 0, "the record wins over the manifest's stale string");
	snprintf(rec.version, sizeof(rec.version), "%s", version);
	check(as_record_put(&model, &rec), "and the real version goes back");

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
