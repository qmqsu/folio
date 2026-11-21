#pragma once
#include <stdbool.h>
#include <stdio.h>

void folio_pdf_metadata(const char* path, char* title, size_t title_cap, char* author, size_t author_cap);
bool folio_pdf_extract(const char *path, FILE *output);

typedef struct {
    unsigned count, width, height;
    unsigned char *pixels;
} folio_pdf_scan_t;

bool folio_pdf_scan_page(const char *path, unsigned page, unsigned max_w, unsigned max_h, folio_pdf_scan_t *result);
