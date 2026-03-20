# nirilike

A [Hyprland](https://hyprland.org) plugin that adds a Niri-style vertical strip workspace overview.

When toggled, all workspaces on the focused monitor zoom out into a column of horizontal strips.
Clicking a strip switches to that workspace and animates it back to full-screen.

## Features

- Smooth open/close animation (uses Hyprland's `windowsMove` animation curve)
- Active workspace highlighted with an accent border
- Hovered workspace highlighted with a neutral border
- Click or touch to select and switch
- Single dispatcher: `nirilike:toggle`

## Installation

### Prerequisites

You need Hyprland's headers installed and matching your running binary.
Because this plugin is built against a custom Hyprland fork, you **cannot** use
`hyprpm update` to manage headers automatically — see the note below.

### Install via hyprpm

```bash
hyprpm add https://github.com/YOUR_USERNAME/nirilike
hyprpm enable nirilike
```

Then add to your `hyprland.conf`:

```ini
exec-once = hyprpm reload
```

### Manual build

```bash
git clone https://github.com/YOUR_USERNAME/nirilike
cd nirilike
cmake -DCMAKE_BUILD_TYPE=Release -B build
cmake --build build -j $(nproc)
# load immediately:
hyprctl plugin load $(pwd)/build/libnirilike.so
```

---

### ⚠️ Custom Hyprland fork — headers note

`hyprpm` installs headers by cloning the **upstream** `hyprwm/Hyprland` repo and
checking out the commit hash reported by `hyprctl version`. If you run a custom
fork whose commit hashes don't exist in upstream, `hyprpm update` will fail at
the headers step with _"failed to check out to running ver"_.

**Workaround — install headers from your fork:**

```bash
cd /path/to/your/hyprland/fork
# Build Hyprland first if you haven't already
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -S . -B build
cmake --build build -j $(nproc)
# Install headers to the location hyprpm expects
sudo cmake --install build --component headers
# Or, if your fork still has the legacy Makefile target:
sudo make installheaders
```

After that, `hyprpm add` / `hyprpm enable` will work normally because the headers
are already present and hyprpm won't need to fetch them from upstream.

When you update your fork, re-run the header install step above and then
`hyprpm update` to rebuild the plugin.

## Configuration

All options are optional. Default values are shown.

```ini
plugin:nirilike:gap_size    = 8      # vertical gap between strips (logical px)
plugin:nirilike:side_margin = 60     # horizontal margin on each side (logical px)
plugin:nirilike:bg_col      = 0xFF141414  # background colour (ARGB hex)
plugin:nirilike:border_col  = 0xFF6699FF  # active workspace border colour (ARGB hex)
plugin:nirilike:border_size = 3      # border thickness (logical px)
```

### Recommended keybind

```ini
bind = SUPER, Tab, nirilike:toggle
```

### Optional: load on startup

```ini
exec-once = hyprpm reload
```

## Dispatcher reference

| Dispatcher        | Argument | Effect                          |
| ----------------- | -------- | ------------------------------- |
| `nirilike:toggle` | _(none)_ | Open if closed, close if open   |
| `nirilike:toggle` | `open`   | Open (no-op if already open)    |
| `nirilike:toggle` | `close`  | Close (no-op if already closed) |

## Building from source (development)

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -B build
cmake --build build -j $(nproc)
hyprctl plugin load $(pwd)/build/libnirilike.so
```
