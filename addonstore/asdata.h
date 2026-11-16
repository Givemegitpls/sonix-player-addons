#ifndef ADDONSTORE_ASDATA_H
#define ADDONSTORE_ASDATA_H

#include <stdbool.h>
#include <stddef.h>

// What the store knows, and where it keeps it.
//
// Three lists, from three places:
//
//   installed  read off the card every time the page opens -- Addons/<id>/
//              addon.json, the same file the player's registry reads. The card
//              is the truth about what is installed: a folder copied over USB
//              is installed, and one deleted by hand is not.
//   releases   the latest release of each repository in repos.txt, cached in
//              the store's own cache/ so the page has something to show with no
//              network at all.
//   records    one line per install the store performed: the repository, the
//              release tag, the asset's size and digest. An update is "the
//              latest release's asset is not the one that was installed", which
//              is a fact about the bytes; the addons' own version strings are
//              hand-written 0.1s and say nothing about a release.
//
// An addon installed by hand has no record and is therefore shown as installed
// with no update claim -- the store will not offer to replace a folder it did
// not write.

#define AS_MAX 32
#define AS_ID_MAX 48
#define AS_TITLE_MAX 64
#define AS_VERSION_MAX 24
#define AS_REPO_MAX 96
#define AS_TAG_MAX 40
#define AS_ASSET_MAX 64
#define AS_DIGEST_MAX 80
#define AS_URL_MAX 512
#define AS_REPOS_MAX 16
#define AS_QUERY_MAX 48

typedef struct {
	char id[AS_ID_MAX];
	char title[AS_TITLE_MAX];
	char version[AS_VERSION_MAX];
	char section[16]; // music, streaming, wireless, more
} as_installed_t;

typedef struct {
	char id[AS_ID_MAX]; // the asset name without ".zip"
	char repo[AS_REPO_MAX];
	char tag[AS_TAG_MAX];
	char asset[AS_ASSET_MAX];
	long size;
	char digest[AS_DIGEST_MAX]; // "sha256:<hex>", or "" when not published
	char url[AS_URL_MAX];

	// The version the release names -- the tag's own number when the
	// catalogue says so, else the manifest's string. Empty for an entry only
	// the GitHub API answered.
	char version[AS_VERSION_MAX];

	// Read out of the release's own manifest, once per published file, and
	// cached with the rest of the list: the tile's name and the page the tile
	// lands on. Empty until that has happened (see as_read_manifest()), or
	// already filled when the entry came from the repository's index.json.
	char title[AS_TITLE_MAX];
	char section[16]; // music, streaming, wireless, more

	// sha256 and size of the addon.so inside the release, read when the
	// manifest is (or from the index). That is what tells a hand-installed
	// copy whether it already is this release: the zip and the folder are
	// different files, so the asset's own digest says nothing about what is on
	// the card.
	char so_sha256[65];
	long so_size;

	// True when the entry came from the repository's index.json: the title,
	// section, library fingerprint and version are already known, so nothing
	// has to be downloaded to draw the row or to answer "up to date".
	bool listed;
} as_release_t;

// What was hashed out of a folder on the card, and when: the interface asks
// "is this the release?" for every row on every rebuild, and hashing a few
// hundred kilobytes per addon off this card each time is what made the page
// slow. The size and mtime are the key.
typedef struct {
	char id[AS_ID_MAX];
	long size;
	long mtime;
	char sha256[65];
} as_local_t;

typedef struct {
	char id[AS_ID_MAX];
	char repo[AS_REPO_MAX];
	char tag[AS_TAG_MAX];
	char asset[AS_ASSET_MAX];
	long size;
	char digest[AS_DIGEST_MAX];
	char version[AS_VERSION_MAX];
} as_record_t;

typedef struct {
	char addon_dir[512];
	char addons_dir[512]; // <card>/Addons
	char cache_dir[544];  // <addon>/cache

	as_installed_t installed[AS_MAX];
	int installed_count;
	as_release_t release[AS_MAX];
	int release_count;
	as_record_t record[AS_MAX];
	int record_count;
	as_local_t local[AS_MAX];
	int local_count;

	char repo[AS_REPOS_MAX][AS_REPO_MAX];
	int repo_count;
	bool repos_created; // the default file was written on this load
} as_model_t;

void as_model_init(as_model_t *m, const char *addon_dir, const char *sd_root, const char *cache_dir);

