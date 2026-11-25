#include "folio_epub.h"
#include "folio_xml.h"
#include "folio_zip.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef FOLIO_SD_ROOT
#define FOLIO_SD_ROOT "/sdcard"
#endif
#define EPUB_CACHE_ROOT FOLIO_SD_ROOT "/.folio/cache"
#define EPUB_ITEM_LIMIT 8192
#define EPUB_INDEX_BLOCK 32
#define EPUB_TOC_LIMIT 2048
#define EPUB_TEXT_LIMIT (32u * 1024u * 1024u)
#define EPUB_ID_CAP 96
#define EPUB_FRAGMENT_CAP 128
#define EPUB_COVER_LIMIT (2u * 1024u * 1024u)

typedef enum { COVER_OTHER, COVER_PNG, COVER_JPEG, COVER_DOCUMENT } cover_type_t;

typedef struct {
    const char* id;
    const char* path;
    uint32_t offset;
    uint16_t next_id;
    bool in_spine;
    bool html;
    bool nav;
    bool ncx;
    cover_type_t cover_type;
    bool cover_image;
} manifest_item_t;

typedef struct {
    const char* path;
    const char* fragment;
    char title[FOLIO_EPUB_TITLE_CAP];
    uint32_t offset;
    bool anchored;
} navigation_item_t;

typedef struct string_block {
    struct string_block* next;
    size_t used;
    char bytes[4096];
} string_block_t;

typedef struct {
    folio_zip_t zip;
    string_block_t* strings;
    manifest_item_t* items[EPUB_ITEM_LIMIT / EPUB_INDEX_BLOCK];
    uint16_t id_heads[512]; // One-based indexes, zero terminates the chain.
    uint16_t* spine;
    navigation_item_t* nav[EPUB_TOC_LIMIT / EPUB_INDEX_BLOCK];
    size_t item_count, spine_count, nav_count;
    char package_path[FOLIO_ZIP_NAME_CAP];
    char nav_path[FOLIO_ZIP_NAME_CAP];
    char ncx_id[EPUB_ID_CAP];
    char title[FOLIO_EPUB_TITLE_CAP];
    char message[128];
    FILE* output;
    uint32_t written, text_crc;
    uint8_t write_buffer[1024];
    size_t write_used;
    bool pending_space, line_start, last_newline;
    int manifest_depth, spine_depth, metadata_depth, title_depth, layout_depth;
    char layout[32];
} epub_context_t;

static bool failure(epub_context_t* context, const char* message) {
    snprintf(context->message, sizeof(context->message), "%s", message);
    return false;
}
static bool make_directory(const char* path) {
    struct stat info;
    return mkdir(path, 0777) == 0 || (stat(path, &info) == 0 && S_ISDIR(info.st_mode));
}
// Small stable blocks avoid growing/copying the entire manifest or TOC.
static const char* save_string(epub_context_t* book, const char* value) {
    if (!*value) return "";
    size_t bytes = strlen(value) + 1;
    string_block_t* block = book->strings;
    if (!block || bytes > sizeof(block->bytes) - block->used) {
        block = folio_epub_alloc(sizeof(*block));
        if (!block) { failure(book, "Not enough memory for EPUB strings"); return NULL; }
        block->next = book->strings; block->used = 0; book->strings = block;
    }
    char* copy = block->bytes + block->used;
    memcpy(copy, value, bytes); block->used += bytes;
    return copy;
}
static manifest_item_t* manifest_at(epub_context_t* book, size_t i) {
    return &book->items[i / EPUB_INDEX_BLOCK][i % EPUB_INDEX_BLOCK];
}
static unsigned id_bucket(const char* id) {
    uint32_t hash = 2166136261u;
    while (*id) hash = (hash ^ (uint8_t)*id++) * 16777619u;
    return hash % 512;
}
static size_t manifest_find(epub_context_t* book, const char* id) {
    for (uint16_t n = book->id_heads[id_bucket(id)]; n; n = manifest_at(book, n-1)->next_id)
        if (!strcmp(manifest_at(book, n-1)->id, id)) return n-1;
    return SIZE_MAX;
}
static navigation_item_t* nav_at(epub_context_t* book, size_t i) {
    return &book->nav[i / EPUB_INDEX_BLOCK][i % EPUB_INDEX_BLOCK];
}
static bool reserve_items(epub_context_t* book) {
    size_t i = book->item_count;
    if (i == EPUB_ITEM_LIMIT) return failure(book, "EPUB manifest exceeds 8192 items");
    if (i % EPUB_INDEX_BLOCK == 0) {
        book->items[i / EPUB_INDEX_BLOCK] = folio_epub_alloc(EPUB_INDEX_BLOCK * sizeof(manifest_item_t));
        if (!book->items[i / EPUB_INDEX_BLOCK]) return failure(book, "Not enough memory for EPUB manifest");
    }
    memset(manifest_at(book, i), 0, sizeof(manifest_item_t));
    return true;
}
static void free_index(epub_context_t* book) {
    for (size_t i = 0; i < EPUB_ITEM_LIMIT / EPUB_INDEX_BLOCK; ++i) free(book->items[i]);
    for (size_t i = 0; i < EPUB_TOC_LIMIT / EPUB_INDEX_BLOCK; ++i) free(book->nav[i]);
    while (book->strings) {
        string_block_t* next = book->strings->next;
        free(book->strings); book->strings = next;
    }
}
static bool token_word(const char* list, const char* word) {
    size_t size = strlen(word);
    while (*list) {
        while (*list == ' ' || *list == '\r' || *list == '\n' || *list == '\t') ++list;
        const char* start = list;
        while (*list && *list != ' ' && *list != '\r' && *list != '\n' && *list != '\t') ++list;
        if ((size_t)(list - start) == size && memcmp(start, word, size) == 0) return true;
    }
    return false;
}
static void trim_label(char* value) {
    size_t input = 0, output = 0;
    bool pending = false;
    while (value[input]) {
        char c = value[input++];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pending = output != 0; continue; }
        if (pending) value[output++] = ' ';
        value[output++] = c;
        pending = false;
    }
    value[output] = '\0';
}
static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool percent_decode(const char* source, size_t length, char* output, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            if (length - i < 3) return false;
            int hi = hex_digit((unsigned char)source[i + 1]), lo = hex_digit((unsigned char)source[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)(hi * 16 + lo); i += 2;
        }
        if (c < 32 || c == 127 || used + 1 >= capacity) return false;
        output[used++] = (char)c;
    }
    output[used] = '\0'; return true;
}

