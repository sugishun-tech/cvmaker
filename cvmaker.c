#define _GNU_SOURCE
#include <cairo.h>
#include <cairo-pdf.h>
#include <pango/pangocairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_FONT_SIZE 12.0
#define DEFAULT_LINE_WIDTH 0.5
#define LOGICAL_DPI 75.0
#define PDF_PT_PER_INCH 72.0
#define PAGE_W_PT (210.0 / 25.4 * PDF_PT_PER_INCH)
#define PAGE_H_PT (297.0 / 25.4 * PDF_PT_PER_INCH)
#define PAGE_MARGIN_PT 36.0
#define CONTENT_W_PT (PAGE_W_PT - PAGE_MARGIN_PT * 2.0)
#define CONTENT_H_PT (PAGE_H_PT - PAGE_MARGIN_PT * 2.0)

#define MINCHO_FAMILY "IPAexMincho"
#define GOTHIC_FAMILY "IPAexGothic"

typedef struct { char *key; char *value; } Pair;

typedef struct {
    Pair *items;
    size_t len;
    size_t cap;
} Map;

typedef struct {
    Map *items;
    size_t len;
    size_t cap;
} List;

typedef struct { char *name; List list; } NamedList;

typedef struct {
    Map scalar;
    NamedList *lists;
    size_t lists_len;
    size_t lists_cap;
} CVData;

typedef struct {
    cairo_surface_t *surface;
    cairo_t *cr;
    CVData data;
    char input_dir[PATH_MAX];
} CVMaker;

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) { perror("strdup"); exit(1); }
    return p;
}

static char *trim_inplace(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}


static int is_absolute_path(const char *path) {
    return path && path[0] == '/';
}

static void dirname_copy(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !*path) {
        snprintf(out, out_size, ".");
        return;
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, out_size, "/");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static char *join_path_alloc(const char *dir, const char *file) {
    if (!dir || !*dir || strcmp(dir, ".") == 0) return xstrdup(file);
    size_t dl = strlen(dir);
    size_t fl = strlen(file);
    int need_slash = dir[dl - 1] != '/';
    char *r = malloc(dl + (size_t)need_slash + fl + 1);
    if (!r) { perror("malloc"); exit(1); }
    memcpy(r, dir, dl);
    if (need_slash) r[dl++] = '/';
    memcpy(r + dl, file, fl + 1);
    return r;
}

static void map_put(Map *m, const char *key, const char *value) {
    for (size_t i = 0; i < m->len; i++) {
        if (strcmp(m->items[i].key, key) == 0) {
            free(m->items[i].value);
            m->items[i].value = xstrdup(value);
            return;
        }
    }
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->items = realloc(m->items, sizeof(Pair) * m->cap);
        if (!m->items) { perror("realloc"); exit(1); }
    }
    m->items[m->len].key = xstrdup(key);
    m->items[m->len].value = xstrdup(value);
    m->len++;
}

static const char *map_get(const Map *m, const char *key) {
    for (size_t i = 0; i < m->len; i++) {
        if (strcmp(m->items[i].key, key) == 0) return m->items[i].value;
    }
    return "";
}

static int map_has(const Map *m, const char *key) {
    for (size_t i = 0; i < m->len; i++) {
        if (strcmp(m->items[i].key, key) == 0) return 1;
    }
    return 0;
}

static Map *list_add_item(List *l) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, sizeof(Map) * l->cap);
        if (!l->items) { perror("realloc"); exit(1); }
    }
    memset(&l->items[l->len], 0, sizeof(Map));
    return &l->items[l->len++];
}

static NamedList *data_add_list(CVData *d, const char *name) {
    if (d->lists_len == d->lists_cap) {
        d->lists_cap = d->lists_cap ? d->lists_cap * 2 : 8;
        d->lists = realloc(d->lists, sizeof(NamedList) * d->lists_cap);
        if (!d->lists) { perror("realloc"); exit(1); }
    }
    NamedList *nl = &d->lists[d->lists_len++];
    nl->name = xstrdup(name);
    memset(&nl->list, 0, sizeof(List));
    return nl;
}

static const List *data_get_list(const CVData *d, const char *name) {
    for (size_t i = 0; i < d->lists_len; i++) {
        if (strcmp(d->lists[i].name, name) == 0) return &d->lists[i].list;
    }
    return NULL;
}

static void free_map(Map *m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->items[i].key);
        free(m->items[i].value);
    }
    free(m->items);
}

