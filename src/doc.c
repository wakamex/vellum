#include "doc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "entity.h"
#include "md4c.h"

typedef struct {
    doc *d;
    int cur, cell;  // the block text goes to, and its cell (-1 for the block's own text); cur -1 for none
    int strong, em, code, del;
    int link;       // the link being written, or -1
    int quote, depth;
    struct { bool ordered, tight; unsigned next; } lists[16];
    char marker[16];  // the marker the next block in a list item takes
    bool in_list_item, header, in_image;
    int table, image_back;  // the block an image interrupted, to go back to
    int image_cell;
} parse;

static doc_text *target(parse *p) {
    if (p->cur < 0) return NULL;
    doc_block *b = &p->d->blocks[p->cur];
    return p->cell >= 0 ? &b->cells[p->cell] : &b->text;
}

static int open_block(parse *p, int kind) {
    doc *d = p->d;
    d->blocks = realloc(d->blocks, sizeof *d->blocks * (size_t)(d->count + 1));
    doc_block *b = &d->blocks[d->count];
    *b = (doc_block){ .kind = kind, .quote = p->quote, .indent = p->depth };
    if (p->in_list_item) {
        memcpy(b->marker, p->marker, sizeof b->marker), p->marker[0] = 0;
        b->tight = p->depth > 0 && p->lists[p->depth - 1].tight;
    }
    p->cur = d->count++, p->cell = -1;
    return p->cur;
}

static void append(doc_text *t, const char *s, size_t n) {
    t->text = realloc(t->text, (size_t)t->len + n + 1);
    memcpy(t->text + t->len, s, n);
    t->len += (int)n, t->text[t->len] = 0;
}

// Text in the current style: a span for it when it is not plain, joined to the span before where they match.
static void add(parse *p, const char *s, size_t n) {
    if (!n) return;
    if (!target(p)) open_block(p, DOC_PARA);  // (a tight list item's text, with no paragraph around it)
    doc_text *t = target(p);
    int start = t->len;
    append(t, s, n);
    int font = p->code ? FONT_CODE : p->strong && p->em ? FONT_BOLD_ITALIC : p->strong ? FONT_BOLD : p->em ? FONT_ITALIC : FONT_PLAIN;
    int flags = (p->link >= 0 ? SPAN_LINK : 0) | (p->code ? SPAN_CODE : 0) | (p->del ? SPAN_STRUCK : 0);
    if (font == FONT_PLAIN && !flags) return;
    doc_span *last = t->nspans ? &t->spans[t->nspans - 1] : NULL;
    if (last && last->end == start && last->font == font && last->flags == flags && last->link == p->link) { last->end = t->len; return; }
    t->spans = realloc(t->spans, sizeof *t->spans * (size_t)(t->nspans + 1));
    t->spans[t->nspans++] = (doc_span){ start, t->len, font, flags, p->link };
}

static void add_codepoint(parse *p, unsigned c) {
    char u[4];
    size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
    if (n == 1) u[0] = (char)c;
    else if (n == 2) u[0] = (char)(0xC0 | c >> 6), u[1] = (char)(0x80 | (c & 63));
    else if (n == 3) u[0] = (char)(0xE0 | c >> 12), u[1] = (char)(0x80 | (c >> 6 & 63)), u[2] = (char)(0x80 | (c & 63));
    else u[0] = (char)(0xF0 | c >> 18), u[1] = (char)(0x80 | (c >> 12 & 63)), u[2] = (char)(0x80 | (c >> 6 & 63)), u[3] = (char)(0x80 | (c & 63));
    add(p, u, n);
}

// "&amp;", "&#233;", "&#x1F600;".
static void add_entity(parse *p, const char *s, size_t n) {
    unsigned c = 0;
    if (n > 3 && s[1] == '#') {
        c = (unsigned)strtoul(s + (s[2] == 'x' || s[2] == 'X' ? 3 : 2), NULL, s[2] == 'x' || s[2] == 'X' ? 16 : 10);
        add_codepoint(p, c && c < 0x110000 ? c : 0xFFFD);
        return;
    }
    const ENTITY *e = entity_lookup(s, n);
    if (!e) { add(p, s, n); return; }
    add_codepoint(p, e->codepoints[0]);
    if (e->codepoints[1]) add_codepoint(p, e->codepoints[1]);
}

