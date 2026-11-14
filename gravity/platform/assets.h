#ifndef GRAVITY_ASSETS_H
#define GRAVITY_ASSETS_H

#ifdef __cplusplus
extern "C" {
#endif

// Where the game's pictures are: <addon>/assets, the directory packed beside
// addon.so. Set by gravity_run() before the game thread starts; the game asks for
// the names upstream embedded ("fender.png", "sprites.png"), so the files keep
// them.
void image_set_assets_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* GRAVITY_ASSETS_H */
