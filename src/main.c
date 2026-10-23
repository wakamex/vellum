// vellum: a Markdown viewer and editor on gesso.
//   vellum [FILE.md]            open a file (or drop one on the window, or Ctrl+O)
//   vellum FILE.md --gpu        draw with the GPU rather than SDL's software renderer
//   vellum FILE.md --shot OUT.png [--size W H] [--scroll Y]
//                               draw it once without showing a window and save the picture
//   vellum FILE.md --bench [--size W H]
//                               time the frames of scrolling down the file
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"
#include "gs_draw.h"
#include "gs_image.h"
#include "gs_jobs.h"
#include "gs_text.h"
#include "gs_ui.h"

#define BODY 15.0f      // the text's size, in points
#define COLUMN 860.0f   // the widest the text runs
#define PAD 32.0f
#define INDENT 26.0f    // a list's
#define QUOTE 18.0f     // a quote's
#define HISTORY 32

typedef struct {
    SDL_FColor back, text, muted, link, code_back, rule, select;
} theme;

static const theme light = {
    { 0.992f, 0.992f, 0.988f, 1 }, { 0.122f, 0.137f, 0.157f, 1 }, { 0.35f, 0.39f, 0.43f, 1 }, { 0.035f, 0.412f, 0.855f, 1 },
    { 0.937f, 0.945f, 0.953f, 1 }, { 0.82f, 0.85f, 0.88f, 1 }, { 0.70f, 0.82f, 0.98f, 1 },
};
static const theme dark = {
    { 0.051f, 0.067f, 0.090f, 1 }, { 0.902f, 0.929f, 0.953f, 1 }, { 0.57f, 0.62f, 0.67f, 1 }, { 0.267f, 0.576f, 0.973f, 1 },
    { 0.15f, 0.17f, 0.21f, 1 }, { 0.24f, 0.27f, 0.31f, 1 }, { 0.15f, 0.30f, 0.52f, 1 },
};

// What a block of text is drawn with, made once when the file is read, and its height at a width.
typedef struct {
    gs_ui_text_span *spans;
    int nspans;
    int from, to;     // what is selected in it
    float h, h_width;
} view_text;

typedef struct {
    view_text text;
    view_text *cells;
    float h, h_width;  // the whole block's
    char image[1100];  // a picture's address, made whole
} view_block;

typedef struct {
    SDL_Window *win;
    SDL_Renderer *r;
    gs_ui *ui;
    gs_jobs *jobs;
    gs_images *images;
    gs_fontset *fonts[FONTS], *heading[FONTS], *code[1];
    theme t;
    char path[1024], dir[1024], name[256];
    char history[HISTORY][1024];
    int nhistory;
    char *source;       // the file's text
    size_t source_len;
    SDL_Time modified;  // when the file was last changed, as read
    doc *d;
    view_block *v;
    float *columns;     // each table's column widths, at `columns_width`
    int ncolumns_tables, ncolumns_max;
    float columns_width;
    bool editing, dirty;
    char *edit;         // the text being edited
    size_t edit_cap;
    char message[256];
    float scroll_to;    // -1, or where to scroll the page next frame
    Uint64 checked;     // when the file was last looked at for changes
    Uint64 frames;      // drawn, since it started
} app;

static const char *ext(const char *path) {
    const char *dot = strrchr(path, '.'), *slash = SDL_max(strrchr(path, '/'), strrchr(path, '\\'));
    return dot && dot > slash ? dot : "";
}

// ---- The document as it is drawn ----

static void free_view(app *a) {
    for (int i = 0; a->d && i < a->d->count; i++) {
        free(a->v[i].text.spans);
        for (int c = 0; c < a->d->blocks[i].ncells; c++) free(a->v[i].cells[c].spans);
        free(a->v[i].cells);
    }
    free(a->v), a->v = NULL;
    free(a->columns), a->columns = NULL, a->columns_width = 0;
    doc_free(a->d), a->d = NULL;
}

static void spans_of(app *a, const doc_text *t, view_text *v) {
    v->spans = t->nspans ? calloc((size_t)t->nspans, sizeof *v->spans) : NULL;
    v->nspans = t->nspans;
    for (int i = 0; i < t->nspans; i++) {
        const doc_span *s = &t->spans[i];
        SDL_FColor color = s->flags & SPAN_LINK ? a->t.link : s->flags & SPAN_STRUCK ? a->t.muted : (SDL_FColor){ 0 };
        SDL_FColor back = s->flags & SPAN_CODE ? a->t.code_back : (SDL_FColor){ 0 };
        v->spans[i] = (gs_ui_text_span){ s->start, s->end, s->font, color, back, (s->flags & SPAN_LINK) != 0 };
    }
}

