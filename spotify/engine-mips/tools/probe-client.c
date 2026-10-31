/*
 * probe-client -- the smallest thing that can talk to the engine's unix socket
 * on the device.
 *
 * The stock firmware's busybox has no `nc` applet and no socat, so the usual
 * `printf '{"op":"probe"}' | nc -U /tmp/x.sock` is not available there. This is
 * that pipeline: connect, write the line given on the command line, print what
 * comes back, exit. It is built with the same mipsel-linux-musl cross compiler
 * the engine is linked with, statically, and lives next to the engine in
 * tools/ so that the probe recipe in README.md is one command.
 *
 *   mipsel-linux-musl-gcc -Os -static -o probe-client probe-client.c
 *   ./probe-client /tmp/spotify-engine.sock '{"op":"probe"}'
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: probe-client <socket> <request-json>\n");
        return 2;
    }

    const char *path = argv[1];
    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof address.sun_path) {
        fprintf(stderr, "probe-client: socket path too long\n");
        return 2;
    }
    strcpy(address.sun_path, path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("probe-client: socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        perror("probe-client: connect");
        return 1;
    }

    size_t length = strlen(argv[2]);
    if (write(fd, argv[2], length) != (ssize_t)length || write(fd, "\n", 1) != 1) {
        perror("probe-client: write");
        return 1;
    }

    /* One response line, then EOF: the engine closes the connection. */
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof buffer)) > 0) {
        if (fwrite(buffer, 1, (size_t)count, stdout) != (size_t)count) {
            perror("probe-client: write stdout");
            return 1;
        }
    }
    if (count < 0) {
        perror("probe-client: read");
        return 1;
    }
    close(fd);
    return 0;
}
