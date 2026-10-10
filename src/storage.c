#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORAGE_DIR "storage"

/* Check whether a filename is safe. */
int storage_validate_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;

    size_t len = strlen(name);

    if (len > 200 ||
        strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;

    for (size_t i = 0; i < len; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return 0;
    }

    return 1;
}

/* Ensure the storage directory exists. */
int storage_init(void)
{
    if (mkdir(STORAGE_DIR, 0755) < 0 && errno != EEXIST)
        return -1;

    struct stat st;

    if (stat(STORAGE_DIR, &st) < 0)
        return -1;

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

/* Read a complete file into memory. */
int storage_read(
    const char *name,
    unsigned char **data,
    size_t *length)
{
    if (!storage_validate_name(name) ||
        data == NULL || length == NULL) {
        errno = EINVAL;
        return -1;
    }

    *data = NULL;
    *length = 0;

    char path[256];

    int n = snprintf(path, sizeof(path),
                     "%s/%s", STORAGE_DIR, name);

    if (n < 0 || (size_t)n >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;

    struct stat st;

    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }

    if (!S_ISREG(st.st_mode) ||
        st.st_size < 0 ||
        (unsigned long long)st.st_size >
            ATLAS_MAX_FILE_SIZE) {
        close(fd);
        errno = EFBIG;
        return -1;
    }

    size_t size = (size_t)st.st_size;

    unsigned char *buffer = malloc(size ? size : 1);

    if (buffer == NULL) {
        close(fd);
        return -1;
    }

    size_t total = 0;

    while (total < size) {
        ssize_t r = read(fd, buffer + total, size - total);

        if (r < 0 && errno == EINTR)
            continue;

        if (r <= 0) {
            free(buffer);
            close(fd);
            errno = EIO;
            return -1;
        }

        total += (size_t)r;
    }

    close(fd);

    *data = buffer;
    *length = size;

    return 0;
}

/* Write data atomically using a temporary file. */
int storage_write(
    const char *name,
    const unsigned char *data,
    size_t length)
{
    if (!storage_validate_name(name) ||
        (data == NULL && length != 0) ||
        length > ATLAS_MAX_FILE_SIZE) {
        errno = EINVAL;
        return -1;
    }

    char final_path[256];
    char temp_path[256];

    int n = snprintf(final_path, sizeof(final_path),
                     "%s/%s", STORAGE_DIR, name);

    if (n < 0 || (size_t)n >= sizeof(final_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    n = snprintf(temp_path, sizeof(temp_path),
                 "%s/.atlas-tmp-XXXXXX", STORAGE_DIR);

    if (n < 0 || (size_t)n >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = mkstemp(temp_path);

    if (fd < 0)
        return -1;

    size_t total = 0;
    int failed = 0;

    while (total < length) {
        ssize_t w = write(fd, data + total, length - total);

        if (w < 0 && errno == EINTR)
            continue;

        if (w <= 0) {
            failed = 1;
            break;
        }

        total += (size_t)w;
    }

    if (!failed && fsync(fd) < 0)
        failed = 1;

    int saved_errno = errno;

    if (close(fd) < 0 && !failed) {
        failed = 1;
        saved_errno = errno;
    }

    if (failed) {
        unlink(temp_path);
        errno = saved_errno;
        return -1;
    }

    if (rename(temp_path, final_path) < 0) {
        saved_errno = errno;
        unlink(temp_path);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

/* Return the names of regular files in storage/. */
int storage_list(char ***names, size_t *count)
{
    if (names == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }

    *names = NULL;
    *count = 0;

    DIR *dir = opendir(STORAGE_DIR);

    if (dir == NULL)
        return -1;

    char **items = NULL;
    size_t used = 0;
    struct dirent *entry;

    for (;;) {
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0)
                goto fail;
            break;
        }

        const char *name = entry->d_name;

        if (name[0] == '.' ||
            !storage_validate_name(name))
            continue;

        struct stat st;

        if (fstatat(dirfd(dir), name, &st,
                    AT_SYMLINK_NOFOLLOW) < 0)
            continue;

        if (!S_ISREG(st.st_mode))
            continue;

        char *copy = strdup(name);

        if (copy == NULL)
            goto fail;

        char **next = realloc(
            items, (used + 1) * sizeof(*items));

        if (next == NULL) {
            free(copy);
            goto fail;
        }

        items = next;
        items[used++] = copy;
    }

    if (closedir(dir) < 0) {
        storage_free_list(items, used);
        return -1;
    }

    *names = items;
    *count = used;

    return 0;

fail:
    {
        int saved_errno = errno ? errno : ENOMEM;
        closedir(dir);
        storage_free_list(items, used);
        errno = saved_errno;
        return -1;
    }
}

/* Release memory allocated by storage_list(). */
void storage_free_list(char **names, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free(names[i]);

    free(names);
}