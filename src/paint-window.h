#pragma once

#include <adwaita.h>

#include "paint-canvas.h"

G_BEGIN_DECLS

#define PAINT_TYPE_WINDOW (paint_window_get_type ())
G_DECLARE_FINAL_TYPE (PaintWindow, paint_window, PAINT, WINDOW, AdwApplicationWindow)

PaintWindow *paint_window_new (AdwApplication *app);
AdwTabPage *paint_window_add_new_tab (PaintWindow *self, PaintDocument *doc);
void paint_window_open_file (PaintWindow *self, GFile *file);
void paint_window_apply_preferences (PaintWindow *self);

G_END_DECLS