static void free_data(CVData *d) {
    free_map(&d->scalar);
    for (size_t i = 0; i < d->lists_len; i++) {
        free(d->lists[i].name);
        for (size_t j = 0; j < d->lists[i].list.len; j++) free_map(&d->lists[i].list.items[j]);
        free(d->lists[i].list.items);
    }
    free(d->lists);
}

static char **read_lines(const char *filename, size_t *nlines) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { perror(filename); exit(1); }
    char **lines = NULL;
    size_t len = 0, cap = 0;
    char *line = NULL;
    size_t n = 0;
    while (getline(&line, &n, fp) >= 0) {
        if (len == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, sizeof(char *) * cap);
            if (!lines) { perror("realloc"); exit(1); }
        }
        lines[len++] = xstrdup(line);
    }
    free(line);
    fclose(fp);
    *nlines = len;
    return lines;
}

static int has_top_key(const char *line, char **key, char **value) {
    if (line[0] == ' ' || line[0] == '\t') return 0;
    char *tmp = xstrdup(line);
    char *p = strchr(tmp, ':');
    if (!p) { free(tmp); return 0; }
    *p = '\0';
    char *k = trim_inplace(tmp);
    char *v = trim_inplace(p + 1);
    if (*k == '\0' || *k == '#') { free(tmp); return 0; }
    *key = xstrdup(k);
    *value = xstrdup(v);
    free(tmp);
    return 1;
}

