#define _GNU_SOURCE
#include "sha256.h"
#include "ucraft.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* A local source: file:///path, an absolute path, or ./relative. Everything else is downloaded. */
static const char *local_path(const char *url)
{
    if (!strncmp(url, "file://", 7)) return url + 7;
    if (url[0] == '/' || !strncmp(url, "./", 2) || !strncmp(url, "../", 3)) return url;
    return NULL;
}

/* Gets `url` into `dest` (written as dest.part and renamed when complete). With a sha256, the file
 * is verified and thrown away if it does not match. Downloads use curl, or wget (BusyBox) if there
 * is no curl; local files are simply copied. */
void fetch_file(const char *url, const char *dest, const char *sha256)
{
    char *dir = xstrdup(dest);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = 0; mkdir_p(dir, 0755); }
    free(dir);

    char *tmp = xasprintf("%s.part", dest);
    const char *lp = local_path(url);
    if (lp) {
        if (copy_file(lp, tmp) != 0) die("cannot read %s: %s", lp, strerror(errno));
    } else if (find_in_path("curl")) {
        char *argv[] = {"curl", "-fL", "--retry", "3", "-sS", "-o", tmp, (char *)url, NULL};
        if (run(argv, NULL) != 0) { unlink(tmp); die("download failed: %s", url); }
    } else if (find_in_path("wget")) {
        /* BusyBox wget does not check TLS certificates: only the sha256 (and, later, a signature)
         * tells a good download from a bad one. */
        char *argv[] = {"wget", "-q", "-O", tmp, (char *)url, NULL};
        if (run(argv, NULL) != 0) { unlink(tmp); die("download failed: %s", url); }
    } else {
        die("'curl' or 'wget' is needed to download %s", url);
    }

    if (sha256) {
        char hex[65];
        if (sha256_file(tmp, hex) != 0) die("cannot read %s", tmp);
        if (strcasecmp(hex, sha256)) {
            unlink(tmp);
            die("wrong sha256 for %s\n  expected: %s\n  got:      %s", url, sha256, hex);
        }
    }
    if (rename(tmp, dest) != 0) die("rename %s: %s", dest, strerror(errno));
    free(tmp);
}
