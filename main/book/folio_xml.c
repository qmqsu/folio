#include "folio_xml.h"
#include "folio_zip.h"

#include <string.h>

#define XML_MAX_DEPTH 64

static bool whitespace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool name_first(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':'; }
static bool name_char(unsigned char c) { return name_first(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
static bool equal(folio_xml_span_t a, folio_xml_span_t b) {
    return a.size == b.size && memcmp(a.data, b.data, a.size) == 0;
}
bool folio_xml_name(folio_xml_span_t name, const char* local_name) {
    for (size_t i = 0; i < name.size; ++i) {
        if (name.data[i] == ':') { name.data += i + 1; name.size -= i + 1; break; }
    }
    return name.size == strlen(local_name) && memcmp(name.data, local_name, name.size) == 0;
}

static bool utf8_next(const char* data, size_t count, size_t* pos, uint32_t* value) {
    if (*pos >= count) return false;
    unsigned char first = (unsigned char)data[(*pos)++];
    unsigned continuation = 0;
    uint32_t cp = first, minimum = 0;
    if (first < 0x80) {
        if (first < 32 && first != '\n' && first != '\r' && first != '\t') return false;
    } else if (first >= 0xC2 && first <= 0xDF) {
        continuation = 1; cp &= 0x1F; minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
        continuation = 2; cp &= 0x0F; minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
        continuation = 3; cp &= 7; minimum = 0x10000;
    } else return false;
    if (continuation > count - *pos) return false;
    for (unsigned i = 0; i < continuation; ++i) {
        unsigned char b = (unsigned char)data[(*pos)++];
        if ((b & 0xC0) != 0x80) return false;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp == 0xFFFE || cp == 0xFFFF) return false;
    *value = cp;
    return true;
}

static bool entity(folio_xml_span_t text, size_t* pos, uint32_t* value) {
    size_t start = *pos;
    while (*pos < text.size && text.data[*pos] != ';' && *pos - start < 24) ++*pos;
    if (*pos == text.size || text.data[*pos] != ';') return false;
    size_t n = *pos - start;
    ++*pos;
    if (n > 1 && text.data[start] == '#') {
        uint32_t cp = 0;
        unsigned base = 10;
        size_t digit = start + 1;
        if (digit < start + n && (text.data[digit] == 'x' || text.data[digit] == 'X')) { base = 16; ++digit; }
        if (digit == start + n) return false;
        while (digit < start + n) {
            unsigned char c = (unsigned char)text.data[digit++];
            unsigned d = c >= '0' && c <= '9' ? c - '0' :
                         (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99));
            if (d >= base || cp > (0x10FFFFu - d) / base) return false;
            cp = cp * base + d;
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp < 32 && cp != '\t' && cp != '\n' && cp != '\r')) return false;
        *value = cp; return true;
    }
    static const struct { const char* name; uint32_t cp; } names[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", 0xA0}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"copy", 0xA9}, {"reg", 0xAE}, {"bull", 0x2022}, {"middot", 0xB7},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strlen(names[i].name) == n && memcmp(text.data + start, names[i].name, n) == 0) {
            *value = names[i].cp; return true;
        }
    }
    return false; // No external entity expansion or silent text corruption.
}

bool folio_xml_emit(folio_xml_span_t text, bool literal, folio_xml_emit_t emit, void* context) {
    size_t pos = 0, checked = 0;
    if (folio_epub_cancelled()) return false;
    while (pos < text.size) {
        if (pos - checked >= 4096) { if (folio_epub_cancelled()) return false; checked = pos; }
        uint32_t cp;
        if (!literal && text.data[pos] == '&') {
            ++pos;
            if (!entity(text, &pos, &cp)) return false;
        } else if (!utf8_next(text.data, text.size, &pos, &cp)) return false;
        if (emit != NULL && !emit(context, cp)) return false;
    }
    return true;
}