static void make_view(app *a) {
    a->v = calloc((size_t)SDL_max(1, a->d->count), sizeof *a->v);
    for (int i = 0; i < a->d->count; i++) {
        doc_block *b = &a->d->blocks[i];
        spans_of(a, &b->text, &a->v[i].text);
        if (b->ncells) a->v[i].cells = calloc((size_t)b->ncells, sizeof *a->v[i].cells);
        for (int c = 0; c < b->ncells; c++) spans_of(a, &b->cells[c], &a->v[i].cells[c]);
        if (b->src) {  // a picture beside the file is read into the image cache under its own path
            if (strstr(b->src, "://")) SDL_strlcpy(a->v[i].image, b->src, sizeof a->v[i].image);
            else {
                SDL_snprintf(a->v[i].image, sizeof a->v[i].image, "%s%s", a->dir, b->src);
                size_t n = 0;
                void *data = SDL_LoadFile(a->v[i].image, &n);
                if (data) gs_images_put(a->images, a->v[i].image, data, n), SDL_free(data);
            }
        }
    }
    a->ncolumns_tables = 0, a->ncolumns_max = 0;
    for (int i = 0; i < a->d->count; i++)
        if (a->d->blocks[i].kind == DOC_ROW) a->ncolumns_tables = SDL_max(a->ncolumns_tables, a->d->blocks[i].table), a->ncolumns_max = SDL_max(a->ncolumns_max, a->d->blocks[i].ncells);
}

static void set_title(app *a) {
    char title[400];
    SDL_snprintf(title, sizeof title, "%s%s - vellum", a->dirty ? "* " : "", a->name[0] ? a->name : "vellum");
    SDL_SetWindowTitle(a->win, title);
}

static void parse_source(app *a) {
    free_view(a);
    a->d = doc_parse(a->source ? a->source : "", a->source_len);
    make_view(a);
}

static bool open_file(app *a, const char *path, bool remember) {
    size_t n = 0;
    char *text = SDL_LoadFile(path, &n);
    if (!text) {
        SDL_snprintf(a->message, sizeof a->message, "Could not open %s", path);
        return false;
    }
    if (remember && a->path[0] && strcmp(a->path, path)) {  // (for Alt+Left)
        if (a->nhistory == HISTORY) memmove(a->history, a->history + 1, sizeof a->history[0] * (HISTORY - 1)), a->nhistory--;
        SDL_strlcpy(a->history[a->nhistory++], a->path, sizeof a->history[0]);
    }
    SDL_free(a->source);
    a->source = text, a->source_len = n;
    SDL_strlcpy(a->path, path, sizeof a->path);
    SDL_strlcpy(a->dir, path, sizeof a->dir);
    char *slash = SDL_max(strrchr(a->dir, '/'), strrchr(a->dir, '\\'));
    if (slash) slash[1] = 0;
    else a->dir[0] = 0;
    SDL_strlcpy(a->name, slash ? path + (slash - a->dir) + 1 : path, sizeof a->name);
    SDL_PathInfo info;
    a->modified = SDL_GetPathInfo(path, &info) ? info.modify_time : 0;
    a->editing = a->dirty = false, a->message[0] = 0, a->scroll_to = 0;
    parse_source(a);
    set_title(a);
    return true;
}

// The file changed on disk, and nothing here is unsaved: read it again, at the same place. True when it was.
static bool watch(app *a) {
    Uint64 now = SDL_GetTicks();
    if (!a->path[0] || a->dirty || a->editing || now - a->checked < 1000) return false;
    a->checked = now;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(a->path, &info) || info.modify_time == a->modified) return false;
    float at = gs_ui_page_scroll(a->ui, "doc");
    char path[1024];
    SDL_strlcpy(path, a->path, sizeof path);
    if (open_file(a, path, false)) a->scroll_to = at;
    return true;
}