// GitHub's anchor for a heading: lower case, spaces as hyphens, other punctuation dropped.
static void anchor_of(const char *text, char *out, size_t size) {
    size_t n = 0;
    out[n++] = '#';
    for (const unsigned char *c = (const unsigned char *)text; *c && n + 2 < size; c++) {
        if (*c >= 0x80 || isalnum(*c) || *c == '-' || *c == '_') out[n++] = (char)(*c >= 0x80 ? *c : tolower(*c));
        else if (*c == ' ') out[n++] = '-';
    }
    out[n] = 0;
}

static char *copy_attr(const MD_ATTRIBUTE *a) {
    char *s = malloc(a->size + 1);
    if (s) memcpy(s, a->text, a->size), s[a->size] = 0;
    return s;
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *user) {
    parse *p = user;
    doc *d = p->d;
    switch (type) {
    case MD_BLOCK_QUOTE: p->quote++, p->cur = -1; break;
    case MD_BLOCK_UL: case MD_BLOCK_OL:
        if (p->depth < 16) {
            bool ordered = type == MD_BLOCK_OL;
            p->lists[p->depth].ordered = ordered;
            p->lists[p->depth].tight = ordered ? ((MD_BLOCK_OL_DETAIL *)detail)->is_tight : ((MD_BLOCK_UL_DETAIL *)detail)->is_tight;
            p->lists[p->depth].next = ordered ? ((MD_BLOCK_OL_DETAIL *)detail)->start : 0;
        }
        p->depth++, p->cur = -1;
        break;
    case MD_BLOCK_LI: {
        MD_BLOCK_LI_DETAIL *li = detail;
        int at = p->depth > 0 && p->depth <= 16 ? p->depth - 1 : 0;
        static const char *const bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };  // • ◦ ▪
        if (li->is_task) snprintf(p->marker, sizeof p->marker, "%s", li->task_mark == ' ' ? "\xE2\x98\x90" : "\xE2\x98\x91");  // ☐ ☑
        else if (p->lists[at].ordered) snprintf(p->marker, sizeof p->marker, "%u.", p->lists[at].next++);
        else snprintf(p->marker, sizeof p->marker, "%s", bullets[at % 3]);
        p->in_list_item = true, p->cur = -1;
        break;
    }
    case MD_BLOCK_HR: open_block(p, DOC_RULE), p->cur = -1; break;
    case MD_BLOCK_H: open_block(p, DOC_HEADING), d->blocks[p->cur].level = (int)((MD_BLOCK_H_DETAIL *)detail)->level; break;
    case MD_BLOCK_CODE: open_block(p, DOC_CODE), p->code++; break;
    case MD_BLOCK_P: open_block(p, DOC_PARA); break;
    case MD_BLOCK_TABLE: p->table++, p->cur = -1; break;
    case MD_BLOCK_THEAD: p->header = true; break;
    case MD_BLOCK_TBODY: p->header = false; break;
    case MD_BLOCK_TR: open_block(p, DOC_ROW), d->blocks[p->cur].header = p->header, d->blocks[p->cur].table = p->table; break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: {
        if (p->cur < 0) break;
        doc_block *b = &d->blocks[p->cur];
        b->cells = realloc(b->cells, sizeof *b->cells * (size_t)(b->ncells + 1));
        MD_ALIGN a = ((MD_BLOCK_TD_DETAIL *)detail)->align;
        b->cells[b->ncells] = (doc_text){ .align = a == MD_ALIGN_CENTER ? 1 : a == MD_ALIGN_RIGHT ? 2 : 0 };
        p->cell = b->ncells++;
        if (type == MD_BLOCK_TH) p->strong++;
        break;
    }
    default: break;  // (the document, and raw HTML blocks, whose text is left out)
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *user) {
    parse *p = user;
    doc *d = p->d;
    switch (type) {
    case MD_BLOCK_QUOTE: p->quote--, p->cur = -1; break;
    case MD_BLOCK_UL: case MD_BLOCK_OL: p->depth--, p->cur = -1; break;
    case MD_BLOCK_LI: p->in_list_item = false, p->marker[0] = 0, p->cur = -1; break;
    case MD_BLOCK_CODE: {
        p->code--;
        doc_text *t = target(p);
        while (t && t->len && t->text[t->len - 1] == '\n') t->text[--t->len] = 0;  // (the fence's own line end)
        p->cur = -1;
        break;
    }
    case MD_BLOCK_H: {
        doc_block *b = p->cur >= 0 ? &d->blocks[p->cur] : NULL;
        if (b) anchor_of(b->text.text ? b->text.text : "", b->anchor, sizeof b->anchor);
        p->cur = -1;
        break;
    }
    case MD_BLOCK_P: case MD_BLOCK_TR: p->cur = -1; break;
    case MD_BLOCK_TH: case MD_BLOCK_TD: p->cell = -1; if (type == MD_BLOCK_TH) p->strong--; break;
    default: break;
    }
    (void)detail;
    return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *user) {
    parse *p = user;
    doc *d = p->d;
    switch (type) {
    case MD_SPAN_EM: p->em++; break;
    case MD_SPAN_STRONG: p->strong++; break;
    case MD_SPAN_CODE: p->code++; break;
    case MD_SPAN_DEL: p->del++; break;
    case MD_SPAN_U: p->em++; break;
    case MD_SPAN_A: case MD_SPAN_WIKILINK:
        d->links = realloc(d->links, sizeof *d->links * (size_t)(d->nlinks + 1));
        d->links[d->nlinks] = copy_attr(type == MD_SPAN_A ? &((MD_SPAN_A_DETAIL *)detail)->href : &((MD_SPAN_WIKILINK_DETAIL *)detail)->target);
        p->link = d->nlinks++;
        break;
    case MD_SPAN_IMG: {  // a picture: a block of its own, its alternative text as the block's text
        p->image_back = p->cur, p->image_cell = p->cell;
        if (p->cur >= 0 && d->blocks[p->cur].kind == DOC_ROW) break;  // (in a table cell: only its text)
        if (p->cur >= 0 && d->blocks[p->cur].kind == DOC_PARA && !d->blocks[p->cur].text.len) d->blocks[p->cur].kind = DOC_IMAGE;  // (a picture alone in its paragraph is the paragraph)
        else open_block(p, DOC_IMAGE);
        d->blocks[p->cur].src = copy_attr(&((MD_SPAN_IMG_DETAIL *)detail)->src);
        p->in_image = true;
        break;
    }
    default: break;
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *user) {
    parse *p = user;
    switch (type) {
    case MD_SPAN_EM: case MD_SPAN_U: p->em--; break;
    case MD_SPAN_STRONG: p->strong--; break;
    case MD_SPAN_CODE: p->code--; break;
    case MD_SPAN_DEL: p->del--; break;
    case MD_SPAN_A: case MD_SPAN_WIKILINK: p->link = -1; break;
    case MD_SPAN_IMG:
        if (p->in_image) p->in_image = false, p->cur = -1;  // (text after it in the paragraph starts a paragraph after it)
        else p->cur = p->image_back, p->cell = p->image_cell;
        break;
    default: break;
    }
    (void)detail;
    return 0;
}

static int text(MD_TEXTTYPE type, const MD_CHAR *s, MD_SIZE n, void *user) {
    parse *p = user;
    switch (type) {
    case MD_TEXT_SOFTBR: add(p, " ", 1); break;
    case MD_TEXT_BR: add(p, "\n", 1); break;
    case MD_TEXT_NULLCHAR: add_codepoint(p, 0xFFFD); break;
    case MD_TEXT_ENTITY: add_entity(p, s, n); break;
    case MD_TEXT_HTML: if (n >= 3 && !strncmp(s, "<br", 3)) add(p, "\n", 1); break;  // (other inline HTML is left out)
    default: add(p, s, n); break;
    }
    return 0;
}

doc *doc_parse(const char *markdown, size_t len) {
    doc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    parse p = { .d = d, .cur = -1, .cell = -1, .link = -1 };
    MD_PARSER parser = { 0, MD_DIALECT_GITHUB | MD_FLAG_WIKILINKS | MD_FLAG_UNDERLINE, enter_block, leave_block, enter_span, leave_span, text, NULL, NULL };
    md_parse(markdown, (MD_SIZE)len, &parser, &p);
    // Paragraphs left with no text (raw HTML only, or a picture's leftovers) go, unless they carry a list's marker.
    int kept = 0;
    for (int i = 0; i < d->count; i++) {
        doc_block *b = &d->blocks[i];
        if (b->kind == DOC_PARA && !b->text.len && !b->marker[0]) { free(b->text.text), free(b->text.spans); continue; }
        d->blocks[kept++] = *b;
    }
    d->count = kept;
    return d;
}

static void free_text(doc_text *t) { free(t->text), free(t->spans); }

void doc_free(doc *d) {
    if (!d) return;
    for (int i = 0; i < d->count; i++) {
        free_text(&d->blocks[i].text);
        for (int c = 0; c < d->blocks[i].ncells; c++) free_text(&d->blocks[i].cells[c]);
        free(d->blocks[i].cells), free(d->blocks[i].src);
    }
    for (int i = 0; i < d->nlinks; i++) free(d->links[i]);
    free(d->blocks), free(d->links), free(d);
}
