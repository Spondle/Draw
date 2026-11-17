#include "paint-tools.h"

#include <math.h>
#include <string.h>

PaintToolSettings *
paint_tool_settings_new (void)
{
  PaintToolSettings *settings = g_new0 (PaintToolSettings, 1);
  settings->color = (GdkRGBA) { 0.11f, 0.44f, 0.96f, 1.0f };
  settings->size = 6.0;
  settings->background_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  settings->active_tool_id = "brush";
  settings->history_points = g_array_new (FALSE, FALSE, sizeof (PaintPoint));
  return settings;
}

void
paint_tool_settings_free (PaintToolSettings *settings)
{
  if (!settings)
    return;
  if (settings->history_points)
    g_array_free (settings->history_points, TRUE);
  g_free (settings);
}

static void
configure_brush_context (cairo_t *cr, const PaintToolSettings *settings)
{
  cairo_set_source_rgba (cr,
                         settings->color.red,
                         settings->color.green,
                         settings->color.blue,
                         settings->color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
}

static void
configure_eraser_context (cairo_t *cr, const PaintToolSettings *settings)
{
  cairo_set_source_rgba (cr,
                         settings->background_color.red,
                         settings->background_color.green,
                         settings->background_color.blue,
                         settings->background_color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
}

static void
configure_for_tool (const PaintTool *tool, cairo_t *cr, const PaintToolSettings *settings)
{
  if (g_strcmp0 (tool->id, "eraser") == 0)
    configure_eraser_context (cr, settings);
  else
    configure_brush_context (cr, settings);
}

static void
freehand_begin_stroke (const PaintTool *tool,
                       cairo_t *cr,
                       double x,
                       double y,
                       PaintToolSettings *settings)
{
  PaintPoint pt = { x, y };
  g_array_set_size (settings->history_points, 0);
  g_array_append_val (settings->history_points, pt);

  configure_for_tool (tool, cr, settings);
  cairo_arc (cr, x, y, MAX (0.5, settings->size / 2.0), 0.0, 2.0 * G_PI);
  cairo_fill (cr);
}

static void
freehand_continue_stroke (const PaintTool *tool,
                          cairo_t *cr,
                          double x,
                          double y,
                          PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (pts->len == 0) {
    freehand_begin_stroke (tool, cr, x, y, settings);
    return;
  }

  configure_for_tool (tool, cr, settings);
  cairo_set_line_width (cr, MAX (1.0, settings->size));
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

  if (pts->len == 1) {
    PaintPoint p0 = g_array_index (pts, PaintPoint, 0);
    cairo_move_to (cr, p0.x, p0.y);
    cairo_line_to (cr, x, y);
    cairo_stroke (cr);
  } else {
    PaintPoint p0 = g_array_index (pts, PaintPoint, pts->len - 2);
    PaintPoint p1 = g_array_index (pts, PaintPoint, pts->len - 1);
    double mid1_x = (p0.x + p1.x) / 2.0;
    double mid1_y = (p0.y + p1.y) / 2.0;
    double mid2_x = (p1.x + x) / 2.0;
    double mid2_y = (p1.y + y) / 2.0;
    cairo_move_to (cr, mid1_x, mid1_y);
    cairo_curve_to (cr, p1.x, p1.y, p1.x, p1.y, mid2_x, mid2_y);
    cairo_stroke (cr);
  }

  PaintPoint next = { x, y };
  g_array_append_val (pts, next);
}

static void
freehand_end_stroke (const PaintTool *tool,
                     cairo_t *cr,
                     double x,
                     double y,
                     PaintToolSettings *settings)
{
  GArray *pts = settings->history_points;
  if (pts->len >= 2) {
    PaintPoint last = g_array_index (pts, PaintPoint, pts->len - 1);
    configure_for_tool (tool, cr, settings);
    cairo_set_line_width (cr, MAX (1.0, settings->size));
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr, last.x, last.y);
    cairo_line_to (cr, x, y);
    cairo_stroke (cr);
  }
  g_array_set_size (pts, 0);
}

static const PaintTool BUILTIN_TOOLS[] = {
  {
    .id = "brush",
    .name = "Brush",
    .icon_name = "document-edit-symbolic",
    .tooltip = "Brush (B)",
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "eraser",
    .name = "Eraser",
    .icon_name = "edit-clear-symbolic",
    .tooltip = "Eraser (E)",
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
};

const PaintTool *
paint_tools_get_all (gsize *n_tools)
{
  if (n_tools)
    *n_tools = G_N_ELEMENTS (BUILTIN_TOOLS);
  return BUILTIN_TOOLS;
}

const PaintTool *
paint_tools_find (const char *tool_id)
{
  for (gsize i = 0; i < G_N_ELEMENTS (BUILTIN_TOOLS); i++) {
    if (g_strcmp0 (BUILTIN_TOOLS[i].id, tool_id) == 0)
      return &BUILTIN_TOOLS[i];
  }
  return &BUILTIN_TOOLS[0];
}