static bool save(app *a) {
    if (!a->path[0]) return false;
    const char *text = a->editing ? a->edit : a->source;
    if (!SDL_SaveFile(a->path, text, strlen(text))) {
        SDL_snprintf(a->message, sizeof a->message, "Could not save: %s", SDL_GetError());
        return false;
    }
    SDL_PathInfo info;
    a->modified = SDL_GetPathInfo(a->path, &info) ? info.modify_time : 0;
    a->dirty = false;
    set_title(a);
    return true;
}

static void start_editing(app *a) {
    a->edit_cap = a->source_len * 2 + 65536;
    free(a->edit);
    a->edit = malloc(a->edit_cap);
    memcpy(a->edit, a->source ? a->source : "", a->source_len), a->edit[a->source_len] = 0;
    a->editing = true;
    gs_ui_focus(a->ui, "source");
}

static void stop_editing(app *a) {
    a->editing = false;
    SDL_free(a->source);
    a->source_len = strlen(a->edit);
    a->source = SDL_malloc(a->source_len + 1);
    memcpy(a->source, a->edit, a->source_len + 1);
    parse_source(a);
}

// ---- Links ----

static void follow(app *a, const char *href) {
    if (!href || !*href) return;
    if (href[0] == '#') {  // a heading in this document
        for (int i = 0; i < a->d->count; i++)
            if (a->d->blocks[i].kind == DOC_HEADING && !SDL_strcasecmp(a->d->blocks[i].anchor, href)) { a->scroll_to = -2 - (float)i; return; }
        return;
    }
    if (strstr(href, "://") || !strncmp(href, "mailto:", 7)) { SDL_OpenURL(href); return; }
    // A path beside this file: a Markdown file opens here, anything else as the system opens it.
    char path[1100], frag[256] = "";
    SDL_snprintf(path, sizeof path, "%s%s", href[0] == '/' ? "" : a->dir, href);
    char *hash = strchr(path, '#');
    if (hash) SDL_strlcpy(frag, hash, sizeof frag), *hash = 0;
    if (!SDL_strcasecmp(ext(path), ".md") || !SDL_strcasecmp(ext(path), ".markdown")) {
        if (open_file(a, path, true) && frag[0]) follow(a, frag);
        return;
    }
    char url[1200];
    SDL_snprintf(url, sizeof url, "file://%s", path);
    SDL_OpenURL(url);
}

// ---- Layout ----

static float heading_px(int level) {
    static const float k[7] = { 1, 2.0f, 1.5f, 1.25f, 1.0f, 0.875f, 0.85f };
    return BODY * k[SDL_clamp(level, 1, 6)];
}

static float text_h(app *a, view_text *v, const char *text, float px, float width, gs_fontset **fonts, int nfonts) {
    if (v->h_width != width) v->h = gs_ui_text_spans_height(a->ui, text ? text : "", px, width, fonts, nfonts, v->spans, v->nspans), v->h_width = width;
    return v->h;
}

// Each table's columns: never narrower than their longest word, and as wide as their text on one
// line where `width` holds it; otherwise what is left after the words is shared out by how much more
// each column wants.
static void table_columns(app *a, float width) {
    if (a->columns_width == width || !a->ncolumns_tables) return;
    free(a->columns);
    int n = a->ncolumns_tables * a->ncolumns_max;
    a->columns = calloc((size_t)n, sizeof *a->columns);
    float *least = calloc((size_t)n, sizeof *least);
    a->columns_width = width;
    for (int i = 0; i < a->d->count; i++) {
        doc_block *b = &a->d->blocks[i];
        if (b->kind != DOC_ROW) continue;
        int base = (b->table - 1) * a->ncolumns_max;
        gs_ui_font(a->ui, b->header ? a->fonts[FONT_BOLD] : a->fonts[FONT_PLAIN]);
        for (int c = 0; c < b->ncells; c++) {
            const char *t = b->cells[c].text ? b->cells[c].text : "";
            a->columns[base + c] = SDL_max(a->columns[base + c], gs_ui_text_width(a->ui, t, BODY) + 22);
            for (const char *w = t; *w;) {  // (its longest word)
                size_t k = strcspn(w, " \t\n");
                char word[256];
                SDL_strlcpy(word, w, SDL_min(k + 1, sizeof word));
                least[base + c] = SDL_max(least[base + c], gs_ui_text_width(a->ui, word, BODY) + 22);
                w += k + (w[k] != 0);
            }
        }
        gs_ui_font(a->ui, NULL);
    }
    for (int t = 0; t < a->ncolumns_tables; t++) {
        float *cols = a->columns + t * a->ncolumns_max, *lo = least + t * a->ncolumns_max, total = 0, floor = 0;
        for (int c = 0; c < a->ncolumns_max; c++) total += cols[c], floor += lo[c];
        if (total <= width) continue;
        float spare = SDL_max(0, width - floor), want = total - floor;
        for (int c = 0; c < a->ncolumns_max; c++) cols[c] = lo[c] + (want > 0 ? (cols[c] - lo[c]) * spare / want : 0);
    }
    free(least);
}