/* Resolve relative archive URIs without ever exposing an archive name to fopen. */
static bool resolve_path(const char* base, const char* uri, char* output, char* fragment) {
    const char* hash = strchr(uri, '#');
    size_t uri_size = hash ? (size_t)(hash - uri) : strlen(uri);
    char decoded[FOLIO_ZIP_NAME_CAP], joined[FOLIO_ZIP_NAME_CAP * 2];
    if (!percent_decode(uri, uri_size, decoded, sizeof(decoded))) return false;
    if (fragment != NULL) {
        fragment[0] = '\0';
        if (hash && !percent_decode(hash + 1, strlen(hash + 1), fragment, EPUB_FRAGMENT_CAP)) return false;
    } else if (hash) return false;
    if (decoded[0] == '/' || strchr(decoded, '\\') || strchr(decoded, ':') || strchr(decoded, '?')) return false;
    if (!decoded[0]) {
        if (!hash || !base[0]) return false;
        if (strlen(base) >= FOLIO_ZIP_NAME_CAP) return false;
        strcpy(output, base); return true;
    }
    const char* slash = strrchr(base, '/');
    size_t prefix = slash ? (size_t)(slash - base) + 1 : 0;
    if (prefix + strlen(decoded) >= sizeof(joined)) return false;
    memcpy(joined, base, prefix); strcpy(joined + prefix, decoded);
    size_t used = 0, position = 0, levels = 0;
    size_t marks[64];
    while (joined[position]) {
        size_t start = position;
        while (joined[position] && joined[position] != '/') ++position;
        size_t n = position - start;
        if (joined[position]) ++position;
        if (n == 0 || (n == 1 && joined[start] == '.')) continue;
        if (n == 2 && joined[start] == '.' && joined[start + 1] == '.') {
            if (levels == 0) return false;
            used = marks[--levels]; continue;
        }
        if (levels == 64 || used + (used ? 1 : 0) + n >= FOLIO_ZIP_NAME_CAP) return false;
        marks[levels++] = used;
        if (used) output[used++] = '/';
        memcpy(output + used, joined + start, n); used += n;
    }
    output[used] = '\0'; return used > 0;
}

static bool parse_resource(epub_context_t* context, const char* path,
                           folio_xml_callback_t callback, void* parse_context) {
    size_t length = 0;
    uint8_t* xml = folio_zip_read(&context->zip, folio_zip_find(&context->zip, path),
                                 FOLIO_EPUB_XML_LIMIT, &length,
                                 context->message, sizeof(context->message));
    if (xml == NULL) return false;
    bool okay = folio_xml_parse((const char*)xml, length, callback, parse_context);
    free(xml);
    if (folio_epub_cancelled()) return failure(context, "EPUB operation cancelled");
    if (!okay && !context->message[0]) return failure(context, "Invalid or unsupported EPUB XML / UTF-8");
    return okay;
}

static bool container_token(void* userdata, const folio_xml_token_t* token) {
    epub_context_t* context = userdata;
    if (token->kind == FOLIO_XML_START && !token->depth && !folio_xml_name(token->name, "container"))
        return failure(context, "Invalid EPUB container");
    if (token->kind == FOLIO_XML_START && folio_xml_name(token->name, "rootfile") && !context->package_path[0]) {
        char type[80], path[FOLIO_ZIP_NAME_CAP];
        if (!folio_xml_attr(token, "media-type", type, sizeof(type)) ||
            strcmp(type, "application/oebps-package+xml") != 0) return true;
        if (!folio_xml_attr(token, "full-path", path, sizeof(path)) ||
            !resolve_path("", path, context->package_path, NULL)) return failure(context, "Invalid EPUB package path");
    }
    return true;
}

static manifest_item_t* read_manifest_item(epub_context_t* book, const folio_xml_token_t* token, char type[96]) {
    char id[EPUB_ID_CAP], href[FOLIO_ZIP_NAME_CAP], path[FOLIO_ZIP_NAME_CAP];
    if (!folio_xml_attr(token, "id", id, sizeof(id)) || !*id ||
        !folio_xml_attr(token, "href", href, sizeof(href)) ||
        !folio_xml_attr(token, "media-type", type, 96) ||
        !resolve_path(book->package_path, href, path, NULL)) {
        failure(book, "Invalid EPUB manifest item"); return NULL;
    }
    if (manifest_find(book, id) != SIZE_MAX) { failure(book, "Duplicate EPUB manifest ID"); return NULL; }
    if (!reserve_items(book)) return NULL;
    manifest_item_t* item = manifest_at(book, book->item_count);
    item->id = save_string(book, id); item->path = save_string(book, path);
    if (!item->id || !item->path) return NULL;
    unsigned bucket = id_bucket(id);
    item->next_id = book->id_heads[bucket];
    book->id_heads[bucket] = (uint16_t)++book->item_count;
    return item;
}

static bool package_token(void* userdata, const folio_xml_token_t* token) {
    epub_context_t* context = userdata;
    if (token->kind == FOLIO_XML_START) {
        if (token->depth == 0 && !folio_xml_name(token->name, "package")) return failure(context, "Invalid EPUB package");
        if (folio_xml_name(token->name, "manifest")) context->manifest_depth = (int)token->depth;
        else if (folio_xml_name(token->name, "metadata")) context->metadata_depth = (int)token->depth;
        else if (folio_xml_name(token->name, "spine")) {
            context->spine_depth = (int)token->depth;
            (void)folio_xml_attr(token, "toc", context->ncx_id, sizeof(context->ncx_id));
        } else if (context->metadata_depth >= 0 && folio_xml_name(token->name, "title") && !context->title[0]) {
            context->title_depth = (int)token->depth;
        } else if (context->metadata_depth >= 0 && folio_xml_name(token->name, "meta")) {
            char property[96];
            if (folio_xml_attr(token, "property", property, sizeof(property)) && strcmp(property, "rendition:layout") == 0) {
                context->layout_depth = (int)token->depth; context->layout[0] = '\0';
            }
        } else if (context->manifest_depth >= 0 && folio_xml_name(token->name, "item")) {
            char type[96], properties[256];
            manifest_item_t* item = read_manifest_item(context, token, type);
            if (!item) return false;
            item->html = strcmp(type, "application/xhtml+xml") == 0 || strcmp(type, "text/html") == 0;
            item->ncx = strcmp(type, "application/x-dtbncx+xml") == 0;
            item->nav = folio_xml_attr(token, "properties", properties, sizeof(properties)) && token_word(properties, "nav");
            if (item->nav && item->html && !context->nav_path[0]) strcpy(context->nav_path, item->path);
            item->offset = UINT32_MAX;
        } else if (context->spine_depth >= 0 && folio_xml_name(token->name, "itemref")) {
            char id[EPUB_ID_CAP], linear[16];
            if (!folio_xml_attr(token, "idref", id, sizeof(id))) return failure(context, "Invalid EPUB spine reference");
            if (folio_xml_attr(token, "linear", linear, sizeof(linear)) && strcmp(linear, "no") == 0) return true;
            if (context->spine_count == EPUB_TOC_LIMIT) return failure(context, "EPUB spine exceeds 2048 chapters");
            size_t index = manifest_find(context, id);
            if (index == SIZE_MAX || !manifest_at(context, index)->html) return failure(context, "EPUB spine contains unsupported content");
            manifest_item_t* item = manifest_at(context, index);
            if (item->in_spine) return failure(context, "Repeated EPUB spine item");
            item->in_spine = true;
            context->spine[context->spine_count++] = (uint16_t)index;
        }
    } else if (token->kind == FOLIO_XML_TEXT && context->title_depth >= 0) {
        if (!folio_xml_append(token->text, token->literal, context->title, sizeof(context->title))) return false;
    } else if (token->kind == FOLIO_XML_TEXT && context->layout_depth >= 0) {
        if (!folio_xml_append(token->text, token->literal, context->layout, sizeof(context->layout))) return false;
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == context->layout_depth) {
            trim_label(context->layout);
            if (strcmp(context->layout, "pre-paginated") == 0) return failure(context, "Fixed-layout EPUB is unsupported");
            context->layout_depth = -1;
        }
        if ((int)token->depth == context->title_depth) context->title_depth = -1;
        if ((int)token->depth == context->metadata_depth) context->metadata_depth = -1;
        if ((int)token->depth == context->manifest_depth) context->manifest_depth = -1;
        if ((int)token->depth == context->spine_depth) context->spine_depth = -1;
    }
    return true;
}

