# hyprwinsnap

A [Hyprland](https://hyprland.org/) plugin that snaps the size of floating windows to
configurable presets while you drag-resize them.

Drag any edge or corner of a floating window: as soon as both dimensions come within
`threshold` pixels of a preset size, the window snaps to it. Edges you are not dragging
stay in place, so the window snaps around whatever corner you grabbed.

## Build

```sh
meson setup build --buildtype=release
ninja -C build
```

Produces `build/hyprwinsnap.so`.

## Install

The plugin ships a `hyprpm.toml`, so use hyprpm, Hyprland's default plugin manager:

```sh
hyprpm add https://github.com/sitiyou/hyprwinsnap.git
hyprpm enable hyprwinsnap
hyprpm reload
```

`hyprpm list` should show `hyprwinsnap`; enabled plugins also load automatically on
compositor start.

Remove it with `hyprpm disable hyprwinsnap` followed by `hyprpm remove hyprwinsnap`.

## Configuration

The plugin registers its options under `plugin.hyprwinsnap.*`. With the Lua config
provider (default since 0.55) set them like this:

```lua
hl.config({
  plugin = {
    hyprwinsnap = {
      enabled = true,   -- master switch

      -- snap strength
      threshold  = 24,  -- capture distance (logical px); both axes must be within this to snap
      hysteresis = 32,  -- release distance (>= threshold); how far you can drag past the edge before it lets go
      presets    = "800x600, 1024x768, 1280x720",

      debug = false,    -- log every snap to the Hyprland log
    },
  },
})
```

Changes apply on `hyprctl reload`; the plugin reads the values live, no plugin reload needed.

Tuning:

- `threshold` — when snapping **starts** (how close the window must get).
- `hysteresis` — how **sticky** the snap is once engaged. Larger values keep the window
  glued to the preset even as you drag past it; set it equal to `threshold` for the least
  sticky behavior.
- `presets` — comma- or semicolon-separated `WxH` sizes.

## Usage

1. Make a window floating.
2. Drag its edge/corner with your resize keybind (e.g. `SUPER + left mouse button`).
3. Release near a preset size — the window pops to that exact size.
