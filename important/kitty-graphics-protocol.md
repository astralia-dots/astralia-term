# Terminal graphics protocol

The goal of this specification is to create a flexible and performant protocol that allows the program running in the terminal, hereafter called the client, to render arbitrary pixel (raster) graphics to the screen of the terminal emulator.

The major design goals are:
- Should not require terminal emulators to understand image formats.
- Should allow specifying graphics to be drawn at individual pixel positions.
- The graphics should integrate with the text, in particular it should be possible to draw graphics below as well as above the text, with alpha blending.
- Clean, robust, and extensible escape code design.
- As few roundtrips as possible between the terminal emulator and the client.
- Support for animations and persistence.

---

## Getting the window size

To properly size images to the terminal, a program often needs to know the window size in pixels and cells.

This can be done using the `TIOCGWINSZ` ioctl:

```c
#include <sys/ioctl.h>
#include <stdio.h>

int main(int argc, char **argv) {
    struct winsize sz;
    ioctl(0, TIOCGWINSZ, &sz);
    printf("number of rows: %i, number of columns: %i, screen width: %i, screen height: %i\n",
           sz.ws_row, sz.ws_col, sz.ws_xpixel, sz.ws_ypixel);
    return 0;
}
```

You can also use the `CSI t` escape code to get the screen size:
- Send `\x1b[14t` to `STDOUT`, and kitty will reply on `STDIN` with `\x1b[4;<height>;<width>t` where height and width are the window dimensions in pixels.
- A more precise variant is `\x1b[16t`, which replies with cell pixel dimensions (`\x1b[6;<height>;<width>t`).

---

## A minimal example

A minimal script (POSIX sh / Python snippet) to display PNG images using the graphics escape code:

```python
#!/usr/bin/env python3
import sys
from base64 import standard_b64encode

def serialize_gr_command(cmd, payload=b""):
    cmd = ','.join(f'{k}={v}' for k, v in cmd.items())
    ans = []
    w = ans.append
    w(b'\033_G')
    w(cmd.encode('ascii'))
    if payload:
        w(b';')
        w(payload)
    w(b'\033\\')
    return b''.join(ans)

def write_chunked(cmd, data):
    data = standard_b64encode(data)
    while data:
        chunk, data = data[:4096], data[4096:]
        m = 1 if data else 0
        cmd['m'] = m
        sys.stdout.buffer.write(serialize_gr_command(cmd, chunk))
        sys.stdout.buffer.flush()
        cmd.clear()

if __name__ == '__main__':
    with open(sys.argv[1], 'rb') as f:
        write_chunked({'a': 'T', 'f': 100}, f.read())
```

---

## The graphics escape code

All graphics escape codes are APC (Application Programming Command) sequences of the form:

```text
ESC _ G <control data> ; <payload> ESC \
```

- **`ESC _ G`** (`\x1b_G`): Introduces the graphics command.
- **`<control data>`**: A comma-separated list of `key=value` pairs configuring the command.
- **`;`**: Separates control headers from the payload.
- **`<payload>`**: Base64-encoded binary image or chunk data.
- **`ESC \`** (`\x1b\\` or `\x07` BEL): String Terminator (ST).

Most terminal emulators safely ignore unknown APC sequences.

---

## Transferring pixel data

### Supported Formats (`f`)
- **`f=24`**: 24-bit RGB (3 bytes per pixel).
- **`f=32`**: 32-bit RGBA (4 bytes per pixel, default).
- **`f=100`**: PNG format (direct compressed container data).

### Chunked Transmission (`m`)
For large payloads, data is broken into chunks of up to 4096 base64-encoded bytes:
- **`m=1`**: More data follows for this image.
- **`m=0`**: Last chunk of the payload.

### Compression (`o`)
- **`o=z`**: Data is compressed using zlib / DEFLATE.

### Transmission Medium (`t`)
- **`t=d`**: Direct in-band transfer via terminal escape sequence payload (default).
- **`t=f`**: A regular file path on disk (must be readable by the terminal process).
- **`t=t`**: A temporary file that the terminal will delete after reading.
- **`t=s`**: POSIX shared memory object (`shm_open`).

---

## Displaying images on screen

### Controlling displayed image layout
- **Anchor position**: Images are placed at the current cursor position starting at the top-left of the cell.
- **Offsets**: Extra pixel offsets inside the starting cell can be specified with `X=<pixels>` and `Y=<pixels>`.
- **Dimensions (`c`, `r`)**: Specify dimensions in character cells (`c` columns, `r` rows). The image is automatically scaled to fit:
  - If only one of `c` or `r` is provided, the other dimension is computed based on source aspect ratio.
  - If both are specified, the image is letterboxed/pillarboxed to preserve aspect ratio without distortion.
- **Z-Index (`z`)**: Controls stacking order:
  - `z >= 0`: Placed over text.
  - `z < 0`: Placed under text (allows transparent graphics behind terminal characters).
  - `z < -1,073,741,824` (`INT32_MIN / 2`): Rendered beneath non-default background colors.

### Unicode placeholders
Allows virtual character placement where images are bound to designated placeholder Unicode characters (such as Private Use Area codepoints) so standard scrollback, reflow, and text selections manage image positions automatically.

---

## Actions and Control Data (`a`)

| Action (`a`) | Description |
| :--- | :--- |
| **`t` / `T`** | Transmit image data and display it immediately (default). |
| **`q` / `Q`** | Query terminal support for graphics protocol / formats. |
| **`d` / `D`** | Delete images or placements from memory and screen. |
| **`f` / `F`** | Transmit animation frame. |
| **`a` / `A`** | Control animation playback. |
| **`p` / `P`** | Place an already transmitted image onto the screen. |

---

## Deleting images (`a=d`)

Deletion criteria are controlled via the `d` parameter:

| Value of `d` | Meaning |
| :--- | :--- |
| **`a` / `A`** | Delete all placements visible on screen. |
| **`i` / `I`** | Delete by image ID specified with the `i` key (or specific placement via `p`). |
| **`n` / `N`** | Delete newest image with number specified via `I`. |
| **`c` / `C`** | Delete all placements intersecting the current cursor position. |
| **`p` / `P`** | Delete placements intersecting cell coordinates `(x, y)`. |
| **`q` / `Q`** | Delete placements intersecting cell `(x, y)` with matching `z`-index. |
| **`r` / `R`** | Delete images whose ID is in the range `x <= id <= y`. |
| **`x` / `X`** | Delete placements intersecting a given column `x`. |
| **`y` / `Y`** | Delete placements intersecting a given row `y`. |
| **`z` / `Z`** | Delete placements matching a specific `z`-index. |

---

## Animation

- **Frames (`a=f`)**: Frames can be loaded sequentially into an image object using frame composition modes, delays, and coordinates.
- **Playback (`a=a`)**: Controls playback loops, frame rates, pausing, and stopping.
- **Compositing**: Supports frame blending, background clears, and partial frame updates.

---

## Image persistence and storage quotas

- Images stored in terminal memory are kept within configurable RAM/storage quotas.
- Eviction follows an LRU (Least Recently Used) policy when client quota thresholds are exceeded.
- Placements scrolled out into the scrollback history buffer can either be retained or discarded based on user configuration.