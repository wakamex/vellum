// The parser on tests/sample.md and on cases of its own: blocks, their text, spans, lists and tables.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc.h"

static int failures;
#define CHECK(c) ((c) ? (void)0 : (void)(failures++, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c)))

static doc *parse(const char *s) { return doc_parse(s, strlen(s)); }

// The span over the bytes of `word` in a block's text, or NULL.
static const doc_span *span_at(const doc_text *t, const char *word) {
    const char *at = t->text ? strstr(t->text, word) : NULL;
    for (int i = 0; at && i < t->nspans; i++)
        if (t->spans[i].start == at - t->text && t->spans[i].end == at - t->text + (int)strlen(word)) return &t->spans[i];
    return NULL;
}

int main(void) {
    doc *d = parse("A **bold** *it* ***both*** `code` ~~gone~~ [link](https://x.y) &amp; &#233;\nsoft");
    CHECK(d->count == 1 && d->blocks[0].kind == DOC_PARA);
    const doc_text *t = &d->blocks[0].text;
    CHECK(!strcmp(t->text, "A bold it both code gone link & \xC3\xA9 soft"));
    CHECK(span_at(t, "bold") && span_at(t, "bold")->font == FONT_BOLD);
    CHECK(span_at(t, "it") && span_at(t, "it")->font == FONT_ITALIC);
    CHECK(span_at(t, "both") && span_at(t, "both")->font == FONT_BOLD_ITALIC);
    CHECK(span_at(t, "code") && span_at(t, "code")->font == FONT_CODE && span_at(t, "code")->flags == SPAN_CODE);
    CHECK(span_at(t, "gone") && span_at(t, "gone")->flags == SPAN_STRUCK);
    CHECK(span_at(t, "link") && span_at(t, "link")->flags == SPAN_LINK && !strcmp(d->links[span_at(t, "link")->link], "https://x.y"));
    doc_free(d);

    d = parse("# Hello, World!\n\n- a\n- b\n  1. c\n\n> q\n\n```\nx\n```\n\n---\n\n| h | i |\n|---|--:|\n| 1 | 2 |\n\n![alt](pic.png)\n");
    const char *kinds = "";
    char seen[32] = "";
    for (int i = 0; i < d->count && i < 30; i++) seen[i] = "PHCRWI"[d->blocks[i].kind];
    (void)kinds;
    CHECK(!strcmp(seen, "HPPPPCRWWI"));
    if (strcmp(seen, "HPPPPCRWWI")) fprintf(stderr, "  blocks %s\n", seen);
    CHECK(d->blocks[0].level == 1 && !strcmp(d->blocks[0].anchor, "#hello-world"));
    CHECK(!strcmp(d->blocks[1].marker, "\xE2\x80\xA2") && d->blocks[1].indent == 1 && d->blocks[1].tight);
    CHECK(!strcmp(d->blocks[3].marker, "1.") && d->blocks[3].indent == 2);
    CHECK(d->blocks[4].quote == 1 && !strcmp(d->blocks[4].text.text, "q"));
    CHECK(!strcmp(d->blocks[5].text.text, "x"));
    CHECK(d->blocks[7].header && d->blocks[7].ncells == 2 && !strcmp(d->blocks[7].cells[0].text, "h"));
    CHECK(d->blocks[8].table == 1 && d->blocks[8].cells[1].align == 2 && !strcmp(d->blocks[8].cells[1].text, "2"));
    CHECK(!strcmp(d->blocks[9].src, "pic.png") && !strcmp(d->blocks[9].text.text, "alt"));
    doc_free(d);

    // Tasks, and a numbered list that starts elsewhere.
    d = parse("- [ ] todo\n- [x] done\n\n4. four\n5. five\n");
    CHECK(d->count == 4 && !strcmp(d->blocks[0].marker, "\xE2\x98\x90") && !strcmp(d->blocks[1].marker, "\xE2\x98\x91"));
    CHECK(!strcmp(d->blocks[2].marker, "4.") && !strcmp(d->blocks[3].marker, "5."));
    doc_free(d);

    // The sample of every block parses, with its rows and its headings to six.
    FILE *f = fopen("tests/sample.md", "rb");
    CHECK(f);
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        d = doc_parse(buf, n);
        int rows = 0, headings = 0;
        for (int i = 0; i < d->count; i++) rows += d->blocks[i].kind == DOC_ROW, headings += d->blocks[i].kind == DOC_HEADING;
        CHECK(rows == 3 && headings == 7);
        doc_free(d);
    }
    printf(failures ? "%d failed\n" : "ok\n", failures);
    return failures != 0;
}
