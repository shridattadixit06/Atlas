#include "metadata.h"

#include <errno.h>
#include <string.h>

#define MAX_METADATA_ENTRIES 1024

static AtlasMetadata entries[MAX_METADATA_ENTRIES];
static size_t entry_count = 0;

int metadata_record(const char *name, uint64_t size,
                    AtlasMetadata *result)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) > ATLAS_NAME_MAX) {
        errno = EINVAL;
        return -1;
    }

    size_t index = 0;

    while (index < entry_count &&
           strcmp(entries[index].name, name) != 0) {
        index++;
    }

    if (index == entry_count) {
        if (entry_count == MAX_METADATA_ENTRIES) {
            errno = ENOSPC;
            return -1;
        }

        strcpy(entries[index].name, name);
        entries[index].version = 1;
        entries[index].size = size;
        entry_count++;
    } else {
        if (entries[index].version == UINT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }

        entries[index].version++;
        entries[index].size = size;
    }

    if (result != NULL)
        *result = entries[index];

    return 0;
}

int metadata_lookup(const char *name, AtlasMetadata *result)
{
    if (name == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            *result = entries[i];
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}