// Where a block's text starts across, and how wide it runs.
static void block_box(const doc_block *b, float x, float w, float *bx, float *bw) {
    float in = b->quote * QUOTE + b->indent * INDENT;
    *bx = x + in, *bw = SDL_max(40, w - in);
}

static float space_before(const doc_block *b, const doc_block *prev) {
    if (!prev) return 0;
    if (b->kind == DOC_HEADING) return b->level <= 2 ? 30 : 22;
    if (b->kind == DOC_ROW && prev->kind == DOC_ROW && prev->table == b->table) return 0;
    if (b->tight && prev->indent && !b->quote) return 4;
    return 14;
}

static float block_h(app *a, int i, float x, float w) {
    doc_block *b = &a->d->blocks[i];
    view_block *v = &a->v[i];
    float bx, bw;
    block_box(b, x, w, &bx, &bw);
    if (v->h_width == w && b->kind != DOC_IMAGE) return v->h;
    switch (b->kind) {
    case DOC_HEADING: v->h = text_h(a, &v->text, b->text.text, heading_px(b->level), bw, a->heading, FONTS) + (b->level <= 2 ? 10 : 0); break;
    case DOC_CODE: v->h = text_h(a, &v->text, b->text.text, BODY * 0.9f, bw - 28, a->code, 1) + 24; break;
    case DOC_RULE: v->h = 2; break;
    case DOC_ROW: {
        table_columns(a, bw);
        float *cols = a->columns + (b->table - 1) * a->ncolumns_max, h = 0;
        for (int c = 0; c < b->ncells; c++)
            h = SDL_max(h, text_h(a, &v->cells[c], b->cells[c].text, BODY, cols[c] - 20, b->header ? a->heading : a->fonts, FONTS));
        v->h = h + 14;
        break;
    }
    case DOC_IMAGE: {
        int iw = 0, ih = 0;
        if (gs_images_size(a->images, v->image, &iw, &ih) && iw > 0) {
            float s = gs_ui_scale(a->ui), shown = SDL_min(bw, iw / s);
            v->h = shown * ih / iw;
        } else v->h = gs_ui_line_height(a->ui, BODY) + 8;
        break;
    }
    default: v->h = text_h(a, &v->text, b->text.text, BODY, bw, a->fonts, FONTS); break;
    }
    v->h_width = w;
    return v->h;
}

static void draw_text(app *a, gs_rect r, int i, int cell, view_text *v, const doc_text *t, float px, gs_fontset **fonts, int nfonts, SDL_FColor c) {
    char id[32];
    SDL_snprintf(id, sizeof id, "b%d.%d", i, cell);
    int clicked = -1;
    gs_ui_text_spans(a->ui, r, id, t->text ? t->text : "", px, c, fonts, nfonts, v->spans, v->nspans, &v->from, &v->to, &clicked);
    if (clicked >= 0 && clicked < t->nspans) follow(a, a->d->links[t->spans[clicked].link]);
}

