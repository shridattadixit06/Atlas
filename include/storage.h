#ifndef ATLAS_STORAGE_H
#define ATLAS_STORAGE_H

#include <stddef.h>
#include <sys/types.h>

#define ATLAS_MAX_FILE_SIZE (1024 * 1024)

int storage_init(void);

int storage_validate_name(const char *name);

int storage_read(
    const char *name,
    unsigned char **data,
    size_t *length
);

int storage_write(
    const char *name,
    const unsigned char *data,
    size_t length
);

int storage_list(char ***names, size_t *count);

void storage_free_list(char **names, size_t count);

#endif