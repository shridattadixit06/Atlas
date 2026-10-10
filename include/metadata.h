#ifndef ATLAS_METADATA_H
#define ATLAS_METADATA_H

#include <stdint.h>
#include <stddef.h>

#define ATLAS_NAME_MAX 200

typedef struct {
    char name[ATLAS_NAME_MAX + 1];
    uint64_t version;
    uint64_t size;
} AtlasMetadata;
int metadata_record(const char *name, uint64_t size,
                    AtlasMetadata *result);

int metadata_lookup(const char *name, AtlasMetadata *result);
#endif