static navigation_item_t* add_navigation(epub_context_t* context, const char* base, const char* href) {
    size_t i = context->nav_count;
    if (i == EPUB_TOC_LIMIT) { failure(context, "EPUB table of contents exceeds 2048 entries"); return NULL; }
    if (i % EPUB_INDEX_BLOCK == 0) {
        context->nav[i / EPUB_INDEX_BLOCK] = folio_epub_alloc(EPUB_INDEX_BLOCK * sizeof(navigation_item_t));
        if (!context->nav[i / EPUB_INDEX_BLOCK]) { failure(context, "Not enough memory for EPUB contents"); return NULL; }
    }
    navigation_item_t* item = nav_at(context, i);
    char path[FOLIO_ZIP_NAME_CAP], fragment[EPUB_FRAGMENT_CAP];
    memset(item, 0, sizeof(*item)); item->offset = UINT32_MAX;
    if (!resolve_path(base, href, path, fragment)) { failure(context, "Invalid EPUB navigation URI"); return NULL; }
    item->path = save_string(context, path); item->fragment = save_string(context, fragment);
    if (!item->path || !item->fragment) return NULL;
    ++context->nav_count;
    return item;
}

typedef struct {
    epub_context_t* book;
    const char* path;
    int nav_depth, link_depth, link_index;
    bool found_toc;
} nav_context_t;

static bool nav_token(void* userdata, const folio_xml_token_t* token) {
    nav_context_t* parser = userdata;
    epub_context_t* book = parser->book;
    if (token->kind == FOLIO_XML_START) {
        if (folio_xml_name(token->name, "nav") && !parser->found_toc) {
            char type[128], role[128];
            bool is_toc = (folio_xml_attr(token, "type", type, sizeof(type)) && token_word(type, "toc")) ||
                          (folio_xml_attr(token, "role", role, sizeof(role)) && token_word(role, "doc-toc"));
            if (is_toc) { parser->nav_depth = (int)token->depth; parser->found_toc = true; }
        } else if (parser->nav_depth >= 0 && folio_xml_name(token->name, "a")) {
            char href[FOLIO_ZIP_NAME_CAP + EPUB_FRAGMENT_CAP];
            if (!folio_xml_attr(token, "href", href, sizeof(href))) return true;
            if (parser->link_depth >= 0) return failure(book, "Nested EPUB navigation link");
            parser->link_index = (int)book->nav_count;
            if (add_navigation(book, parser->path, href) == NULL) return false;
            parser->link_depth = (int)token->depth;
        }
    } else if (token->kind == FOLIO_XML_TEXT && parser->link_depth >= 0) {
        return folio_xml_append(token->text, token->literal, nav_at(book, parser->link_index)->title, FOLIO_EPUB_TITLE_CAP);
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->link_depth) {
            trim_label(nav_at(book, parser->link_index)->title); parser->link_depth = -1; parser->link_index = -1;
        }
        if ((int)token->depth == parser->nav_depth) parser->nav_depth = -1;
    }
    return true;
}

typedef struct { int depth, item; char label[FOLIO_EPUB_TITLE_CAP]; } ncx_level_t;
typedef struct {
    epub_context_t* book;
    const char* path;
    ncx_level_t levels[32];
    unsigned level_count;
    int label_depth, map_depth;
} ncx_context_t;

static bool ncx_token(void* userdata, const folio_xml_token_t* token) {
    ncx_context_t* parser = userdata;
    if (token->kind == FOLIO_XML_START) {
        if (folio_xml_name(token->name, "navMap")) parser->map_depth = (int)token->depth;
        else if (parser->map_depth >= 0 && folio_xml_name(token->name, "navPoint")) {
            if (parser->level_count == 32) return failure(parser->book, "EPUB NCX nesting exceeds 32 levels");
            ncx_level_t* level = &parser->levels[parser->level_count++];
            memset(level, 0, sizeof(*level)); level->depth = (int)token->depth; level->item = -1;
        } else if (parser->level_count > 0 && folio_xml_name(token->name, "navLabel")) {
            parser->label_depth = (int)token->depth;
        } else if (parser->level_count > 0 && folio_xml_name(token->name, "content")) {
            ncx_level_t* level = &parser->levels[parser->level_count - 1];
            char source[FOLIO_ZIP_NAME_CAP + EPUB_FRAGMENT_CAP];
            if (level->item >= 0 || !folio_xml_attr(token, "src", source, sizeof(source))) return false;
            level->item = (int)parser->book->nav_count;
            navigation_item_t* item = add_navigation(parser->book, parser->path, source);
            if (item == NULL) return false;
            memcpy(item->title, level->label, sizeof(item->title));
        }
    } else if (token->kind == FOLIO_XML_TEXT && parser->label_depth >= 0 && parser->level_count > 0) {
        ncx_level_t* level = &parser->levels[parser->level_count - 1];
        if (!folio_xml_append(token->text, token->literal, level->label, sizeof(level->label))) return false;
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->label_depth) {
            if (parser->level_count) trim_label(parser->levels[parser->level_count - 1].label);
            parser->label_depth = -1;
        }
        if (parser->level_count > 0 && (int)token->depth == parser->levels[parser->level_count - 1].depth) {
            ncx_level_t* level = &parser->levels[parser->level_count - 1];
            if (level->item >= 0) memcpy(nav_at(parser->book, level->item)->title, level->label, sizeof(level->label));
            --parser->level_count;
        }
        if ((int)token->depth == parser->map_depth) parser->map_depth = -1;
    }
    return true;
}