static void draw_block(app *a, int i, float x, float y, float w) {
    doc_block *b = &a->d->blocks[i];
    view_block *v = &a->v[i];
    float bx, bw, h = v->h;
    block_box(b, x, w, &bx, &bw);
    for (int q = 0; q < b->quote; q++) gs_ui_fill(a->ui, (gs_rect){ x + q * QUOTE, y - 4, 3, h + 8 }, a->t.rule);  // (the quote's bar)
    SDL_FColor text = b->quote ? a->t.muted : a->t.text;
    if (b->marker[0]) gs_ui_text(a->ui, (gs_rect){ bx - INDENT, y, INDENT - 8, gs_ui_line_height(a->ui, BODY) }, b->marker, BODY, a->t.muted, 1);
    switch (b->kind) {
    case DOC_HEADING:
        draw_text(a, (gs_rect){ bx, y, bw, 0 }, i, -1, &v->text, &b->text, heading_px(b->level), a->heading, FONTS, text);
        if (b->level <= 2) gs_ui_fill(a->ui, (gs_rect){ bx, y + h - 3, bw, 1 }, a->t.rule);
        break;
    case DOC_CODE:
        gs_ui_round(a->ui, (gs_rect){ bx, y, bw, h }, 6, a->t.code_back);
        draw_text(a, (gs_rect){ bx + 14, y + 12, bw - 28, 0 }, i, -1, &v->text, &b->text, BODY * 0.9f, a->code, 1, text);
        break;
    case DOC_RULE: gs_ui_fill(a->ui, (gs_rect){ bx, y, bw, 2 }, a->t.rule); break;
    case DOC_ROW: {
        float *cols = a->columns + (b->table - 1) * a->ncolumns_max, cx = bx;
        if (b->header) gs_ui_fill(a->ui, (gs_rect){ bx, y, 0, h }, a->t.code_back);
        for (int c = 0; c < b->ncells; c++) {
            if (b->header) gs_ui_fill(a->ui, (gs_rect){ cx, y, cols[c], h }, a->t.code_back);
            gs_ui_fill(a->ui, (gs_rect){ cx, y, 1, h }, a->t.rule);
            float shift = 0;  // (a cell on one line, set to the right or centred as its column says)
            if (b->cells[c].align && b->cells[c].text) {
                gs_ui_font(a->ui, b->header ? a->fonts[FONT_BOLD] : a->fonts[FONT_PLAIN]);
                float tw = gs_ui_text_width(a->ui, b->cells[c].text, BODY);
                gs_ui_font(a->ui, NULL);
                if (tw < cols[c] - 20) shift = (cols[c] - 20 - tw) * (b->cells[c].align == 2 ? 1 : 0.5f);
            }
            draw_text(a, (gs_rect){ cx + 10 + shift, y + 7, cols[c] - 20 - shift, 0 }, i, c, &v->cells[c], &b->cells[c], BODY, b->header ? a->heading : a->fonts, FONTS, text);
            cx += cols[c];
        }
        gs_ui_fill(a->ui, (gs_rect){ cx, y, 1, h }, a->t.rule);
        gs_ui_fill(a->ui, (gs_rect){ bx, y + h - 1, cx - bx, 1 }, a->t.rule);
        bool first = i == 0 || a->d->blocks[i - 1].kind != DOC_ROW || a->d->blocks[i - 1].table != b->table;
        if (first) gs_ui_fill(a->ui, (gs_rect){ bx, y, cx - bx, 1 }, a->t.rule);
        break;
    }
    case DOC_IMAGE: {
        int iw = 0, ih = 0;
        SDL_Texture *t = gs_images_full(a->images, v->image, &iw, &ih);
        if (t && iw > 0) {
            float s = gs_ui_scale(a->ui), shown = SDL_min(bw, iw / s);
            gs_ui_texture(a->ui, (gs_rect){ bx, y, shown, shown * ih / iw }, t, NULL);
        } else {  // (while it loads, or where it cannot: its alternative text)
            gs_ui_font(a->ui, a->fonts[FONT_ITALIC]);
            gs_ui_text(a->ui, (gs_rect){ bx, y, bw, h }, b->text.text && b->text.text[0] ? b->text.text : b->src, BODY, a->t.muted, -1);
            gs_ui_font(a->ui, NULL);
        }
        break;
    }
    default: draw_text(a, (gs_rect){ bx, y, bw, 0 }, i, -1, &v->text, &b->text, BODY, a->fonts, FONTS, text); break;
    }
}

// ---- Copying ----

static void add_part(char **out, size_t *n, const char *text, int from, int to, const char *before) {
    int lo = SDL_min(from, to), hi = SDL_max(from, to);
    if (hi <= lo || !text) return;
    size_t k = strlen(before), m = (size_t)(hi - lo);
    *out = realloc(*out, *n + k + m + 1);
    if (*n) memcpy(*out + *n, before, k), *n += k;
    memcpy(*out + *n, text + lo, m), *n += m, (*out)[*n] = 0;
}

