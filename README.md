# Paint

A fast, GNOME-native Paint application written in **C** with **GTK 4**, **Libadwaita**, and **Cairo**, styled after **GNOME Text Editor** and **Ptyxis**.

## Building & Running

```bash
meson setup build
meson compile -C build
./build/paint
```

You can also pass an image path to open it directly:

```bash
./build/paint path/to/image.png
```

## Architecture

- [`meson.build`](meson.build) — Meson build configuration and `GResource` bundling
- [`src/main.c`](src/main.c) — Entry point
- [`src/paint-application.c`](src/paint-application.c) — `AdwApplication`, CSS `GResource`, accelerators, Shortcuts & About dialogs
- [`src/paint-window.c`](src/paint-window.c) — `AdwApplicationWindow` with `AdwToolbarView`, `AdwHeaderBar`, `AdwTabView` / `AdwTabBar` / `AdwTabOverview`, recent files, and Open/Save/Resize dialogs
- [`src/paint-canvas-page.c`](src/paint-canvas-page.c) — Scrollable/zoomable canvas workspace and floating Adwaita OSD tool pill
- [`src/paint-canvas.c`](src/paint-canvas.c) — `PaintDocument` (Cairo surface, fast pixel-snapshot undo/redo, file I/O) and `PaintCanvas` (`GtkDrawingArea`)
- [`src/paint-tools.c`](src/paint-tools.c) — Extensible `PaintTool` registry (`brush`, `eraser`)

## Testing

```bash
meson test -C build -v
```
