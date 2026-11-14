#ifndef UCRAFT_TAR_H
#define UCRAFT_TAR_H
#include <stdio.h>

/* Minimal ustar: files, directories and symbolic links. */
struct tar_hdr {
    char path[258];   /* without trailing '/' */
    char link[101];
    char type;        /* '0' file, '2' link, '5' directory */
    unsigned mode;
    unsigned long long size;
    long mtime;
};

/* 1 if a header was read, 0 at end of archive. Exits with an error if corrupt. */
int tar_read_header(FILE *f, struct tar_hdr *h);
/* After reading the h->size data bytes by hand, skips the padding. */
void tar_finish_data(FILE *f, unsigned long long size);
void tar_skip_data(FILE *f, unsigned long long size);

void tar_write_header(FILE *f, const struct tar_hdr *h);
void tar_write_padding(FILE *f, unsigned long long size);
void tar_write_end(FILE *f);

#endif
