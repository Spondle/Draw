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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/canvas/blank_document", test_blank_document);
  g_test_add_func ("/canvas/brush_stroke_and_undo_redo", test_brush_stroke_and_undo_redo);
  g_test_add_func ("/canvas/eraser_restores_background", test_eraser_restores_background);
  g_test_add_func ("/canvas/resize_and_png_roundtrip", test_resize_and_png_roundtrip);
  return g_test_run ();
}
