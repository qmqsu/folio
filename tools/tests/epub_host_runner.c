#include "folio_epub.h"
#include "folio_zip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long cancel_after = -1, cancel_checks;
bool folio_io_cancelled(void) { return cancel_after >= 0 && cancel_checks++ >= cancel_after; }

int main(int argc, char** argv) {
    const char* cancellation = getenv("FOLIO_CANCEL_AFTER");
    if (cancellation) cancel_after = strtol(cancellation, NULL, 10);
    if (argc == 3 && strcmp(argv[1], "--metadata") == 0) {
        char title[128] = "old title", author[128] = "old author", error[256] = "old error";
        if (!folio_epub_metadata(argv[2], title, sizeof(title), author, sizeof(author), error, sizeof(error))) {
            if (title[0] || author[0] || !error[0]) return 3;
            fprintf(stderr, "%s\n", error); return 1;
        }
        if (error[0]) return 3;
        printf("%s\n%s\n", title, author); return 0;
    }
    if (argc == 3 && (strcmp(argv[1], "--zip-full") == 0 || strcmp(argv[1], "--zip-directory") == 0)) {
        folio_zip_t zip;
        char error[256] = {0};
        bool directory = strcmp(argv[1], "--zip-directory") == 0;
        bool okay = directory ? folio_zip_open_directory(argv[2], &zip, error, sizeof(error)) :
                                folio_zip_open(argv[2], &zip, error, sizeof(error));
        if (!okay) { fprintf(stderr, "%s\n", error); return 1; }
        printf("%08lx %lu\n", (unsigned long)zip.source_crc, (unsigned long)zip.source_size);
        folio_zip_close(&zip);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "--cover") == 0) {
        uint8_t* data = (void*)1;
        size_t size = 1;
        char error[256] = "old error";
        if (!folio_epub_cover(argv[2], &data, &size, error, sizeof(error))) {
            if (data != NULL || size != 0) return 3;
            if (error[0]) { fprintf(stderr, "%s\n", error); return 1; }
            return 4;
        }
        if (data == NULL || size == 0 || error[0]) return 3;
        FILE* output = fopen(argv[3], "wb");
        if (output == NULL) { free(data); return 3; }
        bool okay = fwrite(data, 1, size, output) == size;
        if (fclose(output) != 0) okay = false;
        free(data);
        return okay ? 0 : 3;
    }
    if (argc != 2) return 2;
    folio_epub_t book;
    char error[256] = {0};
    if (!folio_epub_prepare(argv[1], &book, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    printf("TITLE\t%s\nCACHE\t%s\n", book.title, book.text_path);
    for (size_t i = 0; i < book.chapter_count; ++i) {
        printf("TOC\t%u\t%s\n", (unsigned)book.chapters[i].offset, book.chapters[i].title);
    }
    folio_epub_free(&book);
    return 0;
}
