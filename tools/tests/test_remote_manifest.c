#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "folio_remote_manifest.h"

static unsigned char* load(const char* root, const char* name, size_t* size) {
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.json", root, name);
    FILE* file = fopen(path, "rb"); assert(file && fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); assert(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    unsigned char* bytes = malloc((size_t)length); assert(bytes);
    assert(fread(bytes, 1, (size_t)length, file) == (size_t)length && fclose(file) == 0);
    *size = (size_t)length; return bytes;
}
void test_remote_manifests(const char* root) {
    char path[1024], name[128], error[192]; int expected; unsigned tests = 0;
    snprintf(path, sizeof(path), "%s/cases.tsv", root); FILE* cases = fopen(path, "r"); assert(cases);
    while (fscanf(cases, "%d\t%127s", &expected, name) == 2) {
        size_t size; unsigned char* bytes = load(root, name, &size); folio_remote_manifest_t result;
        bool good = folio_remote_manifest_verify(bytes, size, "preview", 8032000, &result, error, sizeof(error));
        if (good != (bool)expected) { fprintf(stderr, "Manifest %s: got %d expected %d (%s)\n", name, good, expected, error); abort(); }
        assert(good ? !*error && result.sequence > 8032000 : *error && !result.sequence && !result.url[0]);
        ++tests; free(bytes);
    }
    assert(fclose(cases) == 0);
    size_t size; unsigned char* bytes = load(root, "valid", &size); folio_remote_manifest_t result;
    assert(!folio_remote_manifest_verify(bytes, size, "stable", 8032000, &result, error, sizeof(error)));
    assert(!folio_remote_manifest_verify(bytes, size, "preview", 8032001, &result, error, sizeof(error)));
    assert(!folio_remote_manifest_verify(bytes, size, "preview", UINT32_MAX, &result, error, sizeof(error)));
    assert(!folio_remote_manifest_verify(NULL, 0, "preview", 0, &result, NULL, 0));
    assert(!folio_remote_manifest_verify(bytes, size, "preview", 0, NULL, NULL, 0));
    // Every truncation of an otherwise authentic envelope must fail closed.
    for (size_t n = 0; n < size; ++n) assert(!folio_remote_manifest_verify(bytes, n, "preview", 0, &result, NULL, 0));
    free(bytes);
    printf("Remote manifest: %u real-crypto fixtures + truncation sweep passed (strict parser/raw-P256 signature/identity/path/sequence)\n", tests);
}
