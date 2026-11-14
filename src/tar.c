#include "tar.h"
#include "util.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define BLK 512

static unsigned long long parse_oct(const char *p, size_t n, const char *what)
{
    if ((unsigned char)p[0] & 0x80) die("tar: field %s in base 256 is not supported", what);
    unsigned long long v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned)(p[i] - '0');
    return v;
}

static void put_oct(char *p, size_t n, unsigned long long v)
{
    /* n-1 digits + NUL */
    for (size_t i = n - 1; i-- > 0;) {
        p[i] = (char)('0' + (v & 7));
        v >>= 3;
    }
    p[n - 1] = 0;
}

static unsigned checksum(const unsigned char *b)
{
    unsigned s = 0;
    for (int i = 0; i < BLK; i++) s += (i >= 148 && i < 156) ? ' ' : b[i];
    return s;
}

int tar_read_header(FILE *f, struct tar_hdr *h)
{
    unsigned char b[BLK];
    size_t n = fread(b, 1, BLK, f);
    if (n == 0 && feof(f)) return 0;
    if (n != BLK) die("tar: truncated archive");
    int zero = 1;
    for (int i = 0; i < BLK; i++)
        if (b[i]) { zero = 0; break; }
    if (zero) return 0;
    if (checksum(b) != parse_oct((char *)b + 148, 8, "chksum")) die("tar: bad checksum");
    if (memcmp(b + 257, "ustar", 5)) die("tar: not ustar format");

    memset(h, 0, sizeof *h);
    char name[101], prefix[156];
    memcpy(name, b, 100); name[100] = 0;
    memcpy(prefix, b + 345, 155); prefix[155] = 0;
    if (prefix[0]) snprintf(h->path, sizeof h->path, "%s/%s", prefix, name);
    else snprintf(h->path, sizeof h->path, "%s", name);
    size_t l = strlen(h->path);
    while (l > 1 && h->path[l - 1] == '/') h->path[--l] = 0;
    memcpy(h->link, b + 157, 100);
    h->link[100] = 0;
    h->mode = (unsigned)parse_oct((char *)b + 100, 8, "mode");
    h->size = parse_oct((char *)b + 124, 12, "size");
    h->mtime = (long)parse_oct((char *)b + 136, 12, "mtime");
    h->type = (char)b[156];
    if (h->type == 0) h->type = '0';
    if (h->type != '0' && h->type != '2' && h->type != '5')
        die("tar: unsupported entry type '%c' (%s)", h->type, h->path);
    if (h->type != '0') h->size = 0;
    return 1;
}

void tar_skip_data(FILE *f, unsigned long long size)
{
    unsigned long long total = (size + BLK - 1) / BLK * BLK;
    if (!total) return;
    if (fseeko(f, (off_t)total, SEEK_CUR) != 0) {
        char buf[BLK];
        while (total) {
            if (fread(buf, 1, BLK, f) != BLK) die("tar: truncated archive");
            total -= BLK;
        }
    }
}

void tar_finish_data(FILE *f, unsigned long long size)
{
    unsigned long long pad = (BLK - size % BLK) % BLK;
    char buf[BLK];
    if (pad && fread(buf, 1, pad, f) != pad) die("tar: truncated archive");
}

void tar_write_header(FILE *f, const struct tar_hdr *h)
{
    unsigned char b[BLK];
    memset(b, 0, BLK);
    char path[300];
    snprintf(path, sizeof path, "%s%s", h->path, h->type == '5' ? "/" : "");
    size_t len = strlen(path);
    if (len <= 100) {
        memcpy(b, path, len);
    } else {
        /* split into prefix (<=155) and name (<=100) at a '/' */
        size_t cut = 0;
        for (size_t i = 0; i < len; i++)
            if (path[i] == '/' && i <= 155 && len - i - 1 <= 100) cut = i;
        if (!cut) die("tar: path too long: %s", h->path);
        memcpy(b + 345, path, cut);
        memcpy(b, path + cut + 1, len - cut - 1);
    }
    if (strlen(h->link) > 100) die("tar: link target too long: %s", h->path);
    memcpy(b + 157, h->link, strlen(h->link));
    put_oct((char *)b + 100, 8, h->mode & 07777);
    put_oct((char *)b + 108, 8, 0);   /* uid 0: no fakeroot needed */
    put_oct((char *)b + 116, 8, 0);   /* gid 0 */
    put_oct((char *)b + 124, 12, h->type == '0' ? h->size : 0);
    put_oct((char *)b + 136, 12, h->mtime > 0 ? (unsigned long long)h->mtime : 0);
    b[156] = (unsigned char)h->type;
    memcpy(b + 257, "ustar", 5);
    b[263] = '0'; b[264] = '0';
    memcpy(b + 265, "root", 4);
    memcpy(b + 297, "root", 4);
    unsigned sum = checksum(b);
    snprintf((char *)b + 148, 8, "%06o", sum);
    b[155] = ' ';
    if (fwrite(b, 1, BLK, f) != BLK) die("tar write: %s", strerror(errno));
}

void tar_write_padding(FILE *f, unsigned long long size)
{
    static const char zero[BLK];
    unsigned long long pad = (BLK - size % BLK) % BLK;
    if (pad && fwrite(zero, 1, pad, f) != pad) die("tar write: %s", strerror(errno));
}

void tar_write_end(FILE *f)
{
    static const char zero[BLK * 2];
    if (fwrite(zero, 1, sizeof zero, f) != sizeof zero) die("tar write: %s", strerror(errno));
}