// What is selected, block after block: paragraphs on lines of their own, a row's cells by tabs.
static void copy_selection(app *a) {
    char *out = NULL;
    size_t n = 0;
    for (int i = 0; i < a->d->count; i++) {
        doc_block *b = &a->d->blocks[i];
        add_part(&out, &n, b->text.text, a->v[i].text.from, a->v[i].text.to, "\n\n");
        for (int c = 0; c < b->ncells; c++) add_part(&out, &n, b->cells[c].text, a->v[i].cells[c].from, a->v[i].cells[c].to, c ? "\t" : "\n");
    }
    if (out) SDL_SetClipboardText(out), free(out);
}

static void clear_selection(app *a, float top, float bottom, const float *ys) {
    for (int i = 0; i < a->d->count; i++) {
        if (ys[i] + a->v[i].h >= top && ys[i] <= bottom) continue;  // (drawn this frame: its own pair is current)
        a->v[i].text.from = a->v[i].text.to = 0;
        for (int c = 0; c < a->d->blocks[i].ncells; c++) a->v[i].cells[c].from = a->v[i].cells[c].to = 0;
    }
}

// ---- The window ----

static void SDLCALL chosen(void *user, const char *const *files, int filter) {
    if (!files || !files[0]) return;
    SDL_Event e = { .user = { .type = *(Uint32 *)user, .data1 = SDL_strdup(files[0]) } };
    SDL_PushEvent(&e);
}

static Uint32 open_event;

static void frame(app *a) {
    gs_ui *ui = a->ui;
    gs_ui_style *st = gs_ui_style_of(ui);
    st->background = a->t.back, st->text = a->t.text, st->muted = a->t.muted, st->border = a->t.select, st->accent = a->t.link;
    SDL_SetRenderDrawColorFloat(a->r, a->t.back.r, a->t.back.g, a->t.back.b, 1);
    SDL_RenderClear(a->r);
    gs_rect area = gs_ui_begin(ui);
    if (gs_ui_key(ui, SDLK_O, SDL_KMOD_CTRL)) {
        static const SDL_DialogFileFilter filters[] = { { "Markdown", "md;markdown" }, { "All files", "*" } };
        SDL_ShowOpenFileDialog(chosen, &open_event, a->win, filters, 2, a->dir[0] ? a->dir : NULL, false);
    }
    if (gs_ui_key(ui, SDLK_E, SDL_KMOD_CTRL) && a->path[0]) a->editing ? stop_editing(a) : start_editing(a);
    if (gs_ui_key(ui, SDLK_S, SDL_KMOD_CTRL) && save(a) && a->editing) stop_editing(a), start_editing(a);
    if (gs_ui_key(ui, SDLK_EQUALS, SDL_KMOD_CTRL) || gs_ui_key(ui, SDLK_PLUS, SDL_KMOD_CTRL)) gs_ui_set_zoom(ui, SDL_min(3, gs_ui_zoom(ui) * 1.1f));
    if (gs_ui_key(ui, SDLK_MINUS, SDL_KMOD_CTRL)) gs_ui_set_zoom(ui, SDL_max(0.5f, gs_ui_zoom(ui) / 1.1f));
    if (gs_ui_key(ui, SDLK_0, SDL_KMOD_CTRL)) gs_ui_set_zoom(ui, 1);
    if (gs_ui_key(ui, SDLK_LEFT, SDL_KMOD_ALT) && a->nhistory) {
        char back[1024];
        SDL_strlcpy(back, a->history[--a->nhistory], sizeof back);
        open_file(a, back, false);
    }

    if (a->editing) {  // the file's text, in the code font
        gs_ui_font(ui, a->code[0]);
        if (gs_ui_textarea(ui, (gs_rect){ area.x + 12, area.y + 12, area.w - 24, area.h - 24 }, "source", a->edit, a->edit_cap, NULL) && !a->dirty) a->dirty = true, set_title(a);
        gs_ui_font(ui, NULL);
        gs_ui_end(ui);
        return;
    }
    if (!a->d) {
        gs_ui_text(ui, (gs_rect){ area.x, area.y + area.h / 2 - 40, area.w, 30 }, a->message[0] ? a->message : "Drop a Markdown file here, or press Ctrl+O", BODY * 1.2f, a->t.muted, 0);
        gs_ui_end(ui);
        return;
    }
    if (gs_ui_key(ui, SDLK_C, SDL_KMOD_CTRL)) copy_selection(a);
    float w = SDL_min(COLUMN, area.w - 2 * PAD), x = area.x + (area.w - w) / 2;
    // The heights first, to scroll to a heading asked for and to draw only what is in view.
    float *ys = malloc(sizeof *ys * (size_t)SDL_max(1, a->d->count)), y = PAD;
    for (int i = 0; i < a->d->count; i++) {
        y += space_before(&a->d->blocks[i], i ? &a->d->blocks[i - 1] : NULL);
        ys[i] = y, y += block_h(a, i, x, w);
    }
    if (a->scroll_to >= 0) gs_ui_page_scroll_to(ui, "doc", a->scroll_to, false), a->scroll_to = -1;
    else if (a->scroll_to <= -2) gs_ui_page_scroll_to(ui, "doc", ys[(int)(-a->scroll_to) - 2] - 16, true), a->scroll_to = -1;
    gs_rect page = gs_ui_page_begin(ui, area, "doc");
    float top = area.y - page.y, bottom = top + area.h;
    gs_ui_select_group(ui, "doc");
    for (int i = 0; i < a->d->count; i++)
        if (ys[i] + a->v[i].h >= top && ys[i] <= bottom) draw_block(a, i, x, page.y + ys[i], w);
    gs_ui_select_group(ui, NULL);
    gs_ui_page_end(ui, page.y + y + PAD);
    if (gs_ui_select_began(ui, "doc")) clear_selection(a, top, bottom, ys);
    free(ys);
    if (gs_images_end_frame(a->images)) gs_ui_wake(ui, 0);
    gs_ui_end(ui);
}