static void parse_yaml_file(const char *filename, CVData *d) {
    size_t n = 0;
    char **lines = read_lines(filename, &n);
    for (size_t i = 0; i < n; i++) {
        char *raw = lines[i];
        char *line = trim_inplace(raw);
        if (*line == '\0' || *line == '#') continue;

        char *key = NULL, *value = NULL;
        if (!has_top_key(raw, &key, &value)) continue;

        if (strcmp(value, "|") == 0) {
            size_t cap = 256, len = 0;
            char *buf = malloc(cap);
            if (!buf) { perror("malloc"); exit(1); }
            buf[0] = '\0';
            while (i + 1 < n) {
                char *next = lines[i + 1];
                if (next[0] != ' ' && next[0] != '\t' && trim_inplace(next)[0] != '\0') break;
                i++;
                char *content = lines[i];
                if (starts_with(content, "  ")) content += 2;
                size_t add = strlen(content);
                if (len + add + 1 > cap) {
                    while (len + add + 1 > cap) cap *= 2;
                    buf = realloc(buf, cap);
                    if (!buf) { perror("realloc"); exit(1); }
                }
                memcpy(buf + len, content, add + 1);
                len += add;
            }
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
            map_put(&d->scalar, key, buf);
            free(buf);
        } else if (*value == '\0') {
            NamedList *nl = data_add_list(d, key);
            Map *cur = NULL;
            while (i + 1 < n) {
                char *next_raw = lines[i + 1];
                char *next_trim = trim_inplace(next_raw);
                if (*next_trim == '\0' || *next_trim == '#') { i++; continue; }
                if (next_raw[0] != ' ' && next_raw[0] != '\t') break;
                i++;
                if (starts_with(next_trim, "-")) {
                    cur = list_add_item(&nl->list);
                    char *after = trim_inplace(next_trim + 1);
                    if (*after) {
                        char *p = strchr(after, ':');
                        if (p) {
                            *p = '\0';
                            map_put(cur, trim_inplace(after), trim_inplace(p + 1));
                        }
                    }
                } else if (cur) {
                    char *p = strchr(next_trim, ':');
                    if (p) {
                        *p = '\0';
                        map_put(cur, trim_inplace(next_trim), trim_inplace(p + 1));
                    }
                }
            }
        } else {
            map_put(&d->scalar, key, value);
        }
        free(key);
        free(value);
    }
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

static double size_to_pt75(const char *s) {
    if (!s) return 0.0;
    char *end = NULL;
    double v = strtod(s, &end);
    while (end && isspace((unsigned char)*end)) end++;
    if (end && starts_with(end, "mm")) return v / 25.4 * LOGICAL_DPI;
    if (end && starts_with(end, "cm")) return v / 25.4 * LOGICAL_DPI * 10.0;
    if (end && starts_with(end, "px")) return v;
    return v;
}

static int utf8_chars(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

static const char *field(const Map *h, const char *key) {
    return map_get(h, key);
}

static double field_size(const Map *h, const char *key) {
    return size_to_pt75(field(h, key));
}

static const char *resolve_value(CVMaker *m, const Map *h) {
    const char *v = field(h, "value");
    if (v[0] == '$') return map_get(&m->data.scalar, v + 1);
    return v;
}

static const char *font_family(const Map *h) {
    const char *face = map_has(h, "font_face") ? field(h, "font_face") : "mincho";
    if (strcmp(face, "gothic") == 0) return GOTHIC_FAMILY;
    if (strcmp(face, "mincho") == 0) return MINCHO_FAMILY;
    return face;
}

static double font_size(const Map *h) {
    return map_has(h, "font_size") ? atof(field(h, "font_size")) : DEFAULT_FONT_SIZE;
}

static void apply_line_style(CVMaker *m, const Map *h) {
    double lw = map_has(h, "line_width") ? atof(field(h, "line_width")) : DEFAULT_LINE_WIDTH;
    cairo_set_line_width(m->cr, lw);
    if (map_has(h, "line_style") && strcmp(field(h, "line_style"), "dashed") == 0) {
        double dashes[] = { DEFAULT_LINE_WIDTH, DEFAULT_LINE_WIDTH };
        cairo_set_dash(m->cr, dashes, 2, 0);
    } else {
        cairo_set_dash(m->cr, NULL, 0, 0);
    }
    cairo_set_source_rgb(m->cr, 0, 0, 0);
}

static double px(double x) { return PAGE_MARGIN_PT + x; }
static double cy(double y) { return PAGE_H_PT - PAGE_MARGIN_PT - y; }

static void draw_text(CVMaker *m, double x, double y, double w, double h, const char *text, double fs, const char *family) {
    if (!text || text[0] == '\0') return;
    cairo_save(m->cr);
    cairo_translate(m->cr, px(x), cy(y));

    PangoLayout *layout = pango_cairo_create_layout(m->cr);
    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, family);
    pango_font_description_set_absolute_size(desc, fs * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
    pango_layout_set_height(layout, (int)(h * PANGO_SCALE));
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_cairo_show_layout(m->cr, layout);

    pango_font_description_free(desc);
    g_object_unref(layout);
    cairo_restore(m->cr);
}

static void cmd_string(CVMaker *m, const Map *h) {
    const char *s = resolve_value(m, h);
    double fs = font_size(h);
    double w = fs * utf8_chars(s) * 1.1 + fs;
    draw_text(m, field_size(h, "x"), field_size(h, "y"), w, fs * 1.8, s, fs, font_family(h));
}

static void put_string(CVMaker *m, double x, double y, const char *s, double fs, const char *family) {
    double w = fs * utf8_chars(s) * 1.1 + fs;
    draw_text(m, x, y, w, fs * 1.8, s, fs, family);
}

static void cmd_textbox(CVMaker *m, const Map *h) {
    draw_text(m, field_size(h, "x"), field_size(h, "y"), field_size(h, "width"), field_size(h, "height"),
              resolve_value(m, h), font_size(h), font_family(h));
}

static void cmd_box(CVMaker *m, const Map *h) {
    apply_line_style(m, h);
    double x = field_size(h, "x"), y = field_size(h, "y"), w = field_size(h, "width"), ht = field_size(h, "height");
    cairo_move_to(m->cr, px(x), cy(y));
    cairo_line_to(m->cr, px(x + w), cy(y));
    cairo_line_to(m->cr, px(x + w), cy(y + ht));
    cairo_line_to(m->cr, px(x), cy(y + ht));
    cairo_close_path(m->cr);
    cairo_stroke(m->cr);
}

static void cmd_line(CVMaker *m, const Map *h) {
    apply_line_style(m, h);
    double x = field_size(h, "x"), y = field_size(h, "y"), dx = field_size(h, "dx"), dyv = field_size(h, "dy");
    cairo_move_to(m->cr, px(x), cy(y));
    cairo_line_to(m->cr, px(x + dx), cy(y + dyv));
    cairo_stroke(m->cr);
}

static void cmd_multi_lines(CVMaker *m, const Map *h) {
    apply_line_style(m, h);
    double x = field_size(h, "x"), y = field_size(h, "y"), dx = field_size(h, "dx"), dyv = field_size(h, "dy");
    double sx = field_size(h, "sx"), sy = field_size(h, "sy");
    int num = atoi(field(h, "num"));
    for (int i = 0; i < num; i++) {
        cairo_move_to(m->cr, px(x), cy(y));
        cairo_line_to(m->cr, px(x + dx), cy(y + dyv));
        cairo_stroke(m->cr);
        x += sx;
        y += sy;
    }
}

static void cmd_lines(CVMaker *m, char **tok, int ntok, const Map *h) {
    apply_line_style(m, h);
    if (ntok < 5) return;
    int n = atoi(tok[1]);
    double x = size_to_pt75(tok[2]);
    double y = size_to_pt75(tok[3]);
    cairo_move_to(m->cr, px(x), cy(y));
    int k = 4;
    for (int i = 1; i < n && k + 1 < ntok; i++, k += 2) {
        x += size_to_pt75(tok[k]);
        y += size_to_pt75(tok[k + 1]);
        cairo_line_to(m->cr, px(x), cy(y));
    }
    if (map_has(h, "close")) cairo_close_path(m->cr);
    cairo_stroke(m->cr);
}

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pix, unsigned char **out_buf) {
    int width = gdk_pixbuf_get_width(pix);
    int height = gdk_pixbuf_get_height(pix);
    int src_stride = gdk_pixbuf_get_rowstride(pix);
    int channels = gdk_pixbuf_get_n_channels(pix);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pix);
    const guchar *src = gdk_pixbuf_get_pixels(pix);
    int dst_stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    unsigned char *dst = calloc((size_t)dst_stride, (size_t)height);
    if (!dst) { perror("calloc"); exit(1); }

    for (int y = 0; y < height; y++) {
        const guchar *srow = src + (size_t)y * src_stride;
        unsigned char *drow = dst + (size_t)y * dst_stride;
        for (int x = 0; x < width; x++) {
            const guchar *sp = srow + (size_t)x * channels;
            unsigned int r = sp[0];
            unsigned int g = sp[1];
            unsigned int b = sp[2];
            unsigned int a = has_alpha ? sp[3] : 255;
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
            unsigned char *dp = drow + (size_t)x * 4;
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            dp[0] = (unsigned char)b;
            dp[1] = (unsigned char)g;
            dp[2] = (unsigned char)r;
            dp[3] = (unsigned char)a;
#else
            dp[0] = (unsigned char)a;
            dp[1] = (unsigned char)r;
            dp[2] = (unsigned char)g;
            dp[3] = (unsigned char)b;
#endif
        }
    }

    *out_buf = dst;
    return cairo_image_surface_create_for_data(dst, CAIRO_FORMAT_ARGB32, width, height, dst_stride);
}

