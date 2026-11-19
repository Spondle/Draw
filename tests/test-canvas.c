#include "paint-canvas.h"
#include "paint-tools.h"

#include <glib/gstdio.h>

static void
read_pixel_rgba (PaintDocument *doc, int x, int y, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
  cairo_surface_t *surface = paint_document_get_surface (doc);
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const guint8 *data = cairo_image_surface_get_data (surface);
  const guint8 *px = data + y * stride + x * 4;
  *b = px[0];
  *g = px[1];
  *r = px[2];
  *a = px[3];
}

static void
test_blank_document (void)
{
  PaintDocument *doc = paint_document_new (64, 48);
  g_assert_cmpint (paint_document_get_width (doc), ==, 64);
  g_assert_cmpint (paint_document_get_height (doc), ==, 48);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));
  g_assert_false (paint_document_can_redo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);
  paint_document_free (doc);
}

static void
test_brush_stroke_and_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (80, 80);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *brush = paint_tools_find ("brush");

  paint_document_begin_stroke (doc, 20.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 30.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, brush, settings);
  paint_document_end_stroke (doc, 50.0, 20.0, brush, settings);

  g_assert_true (paint_document_get_modified (doc));
  g_assert_true (paint_document_can_undo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_true (paint_document_can_redo (doc));

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_eraser_restores_background (void)
{
  PaintDocument *doc = paint_document_new (60, 60);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->size = 10.0;

  const PaintTool *brush = paint_tools_find ("brush");
  const PaintTool *eraser = paint_tools_find ("eraser");

  paint_document_begin_stroke (doc, 30.0, 30.0, brush, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  paint_document_begin_stroke (doc, 30.0, 30.0, eraser, settings);
  paint_document_end_stroke (doc, 30.0, 30.0, eraser, settings);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_resize_and_png_roundtrip (void)
{
  PaintDocument *doc = paint_document_new (40, 40);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 1.0f, 0.0f, 1.0f };
  settings->size = 6.0;
  const PaintTool *brush = paint_tools_find ("brush");

  paint_document_begin_stroke (doc, 10.0, 10.0, brush, settings);
  paint_document_end_stroke (doc, 10.0, 10.0, brush, settings);

  paint_document_resize (doc, 80, 60);
  g_assert_cmpint (paint_document_get_width (doc), ==, 80);
  g_assert_cmpint (paint_document_get_height (doc), ==, 60);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_autofree char *tmp_dir = g_dir_make_tmp ("paint-test-XXXXXX", NULL);
  g_autofree char *png_path = g_build_filename (tmp_dir, "out.png", NULL);
  g_autoptr (GFile) out_file = g_file_new_for_path (png_path);

  g_autoptr (GError) err = NULL;
  g_assert_true (paint_document_save_to_file (doc, out_file, &err));
  g_assert_no_error (err);
  g_assert_false (paint_document_get_modified (doc));

  PaintDocument *loaded = paint_document_new_from_file (out_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (loaded);
  g_assert_cmpint (paint_document_get_width (loaded), ==, 80);
  g_assert_cmpint (paint_document_get_height (loaded), ==, 60);
  read_pixel_rgba (loaded, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);

  paint_document_clear (loaded);
  read_pixel_rgba (loaded, 10, 10, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_remove (png_path);
  g_rmdir (tmp_dir);
  paint_document_free (loaded);
  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_fill_tool_and_undo (void)
{
  PaintDocument *doc = paint_document_new (64, 64);
  PaintToolSettings *settings = paint_tool_settings_new ();

  /* Draw a closed black box from (16,16) to (48,48) */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
  cairo_set_line_width (cr, 4.0);
  cairo_rectangle (cr, 16.0, 16.0, 32.0, 32.0);
  cairo_stroke (cr);
  cairo_destroy (cr);

  /* Flood-fill inside the box with red */
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  const PaintTool *fill = paint_tools_find ("fill");
  g_assert_cmpstr (fill->id, ==, "fill");

  paint_document_begin_stroke (doc, 32.0, 32.0, fill, settings);
  paint_document_end_stroke (doc, 32.0, 32.0, fill, settings);

  guint8 r, g, b, a;
  /* Inside filled */
  read_pixel_rgba (doc, 32, 32, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);

  /* Outside stays white */
  read_pixel_rgba (doc, 4, 4, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  /* Undo restores inside to white */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 32, 32, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_fill_antialiased_circle_no_halo (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.11f, 0.44f, 0.96f, 1.0f };

  /* Draw an anti-aliased blue circle */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.11, 0.44, 0.96, 1.0);
  cairo_set_line_width (cr, 8.0);
  cairo_arc (cr, 50.0, 50.0, 30.0, 0.0, 2.0 * G_PI);
  cairo_stroke (cr);
  cairo_destroy (cr);

  /* Fill inside with the same blue */
  const PaintTool *fill = paint_tools_find ("fill");
  paint_document_begin_stroke (doc, 50.0, 50.0, fill, settings);
  paint_document_end_stroke (doc, 50.0, 50.0, fill, settings);

  /* Every pixel inside radius 26 must be solid blue (no white AA fringe) */
  for (int y = 24; y <= 76; y++) {
    for (int x = 24; x <= 76; x++) {
      int dx = x - 50;
      int dy = y - 50;
      if (dx * dx + dy * dy <= 28 * 28) {
        guint8 r, g, b, a;
        read_pixel_rgba (doc, x, y, &r, &g, &b, &a);
        g_assert_cmpuint (r, <=, 35);
        g_assert_cmpuint (b, >=, 240);
      }
    }
  }

  /* Outside the circle must remain pure white */
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_large_canvas_tiled_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (4096, 4096);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.5f, 1.0f, 1.0f };
  settings->size = 12.0;
  const PaintTool *brush = paint_tools_find ("brush");

  /* Stroke across tile boundaries (x=240..540, y=250) */
  paint_document_begin_stroke (doc, 240.0, 250.0, brush, settings);
  for (int x = 250; x <= 540; x += 10)
    paint_document_continue_stroke (doc, (double) x, 250.0, brush, settings);
  paint_document_end_stroke (doc, 540.0, 250.0, brush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 260, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 260, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 520, 250, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 0);
  g_assert_cmpuint (b, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_cancel_stroke_restores_without_undo (void)
{
  PaintDocument *doc = paint_document_new (128, 128);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 10.0;
  const PaintTool *brush = paint_tools_find ("brush");

  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  paint_document_begin_stroke (doc, 20.0, 20.0, brush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, brush, settings);
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  paint_document_cancel_stroke (doc);
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_line_tool_and_undo_redo (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 6.0;
  const PaintTool *line = paint_tools_find ("line");
  g_assert_cmpstr (line->id, ==, "line");
  g_assert_nonnull (line->draw_preview);
  g_assert_nonnull (line->snap_point);

  paint_document_begin_stroke (doc, 10.0, 20.0, line, settings);
  /* Intermediate drag positions must not bake onto the document */
  paint_document_continue_stroke (doc, 40.0, 60.0, line, settings);
  paint_document_end_stroke (doc, 70.0, 20.0, line, settings);

  g_assert_true (paint_document_get_modified (doc));
  g_assert_true (paint_document_can_undo (doc));

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 10, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 70, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  /* Intermediate drag point stays white */
  read_pixel_rgba (doc, 40, 60, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (b, ==, 255);

  g_assert_true (paint_document_redo (doc));
  read_pixel_rgba (doc, 40, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_line_tool_tiled_undo_cancel_and_snap (void)
{
  PaintDocument *doc = paint_document_new (600, 600);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *line = paint_tools_find ("line");

  /* Cancel mid-drag leaves document clean */
  paint_document_begin_stroke (doc, 100.0, 100.0, line, settings);
  paint_document_continue_stroke (doc, 500.0, 500.0, line, settings);
  paint_document_cancel_stroke (doc);
  g_assert_false (paint_document_get_modified (doc));
  g_assert_false (paint_document_can_undo (doc));

  /* Draw across 256px tile boundaries */
  paint_document_begin_stroke (doc, 100.0, 100.0, line, settings);
  paint_document_continue_stroke (doc, 100.0, 400.0, line, settings);
  paint_document_end_stroke (doc, 500.0, 500.0, line, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 100, 100, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);
  read_pixel_rgba (doc, 500, 500, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 100, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 300, 300, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  read_pixel_rgba (doc, 500, 500, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);

  /* Verify 45-degree Shift snapping */
  double sx = 200.0, sy = 112.0;
  line->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 200.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 100.0, 1e-6);

  sx = 195.0;
  sy = 205.0;
  line->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 200.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 200.0, 1e-6);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_shapes_outline_filled_snap_and_undo (void)
{
  PaintDocument *doc = paint_document_new (200, 200);
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 1.0f, 0.0f, 0.0f, 1.0f };
  settings->size = 6.0;

  /* 1. Outline rectangle: border colored, center stays white */
  const PaintTool *rect = paint_tools_find ("shape-rectangle");
  g_assert_cmpstr (rect->id, ==, "shape-rectangle");
  g_assert_true (rect->is_shape);
  settings->shape_filled = FALSE;

  paint_document_begin_stroke (doc, 20.0, 20.0, rect, settings);
  paint_document_continue_stroke (doc, 180.0, 180.0, rect, settings);
  paint_document_end_stroke (doc, 80.0, 60.0, rect, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 50, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  read_pixel_rgba (doc, 50, 40, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* Undo restores rectangle border to white */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 50, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* 2. Filled star: center is colored, undo restores */
  const PaintTool *star = paint_tools_find ("shape-star");
  g_assert_cmpstr (star->id, ==, "shape-star");
  settings->color = (GdkRGBA) { 0.0f, 0.0f, 1.0f, 1.0f };
  settings->shape_filled = TRUE;

  paint_document_begin_stroke (doc, 40.0, 40.0, star, settings);
  paint_document_end_stroke (doc, 140.0, 140.0, star, settings);
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  /* 3. Filled circle (ellipse + Shift square snap) */
  double sx = 180.0, sy = 140.0;
  const PaintTool *ellipse = paint_tools_find ("shape-ellipse");
  ellipse->snap_point (100.0, 100.0, &sx, &sy);
  g_assert_cmpfloat_with_epsilon (sx, 180.0, 1e-6);
  g_assert_cmpfloat_with_epsilon (sy, 180.0, 1e-6);

  /* 4. Horizontal arrow across y=100: wings extend above/below and must undo cleanly */
  const PaintTool *arrow = paint_tools_find ("shape-arrow");
  settings->shape_filled = FALSE;
  settings->size = 6.0;
  paint_document_begin_stroke (doc, 20.0, 100.0, arrow, settings);
  paint_document_end_stroke (doc, 160.0, 100.0, arrow, settings);
  read_pixel_rgba (doc, 150, 100, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 150, 100, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);

  /* 5. Clear canvas clears star and can be undone */
  paint_document_clear (doc);
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 255);
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 90, 90, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (r, ==, 0);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_transparent_png_open_edit_resize_and_save (void)
{
  g_autofree char *tmp_dir = g_dir_make_tmp ("paint-alpha-test-XXXXXX", NULL);
  g_autofree char *src_path = g_build_filename (tmp_dir, "transparent.png", NULL);
  g_autofree char *out_path = g_build_filename (tmp_dir, "edited.png", NULL);

  /* Create an RGBA PNG with transparent background, opaque red rect, and semi-transparent pixel */
  cairo_surface_t *src = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 60, 60);
  cairo_t *cr = cairo_create (src);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 1.0, 0.0, 0.0, 1.0);
  cairo_rectangle (cr, 20.0, 20.0, 20.0, 20.0);
  cairo_fill (cr);
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 0.5);
  cairo_rectangle (cr, 45.0, 45.0, 10.0, 10.0);
  cairo_fill (cr);
  cairo_destroy (cr);
  g_assert_cmpint (cairo_surface_write_to_png (src, src_path), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy (src);

  /* Open transparent PNG */
  g_autoptr (GFile) src_file = g_file_new_for_path (src_path);
  g_autoptr (GError) err = NULL;
  PaintDocument *doc = paint_document_new_from_file (src_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (doc);
  g_assert_true (paint_document_has_transparent_background (doc));

  /* Background remains transparent (a == 0), opaque rect and semi-transparent rect preserved */
  guint8 r, g, b, a;
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (g, ==, 0);
  g_assert_cmpuint (b, ==, 0);
  g_assert_cmpuint (a, ==, 255);
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, >, 100);
  g_assert_cmpuint (a, <, 160);

  /* Brush draws over transparent canvas while untouched area stays transparent */
  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->color = (GdkRGBA) { 0.0f, 1.0f, 0.0f, 1.0f };
  settings->size = 8.0;
  const PaintTool *brush = paint_tools_find ("brush");
  paint_document_begin_stroke (doc, 10.0, 50.0, brush, settings);
  paint_document_end_stroke (doc, 10.0, 50.0, brush, settings);

  read_pixel_rgba (doc, 10, 50, &r, &g, &b, &a);
  g_assert_cmpuint (g, ==, 255);
  g_assert_cmpuint (a, ==, 255);
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  /* Eraser on a transparent PNG erases back to transparent, not white */
  const PaintTool *eraser = paint_tools_find ("eraser");
  paint_document_begin_stroke (doc, 10.0, 50.0, eraser, settings);
  paint_document_end_stroke (doc, 10.0, 50.0, eraser, settings);
  read_pixel_rgba (doc, 10, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  /* Resizing preserves transparency both in existing and expanded areas */
  paint_document_resize (doc, 80, 80);
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 70, 70, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Save and reopen: transparency survives full roundtrip */
  g_autoptr (GFile) out_file = g_file_new_for_path (out_path);
  g_assert_true (paint_document_save_to_file (doc, out_file, &err));
  g_assert_no_error (err);

  PaintDocument *reloaded = paint_document_new_from_file (out_file, &err);
  g_assert_no_error (err);
  g_assert_nonnull (reloaded);
  g_assert_true (paint_document_has_transparent_background (reloaded));
  read_pixel_rgba (reloaded, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (reloaded, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Clear on transparent document clears to transparent */
  paint_document_clear (reloaded);
  read_pixel_rgba (reloaded, 30, 30, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  g_remove (src_path);
  g_remove (out_path);
  g_rmdir (tmp_dir);
  paint_document_free (reloaded);
  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

static void
test_transparent_brush_and_fill_tools (void)
{
  PaintDocument *doc = paint_document_new (100, 100);
  g_assert_false (paint_document_has_transparent_background (doc));

  PaintToolSettings *settings = paint_tool_settings_new ();
  settings->size = 12.0;

  /* 1. Transparent Brush punches transparent strokes into an opaque white canvas */
  const PaintTool *tbrush = paint_tools_find ("transparent");
  g_assert_cmpstr (tbrush->id, ==, "transparent");
  g_assert_true (tbrush->is_transparency);
  g_assert_true (tbrush->uses_brush_size);

  paint_document_begin_stroke (doc, 20.0, 20.0, tbrush, settings);
  paint_document_continue_stroke (doc, 40.0, 20.0, tbrush, settings);
  paint_document_end_stroke (doc, 50.0, 20.0, tbrush, settings);

  guint8 r, g, b, a;
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 80, 80, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Resizing an opaque canvas with transparent cutouts must preserve those cutouts */
  paint_document_resize (doc, 120, 120);
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 110, 110, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Undo resize + undo transparent stroke restores opaque white */
  g_assert_true (paint_document_undo (doc));
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 30, 20, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* 2. Transparent Fill removes contiguous white background around a solid blue square */
  cairo_t *cr = cairo_create (paint_document_get_surface (doc));
  cairo_set_source_rgba (cr, 0.0, 0.0, 1.0, 1.0);
  cairo_rectangle (cr, 35.0, 35.0, 30.0, 30.0);
  cairo_fill (cr);
  cairo_destroy (cr);

  const PaintTool *tfill = paint_tools_find ("transparent-fill");
  g_assert_cmpstr (tfill->id, ==, "transparent-fill");
  g_assert_true (tfill->is_transparency);
  g_assert_false (tfill->uses_brush_size);

  paint_document_begin_stroke (doc, 5.0, 5.0, tfill, settings);
  paint_document_end_stroke (doc, 5.0, 5.0, tfill, settings);

  /* Outside becomes transparent, blue square stays opaque blue */
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* Undo restores white background */
  g_assert_true (paint_document_undo (doc));
  read_pixel_rgba (doc, 5, 5, &r, &g, &b, &a);
  g_assert_cmpuint (r, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  /* 3. Clear to transparent sets transparent background and undo restores white background */
  paint_document_clear_transparent (doc);
  g_assert_true (paint_document_has_transparent_background (doc));
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (a, ==, 0);

  g_assert_true (paint_document_undo (doc));
  g_assert_false (paint_document_has_transparent_background (doc));
  read_pixel_rgba (doc, 50, 50, &r, &g, &b, &a);
  g_assert_cmpuint (b, ==, 255);
  g_assert_cmpuint (a, ==, 255);

  paint_tool_settings_free (settings);
  paint_document_free (doc);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/canvas/blank_document", test_blank_document);
  g_test_add_func ("/canvas/brush_stroke_and_undo_redo", test_brush_stroke_and_undo_redo);
  g_test_add_func ("/canvas/eraser_restores_background", test_eraser_restores_background);
  g_test_add_func ("/canvas/resize_and_png_roundtrip", test_resize_and_png_roundtrip);
  g_test_add_func ("/canvas/fill_tool_and_undo", test_fill_tool_and_undo);
  g_test_add_func ("/canvas/fill_antialiased_circle_no_halo", test_fill_antialiased_circle_no_halo);
  g_test_add_func ("/canvas/large_canvas_tiled_undo_redo", test_large_canvas_tiled_undo_redo);
  g_test_add_func ("/canvas/cancel_stroke_restores_without_undo", test_cancel_stroke_restores_without_undo);
  g_test_add_func ("/canvas/line_tool_and_undo_redo", test_line_tool_and_undo_redo);
  g_test_add_func ("/canvas/line_tool_tiled_undo_cancel_and_snap", test_line_tool_tiled_undo_cancel_and_snap);
  g_test_add_func ("/canvas/shapes_outline_filled_snap_and_undo", test_shapes_outline_filled_snap_and_undo);
  g_test_add_func ("/canvas/transparent_png_open_edit_resize_and_save", test_transparent_png_open_edit_resize_and_save);
  g_test_add_func ("/canvas/transparent_brush_and_fill_tools", test_transparent_brush_and_fill_tools);
  return g_test_run ();
}