static bool flush_output(epub_context_t* context) {
    if (folio_epub_cancelled()) return failure(context, "EPUB operation cancelled");
    if (context->write_used == 0) return true;
    if (fwrite(context->write_buffer, 1, context->write_used, context->output) != context->write_used) {
        return failure(context, "Cannot write EPUB text cache");
    }
    context->text_crc = folio_epub_crc32(context->text_crc, context->write_buffer, context->write_used);
    context->write_used = 0; return true;
}
static bool write_bytes(epub_context_t* context, const char* bytes, size_t count) {
    if (count > EPUB_TEXT_LIMIT - context->written) return failure(context, "EPUB text exceeds 32 MiB");
    for (size_t i = 0; i < count; ++i) {
        context->write_buffer[context->write_used++] = (uint8_t)bytes[i];
        ++context->written;
        if (context->write_used == sizeof(context->write_buffer) && !flush_output(context)) return false;
    }
    return true;
}
static bool newline(epub_context_t* context) {
    context->pending_space = false;
    if (context->last_newline || context->written == 0) { context->line_start = true; return true; }
    if (!write_bytes(context, "\n", 1)) return false;
    context->last_newline = true; context->line_start = true; return true;
}
static bool text_codepoint(void* userdata, uint32_t cp) {
    epub_context_t* context = userdata;
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xA0) {
        if (!context->line_start) context->pending_space = true;
        return true;
    }
    if (context->pending_space && !write_bytes(context, " ", 1)) return false;
    context->pending_space = false;
    char bytes[4]; size_t n;
    if (cp < 0x80) { bytes[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { bytes[0] = (char)(0xC0 | (cp >> 6)); bytes[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        bytes[0] = (char)(0xE0 | (cp >> 12)); bytes[1] = (char)(0x80 | ((cp >> 6) & 63));
        bytes[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        bytes[0] = (char)(0xF0 | (cp >> 18)); bytes[1] = (char)(0x80 | ((cp >> 12) & 63));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 63)); bytes[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    if (!write_bytes(context, bytes, n)) return false;
    context->line_start = false; context->last_newline = false; return true;
}
static bool block_tag(folio_xml_span_t name) {
    static const char* names[] = {"p", "div", "section", "article", "header", "footer", "aside", "blockquote", "pre", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "dt", "dd", "hr", "br"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) if (folio_xml_name(name, names[i])) return true;
    return false;
}
typedef struct {
    epub_context_t* book;
    manifest_item_t* item;
    int body_depth, skip_depth, heading_depth;
    bool body_found;
    char heading[FOLIO_EPUB_TITLE_CAP];
} body_context_t;

static bool body_token(void* userdata, const folio_xml_token_t* token) {
    body_context_t* parser = userdata;
    epub_context_t* book = parser->book;
    if (token->kind == FOLIO_XML_START) {
        if (folio_xml_name(token->name, "body")) { parser->body_depth = (int)token->depth; parser->body_found = true; }
        if (parser->body_depth < 0 || parser->skip_depth >= 0) return true;
        char hidden[64];
        if (folio_xml_name(token->name, "script") || folio_xml_name(token->name, "style") ||
            folio_xml_name(token->name, "svg") || folio_xml_name(token->name, "math") ||
            folio_xml_attr(token, "hidden", hidden, sizeof(hidden)) ||
            (folio_xml_attr(token, "aria-hidden", hidden, sizeof(hidden)) && strcmp(hidden, "true") == 0)) {
            parser->skip_depth = (int)token->depth; return true;
        }
        if (block_tag(token->name) && !newline(book)) return false;
        char id[EPUB_FRAGMENT_CAP];
        bool has_id = folio_xml_attr(token, "id", id, sizeof(id));
        if (!has_id && folio_xml_name(token->name, "a")) has_id = folio_xml_attr(token, "name", id, sizeof(id));
        if (has_id) {
            for (size_t i = 0; i < book->nav_count; ++i) {
                navigation_item_t* nav = nav_at(book, i);
                if (!nav->anchored && nav->fragment[0] && strcmp(nav->path, parser->item->path) == 0 && strcmp(nav->fragment, id) == 0) {
                    nav->offset = book->written; nav->anchored = true;
                }
            }
        }
        if (!parser->heading[0] && parser->heading_depth < 0 &&
            (folio_xml_name(token->name, "h1") || folio_xml_name(token->name, "h2") || folio_xml_name(token->name, "h3"))) {
            parser->heading_depth = (int)token->depth;
        }
        if (folio_xml_name(token->name, "img")) {
            if (!newline(book) || !write_bytes(book, "[图片]", strlen("[图片]"))) return false;
            book->line_start = false; book->last_newline = false;
            char alt[256];
            if (folio_xml_attr(token, "alt", alt, sizeof(alt)) && alt[0]) {
                if (!write_bytes(book, " ", 1) || !folio_xml_emit((folio_xml_span_t){alt, strlen(alt)}, true, text_codepoint, book)) return false;
            }
            return newline(book);
        }
    } else if (token->kind == FOLIO_XML_TEXT) {
        if (parser->body_depth < 0 || parser->skip_depth >= 0) return true;
        if (parser->heading_depth >= 0 && !folio_xml_append(token->text, token->literal, parser->heading, sizeof(parser->heading))) return false;
        return folio_xml_emit(token->text, token->literal, text_codepoint, book);
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->skip_depth) { parser->skip_depth = -1; return true; }
        if (parser->skip_depth >= 0 || parser->body_depth < 0) return true;
        if (block_tag(token->name) && !newline(book)) return false;
        if ((int)token->depth == parser->heading_depth) { trim_label(parser->heading); parser->heading_depth = -1; }
        if ((int)token->depth == parser->body_depth) parser->body_depth = -1;
    }
    return true;
}

static bool existing_cache_matches(const char* path, uint32_t length, uint32_t crc) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t bytes[1024]; uint32_t size = 0, actual = 0;
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), file)) > 0) {
        if (folio_epub_cancelled()) { fclose(file); return false; }
        if (size > length || n > length - size) { fclose(file); return false; }
        size += (uint32_t)n; actual = folio_epub_crc32(actual, bytes, n);
        if ((size & 0xFFFF) == 0) folio_epub_yield();
    }
    bool okay = !ferror(file) && size == length && actual == crc;
    fclose(file); return okay;
}

// Cache only fixed-size metadata; all filesystem paths are derived from the source.
typedef struct {
    uint32_t magic, source_crc, source_size, text_size, text_crc, count;
    char title[FOLIO_EPUB_TITLE_CAP];
} contents_header_t;

// Remembers the full-archive fingerprint for an unchanged EPUB: same size, mtime
// and central directory (which carries every member's CRC).
typedef struct { uint32_t magic, size, mtime, directory_crc, source_crc; uint64_t source_hash; uint32_t check; } identity_t;
static void identity_path(const char* path, char* out, size_t cap) {
    snprintf(out, cap, EPUB_CACHE_ROOT "/%08lx.id", (unsigned long)folio_epub_crc32(0, path, strlen(path)));
}
static bool identity_load(const char* path, folio_zip_t* zip) {
    char name[64]; identity_t id; struct stat info;
    identity_path(path, name, sizeof(name));
    FILE* file = fopen(name, "rb");
    if (file == NULL) return false;
    bool okay = fread(&id, sizeof(id), 1, file) == 1;
    fclose(file);
    if (!okay || stat(path, &info) != 0 || id.magic != 0x31444950u || id.check != folio_epub_crc32(0, &id, offsetof(identity_t, check)) ||
        id.size != zip->source_size || id.mtime != (uint32_t)info.st_mtime || id.directory_crc != zip->directory_crc) return false;
    zip->source_crc = id.source_crc; zip->source_hash = id.source_hash;
    return true;
}
static void identity_save(const char* path, const folio_zip_t* zip) {
    char name[64]; struct stat info;
    if (stat(path, &info) != 0) return;
    identity_t id = {.magic = 0x31444950u, .size = zip->source_size, .mtime = (uint32_t)info.st_mtime,
        .directory_crc = zip->directory_crc, .source_crc = zip->source_crc, .source_hash = zip->source_hash};
    id.check = folio_epub_crc32(0, &id, offsetof(identity_t, check));
    identity_path(path, name, sizeof(name));
    FILE* file = fopen(name, "wb");
    if (file != NULL) { (void)fwrite(&id, sizeof(id), 1, file); fclose(file); }
}
static bool cache_size_matches(const char* path, uint32_t length) {
    struct stat info;
    return stat(path, &info) == 0 && (uint64_t)info.st_size == length;
}