static void cmd_photo(CVMaker *m, const Map *h) {
    const char *file = map_get(&m->data.scalar, "photo");
    if (!file || !*file) return;

    char *resolved = NULL;
    GError *err = NULL;
    GdkPixbuf *pix = NULL;

    if (is_absolute_path(file)) {
        resolved = xstrdup(file);
        pix = gdk_pixbuf_new_from_file(resolved, &err);
    } else {
        pix = gdk_pixbuf_new_from_file(file, &err);
        if (pix) {
            resolved = xstrdup(file);
        } else {
            if (err) { g_error_free(err); err = NULL; }
            resolved = join_path_alloc(m->input_dir, file);
            pix = gdk_pixbuf_new_from_file(resolved, &err);
        }
    }

    if (!pix) {
        fprintf(stderr, "photo skipped: %s (tried: %s, and relative to input dir: %s)\n",
                err ? err->message : file, file, resolved ? resolved : "");
        if (err) g_error_free(err);
        free(resolved);
        return;
    }

    unsigned char *buf = NULL;
    cairo_surface_t *img = pixbuf_to_cairo_surface(pix, &buf);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "photo skipped: cairo image surface creation failed\n");
        cairo_surface_destroy(img);
        free(buf);
        g_object_unref(pix);
        free(resolved);
        return;
    }

    double x = field_size(h, "x"), y = field_size(h, "y"), w = field_size(h, "width"), ht = field_size(h, "height");
    cairo_save(m->cr);
    cairo_translate(m->cr, px(x), cy(y));
    cairo_scale(m->cr, w / gdk_pixbuf_get_width(pix), ht / gdk_pixbuf_get_height(pix));
    cairo_set_source_surface(m->cr, img, 0, 0);
    cairo_paint(m->cr);
    cairo_restore(m->cr);

    cairo_surface_destroy(img);
    free(buf);
    g_object_unref(pix);
    free(resolved);
}