// <card>/Addons/<id>, into `out`.
void as_addon_path(const as_model_t *m, const char *id, char *out, size_t size);

// Repositories: `repos.txt` next to the addon, one "owner/name" per line. A
// missing file is written with the default repository, which is not an error.
// False only when the file exists and could not be read.
bool as_repos_load(as_model_t *m);

// Normalises what a user typed -- "owner/name", "https://github.com/owner/name",
// "git@github.com:owner/name.git" -- and, when it names a repository this file
// does not have yet, writes it. False for anything that is not owner/name.
bool as_repos_add(as_model_t *m, const char *input);

// Whether the text is exactly owner/name (one slash, no spaces, no "..").
bool as_repo_valid(const char *repo);

// Drops the line and writes the file. `index` is a position in model.repo.
bool as_repos_remove(as_model_t *m, int index);

// Scans <card>/Addons for folders carrying addon.json. Cheap enough to run
// every time the page is built (a dozen small files).
void as_scan_installed(as_model_t *m);

// The release cache: filled on load, replaced by as_cache_save() after a fetch.
void as_cache_load(as_model_t *m);
bool as_cache_save(const as_model_t *m);

// cache/local.tsv: the sha256 of each installed addon's library and the size
// and mtime it had when it was read. Hashing the card on every list rebuild is
// what made the page feel stuck; a stat that matches the record answers without
// touching the file.
void as_local_load(as_model_t *m);

// The installed library's sha256, from the cache when size and mtime are the
// ones it was read with, else read now and remembered. False when there is no
// addon.so.
bool as_local_sha(as_model_t *m, const char *id, char out[65]);

// Records. `as_record_put` replaces the line for that id and writes the file.
void as_records_load(as_model_t *m);
bool as_record_put(as_model_t *m, const as_record_t *r);
bool as_record_drop(as_model_t *m, const char *id);

const as_installed_t *as_installed_find(const as_model_t *m, const char *id);
const as_release_t *as_release_find(const as_model_t *m, const char *id);
const as_record_t *as_record_find(const as_model_t *m, const char *id);

// The version to show for what is on the card. `installed.tsv` holds the
// release's own version for an addon the store installed, and stays right even
// though an update never rewrites `addon.json`; a folder copied by hand has no
// record and falls back to its manifest. Empty when nothing is installed.
void as_installed_version(const as_model_t *m, const char *id, char *out, size_t size);

// Whether `query` is in the addon's title or id, ignoring case in ASCII and in
// Russian: an empty query matches everything. Out of the UI so the host test
// can check it -- the keyboard cannot be driven without a device.
bool as_matches(const char *id, const char *title, const char *query);

// A repository's index.json -- the catalogue this repository publishes, one
// entry per addon, with the title, section, library fingerprint and version
// already in it, so the list needs no zip. Fills `out` (repo set, `listed`
// true) and returns the count, or -1 when `text` is not one (not JSON, another
// schema, no addons). Names come from the asset: an index cannot name a folder
// the asset would not.
int as_index_parse(const char *text, as_release_t *out, int max, const char *repo);

// Whether what is on the card is the published release. For an addon the store
// installed, the asset's size and digest against the record; for one copied by
// hand, the sha256 of Addons/<id>/addon.so against the release's own library.
// False when the release's library has not been read yet or nothing is
// installed, which is the honest answer -- "cannot prove it is current".
// Takes the model by pointer because a hand-installed library may have to be
// hashed and remembered (see as_local_sha()).
bool as_installed_matches(as_model_t *m, const char *id);

// True when something is installed and it is not that release. This is what
// "Update" means, whether the folder was written by the store or copied by
// hand.
bool as_update_available(as_model_t *m, const char *id);

// The id a release asset names: "dino.zip" -> "dino". Empty when the name is
// not a .zip or has no usable stem.
void as_id_from_asset(const char *asset, char *out, size_t size);

// What a manifest says, out of its text: the tile's name in the interface's
// language (the plain title when the language has no translation), the section
// and the version. Fields the text does not carry are left empty. The section
// is the player's own word -- "music", "streaming", "wireless", "more" -- and
// anything else is left empty, which the page shows as "other".
void as_manifest_fields(const char *text, char *title, size_t title_size, char *section, size_t section_size,
						char *version, size_t version_size);

#endif /* ADDONSTORE_ASDATA_H */