static bool load_contents(const char* index_path, const folio_zip_t* zip, folio_epub_t* result, bool trusted) {
    FILE* index = fopen(index_path, "rb");
    if (!index) return false;
    contents_header_t h;
    uint32_t expected_crc;
    bool okay = fread(&h, sizeof(h), 1, index) == 1 && h.magic == 0x31434f54 &&
        h.source_crc == zip->source_crc && h.source_size == zip->source_size &&
        h.count <= EPUB_TOC_LIMIT && h.text_size > 0 && h.text_size <= EPUB_TEXT_LIMIT &&
        memchr(h.title, 0, sizeof(h.title)) != NULL;
    folio_epub_chapter_t* chapters = NULL;
    if (okay && h.count) {
        chapters = folio_epub_alloc(h.count * sizeof(*chapters));
        okay = chapters && fread(chapters, sizeof(*chapters), h.count, index) == h.count;
    }
    if (okay) {
        uint32_t crc = folio_epub_crc32(0, &h, sizeof(h));
        crc = folio_epub_crc32(crc, chapters, h.count * sizeof(*chapters));
        okay = fread(&expected_crc, sizeof(expected_crc), 1, index) == 1 &&
            expected_crc == crc && fgetc(index) == EOF && !ferror(index);
    }
    fclose(index);
    for (size_t i = 0; okay && i < h.count; ++i) {
        okay = chapters[i].offset < h.text_size && (!i || chapters[i].offset >= chapters[i-1].offset) &&
            memchr(chapters[i].title, 0, sizeof(chapters[i].title)) != NULL;
    }
    if (!okay || !(trusted ? cache_size_matches(result->text_path, h.text_size) :
                   existing_cache_matches(result->text_path, h.text_size, h.text_crc))) {
        free(chapters);
        return false;
    }
    result->chapters = chapters;
    result->chapter_count = h.count;
    memcpy(result->title, h.title, sizeof(result->title));
    return true;
}

static void save_contents(const char* path, const epub_context_t* book, const folio_epub_t* result) {
    contents_header_t h = { .magic = 0x31434f54, .source_crc = book->zip.source_crc,
        .source_size = book->zip.source_size, .text_size = book->written,
        .text_crc = book->text_crc, .count = (uint32_t)result->chapter_count };
    memcpy(h.title, result->title, sizeof(h.title));
    uint32_t crc = folio_epub_crc32(0, &h, sizeof(h));
    crc = folio_epub_crc32(crc, result->chapters, h.count * sizeof(*result->chapters));
    // A torn index will fail its checksum and be regenerated from the EPUB.
    FILE* index = fopen(path, "wb");
    if (!index) return;
    bool okay = fwrite(&h, sizeof(h), 1, index) == 1 &&
        fwrite(result->chapters, sizeof(*result->chapters), h.count, index) == h.count &&
        fwrite(&crc, sizeof(crc), 1, index) == 1;
    if (fclose(index) || !okay) (void)remove(path);
}

static int chapter_compare(const void* left, const void* right) {
    const folio_epub_chapter_t* a = left;
    const folio_epub_chapter_t* b = right;
    return a->offset < b->offset ? -1 : (a->offset > b->offset ? 1 : 0);
}

void folio_epub_free(folio_epub_t* epub) {
    if (epub == NULL) return;
    free(epub->chapters); memset(epub, 0, sizeof(*epub));
}

typedef struct {
    epub_context_t* book;
    char legacy_id[EPUB_ID_CAP];
    char guide_uri[FOLIO_ZIP_NAME_CAP];
    int manifest_depth, metadata_depth, guide_depth;
    bool package_found;
} cover_package_t;

static bool has_attribute(const folio_xml_token_t* token, const char* name) {
    for (size_t i = 0; i < token->attr_count; ++i) {
        if (folio_xml_name(token->attrs[i].name, name)) return true;
    }
    return false;
}

typedef struct {
    epub_context_t* book;
    char* title;
    char* author;
    size_t title_cap, author_cap;
    int metadata_depth, value_depth;
    bool package_found, title_seen, author_seen, author_active;
} metadata_parser_t;

static bool metadata_token(void* userdata, const folio_xml_token_t* token) {
    metadata_parser_t* parser = userdata;
    if (token->kind == FOLIO_XML_START) {
        if (token->depth == 0) {
            if (!folio_xml_name(token->name, "package")) return failure(parser->book, "Invalid EPUB package");
            parser->package_found = true;
        }
        if (token->depth == 1 && folio_xml_name(token->name, "metadata")) parser->metadata_depth = 1;
        else if (parser->metadata_depth == 1 && token->depth == 2 &&
                 folio_xml_name(token->name,"dc-metadata")) parser->metadata_depth=2;
        else if (parser->metadata_depth >= 1 && (int)token->depth == parser->metadata_depth+1) {
            if (!parser->title_seen && folio_xml_name(token->name, "title")) {
                parser->title_seen = true; parser->author_active = false; parser->value_depth = (int)token->depth;
            } else if (!parser->author_seen && folio_xml_name(token->name, "creator")) {
                parser->author_seen = true; parser->author_active = true; parser->value_depth = (int)token->depth;
            }
        }
    } else if (token->kind == FOLIO_XML_TEXT && parser->value_depth >= 0) {
        return folio_xml_append(token->text, token->literal,
            parser->author_active ? parser->author : parser->title,
            parser->author_active ? parser->author_cap : parser->title_cap);
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->value_depth) {
            char* value=parser->author_active ? parser->author : parser->title;
            trim_label(value);
            if (!*value) {
                if (parser->author_active) parser->author_seen=false;
                else parser->title_seen=false;
            }
            parser->value_depth=-1;
        }
        if ((int)token->depth == parser->metadata_depth) parser->metadata_depth = parser->metadata_depth==2 ? 1 : -1;
    }
    return true;
}

bool folio_epub_metadata(const char* path, char* title, size_t title_cap,
                         char* author, size_t author_cap, char* error, size_t error_cap) {
    if (error && error_cap) error[0] = '\0';
    if (title && title_cap) title[0] = '\0';
    if (author && author_cap) author[0] = '\0';
    if (!path || !title || !title_cap || !author || !author_cap) {
        if (error && error_cap) snprintf(error, error_cap, "Invalid EPUB metadata arguments");
        return false;
    }
    epub_context_t* book = folio_epub_alloc(sizeof(*book));
    if (!book) { if (error && error_cap) snprintf(error, error_cap, "Not enough memory for EPUB metadata"); return false; }
    memset(book, 0, sizeof(*book));
    bool okay = false;
    metadata_parser_t parser = { .book = book, .title = title, .author = author,
        .title_cap = title_cap, .author_cap = author_cap, .metadata_depth = -1, .value_depth = -1 };
    if (!folio_zip_open_directory(path, &book->zip, book->message, sizeof(book->message))) goto cleanup;
    if (!folio_zip_check_epub_type(&book->zip, book->message, sizeof(book->message))) goto cleanup;
    if (!parse_resource(book, "META-INF/container.xml", container_token, book)) goto cleanup;
    if (!book->package_path[0]) { failure(book, "Missing EPUB package path"); goto cleanup; }
    if (!parse_resource(book, book->package_path, metadata_token, &parser)) goto cleanup;
    if (!parser.package_found) { failure(book, "Invalid EPUB package"); goto cleanup; }
    trim_label(title); trim_label(author); okay = true;
cleanup:
    if (!okay) {
        title[0] = author[0] = '\0';
        if (error && error_cap) snprintf(error, error_cap, "%s", book->message[0] ? book->message : "Cannot read EPUB metadata");
    }
    folio_zip_close(&book->zip); free(book); return okay;
}