static gs_fontset *mono_italic(void) { return gs_fontset_system_italic(400, true); }

static const char usage[] =
    "usage: vellum [FILE.md] [--gpu] [--shot OUT.png | --bench] [--size W H] [--scroll Y]\n"
    "  Shows a Markdown file, reads it again when it changes, and edits it (Ctrl+E, Ctrl+S).\n"
    "  --gpu            draw with the GPU rather than SDL's software renderer\n"
    "  --shot OUT.png   draw the file once without a window and save the picture\n"
    "  --bench          time the frames of scrolling down the file\n"
    "  --size W H       the window's size for --shot and --bench\n"
    "  --scroll Y       how far down the file --shot starts, in points\n";

int main(int argc, char **argv) {
    const char *file = NULL, *shot = NULL;
    bool bench = false, gpu = false;
    int shot_w = 1100, shot_h = 900;
    float shot_scroll = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) shot_w = atoi(argv[i + 1]), shot_h = atoi(argv[i + 2]), i += 2;
        else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) shot_scroll = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--bench")) bench = true;
        else if (!strcmp(argv[i], "--gpu")) gpu = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) return printf("%s", usage), 0;
        else if (argv[i][0] == '-') return fprintf(stderr, "%s", usage), 2;
        else file = argv[i];
    }
    SDL_SetAppMetadata("vellum", "0.1", "dev.wakamex.vellum");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, "wakamex");
    if (shot) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen"), SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (bench) SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
    // SDL's software renderer unless asked for the GPU: a page of text drawn only when it changes
    // takes under 4 ms a frame on a 1440p window, and the GPU's driver costs some 50 MB more memory and
    // 30 more threads for nothing a viewer uses (Windows 11 with Direct3D 11 on an NVIDIA card).
    if (!gpu && !SDL_getenv("SDL_RENDER_DRIVER")) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1"), SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");  // (the window's icon on Windows: the program's own)
    if (!SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "vellum: %s\n", SDL_GetError()), 1;
    static app a;
    a.scroll_to = -1;
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetStringProperty(p, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "vellum");
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, shot || bench ? shot_w : 1000);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, shot || bench ? shot_h : 800);
    SDL_SetNumberProperty(p, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (shot) SDL_SetBooleanProperty(p, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    a.win = SDL_CreateWindowWithProperties(p);
    SDL_DestroyProperties(p);
    a.r = a.win ? SDL_CreateRenderer(a.win, NULL) : NULL;
    if (!a.r) return fprintf(stderr, "vellum: %s\n", SDL_GetError()), 1;
    SDL_SetRenderVSync(a.r, bench ? 0 : 1);
    a.fonts[FONT_PLAIN] = gs_fontset_system();
    a.fonts[FONT_BOLD] = gs_fontset_system_weight(700);
    a.fonts[FONT_ITALIC] = gs_fontset_system_italic(400, false);
    a.fonts[FONT_BOLD_ITALIC] = gs_fontset_system_italic(700, false);
    a.fonts[FONT_CODE] = gs_fontset_system_mono(400);
    gs_fontset *code_italic = mono_italic();
    // A heading's words are bold, its emphasis bold italic; code stays code.
    gs_fontset *heading[FONTS] = { a.fonts[FONT_BOLD], a.fonts[FONT_BOLD], a.fonts[FONT_BOLD_ITALIC], a.fonts[FONT_BOLD_ITALIC], a.fonts[FONT_CODE] };
    memcpy(a.heading, heading, sizeof heading);
    a.code[0] = a.fonts[FONT_CODE];
    a.ui = gs_ui_new(a.win, a.r, a.fonts[FONT_PLAIN]);
    a.t = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? dark : light;
    a.jobs = gs_jobs_new(2);
    a.images = gs_images_new(a.r, a.jobs, 64 << 20);
    open_event = SDL_RegisterEvents(1);
    if (file) open_file(&a, file, false);
    if (bench) {  // the time a frame takes while scrolling down the file, a few points a frame
        SDL_ShowWindow(a.win);
        for (int i = 0; i < 5; i++) frame(&a), SDL_RenderPresent(a.r);
        Uint64 t0 = SDL_GetTicksNS(), worst = 0;
        int n = 0;
        for (float y = 0; n < 300; y += 7, n++) {
            Uint64 f0 = SDL_GetTicksNS();
            gs_ui_page_scroll_to(a.ui, "doc", y, false);
            frame(&a);
            SDL_RenderPresent(a.r);
            worst = SDL_max(worst, SDL_GetTicksNS() - f0);
        }
        printf("%s renderer, %d by %d: %.2f ms a frame scrolling, %.2f ms the slowest\n", SDL_GetRendererName(a.r), shot_w, shot_h, (SDL_GetTicksNS() - t0) / 1e6 / n, worst / 1e6);
        return 0;
    }
    if (shot) {  // a few frames, for the glyphs drawn new to the atlas and the pictures loading
        a.t = light;
        if (a.d) parse_source(&a);
        for (int i = 0; i < 6; i++) {
            if (i == 1) gs_ui_page_scroll_to(a.ui, "doc", shot_scroll, false);
            frame(&a);
            if (i < 5) SDL_RenderPresent(a.r);
            SDL_Delay(30);
        }
        bool ok = gs_draw_save_png(a.r, shot);
        if (!ok) fprintf(stderr, "vellum: %s\n", SDL_GetError());
        return ok ? 0 : 1;
    }
    SDL_ShowWindow(a.win);
    for (bool running = true; running;) {
        // A frame is drawn for an event, for what the interface asked a frame for, and for the file
        // read again; in between it sleeps, waking once a second only to look at the file.
        SDL_Event e;
        int asked = gs_ui_wait_ms(a.ui), wait = asked < 0 || asked > 1000 ? 1000 : asked;
        bool draw = asked >= 0 && asked <= 1000;
        if (SDL_WaitEventTimeout(&e, wait)) {
            draw = true;
            do {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) open_file(&a, e.drop.data, true);
                else if (e.type == open_event) open_file(&a, e.user.data1, true), SDL_free(e.user.data1);
                else if (e.type == SDL_EVENT_SYSTEM_THEME_CHANGED) a.t = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? dark : light, a.d ? parse_source(&a) : (void)0;
                gs_ui_event(a.ui, &e);
            } while (SDL_PollEvent(&e));
        }
        if (watch(&a)) draw = true;
        if (!draw) continue;
        frame(&a);
        SDL_RenderPresent(a.r);
        a.frames++;
    }
    if (SDL_getenv("VELLUM_FRAMES")) fprintf(stderr, "vellum: %llu frames drawn\n", (unsigned long long)a.frames);  // (for checking that it sleeps)
    gs_ui_free(a.ui);
    free_view(&a);
    gs_images_free(a.images), gs_jobs_free(a.jobs);
    for (int i = 0; i < FONTS; i++) gs_fontset_free(a.fonts[i]);
    gs_fontset_free(code_italic);
    SDL_DestroyRenderer(a.r), SDL_DestroyWindow(a.win);
    SDL_Quit();
    return 0;
}
