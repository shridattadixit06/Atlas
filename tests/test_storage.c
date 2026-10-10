#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "atlas_test_roundtrip.bin"

static int contains_name(char **names, size_t count,
                         const char *target)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], target) == 0)
            return 1;
    }

    return 0;
}

int main(void)
{
    const unsigned char original[] = {
        'A', 't', 'l', 'a', 's', '\0', 'O', 'S', '\n'
    };

    size_t original_size = sizeof(original);

    unsigned char *loaded = NULL;
    size_t loaded_size = 0;

    char **names = NULL;
    size_t count = 0;

    unlink("storage/" TEST_FILE);

    /* Test 1: Initialize storage. */
    if (storage_init() < 0) {
        perror("storage_init");
        return 1;
    }

    puts("[PASS] storage initialization");

    /* Test 2: Write a file. */
    if (storage_write(TEST_FILE, original,
                      original_size) < 0) {
        perror("storage_write");
        return 1;
    }

    puts("[PASS] file write");

    /* Test 3: Read the file and compare exact bytes. */
    if (storage_read(TEST_FILE, &loaded,
                     &loaded_size) < 0) {
        perror("storage_read");
        unlink("storage/" TEST_FILE);
        return 1;
    }

    if (loaded_size != original_size ||
        memcmp(loaded, original, original_size) != 0) {
        fprintf(stderr, "[FAIL] file contents differ\n");
        free(loaded);
        unlink("storage/" TEST_FILE);
        return 1;
    }

    free(loaded);
    puts("[PASS] exact binary read-back");

    /* Test 4: List files. */
    if (storage_list(&names, &count) < 0) {
        perror("storage_list");
        unlink("storage/" TEST_FILE);
        return 1;
    }

    int found = contains_name(names, count, TEST_FILE);
    storage_free_list(names, count);

    if (!found) {
        fprintf(stderr, "[FAIL] file missing from listing\n");
        unlink("storage/" TEST_FILE);
        return 1;
    }

    puts("[PASS] file listing");

    /* Test 5: Reject path traversal. */
    if (storage_validate_name("../outside") != 0) {
        fprintf(stderr, "[FAIL] unsafe filename accepted\n");
        unlink("storage/" TEST_FILE);
        return 1;
    }

    puts("[PASS] invalid filename rejection");

    /* Test 6: Reject an oversized write. */
    unsigned char byte = 0;

    if (storage_write("oversized.bin", &byte,
                      (size_t)ATLAS_MAX_FILE_SIZE + 1) == 0) {
        fprintf(stderr, "[FAIL] oversized write accepted\n");
        unlink("storage/" TEST_FILE);
        unlink("storage/oversized.bin");
        return 1;
    }

    puts("[PASS] oversized write rejection");

    /* Remove the test artifact. */
    if (unlink("storage/" TEST_FILE) < 0) {
        perror("unlink test file");
        return 1;
    }

    puts("\nAll storage tests passed.");
    return 0;
}