static void draw_history_item(CVMaker *m, double *y, double year_x, double month_x, double value_x, double dy, const Map *row, double fs, const char *family) {
    const char *year = map_get(row, "year");
    const char *month = map_get(row, "month");
    const char *value = map_get(row, "value");
    put_string(m, year_x, *y, year, fs, family);
    double mx = month_x - (utf8_chars(month) - 1) * fs * 0.3;
    put_string(m, mx, *y, month, fs, family);
    put_string(m, value_x, *y, value, fs, family);
    *y += dy;
}

static void cmd_education_experience(CVMaker *m, const Map *h) {
    double y = field_size(h, "y");
    double year_x = field_size(h, "year_x"), month_x = field_size(h, "month_x"), value_x = field_size(h, "value_x");
    double dy = -field_size(h, "dy");
    double caption_x = field_size(h, "caption_x"), ijo_x = field_size(h, "ijo_x");
    double fs = font_size(h);
    const char *family = font_family(h);

    put_string(m, caption_x, y, "学歴", fs, family);
    y += dy;
    const List *edu = data_get_list(&m->data, "education");
    if (edu) for (size_t i = 0; i < edu->len; i++) draw_history_item(m, &y, year_x, month_x, value_x, dy, &edu->items[i], fs, family);
    put_string(m, caption_x, y, "職歴", fs, family);
    y += dy;
    const List *exp = data_get_list(&m->data, "experience");
    if (exp) for (size_t i = 0; i < exp->len; i++) draw_history_item(m, &y, year_x, month_x, value_x, dy, &exp->items[i], fs, family);
    put_string(m, ijo_x, y, "以上", fs, family);
}

static void cmd_history(CVMaker *m, const Map *h) {
    const char *v = field(h, "value");
    if (v[0] != '$') return;
    const List *list = data_get_list(&m->data, v + 1);
    if (!list) return;
    double y = field_size(h, "y");
    double year_x = field_size(h, "year_x"), month_x = field_size(h, "month_x"), value_x = field_size(h, "value_x");
    double dy = field_size(h, "dy");
    double fs = font_size(h);
    const char *family = font_family(h);
    for (size_t i = 0; i < list->len; i++) draw_history_item(m, &y, year_x, month_x, value_x, dy, &list->items[i], fs, family);
}

static char **split_csv_simple(const char *line, int *count) {
    char *tmp = xstrdup(line);
    int cap = 16, n = 0;
    char **tok = malloc(sizeof(char *) * cap);
    if (!tok) { perror("malloc"); exit(1); }
    char *p = tmp;
    while (1) {
        if (n == cap) {
            cap *= 2;
            tok = realloc(tok, sizeof(char *) * cap);
            if (!tok) { perror("realloc"); exit(1); }
        }
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        tok[n++] = xstrdup(trim_inplace(p));
        if (!comma) break;
        p = comma + 1;
    }
    free(tmp);
    *count = n;
    return tok;
}

static void free_tokens(char **tok, int n) {
    for (int i = 0; i < n; i++) free(tok[i]);
    free(tok);
}

static void rest_to_map(Map *h, char **tok, int from, int ntok) {
    for (int i = from; i < ntok; i++) {
        char *p = strchr(tok[i], '=');
        if (!p) continue;
        *p = '\0';
        map_put(h, trim_inplace(tok[i]), trim_inplace(p + 1));
    }
}

