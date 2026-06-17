# Vanta Game — native games, written in plain English

Write a game in **Vanta**, compile it with **`vc`** to a **native binary**, run it
in a real window via **SDL2**. No Python and no browser at runtime — the proof
that Vanta is a real, self-hosting, native language.

```sh
./build.sh snake      # Vanta --(vc)--> C --(cc + SDL2)--> ./snake
./snake               # a native window opens, with sound
```

## Games

| | |
| --- | --- |
| **`tetris`** | full Tetris — 7 pieces, rotation, line clears, levels. ←/→ move, ↑ rotate, ↓ soft-drop, Space hard-drop. |
| **`snake`** | classic Snake — grow, eat, don't crash. Arrow keys, Space to restart. |
| **`pong`** | Pong vs the computer. Up/Down arrows. |
| **`bounce`** | a Breakout-style paddle bouncer. ←/→ arrows. |
| **`sprites`** | a demo of pixel-art sprites defined as plain Vanta strings. |
| **`vantagotchi`** | a virtual pet — feed/play/sleep/clean as Food, Fun and Energy decay in real time; Vee changes mood, naps and needs cleaning up after. `F` feed, `P` play, `S` sleep, `C` clean. |

Build any of them with `./build.sh <name>` and run `./<name>`.

## How it works

```
bounce.va  ──vc -k──▶  bounce.va.c        (Vanta compiled to C, no runtime)
                              +
sdlrt.c (the SDL runtime: Value system, 8×8 font, drawing, input, a window)
                              ▼
                     cc + SDL2  ──▶  ./bounce   (one native executable)
```

`sdlrt.c` is the **same graphics model as the V-NOx bare-metal kernel** — a `BACK`
pixel buffer, the 8×8 font, `fill`/`rgb`/`text`/`clear`/`present` — but hosted on
SDL instead of a raw framebuffer. So the *same* Vanta graphics code can target
**bare metal** (the kernel runtime) **or** a **native desktop app** (this runtime).

## The game API (call these from Vanta)

| | |
| --- | --- |
| `screen_w()` `screen_h()` | window size |
| `rgb(r,g,b)` | pack a colour |
| `background(c)` | set the clear colour |
| `clear()` / `present()` | start a frame / show it |
| `fill(x,y,w,h,c)` `rfill(x,y,w,h,c,r)` `circle(x,y,r,c)` | shapes |
| `rgrad` `rblend` | gradient / translucent shapes |
| `text_at` `text_big` `text_huge` `(x,y,s,c)` | text |
| `line(x0,y0,x1,y1,c)` `rect(x,y,w,h,c)` | line / outlined rectangle |
| `sprite(x,y, rows, scale, palette)` | pixel art — `rows` = list of strings, each char → a colour in the `palette` map (`' '`/`'.'` = transparent) |
| `window(w, h)` | set the window size (call once at the start) |
| `poll()` | pump input each frame |
| `held(name)` | is a key held now (`left`/`right`/`up`/`down`/`space`/`escape`/`a`..`z`) |
| `pressed(name)` | was a key *just* pressed this frame (edge) |
| `key()` | the last typed character |
| `mouse_x()` `mouse_y()` `mouse_down()` | pointer |
| `sound(freq, ms)` | play a tone (a tiny square-wave synth) |
| `random(n)` `random_range(a,b)` | random integers |
| `quit()` | did the user close the window |
| `ticks()` `delay(ms)` | timing |
| `title(s)` | window title |

## A game loop looks like this

```
background(rgb(12,14,32))
gc_mark()
while quit() is 0
    frame_reset()
    poll()
    if held("left") is 1
        change x to x - 6
    end
    clear()
    rfill(x, y, 40, 40, rgb(94,240,200), 8)
    present()
    delay(16)
end
```

## Memory

The runtime has a small **conservative mark-sweep garbage collector**, so games
can grow lists/maps freely (a Snake body, particles, etc.) and memory stays
bounded — no manual frees, no leaks. Numbers are integers (no floats), which is
plenty for arcade games.

## Requirements

`python3` (for `vc` at build time), a C compiler, and **SDL2**
(`brew install sdl2`). The built binary itself needs only SDL2 — no Python.
