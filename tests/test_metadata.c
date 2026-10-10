#include "metadata.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    AtlasMetadata meta;

    /* A new file should start at version 1. */
    assert(metadata_record("notes.txt", 100, &meta) == 0);
    assert(meta.version == 1);
    assert(meta.size == 100);

    /* Updating the same file should increment its version. */
    assert(metadata_record("notes.txt", 250, &meta) == 0);
    assert(meta.version == 2);
    assert(meta.size == 250);

    /* Lookup should return the latest metadata. */
    assert(metadata_lookup("notes.txt", &meta) == 0);
    assert(meta.version == 2);
    assert(meta.size == 250);

    /* A different file should have its own version history. */
    assert(metadata_record("image.bin", 512, &meta) == 0);
    assert(meta.version == 1);
    assert(meta.size == 512);

    /* Looking up an unknown file should fail. */
    errno = 0;
    assert(metadata_lookup("missing.txt", &meta) == -1);
    assert(errno == ENOENT);

    puts("All metadata tests passed.");
    return 0;
}
