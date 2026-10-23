# vellum

A Markdown viewer and editor for Windows, macOS and Linux, small enough to leave open: one executable of a few megabytes, built on [gesso](https://github.com/wakamex/gesso), a C library on [Simple DirectMedia Layer (SDL)](https://www.libsdl.org/). It shows a file as GitHub does (headings, lists and task lists, quotes, code, tables, pictures, links, bold, italic and inline code), reads it again when it changes on disk, and edits its text in place.

## Use

```
vellum NOTES.md
```

Or drop a file on the window, or open one with Ctrl+O.

| Key | What it does |
|---|---|
| Ctrl+O | Open a file |
| Ctrl+E | Edit the file's text; again to go back to the rendered page |
| Ctrl+S | Save |
| Ctrl+C | Copy what is selected (drag across paragraphs, double-click a word, triple-click a paragraph) |
| Ctrl+= / Ctrl+- / Ctrl+0 | Larger, smaller, as it was |
| Alt+Left | Back to the file a link came from |
| Page Up, Page Down, the wheel | Scroll |

A link to a Markdown file beside this one opens it here, a `#heading` link scrolls to that heading, and any other link opens in the system's browser. The window comes back where it was. It follows the system's light or dark theme.

## What it costs

Measured on a Windows 11 PC (16 cores, 32 GB) with the window open on a README of 7 KB, against Cursor viewing Markdown on the same machine:

| | vellum | Cursor |
|---|---|---|
| Processes | 1 | 16 |
| Memory, private | 25 MB | 1,784 MB |
| Threads | 10 | (not measured) |
| CPU while idle, over 10 s | 0% | 2 to 10% of one core |
| On disk | 3.3 MB, one file | 2.5 GB |

It draws with SDL's software renderer: a frame of scrolling takes 3.9 ms at 1920 by 1080 and 3.7 ms at 2560 by 1400, the slowest 5.7 ms, while drawing with the GPU through Direct3D 11 costs some 50 MB more memory and 30 more threads for frames of 2 ms. `--gpu` draws with the GPU. `vellum FILE.md --bench --size W H` times the frames of scrolling a file.

## Build

Needs [Zig](https://ziglang.org/) 0.17.0 and gesso checked out beside this folder, at `../gesso`.

```
zig build -Doptimize=ReleaseSmall          # zig-out/bin/vellum
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-windows-gnu
zig build test                             # the parser's tests
```

`vellum FILE.md --shot OUT.png [--size W H] [--scroll Y]` draws a file once without showing a window and saves the picture, for checking how it looks.

## How it is made

[md4c](https://github.com/mity/md4c) (MIT licence, vendored in `vendor/md4c`) parses the Markdown as GitHub writes it. `src/doc.c` turns its events into blocks of text with spans for fonts, links and inline code, and `src/main.c` lays them out with gesso's wrapped text in several fonts (`gs_ui_text_spans`), which also gives selection across paragraphs and clickable links. Only what is on screen is shaped and drawn, and the window draws nothing while nothing changes.

Not done: editing in the rendered page itself (the editor is the file's text), raw HTML (left out), strikethrough is drawn grey rather than struck, and a table cell set to the right or centre only when it fits on one line.
