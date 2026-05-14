# EPub-M5Stack-Paper-S3

An EPub reader for the **M5Stack Paper S3** (ESP32-S3), forked from [EPub-InkPlate](https://github.com/turgu1/EPub-InkPlate) by turgu1.

- **Upstream**: https://github.com/turgu1/EPub-InkPlate
- **This fork**: https://github.com/juicecultus/EPub-M5Stack-Paper-S3

---

## Features

### Core reader
- EPub (V2, V3) book format support
- TTF and OTF embedded fonts; Normal, Bold, Italic, Bold+Italic faces
- Left, center, right, and justified text alignment
- Configurable font size and indentation
- Bitmap image display with dithering (JPEG, PNG)
- Table of contents navigation
- Multiple built-in font choices, user-selectable
- Keeps last-read position for the last 10 books
- Linear and matrix view of the book library
- UTF-8 / Latin-1 character support
- Limited CSS formatting

### M5Stack Paper S3 specific

#### Touch gestures
The GT911 capacitive touch controller is polled every 20 ms and classifies touches into a full gesture vocabulary:

| Gesture | Action |
|---|---|
| Swipe left | Next page |
| Swipe right | Previous page |
| Swipe down (from top edge) | Open reading menu |
| Swipe up | Dismiss reading menu |
| Tap | Button / selection |
| Long press (≥ 600 ms) | Hold action |

Swipes are recognized at 60 px displacement or 40 px + under 200 ms. A diagonal swipe favors the horizontal axis (page turns are the dominant gesture). Multiple queued swipes from a boot window are coalesced to the most recent — no ghost taps after wake.

#### Page navigation
A dedicated navigation overlay lets you jump anywhere in the book without paging through one by one:

- **Progress slider** — drag a thumb along a horizontal track; position maps linearly to page number
- **Arrow buttons** — step ±1 or ±10 pages
- **Numeric keypad** — tap the current page number to type a destination directly
- **Progress display** — shows `current / total (percentage%)`

#### USB Drive Mode
From the Options menu, select *USB Drive Mode* to mount the SD card as a standard USB mass storage device. The e-ink panel displays instructions and holds that image for the entire session (e-ink is persistent — no power needed to maintain the display). Once mounted, drag EPUBs into `/books` from your computer's file manager like any USB stick.

To exit: eject the drive from your computer first, then tap the screen. The device restarts and remounts the SD card normally. Note that while USB Drive Mode is active, fonts and other SD card resources are unavailable to the app — the restart is necessary.

#### WiFi web server
From the Options menu, select *WiFi Access to the e-books folder*. The device connects to the network (credentials from `/sdcard/.config`), starts an HTTP server, and displays its IP address. Open that address in a browser to:

- Browse the `/books` directory
- Upload EPUBs (up to 25 MB per file; existing files are not overwritten)
- Delete books (also cleans up the associated page-location and TOC cache files)
- Download books from the card to your computer

Tap the screen to shut down the server and return to the reader.

#### PSRAM-backed page cache
A background thread pre-renders up to 10 pages into a 2.56 MB PSRAM allocation, using the same layout and DOM pipeline as foreground rendering but writing into off-screen framebuffers instead of the panel. When you turn a page:

- **Cache hit**: the pre-rendered framebuffer is pushed to the panel in ~120 ms (fast EPD waveform, no layout work)
- **Cache miss**: full render runs on the main thread, ~1.4 s as before

The cache is invalidated automatically when you change font size, orientation, or image display settings. The background thread is pinned to core 0 and paused while menus or other screens are painting to prevent framebuffer conflicts.

#### Wake snapshot persistence
When the device enters deep sleep, the current page and up to 9 neighboring pre-rendered pages are written to `/sdcard/.wake_snapshot.bin`. On the next boot:

1. The primary page is painted immediately from the snapshot (~700 ms after boot, before any book parsing)
2. Once the book opens, neighboring pages are injected back into the live page cache — the first few swipes after wake are instant

The snapshot includes a hash of the current format settings (font size, orientation, etc.). If you change settings before the next wake, the cache entries are skipped rather than displaying stale layouts.

#### Non-blocking page navigation
Page-turn requests are handled asynchronously via a message queue. The UI never blocks on layout computation — if a page isn't cached yet, the request is queued and the app remains responsive. This replaced a design that could freeze the UI for up to 10 seconds waiting on the page-location retriever.

#### Async books-directory refresh
The library scan (which builds `books_dir.db`) runs on a dedicated worker thread. This means USB and WiFi stay usable while the directory refreshes, and the UI redraws in place as results come in rather than blocking on a full rescan.

#### Other improvements
- **Persistent menu strip**: the top bar stays painted while sub-forms (options, parameters) render below it, so the screen doesn't go blank during navigation
- **Fast EPD update mode**: menus and progress indicators use the panel's fast waveform path, reducing visible flicker on transient repaints
- **Transactional font swap**: changing the default font is atomic — the new font is validated and capped at 2 MB before replacing the old one, preventing OOM crashes on oversized font files
- **Graceful book-load failure**: if the page-location retriever times out or fails, stale NVS state is cleared and the app returns to the library rather than looping
- **Instant deep-sleep feel**: the sleep screen is painted to the panel before the ESP32 enters deep sleep, so the last thing the user sees isn't a half-rendered page

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/juicecultus/EPub-M5Stack-Paper-S3.git
cd EPub-M5Stack-Paper-S3

# Build
pio run -e paper_s3

# Flash
pio run -e paper_s3 -t upload
```

The PlatformIO environment for this device is `paper_s3` (see `platformio.ini`).

---

## SD card setup

The app requires a FAT32-formatted micro-SD card with two folders:

```
/books/   ← put .epub files here (extension must be lowercase)
/fonts/   ← put the bundled font files here
```

The `SDCard/` folder in this repo mirrors the expected card layout. The `books_dir.db` file is managed automatically by the app — it caches book metadata for fast library display and is refreshed at boot and on demand from the parameters menu.

A mandatory icon font (`drawings.otf`) must be present in the `fonts/` folder. It ships with the project under `SDCard/fonts/`.

---

## Runtime environment

Eight font families are included. Each has four faces (regular, bold, oblique, bold-italic). Fonts are subsetted to Latin-1 for size; the originals live in `fonts/orig/` and can be re-subsetted with `fonts/subsetter.sh` (requires `pip install fonttools brotli zopfli`).

---

## Development environment

[Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO](https://platformio.org/) extension.

This fork builds with **C++20** (`-std=gnu++20`). The ESP32-S3 toolchain (GCC 14.2.0) supports it fully, which enables `std::jthread`, `std::stop_token`, `std::shared_mutex`, `std::span`, designated initializers, and constexpr improvements used throughout the codebase. The upstream project used C++11/14.

| Folder | Contents |
|---|---|
| `include/`, `src/` | Shared source (ESP32 + Linux) |
| `lib_esp32/` | ESP32/M5Stack-specific code |
| `lib_linux/` | Linux simulator code |
| `lib_freetype/` | Pre-compiled FreeType for ESP32 |
| `freetype-2.10.4/` | FreeType source (subsetted config) |

### Dependencies

- [FreeType 2.10.4](https://www.freetype.org) — TrueType/OpenType rasterizer, compiled for ESP32 with a stripped module set
- [PugiXML](https://pugixml.org/) — XML parsing
- [PNGLE](https://github.com/kikuchan/pngle) — PNG decoding (modified for grayscale)
- [TJPGD](http://elm-chan.org/fsw/tjpgd/00index.html) — JPEG decoding
- [MINIZ](https://github.com/richgel999/miniz) — zip/deflate (used for epub and PNG)
- [STB](https://github.com/nothings/stb) (`stb_image_resize.h`) — image resizing
- [GTK+3](https://www.gtk.org/) — Linux simulator only (`sudo apt-get install libgtk-3-dev`)

---

## In Memoriam

*From the original EPub-InkPlate author, turgu1:*

When I started this effort, I was aiming at supplying a tailored ebook reader for a friend of mine that has been impaired by a spinal cord injury for the last 13 years and a half. Reading books and looking at TV were the only activities she was able to do as she lost control of her body, from the neck down to the feet. After several years of physiotherapy, she was able to do some movement with her arms, without any control of her fingers. She was then able to push on buttons of an ebook reader with a lot of difficulties. I wanted to build a joystick-based interface to help her with any standard ebook reader but none of the commercially available readers allowed for this kind of integration.

On September 27th, 2020, we learned that she was diagnosed with the Covid-19 virus. She passed away during the night of October 1st.

I dedicate this effort to her. Claudette, my wife and I will always remember you!