/* Cover discovery is independent of spine support: a shelf can show a cover
 * even when a book uses fixed layout or an unsupported chapter format. */
static bool cover_package_token(void* userdata, const folio_xml_token_t* token) {
    cover_package_t* parser = userdata;
    epub_context_t* book = parser->book;
    if (token->kind == FOLIO_XML_START) {
        if (token->depth == 0) {
            if (!folio_xml_name(token->name, "package")) return failure(book, "Invalid EPUB package");
            parser->package_found = true;
        }
        if (has_attribute(token, "base")) return failure(book, "EPUB cover xml:base is unsupported");
        if (token->depth == 1 && folio_xml_name(token->name, "manifest")) parser->manifest_depth = (int)token->depth;
        else if (token->depth == 1 && folio_xml_name(token->name, "metadata")) parser->metadata_depth = (int)token->depth;
        else if (token->depth == 1 && folio_xml_name(token->name, "guide")) parser->guide_depth = (int)token->depth;
        else if (parser->manifest_depth >= 0 && token->depth == (unsigned)parser->manifest_depth + 1 &&
                 folio_xml_name(token->name, "item")) {
            char type[96], properties[256] = {0};
            manifest_item_t* item = read_manifest_item(book, token, type);
            if (!item) return false;
            if (has_attribute(token, "properties") && !folio_xml_attr(token, "properties", properties, sizeof(properties)))
                return failure(book, "EPUB manifest properties are too long");
            item->cover_type = strcmp(type, "image/png") == 0 ? COVER_PNG :
                               strcmp(type, "image/jpeg") == 0 ? COVER_JPEG :
                               (strcmp(type, "application/xhtml+xml") == 0 || strcmp(type, "text/html") == 0 ||
                                strcmp(type, "image/svg+xml") == 0) ? COVER_DOCUMENT : COVER_OTHER;
            item->cover_image = token_word(properties, "cover-image");
            if (item->cover_image) {
                for (size_t i = 0; i + 1 < book->item_count; ++i) {
                    if (manifest_at(book, i)->cover_image) return failure(book, "Multiple EPUB cover-image declarations");
                }
            }
        } else if (parser->metadata_depth >= 0 && folio_xml_name(token->name, "meta")) {
            char name[96], id[EPUB_ID_CAP];
            if (folio_xml_attr(token, "name", name, sizeof(name)) && strcmp(name, "cover") == 0) {
                if (!folio_xml_attr(token, "content", id, sizeof(id)) || !id[0]) return failure(book, "Invalid EPUB 2 cover ID");
                if (parser->legacy_id[0] && strcmp(parser->legacy_id, id) != 0) return failure(book, "Conflicting EPUB 2 cover IDs");
                strcpy(parser->legacy_id, id);
            }
        } else if (parser->guide_depth >= 0 && folio_xml_name(token->name, "reference")) {
            char type[128];
            if (folio_xml_attr(token, "type", type, sizeof(type)) && token_word(type, "cover") && !parser->guide_uri[0]) {
                if (!folio_xml_attr(token, "href", parser->guide_uri, sizeof(parser->guide_uri)) || !parser->guide_uri[0])
                    return failure(book, "Invalid EPUB guide cover reference");
            }
        }
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->manifest_depth) parser->manifest_depth = -1;
        if ((int)token->depth == parser->metadata_depth) parser->metadata_depth = -1;
        if ((int)token->depth == parser->guide_depth) parser->guide_depth = -1;
    }
    return true;
}

typedef struct {
    epub_context_t* book;
    const char* path;
    const char* fragment;
    char image_path[FOLIO_ZIP_NAME_CAP];
    int target_depth, skip_depth;
} cover_document_t;

static bool cover_document_token(void* userdata, const folio_xml_token_t* token) {
    cover_document_t* parser = userdata;
    if (token->kind == FOLIO_XML_START) {
        if (has_attribute(token, "base") || folio_xml_name(token->name, "base"))
            return failure(parser->book, "EPUB cover base URL is unsupported");
        if (folio_xml_name(token->name, "script") || folio_xml_name(token->name, "style")) {
            if (parser->skip_depth < 0) parser->skip_depth = (int)token->depth;
        }
        if (parser->skip_depth >= 0) return true;
        if (parser->fragment[0] && parser->target_depth < 0) {
            char id[EPUB_FRAGMENT_CAP];
            if (folio_xml_attr(token, "id", id, sizeof(id)) && strcmp(id, parser->fragment) == 0)
                parser->target_depth = (int)token->depth;
        }
        if (parser->image_path[0] || (parser->fragment[0] && parser->target_depth < 0)) return true;
        bool html_image = folio_xml_name(token->name, "img");
        if (html_image || folio_xml_name(token->name, "image")) {
            char uri[FOLIO_ZIP_NAME_CAP];
            /* XML attribute lookup accepts both SVG 1.1 xlink:href and SVG 2 href. */
            if (!folio_xml_attr(token, html_image ? "src" : "href", uri, sizeof(uri)) ||
                !resolve_path(parser->path, uri, parser->image_path, NULL))
                return failure(parser->book, "Invalid or external EPUB cover image URL");
        }
    } else if (token->kind == FOLIO_XML_END) {
        if ((int)token->depth == parser->target_depth) parser->target_depth = -1;
        if ((int)token->depth == parser->skip_depth) parser->skip_depth = -1;
    }
    return true;
}

static uint32_t cover_be32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Check the resource envelope here; the image decoder validates pixels later.
 * PNG chunk CRCs catch corrupt images even if their containing ZIP is valid. */
static bool valid_cover_png(const uint8_t* data, size_t size) {
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || memcmp(data, signature, 8) != 0) return false;
    size_t pos = 8;
    bool header = false, image = false;
    while (size - pos >= 12) {
        if (folio_epub_cancelled()) return false;
        uint32_t length = cover_be32(data + pos);
        if (length > size - pos - 12) return false;
        const uint8_t* type = data + pos + 4;
        if (folio_epub_crc32(0, type, (size_t)length + 4) != cover_be32(type + 4 + length)) return false;
        if (!header && memcmp(type, "IHDR", 4) != 0) return false;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (header || length != 13 || cover_be32(type + 4) == 0 || cover_be32(type + 8) == 0) return false;
            unsigned depth = type[12], color = type[13];
            if (!((color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                  (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                  ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16))) ||
                type[14] != 0 || type[15] != 0 || type[16] > 1) return false;
            header = true;
        } else if (memcmp(type, "IDAT", 4) == 0) image = true;
        else if (memcmp(type, "IEND", 4) == 0) return header && image && length == 0 && pos + 12 == size;
        pos += (size_t)length + 12;
        folio_epub_yield();
    }
    return false;
}

