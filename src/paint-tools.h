#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct {
  double x;
  double y;
} PaintPoint;

typedef struct {
  GdkRGBA color;
  double size;
  GdkRGBA background_color;
  const char *active_tool_id;
  GArray *history_points; /* element-type: PaintPoint */
} PaintToolSettings;

typedef struct _PaintTool PaintTool;

struct _PaintTool {
  const char *id;
  const char *name;
  const char *icon_name;
  const char *tooltip;

  void (*begin_stroke) (const PaintTool *tool,
                        cairo_t *cr,
                        double x,
                        double y,
                        PaintToolSettings *settings);
  void (*continue_stroke) (const PaintTool *tool,
                           cairo_t *cr,
                           double x,
                           double y,
                           PaintToolSettings *settings);
  void (*end_stroke) (const PaintTool *tool,
                      cairo_t *cr,
                      double x,
                      double y,
                      PaintToolSettings *settings);
};

PaintToolSettings *paint_tool_settings_new (void);
void paint_tool_settings_free (PaintToolSettings *settings);

const PaintTool *paint_tools_get_all (gsize *n_tools);
const PaintTool *paint_tools_find (const char *tool_id);

G_END_DECLS
