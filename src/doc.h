// A Markdown document as blocks to lay out: paragraphs, headings, list items, code, rules, table
// rows and pictures, each with its text and the spans that set fonts, links and inline code within
// it. Parsed by md4c as GitHub writes Markdown (tables, task lists, strikethrough, bare links).
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { DOC_PARA, DOC_HEADING, DOC_CODE, DOC_RULE, DOC_ROW, DOC_IMAGE };
// The fonts a span names: plain, bold, italic, bold italic, code.
enum { FONT_PLAIN, FONT_BOLD, FONT_ITALIC, FONT_BOLD_ITALIC, FONT_CODE, FONTS };
// What a span is, beyond its font.
enum { SPAN_LINK = 1, SPAN_CODE = 2, SPAN_STRUCK = 4 };

typedef struct {
    int start, end, font, flags;
    int link;  // index into the document's links, for SPAN_LINK
} doc_span;

typedef struct {
    char *text;
    int len;
    doc_span *spans;
    int nspans;
    int align;  // a table cell's: 0 left, 1 centre, 2 right
} doc_text;

typedef struct {
    int kind;
    int level;        // a heading's, 1 to 6
    int quote;        // how many quotes it is inside
    int indent;       // how many lists it is inside
    char marker[16];  // a list item's first block: "•", "3.", or a task's box; empty otherwise
    bool tight;       // in a tight list: no space before it
    doc_text text;    // the block's own text; a row's cells are in `cells`
    doc_text *cells;
    int ncells;
    bool header;      // a table's header row
    int table;        // which table a row belongs to, counted from 1
    char *src;        // a picture's address, as written
    char anchor[96];  // a heading's anchor, as GitHub makes them ("#my-heading")
} doc_block;

typedef struct {
    doc_block *blocks;
    int count;
    char **links;
    int nlinks;
} doc;

doc *doc_parse(const char *markdown, size_t len);
void doc_free(doc *d);