static bool valid_cover_jpeg(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t pos = 2;
    bool frame = false, scan = false, entropy = false;
    while (pos < size) {
        if (folio_epub_cancelled()) return false;
        if (entropy) {
            while (pos < size && data[pos] != 0xFF) ++pos;
        }
        if (pos == size || data[pos++] != 0xFF) return false;
        while (pos < size && data[pos] == 0xFF) ++pos;
        if (pos == size) return false;
        unsigned marker = data[pos++];
        if (entropy && (marker == 0 || (marker >= 0xD0 && marker <= 0xD7))) continue;
        entropy = false;
        if (marker == 0xD9) return frame && scan && pos == size;
        if (marker == 0 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || size - pos < 2) return false;
        if (marker == 1) continue;
        size_t length = (size_t)data[pos] * 256 + data[pos + 1];
        if (length < 2 || length > size - pos) return false;
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (frame || length < 11 || data[pos + 2] == 0 ||
                (data[pos + 3] == 0 && data[pos + 4] == 0) || (data[pos + 5] == 0 && data[pos + 6] == 0) ||
                data[pos + 7] == 0 || length != 8u + 3u * data[pos + 7]) return false;
            frame = true;
        } else if (marker == 0xDA) {
            if (!frame || length < 8 || data[pos + 2] == 0 || length != 6u + 2u * data[pos + 2]) return false;
            scan = true; entropy = true;
        }
        pos += length;
    }
    return false;
}

bool folio_epub_cover(const char* path, uint8_t** data, size_t* size, char* error, size_t error_cap) {
    if (error != NULL && error_cap) error[0] = '\0';
    if (data != NULL) *data = NULL;
    if (size != NULL) *size = 0;
    if (path == NULL || data == NULL || size == NULL) {
        if (error != NULL && error_cap) snprintf(error, error_cap, "Invalid EPUB cover arguments");
        return false;
    }
    epub_context_t* book = folio_epub_alloc(sizeof(*book));
    if (book == NULL) { if (error != NULL && error_cap) snprintf(error, error_cap, "Not enough memory for EPUB cover"); return false; }
    memset(book, 0, sizeof(*book));
    bool okay = false;
    cover_package_t parser = {.book = book, .manifest_depth = -1, .metadata_depth = -1, .guide_depth = -1};
    if (!folio_zip_open_directory(path, &book->zip, book->message, sizeof(book->message))) goto cleanup;
    if (!folio_zip_check_epub_type(&book->zip, book->message, sizeof(book->message))) goto cleanup;
    if (folio_zip_find(&book->zip, "META-INF/encryption.xml") != NULL) {
        failure(book, "Encrypted or font-obfuscated EPUB is unsupported"); goto cleanup;
    }
    if (!parse_resource(book, "META-INF/container.xml", container_token, book)) goto cleanup;
    if (!book->package_path[0]) { failure(book, "Missing EPUB package path"); goto cleanup; }
    if (!parse_resource(book, book->package_path, cover_package_token, &parser)) goto cleanup;
    if (!parser.package_found) { failure(book, "Invalid EPUB package"); goto cleanup; }
    manifest_item_t* selected = NULL;
    for (size_t i = 0; i < book->item_count; ++i) {
        if (manifest_at(book, i)->cover_image) { selected = manifest_at(book, i); break; }
    }
    if (selected == NULL && parser.legacy_id[0]) {
        for (size_t i = 0; i < book->item_count; ++i) {
            if (strcmp(manifest_at(book, i)->id, parser.legacy_id) == 0) { selected = manifest_at(book, i); break; }
        }
        if (selected == NULL) { failure(book, "EPUB cover ID is missing from manifest"); goto cleanup; }
    }
    char image_path[FOLIO_ZIP_NAME_CAP], fragment[EPUB_FRAGMENT_CAP] = {0};
    cover_type_t expected = COVER_OTHER;
    if (selected != NULL) {
        strcpy(image_path, selected->path); expected = selected->cover_type;
        if (expected == COVER_OTHER) { failure(book, "EPUB cover format is unsupported"); goto cleanup; }
    } else if (parser.guide_uri[0]) {
        if (!resolve_path(book->package_path, parser.guide_uri, image_path, fragment)) {
            failure(book, "Invalid or external EPUB guide cover URL"); goto cleanup;
        }
        for (size_t i = 0; i < book->item_count; ++i) {
            if (strcmp(manifest_at(book, i)->path, image_path) == 0) { expected = manifest_at(book, i)->cover_type; break; }
        }
        /* A guide may name a cover image/document absent from the manifest. */
        if (expected == COVER_OTHER) {
            const char* ext = strrchr(image_path, '.');
            if (ext && (strcmp(ext, ".xhtml") == 0 || strcmp(ext, ".html") == 0 || strcmp(ext, ".svg") == 0)) expected = COVER_DOCUMENT;
        }
    } else goto cleanup; // Absence is a normal shelf fallback, not a parse error.
    if (expected == COVER_DOCUMENT) {
        cover_document_t document = {.book = book, .path = image_path, .fragment = fragment, .target_depth = -1, .skip_depth = -1};
        if (!parse_resource(book, image_path, cover_document_token, &document)) goto cleanup;
        if (!document.image_path[0]) { failure(book, "EPUB cover document has no PNG/JPEG image"); goto cleanup; }
        strcpy(image_path, document.image_path); expected = COVER_OTHER;
    }
    const folio_zip_entry_t* entry = folio_zip_find(&book->zip, image_path);
    if (entry == NULL) { failure(book, "EPUB cover image is missing"); goto cleanup; }
    if (entry->size > EPUB_COVER_LIMIT || entry->compressed > EPUB_COVER_LIMIT) {
        failure(book, "EPUB cover image exceeds 2 MiB"); goto cleanup;
    }
    *data = folio_zip_read(&book->zip, entry, EPUB_COVER_LIMIT, size, book->message, sizeof(book->message));
    if (*data == NULL) goto cleanup;
    bool png = *size >= 8 && (*data)[0] == 137 && memcmp(*data + 1, "PNG\r\n\x1A\n", 7) == 0;
    bool jpeg = *size >= 2 && (*data)[0] == 0xFF && (*data)[1] == 0xD8;
    if ((expected == COVER_PNG && !png) || (expected == COVER_JPEG && !jpeg) || (!png && !jpeg)) {
        failure(book, "EPUB cover must be a PNG or JPEG image"); goto cleanup;
    }
    if (!(png ? valid_cover_png(*data, *size) : valid_cover_jpeg(*data, *size))) {
        failure(book, "EPUB cover image is damaged"); goto cleanup;
    }
    okay = true;
cleanup:
    if (!okay) { free(*data); *data = NULL; *size = 0; }
    if (!okay && folio_epub_cancelled()) failure(book, "EPUB operation cancelled");
    if (error != NULL && error_cap) snprintf(error, error_cap, "%s", book->message);
    folio_zip_close(&book->zip); free_index(book); free(book);
    return okay;
}

