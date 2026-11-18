#include "paint-tools.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FILL_TOLERANCE 72
#define FILL_FRINGE_MAX_DIST 185
#define FILL_COLLINEAR_ERR 18

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

typedef struct {
  int x;
  int y;
} FillSeed;

static inline gboolean
pixel_matches (uint32_t px, uint32_t target, int tol)
{
  if (px == target)
    return TRUE;
  int a0 = (int) ((target >> 24) & 0xFF);
  int r0 = (int) ((target >> 16) & 0xFF);
  int g0 = (int) ((target >> 8) & 0xFF);
  int b0 = (int) (target & 0xFF);

  int a1 = (int) ((px >> 24) & 0xFF);
  int r1 = (int) ((px >> 16) & 0xFF);
  int g1 = (int) ((px >> 8) & 0xFF);
  int b1 = (int) (px & 0xFF);

  return abs (a1 - a0) <= tol &&
         abs (r1 - r0) <= tol &&
         abs (g1 - g0) <= tol &&
         abs (b1 - b0) <= tol;
}

static inline int
pixel_max_diff (uint32_t px, uint32_t target)
{
  int da = abs ((int) ((px >> 24) & 0xFF) - (int) ((target >> 24) & 0xFF));
  int dr = abs ((int) ((px >> 16) & 0xFF) - (int) ((target >> 16) & 0xFF));
  int dg = abs ((int) ((px >> 8) & 0xFF) - (int) ((target >> 8) & 0xFF));
  int db = abs ((int) (px & 0xFF) - (int) (target & 0xFF));
  return MAX (MAX (da, dr), MAX (dg, db));
}

static inline gboolean
pixel_is_blend_between (uint32_t px, uint32_t target, uint32_t fill)
{
  double ta = (double) ((target >> 24) & 0xFF);
  double tr = (double) ((target >> 16) & 0xFF);
  double tg = (double) ((target >> 8) & 0xFF);
  double tb = (double) (target & 0xFF);

  double da = (double) ((fill >> 24) & 0xFF) - ta;
  double dr = (double) ((fill >> 16) & 0xFF) - tr;
  double dg = (double) ((fill >> 8) & 0xFF) - tg;
  double db = (double) (fill & 0xFF) - tb;

  double len2 = da * da + dr * dr + dg * dg + db * db;
  if (len2 < 1.0)
    return FALSE;

  double pa = (double) ((px >> 24) & 0xFF) - ta;
  double pr = (double) ((px >> 16) & 0xFF) - tr;
  double pg = (double) ((px >> 8) & 0xFF) - tg;
  double pb = (double) (px & 0xFF) - tb;

  double t = (pa * da + pr * dr + pg * dg + pb * db) / len2;
  if (t <= 0.0 || t >= 0.995)
    return FALSE;
  if (t > 1.0)
    t = 1.0;

  double ea = fabs (pa - t * da);
  double er = fabs (pr - t * dr);
  double eg = fabs (pg - t * dg);
  double eb = fabs (pb - t * db);
  return MAX (MAX (ea, er), MAX (eg, eb)) <= (double) FILL_COLLINEAR_ERR;
}

static inline uint32_t
rgba_to_premultiplied_argb32 (const GdkRGBA *c, uint8_t *out_a)
{
  uint8_t a = (uint8_t) lround (CLAMP (c->alpha, 0.0f, 1.0f) * 255.0);
  uint8_t r = (uint8_t) lround (CLAMP (c->red, 0.0f, 1.0f) * a);
  uint8_t g = (uint8_t) lround (CLAMP (c->green, 0.0f, 1.0f) * a);
  uint8_t b = (uint8_t) lround (CLAMP (c->blue, 0.0f, 1.0f) * a);
  if (out_a)
    *out_a = a;
  return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | (uint32_t) b;
}

