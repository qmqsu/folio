#pragma once
#include <stdbool.h>
#include <stdio.h>

bool folio_pdf_extract(const char *path, FILE *output);

typedef struct {
    unsigned count, width, height;
    unsigned char *pixels;
} folio_pdf_scan_t;

bool folio_pdf_scan_page(const char *path, unsigned page, unsigned max_w, unsigned max_h, folio_pdf_scan_t *result);
