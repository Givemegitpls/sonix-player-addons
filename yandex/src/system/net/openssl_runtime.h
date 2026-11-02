#ifndef SONIX_OPENSSL_RUNTIME_H
#define SONIX_OPENSSL_RUNTIME_H
#include <stdbool.h>
// Process-owned matched handles. Both consumers use the same selection;
// callers never close them. SONIX_OPENSSL_DIR is an isolated-test override.
bool openssl_runtime_get(void **ssl, void **crypto);

// The one caller that does close them, and only from an addon: stop() (addon.h)
// has to leave the process as it found it, so the handles this library opened
// are dropped and the next get() loads a fresh pair. The player's own handles on
// the same files (tls.c) keep the mapping alive.
void openssl_runtime_close(void);
#endif