static inline uint32_t
blend_over_argb32 (uint32_t dst, uint32_t src_pm, uint8_t src_a)
{
  if (src_a == 255)
    return src_pm;
  if (src_a == 0)
    return dst;

  uint32_t inv_a = 255 - src_a;
  uint32_t dst_a = (dst >> 24) & 0xFF;
  uint32_t dst_r = (dst >> 16) & 0xFF;
  uint32_t dst_g = (dst >> 8) & 0xFF;
  uint32_t dst_b = dst & 0xFF;

  uint32_t src_r = (src_pm >> 16) & 0xFF;
  uint32_t src_g = (src_pm >> 8) & 0xFF;
  uint32_t src_b = src_pm & 0xFF;

  uint32_t out_a = src_a + (dst_a * inv_a + 127) / 255;
  uint32_t out_r = src_r + (dst_r * inv_a + 127) / 255;
  uint32_t out_g = src_g + (dst_g * inv_a + 127) / 255;
  uint32_t out_b = src_b + (dst_b * inv_a + 127) / 255;

  return (MIN (255u, out_a) << 24) |
         (MIN (255u, out_r) << 16) |
         (MIN (255u, out_g) << 8) |
         MIN (255u, out_b);
}

static void
fill_begin_stroke (const PaintTool *tool G_GNUC_UNUSED,
                   cairo_t *cr,
                   double x,
                   double y,
                   PaintToolSettings *settings)
{
  cairo_surface_t *surface = cairo_get_target (cr);
  if (!surface || cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return;

  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *data = cairo_image_surface_get_data (surface);

  int sx = (int) floor (x);
  int sy = (int) floor (y);
  if (sx < 0 || sx >= w || sy < 0 || sy >= h)
    return;

  uint8_t fill_a = 255;
  uint32_t fill_px = rgba_to_premultiplied_argb32 (&settings->color, &fill_a);
  if (fill_a == 0)
    return;

  uint32_t *start_row = (uint32_t *) (data + (gsize) sy * (gsize) stride);
  uint32_t target_px = start_row[sx];
  if (fill_a == 255 && target_px == fill_px)
    return;

  guint8 *visited = g_malloc0 ((gsize) w * (gsize) h);
  GArray *stack = g_array_new (FALSE, FALSE, sizeof (FillSeed));
  FillSeed init = { sx, sy };
  g_array_append_val (stack, init);

  int min_x = sx;
  int max_x = sx;
  int min_y = sy;
  int max_y = sy;

  while (stack->len > 0) {
    FillSeed seed = g_array_index (stack, FillSeed, stack->len - 1);
    g_array_set_size (stack, stack->len - 1);

    int row_y = seed.y;
    if (visited[(gsize) row_y * (gsize) w + (gsize) seed.x])
      continue;

    uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
    guint8 *vis_row = visited + (gsize) row_y * (gsize) w;
    if (!pixel_matches (row[seed.x], target_px, FILL_TOLERANCE))
      continue;

    int left = seed.x;
    while (left > 0 && !vis_row[left - 1] &&
           pixel_matches (row[left - 1], target_px, FILL_TOLERANCE)) {
      left--;
    }

    int right = seed.x;
    while (right + 1 < w && !vis_row[right + 1] &&
           pixel_matches (row[right + 1], target_px, FILL_TOLERANCE)) {
      right++;
    }

    if (left < min_x) min_x = left;
    if (right > max_x) max_x = right;
    if (row_y < min_y) min_y = row_y;
    if (row_y > max_y) max_y = row_y;

    gboolean span_above = FALSE;
    gboolean span_below = FALSE;

    for (int col = left; col <= right; col++) {
      vis_row[col] = 1;
      row[col] = blend_over_argb32 (row[col], fill_px, fill_a);

      if (row_y > 0) {
        uint32_t *above_row = (uint32_t *) (data + (gsize) (row_y - 1) * (gsize) stride);
        guint8 *above_vis = visited + (gsize) (row_y - 1) * (gsize) w;
        gboolean match = !above_vis[col] && pixel_matches (above_row[col], target_px, FILL_TOLERANCE);
        if (!span_above && match) {
          FillSeed s = { col, row_y - 1 };
          g_array_append_val (stack, s);
          span_above = TRUE;
        } else if (span_above && !match) {
          span_above = FALSE;
        }
      }

      if (row_y + 1 < h) {
        uint32_t *below_row = (uint32_t *) (data + (gsize) (row_y + 1) * (gsize) stride);
        guint8 *below_vis = visited + (gsize) (row_y + 1) * (gsize) w;
        gboolean match = !below_vis[col] && pixel_matches (below_row[col], target_px, FILL_TOLERANCE);
        if (!span_below && match) {
          FillSeed s = { col, row_y + 1 };
          g_array_append_val (stack, s);
          span_below = TRUE;
        } else if (span_below && !match) {
          span_below = FALSE;
        }
      }
    }
  }

  /* Pass 2: smooth/fill 1px anti-aliased fringe adjacent to the filled mask */
  int y0 = MAX (0, min_y - 1);
  int y1 = MIN (h - 1, max_y + 1);
  int x0 = MAX (0, min_x - 1);
  int x1 = MIN (w - 1, max_x + 1);

  for (int row_y = y0; row_y <= y1; row_y++) {
    uint32_t *row = (uint32_t *) (data + (gsize) row_y * (gsize) stride);
    guint8 *vis_row = visited + (gsize) row_y * (gsize) w;

    for (int col = x0; col <= x1; col++) {
      if (vis_row[col])
        continue;

      gboolean has_filled_neighbor = FALSE;
      for (int dy = -1; dy <= 1 && !has_filled_neighbor; dy++) {
        int ny = row_y + dy;
        if (ny < 0 || ny >= h)
          continue;
        guint8 *nvis = visited + (gsize) ny * (gsize) w;
        for (int dx = -1; dx <= 1; dx++) {
          int nx = col + dx;
          if (nx >= 0 && nx < w && nvis[nx]) {
            has_filled_neighbor = TRUE;
            break;
          }
        }
      }
      if (!has_filled_neighbor)
        continue;

      uint32_t px = row[col];
      if (pixel_is_blend_between (px, target_px, fill_px)) {
        row[col] = blend_over_argb32 (px, fill_px, fill_a);
      } else {
        int diff = pixel_max_diff (px, target_px);
        if (diff <= FILL_FRINGE_MAX_DIST) {
          double weight = (double) (FILL_FRINGE_MAX_DIST + 10 - diff) /
                          (double) (FILL_FRINGE_MAX_DIST + 10);
          uint8_t blend_a = (uint8_t) lround (CLAMP (weight, 0.0, 1.0) * (double) fill_a);
          if (blend_a > 0) {
            uint8_t pm_r = (uint8_t) lround (CLAMP (settings->color.red, 0.0f, 1.0f) * blend_a);
            uint8_t pm_g = (uint8_t) lround (CLAMP (settings->color.green, 0.0f, 1.0f) * blend_a);
            uint8_t pm_b = (uint8_t) lround (CLAMP (settings->color.blue, 0.0f, 1.0f) * blend_a);
            uint32_t pm_px = ((uint32_t) blend_a << 24) |
                             ((uint32_t) pm_r << 16) |
                             ((uint32_t) pm_g << 8) |
                             (uint32_t) pm_b;
            row[col] = blend_over_argb32 (px, pm_px, blend_a);
          }
        }
      }
    }
  }

  g_array_free (stack, TRUE);
  g_free (visited);
  cairo_surface_mark_dirty (surface);
}

static void
fill_noop (const PaintTool *tool G_GNUC_UNUSED,
           cairo_t *cr G_GNUC_UNUSED,
           double x G_GNUC_UNUSED,
           double y G_GNUC_UNUSED,
           PaintToolSettings *settings G_GNUC_UNUSED)
{
}

static const PaintTool BUILTIN_TOOLS[] = {
  {
    .id = "brush",
    .name = "Brush",
    .icon_name = "document-edit-symbolic",
    .tooltip = "Brush (B)",
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "eraser",
    .name = "Eraser",
    .icon_name = "tool-eraser-symbolic",
    .tooltip = "Eraser (E)",
    .uses_brush_size = TRUE,
    .begin_stroke = freehand_begin_stroke,
    .continue_stroke = freehand_continue_stroke,
    .end_stroke = freehand_end_stroke,
  },
  {
    .id = "fill",
    .name = "Fill",
    .icon_name = "tool-fill-symbolic",
    .tooltip = "Fill (F)",
    .uses_brush_size = FALSE,
    .begin_stroke = fill_begin_stroke,
    .continue_stroke = fill_noop,
    .end_stroke = fill_noop,
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
