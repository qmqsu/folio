/* Bounded, stored-ZIP EPUB 2 writer. Streams chapter text, never holds a book
 * in RAM. The first member is the uncompressed OCF mimetype. */

/* Weak default: a long export still works with no observer attached. The
 * WeRead worker overrides this to report progress and keep the UI awake. */
__attribute__((weak)) void folio_weread_export_progress(size_t done, size_t total) {
    (void)done;
    (void)total;
}

typedef struct {
    char name[80];
    uint32_t offset, size, crc;
} wr_zip_entry_t;
/* Plain fd buffered sink. A stdio FILE costs an internal-heap buffer plus a
 * lock semaphore per open, and closing them from the export worker
 * double-released that state (tlsf_free assert inside __bufio_close). */
#define WR_BUF 4096
#define WR_COPY 8192
typedef struct {
    int fd;
    size_t used;
    bool okay;
    uint8_t *buf;
    size_t cap;
    uint32_t crc;
    bool crc_active;
} wr_out_t;
typedef struct {
    wr_out_t out;
    wr_zip_entry_t *entries;
    uint8_t *copy;   /* chapter text / CRC scratch, keeps 8 KiB off the stack */
    size_t copy_cap;
    size_t count;
    bool okay;
} wr_zip_t;
static uint32_t wr_crc(uint32_t crc, const uint8_t *p, size_t n) {
    static const uint32_t table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return ~crc;
}
static void wr_flush(wr_out_t *o) {
    // Hash each data buffer once while writing it. No whole-member reread.
    if (o->crc_active) o->crc = wr_crc(o->crc, o->buf, o->used);
    size_t off = 0;
    while (off < o->used && o->okay) {
        ssize_t n = write(o->fd, o->buf + off, o->used - off);
        if (n <= 0) {
            o->okay = false;
            break;
        }
        off += (size_t)n;
    }
    o->used = 0;
}
static void wr_raw(wr_out_t *o, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n && o->okay) {
        if (o->used == o->cap)
            wr_flush(o);
        size_t room = o->cap - o->used;
        size_t take = n < room ? n : room;
        memcpy(o->buf + o->used, p, take);
        o->used += take;
        p += take;
        n -= take;
    }
}
static void wr_fputs(const char *s, wr_out_t *o) { wr_raw(o, s, strlen(s)); }
static void wr_fputc(int c, wr_out_t *o) {
    uint8_t b = (uint8_t)c;
    wr_raw(o, &b, 1);
}
static void wr_printf(wr_out_t *o, const char *fmt, ...) {
    char text[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(text)) {
        /* A truncated XML member is never a successful export. Long fixed
         * markup is streamed with wr_fputs instead of this small formatter. */
        o->okay = false;
        return;
    }
    wr_raw(o, text, (size_t)n);
}
static long wr_tell(wr_out_t *o) {
    wr_flush(o);
    return o->okay ? (long)lseek(o->fd, 0, SEEK_CUR) : -1;
}
static bool wr_seek(wr_out_t *o, long pos) {
    wr_flush(o);
    return o->okay && lseek(o->fd, (off_t)pos, SEEK_SET) == (off_t)pos;
}
static void wr_u16(wr_out_t *f, uint16_t v) {
    wr_fputc(v & 255, f);
    wr_fputc(v >> 8, f);
}
static void wr_u32(wr_out_t *f, uint32_t v) {
    wr_u16(f, (uint16_t)v);
    wr_u16(f, (uint16_t)(v >> 16));
}
static void wr_xml(wr_out_t *f, const char *s) {
    for (; *s; ++s) {
        if((unsigned char)*s<32 && *s!='\t' && *s!='\n' && *s!='\r')continue;
        switch (*s) {
        case '&':
            wr_fputs("&amp;", f);
            break;
        case '<':
            wr_fputs("&lt;", f);
            break;
        case '>':
            wr_fputs("&gt;", f);
            break;
        case '"':
            wr_fputs("&quot;", f);
            break;
        default:
            wr_fputc((unsigned char)*s, f);
        }
    }
}
static bool wr_zip_begin(wr_zip_t *z, const char *name) {
    if (!z->okay || cancelled() || z->count >= FOLIO_WEREAD_CHAPTER_LIMIT + 8 || strlen(name) >= 80)
        return z->okay = false;
    wr_zip_entry_t *e = &z->entries[z->count];
    copy_text(e->name, sizeof(e->name), name);
    long pos = wr_tell(&z->out);
    if (pos < 0 || pos > FOLIO_WEREAD_BOOK_BYTES)
        return z->okay = false;
    e->offset = (uint32_t)pos;
    wr_u32(&z->out, 0x04034b50);
    wr_u16(&z->out, 20);
    wr_u16(&z->out, 0);
    wr_u16(&z->out, 0);
    wr_u16(&z->out, 0);
    wr_u16(&z->out, 33);
    wr_u32(&z->out, 0);
    wr_u32(&z->out, 0);
    wr_u32(&z->out, 0);
    wr_u16(&z->out, (uint16_t)strlen(name));
    wr_u16(&z->out, 0);
    wr_fputs(name, &z->out);
    wr_flush(&z->out);
    z->out.crc = 0;
    z->out.crc_active = true;
    return z->okay = z->out.okay;
}
static bool wr_zip_end(wr_zip_t *z) {
    wr_zip_entry_t *e = &z->entries[z->count];
    long end = wr_tell(&z->out);
    uint32_t start = e->offset + 30 + (uint32_t)strlen(e->name);
    z->out.crc_active = false;
    if (!z->okay || cancelled() || end < (long)start || end > FOLIO_WEREAD_BOOK_BYTES)
        return z->okay = false;
    e->size = (uint32_t)end - start;
    e->crc = z->out.crc;
    if (!wr_seek(&z->out, (long)e->offset + 14))
        return z->okay = false;
    wr_u32(&z->out, e->crc);
    wr_u32(&z->out, e->size);
    wr_u32(&z->out, e->size);
    if (!wr_seek(&z->out, end) || !z->out.okay)
        return z->okay = false;
    ++z->count;
    return true;
}
static bool wr_zip_finish(wr_zip_t *z) {
    long start = wr_tell(&z->out);
    if (!z->okay || start < 0)
        return false;
    for (size_t i = 0; i < z->count; ++i) {
        wr_zip_entry_t *e = &z->entries[i];
        wr_u32(&z->out, 0x02014b50);
        wr_u16(&z->out, 20);
        wr_u16(&z->out, 20);
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 33);
        wr_u32(&z->out, e->crc);
        wr_u32(&z->out, e->size);
        wr_u32(&z->out, e->size);
        wr_u16(&z->out, (uint16_t)strlen(e->name));
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 0);
        wr_u16(&z->out, 0);
        wr_u32(&z->out, 0);
        wr_u32(&z->out, e->offset);
        wr_fputs(e->name, &z->out);
    }
    long end = wr_tell(&z->out);
    if (end < start || end > FOLIO_WEREAD_BOOK_BYTES)
        return false;
    wr_u32(&z->out, 0x06054b50);
    wr_u16(&z->out, 0);
    wr_u16(&z->out, 0);
    wr_u16(&z->out, (uint16_t)z->count);
    wr_u16(&z->out, (uint16_t)z->count);
    wr_u32(&z->out, (uint32_t)(end - start));
    wr_u32(&z->out, (uint32_t)start);
    wr_u16(&z->out, 0);
    return z->out.okay;
}
static bool wr_epub_name(const manifest_t *m, char out[PATH_CAP]) {
    char title[64], hex[127];
    copy_text(title, sizeof(title), m->title);
    for (char *p = title; *p; ++p)
        if ((unsigned char)*p < 32 || strchr("/\\:*?\"<>|", *p))
            *p = '_';
    if (!title[0])
        copy_text(title, sizeof(title), "微信读书");
    if (!id_hex(m->book_id, hex))
        return false;
    int n = snprintf(out, PATH_CAP, BOOK_ROOT "/%s [wr-%s].epub", title, hex);
    return n > 0 && n < PATH_CAP;
}
static bool wr_move_aux(const char *old, const char *hidden) {
    struct stat a, b;
    if (cache_lstat(old, &a))
        return errno == ENOENT;
    if (!ordinary_file(&a) || !writable_path(old) || !writable_path(hidden))
        return false;
    if (cache_lstat(hidden, &b) == 0)
        return false; // Never discard a conflicting legacy record.
    return errno == ENOENT && rename(old, hidden) == 0;
}
static void wr_migrate_cover(const char *legacy, const char *id) {
    const char *ext[] = {".jpg", ".png", ".jpeg"};
    for (unsigned i = 0; i < 3; ++i) {
        char old[PATH_CAP], dest[PATH_CAP], name[24];
        snprintf(old, sizeof(old), "%.*s%s", (int)strlen(legacy) - 4, legacy, ext[i]);
        snprintf(name, sizeof(name), "cover%s", ext[i]);
        if (named_path(dest, id, name))
            (void)wr_move_aux(old, dest);
    }
}
static bool wr_cover_path(const manifest_t *m, char out[PATH_CAP], const char **extension) {
    const char *types[] = {"jpg", "png", "jpeg"};
    for (unsigned i = 0; i < 3; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "cover.%s", types[i]);
        struct stat st;
        if (named_path(out, m->book_id, name) && !cache_lstat(out, &st) && ordinary_file(&st) &&
            st.st_size > 8 && st.st_size <= 2 * 1024 * 1024) {
            *extension = types[i];
            return true;
        }
    }
    out[0] = 0;
    return false;
}
static bool wr_write_chapter(wr_zip_t *z, const manifest_t *m, size_t index) {
    const chapter_record_t *r = &m->records[index];
    chapter_header_t h;
    int source = -1;
    if (!chapter_open_fd(m, r, &source, &h))
        return false;
    bool okay = lseek(source, (off_t)(sizeof(h) + h.source_size), SEEK_SET) ==
                (off_t)(sizeof(h) + h.source_size);
    char name[48];
    snprintf(name, sizeof(name), "OEBPS/c%u.xhtml", (unsigned)index);
    if (okay)
        okay = wr_zip_begin(z, name);
    if (okay) {
        wr_fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?><html "
              "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>",
              &z->out);
        wr_xml(&z->out, r->chapter.title);
        wr_fputs(
            "</title><link rel=\"stylesheet\" href=\"style.css\" type=\"text/css\"/></head><body>",
            &z->out);
        bool paragraph = false;
        uint32_t left = h.text_size;
        uint8_t *bytes = z->copy;
        size_t budget = 0;
        while (okay && left) {
            size_t n = left < z->copy_cap ? left : z->copy_cap;
            if (cancelled() || !read_all(source, bytes, n)) {
#ifdef ESP_PLATFORM
                ESP_LOGW("folio_wr_zip", "chapter %u short read: left=%u n=%u", (unsigned)index,
                         (unsigned)left, (unsigned)n);
#endif
                okay = false;
                break;
            }
            for (size_t k = 0; k < n; ++k) {
                unsigned char c = bytes[k];
                if(c<32 && c!='\t' && c!='\n' && c!='\r')continue;
                if (c == '\r' || c == '\n') {
                    if (paragraph) {
                        wr_fputs("</p>\n", &z->out);
                        paragraph = false;
                    }
                    continue;
                }
                if (!paragraph && (c == ' ' || c == '\t'))
                    continue;
                if (!paragraph) {
                    wr_fputs("<p>", &z->out);
                    paragraph = true;
                }
                if (c == '&')
                    wr_fputs("&amp;", &z->out);
                else if (c == '<')
                    wr_fputs("&lt;", &z->out);
                else if (c == '>')
                    wr_fputs("&gt;", &z->out);
                else
                    wr_fputc(c, &z->out);
            }
            left -= (uint32_t)n;
            if ((budget += n) >= 64 * 1024) { budget = 0; cooperate(); }
        }
        if (paragraph)
            wr_fputs("</p>", &z->out);
        wr_fputs("</body></html>", &z->out);
    }
    if (close(source)) {
#ifdef ESP_PLATFORM
        ESP_LOGE("folio_wr_zip", "chapter %u close failed", (unsigned)index);
#endif
        okay = false;
    }
    return okay && wr_zip_end(z);
}
static bool wr_write_epub(int fd, const manifest_t *m, const char *cover, const char *ext) {
    wr_zip_t z;
    memset(&z, 0, sizeof(z));
    z.out.fd = fd;
    z.out.okay = true;
    z.okay = true;
    z.entries = allocate((FOLIO_WEREAD_CHAPTER_LIMIT + 8) * sizeof(*z.entries));
    if (!z.entries)
        return false;
    uint8_t *io = allocate(WR_BUF + WR_COPY);
    if (!io) {
        free(z.entries);
        return false;
    }
    z.out.buf = io;
    z.out.cap = WR_BUF;
    z.copy = io + WR_BUF;
    z.copy_cap = WR_COPY;
    size_t count = 0;
    for (size_t i = 0; i < m->count; ++i)
        if (m->records[i].key && m->records[i].text_size)
            ++count;
    bool okay = wr_zip_begin(&z, "mimetype");
    if (okay) {
        wr_fputs("application/epub+zip", &z.out);
        okay = wr_zip_end(&z);
    }
    if (okay)
        okay = wr_zip_begin(&z, "META-INF/container.xml");
    if (okay) {
        wr_fputs("<?xml version=\"1.0\"?><container version=\"1.0\" "
              "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
              "full-path=\"OEBPS/content.opf\" "
              "media-type=\"application/oebps-package+xml\"/></rootfiles></container>",
              &z.out);
        okay = wr_zip_end(&z);
    }
    if (okay)
        okay = wr_zip_begin(&z, "OEBPS/style.css");
    if (okay) {
        wr_fputs("body{margin:0;padding:0}p{margin:0 0 "
              ".45em;text-indent:2em;line-height:1.5}img{max-width:100%;max-height:100%}",
              &z.out);
        okay = wr_zip_end(&z);
    }
    if (okay)
        okay = wr_zip_begin(&z, "OEBPS/content.opf");
    if (okay) {
        wr_fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?><package version=\"2.0\" "
              "unique-identifier=\"bookid\" xmlns=\"http://www.idpf.org/2007/opf\"><metadata "
              "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>",
              &z.out);
        wr_xml(&z.out, m->title);
        wr_fputs("</dc:title><dc:creator>", &z.out);
        wr_xml(&z.out, m->author);
        wr_fputs("</dc:creator><dc:language>zh-CN</dc:language><dc:identifier "
                 "id=\"bookid\">urn:folio:weread:", &z.out);
        wr_xml(&z.out, m->book_id);
        wr_printf(&z.out, "</dc:identifier><dc:description>已下载 %u / %u 章</dc:description>",
                  (unsigned)count, (unsigned)m->count);
        if (cover[0])
            wr_fputs("<meta name=\"cover\" content=\"cover-image\"/>", &z.out);
        wr_fputs("</metadata><manifest><item id=\"ncx\" href=\"toc.ncx\" "
              "media-type=\"application/x-dtbncx+xml\"/><item id=\"css\" href=\"style.css\" "
              "media-type=\"text/css\"/>",
              &z.out);
        if (cover[0])
            wr_printf(&z.out,
                    "<item id=\"cover-image\" href=\"cover.%s\" media-type=\"image/%s\"/><item "
                    "id=\"cover\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>",
                    ext, !strcmp(ext, "png") ? "png" : "jpeg");
        for (size_t i = 0; i < m->count; ++i)
            if (m->records[i].key && m->records[i].text_size)
                wr_printf(
                    &z.out,
                    "<item id=\"c%u\" href=\"c%u.xhtml\" media-type=\"application/xhtml+xml\"/>",
                    (unsigned)i, (unsigned)i);
        wr_fputs("</manifest><spine toc=\"ncx\">", &z.out);
        if (cover[0])
            wr_fputs("<itemref idref=\"cover\"/>", &z.out);
        for (size_t i = 0; i < m->count; ++i)
            if (m->records[i].key && m->records[i].text_size)
                wr_printf(&z.out, "<itemref idref=\"c%u\"/>", (unsigned)i);
        wr_fputs("</spine>", &z.out);
        if (cover[0])
            wr_fputs("<guide><reference type=\"cover\" title=\"封面\" href=\"cover.xhtml\"/></guide>",
                  &z.out);
        wr_fputs("</package>", &z.out);
        okay = wr_zip_end(&z);
    }
    if (okay)
        okay = wr_zip_begin(&z, "OEBPS/toc.ncx");
    if (okay) {
        wr_fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?><ncx "
                 "xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head><meta "
                 "name=\"dtb:uid\" content=\"urn:folio:weread:", &z.out);
        wr_xml(&z.out, m->book_id);
        wr_fputs("\"/><meta name=\"dtb:depth\" content=\"1\"/><meta "
                 "name=\"dtb:totalPageCount\" content=\"0\"/><meta "
                 "name=\"dtb:maxPageNumber\" content=\"0\"/></head><docTitle><text>", &z.out);
        wr_xml(&z.out, m->title);
        wr_fputs("</text></docTitle><navMap>", &z.out);
        unsigned order = 0;
        for (size_t i = 0; i < m->count; ++i)
            if (m->records[i].key && m->records[i].text_size) {
                wr_printf(&z.out, "<navPoint id=\"c%u\" playOrder=\"%u\"><navLabel><text>", (unsigned)i,
                        ++order);
                wr_xml(&z.out, m->records[i].chapter.title);
                wr_printf(&z.out, "</text></navLabel><content src=\"c%u.xhtml\"/></navPoint>",
                        (unsigned)i);
            }
        wr_fputs("</navMap></ncx>", &z.out);
        okay = wr_zip_end(&z);
    }
    if (okay && cover[0]) {
        char name[64];
        snprintf(name, sizeof(name), "OEBPS/cover.%s", ext);
        okay = wr_zip_begin(&z, name);
        int f = okay ? open(cover, O_RDONLY) : -1;
        if (f < 0)
            okay = false;
        if (f >= 0) {
            uint8_t bytes[512];
            ssize_t n;
            while ((n = read(f, bytes, sizeof(bytes))) > 0) {
                if (cancelled()) {
                    okay = false;
                    break;
                }
                wr_raw(&z.out, bytes, (size_t)n);
                if (!z.out.okay) {
                    okay = false;
                    break;
                }
                cooperate();
            }
            if (n < 0)
                okay = false;
            if (close(f))
                okay = false;
        }
        if (okay)
            okay = wr_zip_end(&z);
        if (okay)
            okay = wr_zip_begin(&z, "OEBPS/cover.xhtml");
        if (okay) {
            wr_fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?><html "
                     "xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>封面</title></"
                     "head><body><div><img src=\"cover.", &z.out);
            wr_xml(&z.out, ext);
            wr_fputs("\" alt=\"封面\" style=\"max-width:100%;max-height:100%\"/>"
                     "</div></body></html>", &z.out);
            okay = wr_zip_end(&z);
        }
    }
    size_t written = 0;
    for (size_t i = 0; okay && i < m->count; ++i)
        if (m->records[i].key && m->records[i].text_size) {
            okay = wr_write_chapter(&z, m, i);
            if (okay)
                folio_weread_export_progress(++written, count);
        }
    if (okay)
        okay = wr_zip_finish(&z);
    wr_flush(&z.out);
    if (!z.out.okay)
        okay = false;
    free(io);
    free(z.entries);
    return okay;
}
bool folio_weread_cache_public_path(const char *path, char *out, size_t cap) {
    if (!out || !cap || !folio_storage_lock(WAIT_MS))
        return false;
    manifest_t *m = open_manifest(path, NULL, 0, false);
    char target[PATH_CAP];
    struct stat st;
    bool okay = m && wr_epub_name(m, target) && strlen(target) < cap && !cache_lstat(target, &st) &&
                ordinary_file(&st);
    if (okay)
        copy_text(out, cap, target);
    free(m);
    folio_storage_unlock();
    return okay;
}
bool folio_weread_cache_export(const char *path, char *out, size_t out_cap, char *error,
                               size_t cap) {
    clear_error(error, cap);
    if (!out || !out_cap)
        return false;
    LOCK();
    manifest_t *m = open_manifest(path, error, cap, true);
    bool okay = false;
    char final[PATH_CAP], part[PATH_CAP], backup[PATH_CAP], cover[PATH_CAP], legacy[PATH_CAP],
        hidden[PATH_CAP];
    const char *ext = "jpg";
    if (!m || !wr_epub_name(m, final) || strlen(final) >= out_cap ||
        !book_path(legacy, m->book_id) || !named_path(part, m->book_id, "export.part") ||
        !named_path(backup, m->book_id, "export.bak") ||
        !named_path(hidden, m->book_id, "entry.wrb"))
        goto done;
    wr_migrate_cover(legacy, m->book_id);
    (void)wr_cover_path(m, cover, &ext);
    if (!writable_path(part) || !writable_path(final) || !writable_path(backup))
        goto done;
    struct stat st;
    if (cache_lstat(backup, &st) == 0) {
        if (cache_lstat(final, &st) != 0) {
            if (rename(backup, final))
                goto done;
        } else if (unlink(backup))
            goto done;
    }
    int f = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0)
        goto done;
    bool wrote = wr_write_epub(f, m, cover, ext);
    bool closed = fsync(f) == 0;
    if (close(f))
        closed = false;
    if (!wrote || !closed || cancelled()) {
        unlink(part);
        goto done;
    }
    bool existed = cache_lstat(final, &st) == 0;
    if (existed && rename(final, backup)) {
        unlink(part);
        goto done;
    }
    if (rename(part, final)) {
        if (existed)
            (void)rename(backup, final);
        goto done;
    }
    if (existed)
        (void)unlink(backup);
    /* Preserve the exact pointer bytes: old reader fingerprints still match. */
    if (cache_lstat(hidden, &st) != 0) {
        if (!wr_move_aux(legacy, hidden))
            goto done;
    } else if (cache_lstat(legacy, &st) == 0) {
        pointer_t a[2], b[2];
        struct stat hidden_stat;
        size_t bytes = (size_t)st.st_size;
        if ((bytes == sizeof(pointer_t) || bytes == sizeof(a)) &&
            !cache_lstat(hidden, &hidden_stat) && hidden_stat.st_size == st.st_size &&
            read_exact(legacy, a, bytes) && read_exact(hidden, b, bytes) && !memcmp(a, b, bytes))
            (void)unlink(legacy);
    }
    for (char slot = 'a'; slot <= 'b'; ++slot) {
        char old[288], dest[PATH_CAP], name[16];
        snprintf(old, sizeof(old), "%s.wrt.%c", legacy, slot);
        snprintf(name, sizeof(name), "time.%c", slot);
        if (named_path(dest, m->book_id, name))
            (void)wr_move_aux(old, dest);
    }
    copy_text(out, out_cap, final);
    okay = true;