static void execute_style_line(CVMaker *m, const char *line) {
    int ntok = 0;
    char **tok = split_csv_simple(line, &ntok);
    if (ntok == 0) { free_tokens(tok, ntok); return; }

    Map h = {0};
    const char *type = tok[0];
    if (strcmp(type, "string") == 0 && ntok >= 4) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "value", tok[3]); rest_to_map(&h, tok, 4, ntok); cmd_string(m, &h);
    } else if (strcmp(type, "box") == 0 && ntok >= 5) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "width", tok[3]); map_put(&h, "height", tok[4]); rest_to_map(&h, tok, 5, ntok); cmd_box(m, &h);
    } else if (strcmp(type, "line") == 0 && ntok >= 5) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "dx", tok[3]); map_put(&h, "dy", tok[4]); rest_to_map(&h, tok, 5, ntok); cmd_line(m, &h);
    } else if (strcmp(type, "multi_lines") == 0 && ntok >= 8) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "dx", tok[3]); map_put(&h, "dy", tok[4]);
        map_put(&h, "num", tok[5]); map_put(&h, "sx", tok[6]); map_put(&h, "sy", tok[7]); rest_to_map(&h, tok, 8, ntok); cmd_multi_lines(m, &h);
    } else if (strcmp(type, "lines") == 0) {
        int n = ntok >= 2 ? atoi(tok[1]) : 0;
        int attr_from = 4 + (n - 1) * 2;
        if (attr_from < 4) attr_from = 4;
        if (attr_from > ntok) attr_from = ntok;
        rest_to_map(&h, tok, attr_from, ntok);
        cmd_lines(m, tok, ntok, &h);
    } else if (strcmp(type, "photo") == 0 && ntok >= 5) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "width", tok[3]); map_put(&h, "height", tok[4]); cmd_photo(m, &h);
    } else if (strcmp(type, "education_experience") == 0 && ntok >= 8) {
        map_put(&h, "y", tok[1]); map_put(&h, "year_x", tok[2]); map_put(&h, "month_x", tok[3]); map_put(&h, "value_x", tok[4]);
        map_put(&h, "dy", tok[5]); map_put(&h, "caption_x", tok[6]); map_put(&h, "ijo_x", tok[7]); rest_to_map(&h, tok, 8, ntok); cmd_education_experience(m, &h);
    } else if (strcmp(type, "history") == 0 && ntok >= 7) {
        map_put(&h, "y", tok[1]); map_put(&h, "year_x", tok[2]); map_put(&h, "month_x", tok[3]); map_put(&h, "value_x", tok[4]);
        map_put(&h, "dy", tok[5]); map_put(&h, "value", tok[6]); rest_to_map(&h, tok, 7, ntok); cmd_history(m, &h);
    } else if (strcmp(type, "textbox") == 0 && ntok >= 6) {
        map_put(&h, "x", tok[1]); map_put(&h, "y", tok[2]); map_put(&h, "width", tok[3]); map_put(&h, "height", tok[4]); map_put(&h, "value", tok[5]); rest_to_map(&h, tok, 6, ntok); cmd_textbox(m, &h);
    } else if (strcmp(type, "new_page") == 0) {
        cairo_show_page(m->cr);
    } else {
        fprintf(stderr, "unknown or invalid style command: %s\n", line);
    }

    free_map(&h);
    free_tokens(tok, ntok);
}

static void execute_style_file(CVMaker *m, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { perror(filename); exit(1); }
    char *line = NULL;
    size_t n = 0;
    while (getline(&line, &n, fp) >= 0) {
        char *s = trim_inplace(line);
        if (*s == '\0' || *s == '#') continue;
        execute_style_line(m, s);
    }
    free(line);
    fclose(fp);
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [-i data.yaml] [-s style.txt] [-o output.pdf]\n", prog);
}

int main(int argc, char **argv) {
    const char *input = "data.yaml";
    const char *style = "style.txt";
    const char *output = "output.pdf";

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--input") == 0) && i + 1 < argc) input = argv[++i];
        else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--style") == 0) && i + 1 < argc) style = argv[++i];
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) output = argv[++i];
        else { usage(argv[0]); return 2; }
    }

    CVMaker m;
    memset(&m, 0, sizeof(m));
    dirname_copy(input, m.input_dir, sizeof(m.input_dir));
    parse_yaml_file(input, &m.data);

    m.surface = cairo_pdf_surface_create(output, PAGE_W_PT, PAGE_H_PT);
    if (cairo_surface_status(m.surface) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "failed to create PDF: %s\n", cairo_status_to_string(cairo_surface_status(m.surface)));
        free_data(&m.data);
        return 1;
    }
    m.cr = cairo_create(m.surface);
    cairo_set_source_rgb(m.cr, 0, 0, 0);

    printf("input  file: %s\n", input);
    printf("style  file: %s\n", style);
    printf("output file: %s\n", output);
    printf("page size : %.2fpt x %.2fpt (A4), margins %.2fpt, content %.2fpt x %.2fpt\n",
           PAGE_W_PT, PAGE_H_PT, PAGE_MARGIN_PT, CONTENT_W_PT, CONTENT_H_PT);

    execute_style_file(&m, style);

    cairo_destroy(m.cr);
    cairo_surface_finish(m.surface);
    cairo_surface_destroy(m.surface);
    free_data(&m.data);
    puts("Done.");
    return 0;
}
