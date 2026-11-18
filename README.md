# Paint

A fast, GNOME-native Paint application written in **C** with **GTK 4**, **Libadwaita**, and **Cairo**, styled after **GNOME Text Editor** and **Ptyxis**.

## Building & Running

```bash
meson setup build
meson compile -C build
./build/paint
```

To install locally for your user (adds the app & icon to GNOME's app launcher):

```bash
meson setup --reconfigure build --prefix="$HOME/.local"
meson install -C build
```

## Packaging & Updates

### Flatpak (Cross-distro + GNOME Software)

1. Build and install locally with `flatpak-builder`:
   ```bash
   sudo dnf install flatpak-builder
   flatpak install flathub org.gnome.Sdk//51
   flatpak-builder --user --install --force-clean build-flatpak packaging/flatpak/io.github.spondle.Paint.yml
   ```
2. Automated GitHub Releases: pushing a version tag (`git tag v0.1.0 && git push origin v0.1.0`) triggers [ci-release.yml](.github/workflows/ci-release.yml) to build and attach `io.github.spondle.Paint.flatpak` to a GitHub Release. Users can double-click it in GNOME Software or run:
   ```bash
   flatpak install --user io.github.spondle.Paint.flatpak
   ```

### Fedora COPR (Native `dnf` install & updates)

1. Create a project on [copr.fedorainfracloud.org](https://copr.fedorainfracloud.org/) and add a package backed by `https://github.com/Spondle/Draw` using `packaging/rpm/paint.spec`.
2. Users install and receive automatic updates with:
   ```bash
   sudo dnf copr enable spondle/draw
   sudo dnf install paint
   ```

## Architecture

- [`meson.build`](meson.build) — Meson build configuration, `GResource` bundling, and install targets
- [`src/main.c`](src/main.c) & [`src/paint-application.c`](src/paint-application.c) — `AdwApplication`, resources, preferences, shortcuts, About dialog
- [`src/paint-window.c`](src/paint-window.c) — `AdwApplicationWindow`, `AdwHeaderBar`, `AdwTabView` tear-off, `AdwTabOverview`, session restore, dialogs
- [`src/paint-canvas-page.c`](src/paint-canvas-page.c) — Scrollable/zoomable canvas workspace and floating Adwaita OSD tool pill
- [`src/paint-canvas.c`](src/paint-canvas.c) — `PaintDocument` (Cairo surface, undo/redo, file I/O) and `PaintCanvas`
- [`src/paint-tools.c`](src/paint-tools.c) — Extensible `PaintTool` registry (`brush`, `eraser`, `fill`)
- [`src/paint-preferences.c`](src/paint-preferences.c) — `AdwPreferencesDialog` and circular GNOME theme selector

## Testing

```bash
meson test -C build -v
```
