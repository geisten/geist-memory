#ifndef GM_PLATFORM_H
#define GM_PLATFORM_H
#include "geist_memory.h"
#include <sys/types.h>
/* POSIX boundary: regular local files, 64-bit offsets, exclusive flock. */
[[nodiscard]] enum gm_status gm_file_read(const char *path, size_t limit, char **out, size_t *len);
[[nodiscard]] enum gm_status gm_file_digest(const char *path, uint8_t hash[32]);
bool gm_read_at(int fd, void *out, size_t n, off_t offset);
bool gm_write_all(int fd, const void *data, size_t n);
bool gm_sync(int fd);
bool gm_move(int dirfd, const char *from, const char *to);
bool gm_remove(int dirfd, const char *name);
bool gm_truncate(int fd, off_t size);
#endif