typedef struct { char* out; size_t length, cap; bool overflow; } copy_state_t;
static bool copy_cp(void* context, uint32_t cp) {
    copy_state_t* state = context;
    char utf8[4]; size_t n;
    if (cp < 0x80) { utf8[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { utf8[0] = (char)(0xC0 | (cp >> 6)); utf8[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        utf8[0] = (char)(0xE0 | (cp >> 12)); utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        utf8[0] = (char)(0xF0 | (cp >> 18)); utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf8[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (!state->overflow && n < state->cap - state->length) {
        memcpy(state->out + state->length, utf8, n); state->length += n; state->out[state->length] = '\0';
    } else state->overflow = true;
    return true;
}
bool folio_xml_append(folio_xml_span_t text, bool literal, char* out, size_t cap) {
    if (out == NULL || cap == 0) return false;
    copy_state_t state = {out, strlen(out), cap, false};
    return state.length < cap && folio_xml_emit(text, literal, copy_cp, &state);
}
bool folio_xml_attr(const folio_xml_token_t* token, const char* name, char* out, size_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    for (size_t i = 0; i < token->attr_count; ++i) {
        if (folio_xml_name(token->attrs[i].name, name)) {
            copy_state_t state = {out, 0, cap, false};
            return folio_xml_emit(token->attrs[i].value, false, copy_cp, &state) && !state.overflow;
        }
    }
    return false;
}

static bool span_at(const char* data, size_t size, size_t pos, const char* match) {
    size_t count = strlen(match);
    return pos <= size && count <= size - pos && memcmp(data + pos, match, count) == 0;
}
static bool skip_until(const char* data, size_t size, size_t* pos, const char* end) {
    while (*pos < size) {
        if (span_at(data, size, *pos, end)) { *pos += strlen(end); return true; }
        ++*pos;
    }
    return false;
}

bool folio_xml_parse(const char* data, size_t size, folio_xml_callback_t callback, void* context) {
    if (data == NULL || callback == NULL) return false;
    size_t pos = 0;
    // Validate the whole byte stream first, including skipped comments/attributes.
    size_t checked = 0;
    if (folio_epub_cancelled()) return false;
    while (pos < size) {
        uint32_t cp; if (!utf8_next(data, size, &pos, &cp)) return false;
        if (pos - checked >= 4096) { if (folio_epub_cancelled()) return false; checked = pos; }
    }
    pos = span_at(data, size, 0, "\xEF\xBB\xBF") ? 3 : 0;
    folio_xml_span_t stack[XML_MAX_DEPTH];
    unsigned depth = 0, roots = 0;
    size_t yielded = 0;
    while (pos < size) {
        if (pos - yielded >= 8192) { folio_epub_yield(); yielded = pos; if (folio_epub_cancelled()) return false; }
        folio_xml_token_t token = {0};
        token.depth = depth;
        if (data[pos] != '<') {
            size_t start = pos;
            while (pos < size && data[pos] != '<') ++pos;
            token.kind = FOLIO_XML_TEXT; token.text = (folio_xml_span_t){data + start, pos - start};
            if (depth == 0) {
                for (size_t i = start; i < pos; ++i) if (!whitespace((unsigned char)data[i])) return false;
            } else if (!callback(context, &token)) return false;
            continue;
        }
        if (span_at(data, size, pos, "<!--")) { pos += 4; if (!skip_until(data, size, &pos, "-->")) return false; continue; }
        if (span_at(data, size, pos, "<?")) { pos += 2; if (!skip_until(data, size, &pos, "?>")) return false; continue; }
        if (span_at(data, size, pos, "<![CDATA[")) {
            if (depth == 0) return false;
            pos += 9; size_t start = pos;
            if (!skip_until(data, size, &pos, "]]>")) return false;
            token.kind = FOLIO_XML_TEXT; token.literal = true;
            token.text = (folio_xml_span_t){data + start, pos - start - 3};
            if (!callback(context, &token)) return false;
            continue;
        }
        if (span_at(data, size, pos, "<!DOCTYPE")) {
            if (depth != 0 || roots != 0) return false;
            pos += 9; char quote = 0; bool ended = false;
            while (pos < size) {
                char c = data[pos++];
                if (quote) { if (c == quote) quote = 0; }
                else if (c == '"' || c == '\'') quote = c;
                else if (c == '[') return false; // Internal entity declarations intentionally unsupported.
                else if (c == '>') { ended = true; break; }
            }
            if (!ended) return false;
            continue;
        }
        ++pos;
        bool end_tag = pos < size && data[pos] == '/';
        if (end_tag) ++pos;
        size_t name_start = pos;
        if (pos == size || !name_first((unsigned char)data[pos])) return false;
        while (pos < size && name_char((unsigned char)data[pos])) ++pos;
        token.name = (folio_xml_span_t){data + name_start, pos - name_start};
        if (token.name.size > 127) return false;
        if (end_tag) {
            while (pos < size && whitespace((unsigned char)data[pos])) ++pos;
            if (pos == size || data[pos++] != '>' || depth == 0 || !equal(stack[depth - 1], token.name)) return false;
            token.kind = FOLIO_XML_END; token.depth = --depth;
            if (!callback(context, &token)) return false;
            continue;
        }
        bool self_close = false;
        for (;;) {
            size_t before_space = pos;
            while (pos < size && whitespace((unsigned char)data[pos])) ++pos;
            if (pos == size) return false;
            if (data[pos] == '>') { ++pos; break; }
            if (data[pos] == '/' && pos + 1 < size && data[pos + 1] == '>') { pos += 2; self_close = true; break; }
            if (pos == before_space || token.attr_count == 32 || !name_first((unsigned char)data[pos])) return false;
            folio_xml_attr_t* attr = &token.attrs[token.attr_count];
            size_t attr_start = pos;
            while (pos < size && name_char((unsigned char)data[pos])) ++pos;
            attr->name = (folio_xml_span_t){data + attr_start, pos - attr_start};
            for (size_t i = 0; i < token.attr_count; ++i) if (equal(token.attrs[i].name, attr->name)) return false;
            while (pos < size && whitespace((unsigned char)data[pos])) ++pos;
            if (pos == size || data[pos++] != '=') return false;
            while (pos < size && whitespace((unsigned char)data[pos])) ++pos;
            if (pos == size || (data[pos] != '"' && data[pos] != '\'')) return false;
            char quote = data[pos++]; size_t value_start = pos;
            while (pos < size && data[pos] != quote) { if (data[pos] == '<') return false; ++pos; }
            if (pos == size) return false;
            attr->value = (folio_xml_span_t){data + value_start, pos - value_start}; ++pos;
            if (!folio_xml_emit(attr->value, false, NULL, NULL)) return false;
            ++token.attr_count;
        }
        if (depth == 0 && ++roots != 1) return false;
        if (depth == XML_MAX_DEPTH) return false;
        token.kind = FOLIO_XML_START;
        if (!callback(context, &token)) return false;
        if (self_close) { token.kind = FOLIO_XML_END; if (!callback(context, &token)) return false; }
        else stack[depth++] = token.name;
    }
    return roots == 1 && depth == 0;
}
