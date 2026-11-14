#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { const char* data; size_t size; } folio_xml_span_t;
typedef struct { folio_xml_span_t name, value; } folio_xml_attr_t;
typedef enum { FOLIO_XML_START, FOLIO_XML_END, FOLIO_XML_TEXT } folio_xml_kind_t;
typedef struct {
    folio_xml_kind_t kind;
    folio_xml_span_t name;
    folio_xml_span_t text;
    folio_xml_attr_t attrs[32];
    size_t attr_count;
    unsigned depth;
    bool literal;
} folio_xml_token_t;

typedef bool (*folio_xml_callback_t)(void* context, const folio_xml_token_t* token);
typedef bool (*folio_xml_emit_t)(void* context, uint32_t codepoint);
bool folio_xml_parse(const char* data, size_t size, folio_xml_callback_t callback, void* context);
bool folio_xml_name(folio_xml_span_t name, const char* local_name);
bool folio_xml_attr(const folio_xml_token_t* token, const char* name, char* out, size_t cap);
bool folio_xml_emit(folio_xml_span_t text, bool literal, folio_xml_emit_t emit, void* context);
/* Appends decoded text, truncating only on UTF-8 codepoint boundaries. */
bool folio_xml_append(folio_xml_span_t text, bool literal, char* out, size_t cap);