bool folio_epub_prepare(const char* path, folio_epub_t* result, char* error, size_t error_cap) {
    if (result == NULL || path == NULL) return false;
    memset(result, 0, sizeof(*result));
    if (error != NULL && error_cap) error[0] = '\0';
    epub_context_t* book = folio_epub_alloc(sizeof(*book));
    if (book == NULL) { if (error != NULL && error_cap) snprintf(error, error_cap, "Not enough memory for EPUB"); return false; }
    memset(book, 0, sizeof(*book));
    book->manifest_depth = book->spine_depth = book->metadata_depth = book->title_depth = book->layout_depth = -1;
    book->line_start = true;
    char temporary[272] = {0};
    bool okay = false;
    if (!folio_zip_open_directory(path, &book->zip, book->message, sizeof(book->message))) goto cleanup;
    bool trusted = identity_load(path, &book->zip);
    if (!trusted) {
        if (!folio_zip_fingerprint(&book->zip)) { failure(book, "Cannot read EPUB"); goto cleanup; }
    }
    result->source_hash = book->zip.source_hash;
    result->source_size = book->zip.source_size;
    if (!folio_zip_check_epub_type(&book->zip, book->message, sizeof(book->message))) goto cleanup;
    if (folio_zip_find(&book->zip, "META-INF/encryption.xml") != NULL) {
        failure(book, "Encrypted or font-obfuscated EPUB is unsupported"); goto cleanup;
    }
    uint32_t path_crc = folio_epub_crc32(0, path, strlen(path));
    int n = snprintf(result->text_path, sizeof(result->text_path), EPUB_CACHE_ROOT "/%08lx-%08lx.txt",
                     (unsigned long)path_crc, (unsigned long)book->zip.source_crc);
    if (n < 0 || (size_t)n >= sizeof(result->text_path)) { failure(book, "EPUB cache path is too long"); goto cleanup; }
    char index_path[272];
    n = snprintf(index_path, sizeof(index_path), "%s.toc", result->text_path);
    if (n < 0 || (size_t)n >= sizeof(index_path)) goto cleanup;
    if (load_contents(index_path, &book->zip, result, trusted)) { okay = true; goto cleanup; }
    if (folio_epub_cancelled()) goto cleanup;
    if (!parse_resource(book, "META-INF/container.xml", container_token, book) || !book->package_path[0]) goto cleanup;
    book->spine = folio_epub_alloc(EPUB_TOC_LIMIT * sizeof(*book->spine));
    if (book->spine == NULL) {
        failure(book, "Not enough memory for EPUB metadata"); goto cleanup;
    }
    if (!parse_resource(book, book->package_path, package_token, book)) goto cleanup;
    if (book->spine_count == 0) { failure(book, "EPUB contains no readable spine chapters"); goto cleanup; }
    trim_label(book->title);
    if (book->nav_path[0]) {
        nav_context_t parser = {.book = book, .path = book->nav_path, .nav_depth = -1, .link_depth = -1, .link_index = -1};
        if (!parse_resource(book, book->nav_path, nav_token, &parser)) goto cleanup;
    }
    if (book->nav_count == 0) {
        const char* ncx = NULL;
        for (size_t i = 0; i < book->item_count; ++i) {
            manifest_item_t* item = manifest_at(book, i);
            if (item->ncx && (!book->ncx_id[0] || strcmp(item->id, book->ncx_id) == 0)) { ncx = item->path; break; }
        }
        if (ncx != NULL) {
            ncx_context_t* parser = folio_epub_alloc(sizeof(*parser));
            if (parser == NULL) { failure(book, "Not enough memory for EPUB NCX"); goto cleanup; }
            memset(parser, 0, sizeof(*parser)); parser->book = book; parser->path = ncx;
            parser->label_depth = parser->map_depth = -1;
            bool parsed = parse_resource(book, ncx, ncx_token, parser); free(parser);
            if (!parsed) goto cleanup;
        }
    }
    if (!make_directory(FOLIO_SD_ROOT "/.folio") || !make_directory(EPUB_CACHE_ROOT)) {
        failure(book, "Cannot create EPUB cache directory"); goto cleanup;
    }
    n = snprintf(temporary, sizeof(temporary), "%s.tmp", result->text_path);
    if (n < 0 || (size_t)n >= sizeof(temporary)) goto cleanup;
    book->output = fopen(temporary, "wb");
    if (book->output == NULL) { failure(book, "Cannot create EPUB text cache"); goto cleanup; }
    for (size_t i = 0; i < book->spine_count; ++i) {
        if (folio_epub_cancelled()) { failure(book, "EPUB operation cancelled"); goto cleanup; }
        manifest_item_t* item = manifest_at(book, book->spine[i]);
        if (!newline(book)) goto cleanup;
        item->offset = book->written;
        size_t matching = 0;
        for (size_t j = 0; j < book->nav_count; ++j) {
            if (strcmp(nav_at(book, j)->path, item->path) == 0) { nav_at(book, j)->offset = item->offset; ++matching; }
        }
        body_context_t parser = {.book = book, .item = item, .body_depth = -1, .skip_depth = -1, .heading_depth = -1};
        if (!parse_resource(book, item->path, body_token, &parser) || !parser.body_found) goto cleanup;
        if (matching == 0) {
            navigation_item_t* nav = add_navigation(book, "", item->path);
            if (nav == NULL) goto cleanup;
            nav->offset = item->offset;
            if (parser.heading[0]) memcpy(nav->title, parser.heading, sizeof(nav->title));
            else snprintf(nav->title, sizeof(nav->title), "第 %u 章", (unsigned)i + 1);
        }
        folio_epub_yield();
    }
    if (!newline(book) || !flush_output(book) || fflush(book->output) != 0) goto cleanup;
    int close_status = fclose(book->output); book->output = NULL;
    if (close_status != 0) { failure(book, "Cannot save EPUB text cache"); goto cleanup; }
    if (book->written == 0) { failure(book, "EPUB contains no readable text"); goto cleanup; }
    result->chapters = folio_epub_alloc(book->nav_count * sizeof(*result->chapters));
    if (result->chapters == NULL) { failure(book, "Not enough memory for EPUB contents"); goto cleanup; }
    for (size_t i = 0; i < book->nav_count; ++i) {
        navigation_item_t* nav = nav_at(book, i);
        if (nav->offset == UINT32_MAX || nav->offset >= book->written) continue;
        folio_epub_chapter_t* chapter = &result->chapters[result->chapter_count++];
        memcpy(chapter->title, nav->title, sizeof(chapter->title));
        if (!chapter->title[0]) snprintf(chapter->title, sizeof(chapter->title), "第 %u 节", (unsigned)result->chapter_count);
        chapter->offset = nav->offset;
    }
    qsort(result->chapters, result->chapter_count, sizeof(*result->chapters), chapter_compare);
    if (book->title[0]) memcpy(result->title, book->title, sizeof(result->title));
    else {
        const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
        (void)folio_xml_append((folio_xml_span_t){name, strlen(name)}, true, result->title, sizeof(result->title));
    }
    bool cache_matches = existing_cache_matches(result->text_path, book->written, book->text_crc);
    if (folio_epub_cancelled()) { failure(book, "EPUB operation cancelled"); goto cleanup; }
    if (cache_matches) {
        if (remove(temporary) != 0) { failure(book, "Cannot finish EPUB cache"); goto cleanup; }
    } else {
        // FAT rename cannot replace an existing destination. A corrupt cache is
        // disposable; the source EPUB is never removed or modified.
        if (remove(result->text_path) != 0 && errno != ENOENT) { failure(book, "Cannot replace EPUB cache"); goto cleanup; }
        if (rename(temporary, result->text_path) != 0) { failure(book, "Cannot commit EPUB text cache"); goto cleanup; }
    }
    temporary[0] = '\0';
    save_contents(index_path, book, result);
    okay = true;
cleanup:
    if (okay && !trusted) identity_save(path, &book->zip);
    if (!okay && folio_epub_cancelled()) failure(book, "EPUB operation cancelled");
    if (book->output != NULL) fclose(book->output);
    if (temporary[0]) (void)remove(temporary);
    folio_zip_close(&book->zip);
    free_index(book); free(book->spine);
    if (!okay) {
        folio_epub_free(result);
        if (error != NULL && error_cap) snprintf(error, error_cap, "%s", book->message[0] ? book->message : "Invalid EPUB structure or cache write failed");
    }
    free(book); return okay;
}