done:
    free(m);
    folio_storage_unlock();
    if (!okay && error && cap && !*error)
        fail(error, cap, "EPUB 未完成，已下载章节仍保留，请重试。");
    return okay;
}
void folio_weread_cache_migrate(void) {
    if (!folio_storage_lock(WAIT_MS))
        return;
    DIR *pending = opendir(CACHE_ROOT);
    if (pending) {
        struct dirent *e;
        while ((e = readdir(pending))) {
            char logical[PATH_CAP], id[64], backup[PATH_CAP];
            int n = snprintf(logical, sizeof(logical), BOOK_ROOT "/wr-%s.wrb", e->d_name);
            if (n <= 0 || n >= PATH_CAP || !folio_wr_path_id(logical, id, sizeof(id)) ||
                !named_path(backup, id, "export.bak"))
                continue;
            struct stat st;
            if (cache_lstat(backup, &st) || !ordinary_file(&st))
                continue;
            manifest_t *m = open_manifest(logical, NULL, 0, false);
            char final[PATH_CAP];
            if (m && wr_epub_name(m, final) && writable_path(final) && cache_lstat(final, &st) &&
                errno == ENOENT)
                (void)rename(backup, final);
            free(m);
        }
        closedir(pending);
    }
    DIR *d = opendir(BOOK_ROOT);
    if (d) {
        struct dirent *e;
        while (!cancelled() && (e = readdir(d))) {
            char path[PATH_CAP], id[64], out[PATH_CAP];
            int n = snprintf(path, sizeof(path), BOOK_ROOT "/%s", e->d_name);
            if (n > 0 && n < PATH_CAP && strstr(e->d_name, ".wrb") &&
                folio_wr_path_id(path, id, sizeof(id)))
                (void)folio_weread_cache_export(path, out, sizeof(out), NULL, 0);
        }
        closedir(d);
    }
    folio_storage_unlock();
}
