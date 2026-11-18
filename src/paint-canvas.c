#include "paint-canvas.h"

#include <math.h>
#include <string.h>

#define MAX_UNDO_STEPS 40
#define UNDO_TILE_SIZE 256
#define VIEWPORT_OVERSCAN 192

static const double ZOOM_STEPS[] = {
  0.05, 0.08, 0.10, 0.15, 0.20, 0.25, 0.33, 0.5, 0.67, 0.75,
  1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0
};

/* ============================================================================
 * Tiled / Full-Surface Undo Snapshots
 * ============================================================================ */

typedef struct {
  int x;
  int y;
  int w;
  int h;
  GBytes *pixels; /* packed row-major w * 4 bytes */
} UndoTile;

static void
undo_tile_free (gpointer ptr)
{
  UndoTile *t = ptr;
  if (!t)
    return;
  g_clear_pointer (&t->pixels, g_bytes_unref);
  g_free (t);
}

typedef struct {
  int width;
  int height;
  gboolean is_full;
  int full_stride;
  GBytes *full_pixels;
  GPtrArray *tiles;       /* element-type: UndoTile* */
  GHashTable *saved_keys; /* key: uint32 packed (ty << 16) | tx */
} UndoStep;

static void
undo_step_free (gpointer ptr)
{
  UndoStep *step = ptr;
  if (!step)
    return;
  g_clear_pointer (&step->full_pixels, g_bytes_unref);
  g_clear_pointer (&step->tiles, g_ptr_array_unref);
  g_clear_pointer (&step->saved_keys, g_hash_table_unref);
  g_free (step);
}

static UndoTile *
capture_rect_tile (cairo_surface_t *surface, int x, int y, int w, int h)
{
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *src = cairo_image_surface_get_data (surface);
  gsize row_bytes = (gsize) w * 4;
  unsigned char *buf = g_malloc (row_bytes * (gsize) h);

  for (int row = 0; row < h; row++) {
    const unsigned char *src_row = src + (gsize) (y + row) * (gsize) stride + (gsize) x * 4;
    memcpy (buf + (gsize) row * row_bytes, src_row, row_bytes);
  }

  UndoTile *t = g_new0 (UndoTile, 1);
  t->x = x;
  t->y = y;
  t->w = w;
  t->h = h;
  t->pixels = g_bytes_new_take (buf, row_bytes * (gsize) h);
  return t;
}

static void
restore_rect_tile (cairo_surface_t *surface, const UndoTile *t)
{
  int stride = cairo_image_surface_get_stride (surface);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (t->pixels, &len);
  gsize row_bytes = (gsize) t->w * 4;

  for (int row = 0; row < t->h; row++) {
    unsigned char *dst_row = dst + (gsize) (t->y + row) * (gsize) stride + (gsize) t->x * 4;
    memcpy (dst_row, src + (gsize) row * row_bytes, row_bytes);
  }
}

static UndoStep *
undo_step_new_full (cairo_surface_t *surface)
{
  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);

  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = w;
  step->height = h;
  step->is_full = TRUE;
  step->full_stride = stride;
  step->full_pixels = g_bytes_new (data, (gsize) stride * (gsize) h);
  return step;
}

static UndoStep *
undo_step_new_tiled (int width, int height)
{
  UndoStep *step = g_new0 (UndoStep, 1);
  step->width = width;
  step->height = height;
  step->is_full = FALSE;
  step->tiles = g_ptr_array_new_with_free_func (undo_tile_free);
  step->saved_keys = g_hash_table_new (g_direct_hash, g_direct_equal);
  return step;
}

static void
undo_step_capture_rect (UndoStep *step,
                        cairo_surface_t *surface,
                        int rx,
                        int ry,
                        int rw,
                        int rh)
{
  int x0 = MAX (0, rx);
  int y0 = MAX (0, ry);
  int x1 = MIN (step->width, rx + rw);
  int y1 = MIN (step->height, ry + rh);
  if (x0 >= x1 || y0 >= y1)
    return;

  int tx0 = x0 / UNDO_TILE_SIZE;
  int ty0 = y0 / UNDO_TILE_SIZE;
  int tx1 = (x1 - 1) / UNDO_TILE_SIZE;
  int ty1 = (y1 - 1) / UNDO_TILE_SIZE;

  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      guint key = (((guint) ty) << 16) | (guint) tx;
      if (g_hash_table_contains (step->saved_keys, GUINT_TO_POINTER (key)))
        continue;
      g_hash_table_add (step->saved_keys, GUINT_TO_POINTER (key));

      int tile_x = tx * UNDO_TILE_SIZE;
      int tile_y = ty * UNDO_TILE_SIZE;
      int tile_w = MIN (UNDO_TILE_SIZE, step->width - tile_x);
      int tile_h = MIN (UNDO_TILE_SIZE, step->height - tile_y);
      g_ptr_array_add (step->tiles, capture_rect_tile (surface, tile_x, tile_y, tile_w, tile_h));
    }
  }
}

static cairo_surface_t *
restore_full_surface (const UndoStep *step)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         step->width,
                                                         step->height);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (step->full_pixels, &len);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  int dst_stride = cairo_image_surface_get_stride (surface);

  if (dst_stride == step->full_stride) {
    memcpy (dst, src, len);
  } else {
    int copy_bytes = MIN (dst_stride, step->full_stride);
    for (int y = 0; y < step->height; y++)
      memcpy (dst + y * dst_stride, src + y * step->full_stride, (gsize) copy_bytes);
  }
  cairo_surface_mark_dirty (surface);
  return surface;
}

/* ============================================================================
 * PaintDocument
 * ============================================================================ */

struct _PaintDocument {
  int width;
  int height;
  GdkRGBA background_color;
  cairo_surface_t *surface;
  GFile *file;
  gboolean modified;
  GPtrArray *undo_stack; /* element-type: UndoStep* */
  GPtrArray *redo_stack; /* element-type: UndoStep* */

  UndoStep *active_stroke_undo;
  double prev_stroke_x;
  double prev_stroke_y;
  double prev2_stroke_x;
  double prev2_stroke_y;

  gboolean all_dirty;
  gboolean has_dirty_rect;
  cairo_rectangle_int_t dirty_rect;
};

static void
mark_all_dirty (PaintDocument *doc)
{
  doc->all_dirty = TRUE;
  doc->has_dirty_rect = FALSE;
}

static void
mark_rect_dirty (PaintDocument *doc, int x, int y, int w, int h)
{
  if (doc->all_dirty)
    return;
  int x0 = MAX (0, x);
  int y0 = MAX (0, y);
  int x1 = MIN (doc->width, x + w);
  int y1 = MIN (doc->height, y + h);
  if (x0 >= x1 || y0 >= y1)
    return;

  if (!doc->has_dirty_rect) {
    doc->dirty_rect = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
    doc->has_dirty_rect = TRUE;
  } else {
    int ux0 = MIN (doc->dirty_rect.x, x0);
    int uy0 = MIN (doc->dirty_rect.y, y0);
    int ux1 = MAX (doc->dirty_rect.x + doc->dirty_rect.width, x1);
    int uy1 = MAX (doc->dirty_rect.y + doc->dirty_rect.height, y1);
    doc->dirty_rect = (cairo_rectangle_int_t) { ux0, uy0, ux1 - ux0, uy1 - uy0 };
  }
}

static void
push_undo_step (PaintDocument *doc, UndoStep *step)
{
  g_ptr_array_add (doc->undo_stack, step);
  if (doc->undo_stack->len > MAX_UNDO_STEPS)
    g_ptr_array_remove_index (doc->undo_stack, 0);
  g_ptr_array_set_size (doc->redo_stack, 0);
}

static cairo_surface_t *
create_blank_surface (int width, int height, const GdkRGBA *bg)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create (surface);
  cairo_set_source_rgba (cr, bg->red, bg->green, bg->blue, bg->alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  return surface;
}

PaintDocument *
paint_document_new (int width, int height)
{
  PaintDocument *doc = g_new0 (PaintDocument, 1);
  doc->width = MAX (1, width);
  doc->height = MAX (1, height);
  doc->background_color = (GdkRGBA) { 1.0f, 1.0f, 1.0f, 1.0f };
  doc->surface = create_blank_surface (doc->width, doc->height, &doc->background_color);
  doc->undo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->redo_stack = g_ptr_array_new_with_free_func (undo_step_free);
  doc->all_dirty = TRUE;
  return doc;
}

typedef struct {
  const guint8 *data;
  gsize size;
  gsize offset;
} PngReadCursor;

static cairo_status_t
cairo_read_from_bytes (void *closure, unsigned char *out, unsigned int length)
{
  PngReadCursor *cursor = closure;
  if (cursor->offset + length > cursor->size)
    return CAIRO_STATUS_READ_ERROR;
  memcpy (out, cursor->data + cursor->offset, length);
  cursor->offset += length;
  return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
cairo_write_to_byte_array (void *closure, const unsigned char *data, unsigned int length)
{
  GByteArray *arr = closure;
  g_byte_array_append (arr, data, length);
  return CAIRO_STATUS_SUCCESS;
}

PaintDocument *
paint_document_new_from_file (GFile *file, GError **error)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  g_autofree char *path = g_file_get_path (file);
  cairo_surface_t *loaded = NULL;

  if (path && g_str_has_suffix (g_ascii_strdown (path, -1), ".png")) {
    loaded = cairo_image_surface_create_from_png (path);
    if (cairo_surface_status (loaded) != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to read PNG image: %s",
                   cairo_status_to_string (cairo_surface_status (loaded)));
      cairo_surface_destroy (loaded);
      return NULL;
    }
  } else {
    g_autoptr (GdkTexture) texture = gdk_texture_new_from_file (file, error);
    if (!texture)
      return NULL;
    g_autoptr (GBytes) png_bytes = gdk_texture_save_to_png_bytes (texture);
    gsize size = 0;
    const guint8 *ptr = g_bytes_get_data (png_bytes, &size);
    PngReadCursor cursor = { ptr, size, 0 };
    loaded = cairo_image_surface_create_from_png_stream (cairo_read_from_bytes, &cursor);
    if (cairo_surface_status (loaded) != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to decode image surface");
      cairo_surface_destroy (loaded);
      return NULL;
    }
  }

  int w = cairo_image_surface_get_width (loaded);
  int h = cairo_image_surface_get_height (loaded);
  PaintDocument *doc = paint_document_new (w, h);
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_source_surface (cr, loaded, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (loaded);

  doc->file = g_object_ref (file);
  doc->modified = FALSE;
  mark_all_dirty (doc);
  return doc;
}

PaintDocument *
paint_document_new_from_draft (GFile *draft_file,
                               GFile *original_file,
                               gboolean modified,
                               GError **error)
{
  PaintDocument *doc = paint_document_new_from_file (draft_file, error);
  if (!doc)
    return NULL;
  g_set_object (&doc->file, original_file);
  doc->modified = modified;
  return doc;
}

void
paint_document_free (PaintDocument *doc)
{
  if (!doc)
    return;
  g_clear_pointer (&doc->active_stroke_undo, undo_step_free);
  g_clear_pointer (&doc->surface, cairo_surface_destroy);
  g_clear_object (&doc->file);
  g_clear_pointer (&doc->undo_stack, g_ptr_array_unref);
  g_clear_pointer (&doc->redo_stack, g_ptr_array_unref);
  g_free (doc);
}

int
paint_document_get_width (const PaintDocument *doc)
{
  return doc->width;
}

int
paint_document_get_height (const PaintDocument *doc)
{
  return doc->height;
}

cairo_surface_t *
paint_document_get_surface (const PaintDocument *doc)
{
  return doc->surface;
}

GFile *
paint_document_get_file (const PaintDocument *doc)
{
  return doc->file;
}

gboolean
paint_document_get_modified (const PaintDocument *doc)
{
  return doc->modified;
}

void
paint_document_set_modified (PaintDocument *doc, gboolean modified)
{
  doc->modified = modified;
}

gboolean
paint_document_can_undo (const PaintDocument *doc)
{
  return doc->undo_stack->len > 0 || doc->active_stroke_undo != NULL;
}

gboolean
paint_document_can_redo (const PaintDocument *doc)
{
  return doc->redo_stack->len > 0;
}

static UndoStep *
apply_undo_step (PaintDocument *doc, const UndoStep *step)
{
  if (step->is_full) {
    UndoStep *reverse = undo_step_new_full (doc->surface);
    cairo_surface_destroy (doc->surface);
    doc->surface = restore_full_surface (step);
    doc->width = cairo_image_surface_get_width (doc->surface);
    doc->height = cairo_image_surface_get_height (doc->surface);
    mark_all_dirty (doc);
    return reverse;
  }

  UndoStep *reverse = undo_step_new_tiled (doc->width, doc->height);
  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    g_ptr_array_add (reverse->tiles, capture_rect_tile (doc->surface, t->x, t->y, t->w, t->h));
    restore_rect_tile (doc->surface, t);
    mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (doc->surface);
  return reverse;
}

gboolean
paint_document_undo (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  if (doc->undo_stack->len == 0)
    return FALSE;

  guint last = doc->undo_stack->len - 1;
  const UndoStep *step = g_ptr_array_index (doc->undo_stack, last);
  g_ptr_array_add (doc->redo_stack, apply_undo_step (doc, step));
  g_ptr_array_remove_index (doc->undo_stack, last);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_redo (PaintDocument *doc)
{
  if (doc->redo_stack->len == 0)
    return FALSE;

  guint last = doc->redo_stack->len - 1;
  const UndoStep *step = g_ptr_array_index (doc->redo_stack, last);
  g_ptr_array_add (doc->undo_stack, apply_undo_step (doc, step));
  g_ptr_array_remove_index (doc->redo_stack, last);
  doc->modified = TRUE;
  return TRUE;
}

void
paint_document_begin_stroke (PaintDocument *doc,
                             double x,
                             double y,
                             const PaintTool *tool,
                             PaintToolSettings *settings)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }

  if (tool->uses_brush_size) {
    int pad = (int) ceil (settings->size / 2.0) + 4;
    int rx = (int) floor (x) - pad;
    int ry = (int) floor (y) - pad;
    int rsize = pad * 2 + 2;

    doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height);
    doc->prev_stroke_x = x;
    doc->prev_stroke_y = y;
    doc->prev2_stroke_x = x;
    doc->prev2_stroke_y = y;
    undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rsize, rsize);

    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rsize, rsize);
  } else {
    push_undo_step (doc, undo_step_new_full (doc->surface));
    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_all_dirty (doc);
  }

  doc->modified = TRUE;
}

void
paint_document_continue_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings)
{
  if (tool->uses_brush_size) {
    if (!doc->active_stroke_undo)
      doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height);

    int pad = (int) ceil (settings->size / 2.0) + 4;
    double min_x = MIN (MIN (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double max_x = MAX (MAX (doc->prev2_stroke_x, doc->prev_stroke_x), x);
    double min_y = MIN (MIN (doc->prev2_stroke_y, doc->prev_stroke_y), y);
    double max_y = MAX (MAX (doc->prev2_stroke_y, doc->prev_stroke_y), y);

    int rx = (int) floor (min_x) - pad;
    int ry = (int) floor (min_y) - pad;
    int rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
    int rh = (int) ceil (max_y - min_y) + pad * 2 + 2;

    undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    doc->prev2_stroke_x = doc->prev_stroke_x;
    doc->prev2_stroke_y = doc->prev_stroke_y;
    doc->prev_stroke_x = x;
    doc->prev_stroke_y = y;
  } else {
    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
  }

  doc->modified = TRUE;
}

void
paint_document_end_stroke (PaintDocument *doc,
                           double x,
                           double y,
                           const PaintTool *tool,
                           PaintToolSettings *settings)
{
  if (tool->uses_brush_size) {
    int pad = (int) ceil (settings->size / 2.0) + 4;
    double min_x = MIN (doc->prev_stroke_x, x);
    double max_x = MAX (doc->prev_stroke_x, x);
    double min_y = MIN (doc->prev_stroke_y, y);
    double max_y = MAX (doc->prev_stroke_y, y);

    int rx = (int) floor (min_x) - pad;
    int ry = (int) floor (min_y) - pad;
    int rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
    int rh = (int) ceil (max_y - min_y) + pad * 2 + 2;

    if (doc->active_stroke_undo)
      undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    if (doc->active_stroke_undo) {
      push_undo_step (doc, doc->active_stroke_undo);
      doc->active_stroke_undo = NULL;
    }
  } else {
    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
  }

  doc->modified = TRUE;
}

void
paint_document_clear (PaintDocument *doc)
{
  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  push_undo_step (doc, undo_step_new_full (doc->surface));
  cairo_t *cr = cairo_create (doc->surface);
  cairo_set_source_rgba (cr,
                         doc->background_color.red,
                         doc->background_color.green,
                         doc->background_color.blue,
                         doc->background_color.alpha);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

void
paint_document_resize (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  if (doc->active_stroke_undo) {
    push_undo_step (doc, doc->active_stroke_undo);
    doc->active_stroke_undo = NULL;
  }
  push_undo_step (doc, undo_step_new_full (doc->surface));

  cairo_surface_t *new_surface = create_blank_surface (w, h, &doc->background_color);
  cairo_t *cr = cairo_create (new_surface);
  cairo_set_source_surface (cr, doc->surface, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);

  cairo_surface_destroy (doc->surface);
  doc->surface = new_surface;
  doc->width = w;
  doc->height = h;
  doc->modified = TRUE;
  mark_all_dirty (doc);
}

gboolean
paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error)
{
  g_return_val_if_fail (doc != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  g_autofree char *path = g_file_get_path (file);
  if (!path) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 "Saving to non-local URIs is not supported yet.");
    return FALSE;
  }

  g_autofree char *lower = g_ascii_strdown (path, -1);
  g_autoptr (GFile) target_file = NULL;
  g_autofree char *target_path = NULL;

  const char *ext = strrchr (lower, '.');
  if (!ext || g_strcmp0 (ext, ".png") == 0) {
    if (!ext) {
      target_path = g_strconcat (path, ".png", NULL);
      target_file = g_file_new_for_path (target_path);
    } else {
      target_path = g_strdup (path);
      target_file = g_object_ref (file);
    }
    cairo_status_t status = cairo_surface_write_to_png (doc->surface, target_path);
    if (status != CAIRO_STATUS_SUCCESS) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to save PNG: %s", cairo_status_to_string (status));
      return FALSE;
    }
  } else {
    target_file = g_object_ref (file);
    GByteArray *arr = g_byte_array_new ();
    cairo_status_t status = cairo_surface_write_to_png_stream (doc->surface,
                                                               cairo_write_to_byte_array,
                                                               arr);
    if (status != CAIRO_STATUS_SUCCESS) {
      g_byte_array_unref (arr);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to encode image");
      return FALSE;
    }

    g_autoptr (GBytes) bytes = g_byte_array_free_to_bytes (arr);
    g_autoptr (GInputStream) stream = g_memory_input_stream_new_from_bytes (bytes);
    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_stream (stream, NULL, error);
    if (!pixbuf)
      return FALSE;

    const char *fmt = (g_strcmp0 (ext, ".jpg") == 0 || g_strcmp0 (ext, ".jpeg") == 0)
                        ? "jpeg"
                        : ext + 1;
    if (!gdk_pixbuf_save (pixbuf, path, fmt, error, NULL))
      return FALSE;
  }

  g_set_object (&doc->file, target_file);
  doc->modified = FALSE;
  return TRUE;
}

/* ============================================================================
 * PaintCanvas (Overscanned Single-Viewport-Texture GtkWidget)
 * ============================================================================ */

enum {
  SIGNAL_CHANGED,
  SIGNAL_ZOOM_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PaintCanvas {
  GtkWidget parent_instance;

  PaintDocument *document;
  PaintToolSettings *settings; /* unowned shared settings */

  cairo_surface_t *vp_surface;
  GdkTexture *vp_texture;
  cairo_surface_t *cached_doc_surface;
  gboolean vp_valid;
  gboolean vp_is_scaled;
  double vp_zoom;
  int vp_x;
  int vp_y;
  int vp_w;
  int vp_h;

  double zoom;
  gboolean has_hover;
  double hover_x;
  double hover_y;
  gboolean drawing_active;
  double drag_start_x;
  double drag_start_y;
  double last_drag_off_x;
  double last_drag_off_y;
  gboolean show_cursor_ring;
};

G_DEFINE_FINAL_TYPE (PaintCanvas, paint_canvas, GTK_TYPE_WIDGET)

static void
invalidate_viewport_cache (PaintCanvas *self)
{
  self->vp_valid = FALSE;
  self->cached_doc_surface = NULL;
  g_clear_object (&self->vp_texture);
}

static graphene_rect_t
compute_visible_widget_rect (PaintCanvas *self)
{
  float draw_w = (float) gtk_widget_get_width (GTK_WIDGET (self));
  float draw_h = (float) gtk_widget_get_height (GTK_WIDGET (self));
  graphene_rect_t vis = GRAPHENE_RECT_INIT (0.0f, 0.0f, draw_w, draw_h);
  if (draw_w <= 0.0f || draw_h <= 0.0f)
    return vis;

  GtkWidget *scrolled = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (scrolled) {
    graphene_rect_t computed;
    if (gtk_widget_compute_bounds (scrolled, GTK_WIDGET (self), &computed))
      graphene_rect_intersection (&vis, &computed, &vis);
  }
  return vis;
}

static gboolean
is_visible_covered_by_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  if (!self->vp_valid || !self->vp_texture || !self->vp_surface ||
      !self->document || self->cached_doc_surface != self->document->surface) {
    return FALSE;
  }

  gboolean want_scaled = (self->zoom < 1.0);
  if (self->vp_is_scaled != want_scaled)
    return FALSE;
  if (want_scaled && fabs (self->vp_zoom - self->zoom) > 1e-4)
    return FALSE;

  int req_x0, req_y0, req_x1, req_y1;
  if (want_scaled) {
    int draw_w = MAX (1, (int) lround (self->document->width * self->zoom));
    int draw_h = MAX (1, (int) lround (self->document->height * self->zoom));
    req_x0 = MAX (0, (int) floor (vis->origin.x));
    req_y0 = MAX (0, (int) floor (vis->origin.y));
    req_x1 = MIN (draw_w, (int) ceil (vis->origin.x + vis->size.width));
    req_y1 = MIN (draw_h, (int) ceil (vis->origin.y + vis->size.height));
  } else {
    req_x0 = MAX (0, (int) floor (vis->origin.x / self->zoom));
    req_y0 = MAX (0, (int) floor (vis->origin.y / self->zoom));
    req_x1 = MIN (self->document->width, (int) ceil ((vis->origin.x + vis->size.width) / self->zoom));
    req_y1 = MIN (self->document->height, (int) ceil ((vis->origin.y + vis->size.height) / self->zoom));
  }

  return req_x0 >= self->vp_x &&
         req_y0 >= self->vp_y &&
         req_x1 <= self->vp_x + self->vp_w &&
         req_y1 <= self->vp_y + self->vp_h;
}

static void
upload_viewport_texture (PaintCanvas *self, const cairo_rectangle_int_t *update_rect)
{
  cairo_surface_flush (self->vp_surface);
  int stride = cairo_image_surface_get_stride (self->vp_surface);
  const unsigned char *data = cairo_image_surface_get_data (self->vp_surface);
  g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) self->vp_h);

#if GTK_CHECK_VERSION (4, 16, 0)
  if (update_rect && self->vp_texture &&
      update_rect->width > 0 && update_rect->height > 0) {
    cairo_region_t *region = cairo_region_create_rectangle (update_rect);
    g_autoptr (GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new ();
    gdk_memory_texture_builder_set_width (builder, self->vp_w);
    gdk_memory_texture_builder_set_height (builder, self->vp_h);
    gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_DEFAULT);
    gdk_memory_texture_builder_set_stride (builder, (gsize) stride);
    gdk_memory_texture_builder_set_bytes (builder, bytes);
    gdk_memory_texture_builder_set_update_texture (builder, self->vp_texture);
    gdk_memory_texture_builder_set_update_region (builder, region);

    GdkTexture *updated = gdk_memory_texture_builder_build (builder);
    g_set_object (&self->vp_texture, updated);
    g_object_unref (updated);
    cairo_region_destroy (region);
    return;
  }
#else
  (void) update_rect;
#endif

  g_clear_object (&self->vp_texture);
  self->vp_texture = gdk_memory_texture_new (
      self->vp_w, self->vp_h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
}

static void
render_doc_rect_to_viewport (PaintCanvas *self, int vx0, int vy0, int vw, int vh)
{
  PaintDocument *doc = self->document;
  if (!self->vp_is_scaled) {
    cairo_surface_flush (doc->surface);
    int src_stride = cairo_image_surface_get_stride (doc->surface);
    const unsigned char *src = cairo_image_surface_get_data (doc->surface);
    int dst_stride = cairo_image_surface_get_stride (self->vp_surface);
    unsigned char *dst = cairo_image_surface_get_data (self->vp_surface);

    int sx0 = self->vp_x + vx0;
    int sy0 = self->vp_y + vy0;
    gsize row_bytes = (gsize) vw * 4;
    for (int row = 0; row < vh; row++) {
      const unsigned char *srow = src + (gsize) (sy0 + row) * (gsize) src_stride + (gsize) sx0 * 4;
      unsigned char *drow = dst + (gsize) (vy0 + row) * (gsize) dst_stride + (gsize) vx0 * 4;
      memcpy (drow, srow, row_bytes);
    }
    cairo_surface_mark_dirty_rectangle (self->vp_surface, vx0, vy0, vw, vh);
  } else {
    cairo_t *cr = cairo_create (self->vp_surface);
    cairo_rectangle (cr, vx0, vy0, vw, vh);
    cairo_clip (cr);
    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_translate (cr, -self->vp_x, -self->vp_y);
    cairo_scale (cr, self->vp_zoom, self->vp_zoom);
    cairo_set_source_surface (cr, doc->surface, 0, 0);
    cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
    cairo_paint (cr);
    cairo_destroy (cr);
  }
}

static void
rebuild_viewport_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  PaintDocument *doc = self->document;
  gboolean scaled = (self->zoom < 1.0);
  int cx0, cy0, cx1, cy1;

  if (scaled) {
    int draw_w = MAX (1, (int) lround (doc->width * self->zoom));
    int draw_h = MAX (1, (int) lround (doc->height * self->zoom));
    cx0 = MAX (0, (int) floor (vis->origin.x) - VIEWPORT_OVERSCAN);
    cy0 = MAX (0, (int) floor (vis->origin.y) - VIEWPORT_OVERSCAN);
    cx1 = MIN (draw_w, (int) ceil (vis->origin.x + vis->size.width) + VIEWPORT_OVERSCAN);
    cy1 = MIN (draw_h, (int) ceil (vis->origin.y + vis->size.height) + VIEWPORT_OVERSCAN);
  } else {
    int pad = MAX (32, (int) ceil ((double) VIEWPORT_OVERSCAN / self->zoom));
    cx0 = MAX (0, (int) floor (vis->origin.x / self->zoom) - pad);
    cy0 = MAX (0, (int) floor (vis->origin.y / self->zoom) - pad);
    cx1 = MIN (doc->width, (int) ceil ((vis->origin.x + vis->size.width) / self->zoom) + pad);
    cy1 = MIN (doc->height, (int) ceil ((vis->origin.y + vis->size.height) / self->zoom) + pad);
  }

  int cw = MAX (1, cx1 - cx0);
  int ch = MAX (1, cy1 - cy0);

  if (!self->vp_surface ||
      cairo_image_surface_get_width (self->vp_surface) != cw ||
      cairo_image_surface_get_height (self->vp_surface) != ch) {
    g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
    self->vp_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, cw, ch);
    g_clear_object (&self->vp_texture);
  }

  self->vp_is_scaled = scaled;
  self->vp_zoom = self->zoom;
  self->vp_x = cx0;
  self->vp_y = cy0;
  self->vp_w = cw;
  self->vp_h = ch;
  self->cached_doc_surface = doc->surface;
  self->vp_valid = TRUE;

  render_doc_rect_to_viewport (self, 0, 0, cw, ch);
  upload_viewport_texture (self, NULL);

  doc->all_dirty = FALSE;
  doc->has_dirty_rect = FALSE;
}

static GdkTexture *
ensure_viewport_texture (PaintCanvas *self, const graphene_rect_t *vis)
{
  PaintDocument *doc = self->document;
  if (!doc || !doc->surface)
    return NULL;

  if (!is_visible_covered_by_cache (self, vis)) {
    rebuild_viewport_cache (self, vis);
    return self->vp_texture;
  }

  if (doc->all_dirty) {
    doc->all_dirty = FALSE;
    doc->has_dirty_rect = FALSE;
    render_doc_rect_to_viewport (self, 0, 0, self->vp_w, self->vp_h);
    upload_viewport_texture (self, NULL);
    return self->vp_texture;
  }

  if (doc->has_dirty_rect) {
    cairo_rectangle_int_t d = doc->dirty_rect;
    doc->has_dirty_rect = FALSE;

    int vx0, vy0, vx1, vy1;
    if (!self->vp_is_scaled) {
      vx0 = MAX (0, d.x - self->vp_x);
      vy0 = MAX (0, d.y - self->vp_y);
      vx1 = MIN (self->vp_w, (d.x + d.width) - self->vp_x);
      vy1 = MIN (self->vp_h, (d.y + d.height) - self->vp_y);
    } else {
      vx0 = MAX (0, (int) floor (d.x * self->vp_zoom) - 2 - self->vp_x);
      vy0 = MAX (0, (int) floor (d.y * self->vp_zoom) - 2 - self->vp_y);
      vx1 = MIN (self->vp_w, (int) ceil ((d.x + d.width) * self->vp_zoom) + 2 - self->vp_x);
      vy1 = MIN (self->vp_h, (int) ceil ((d.y + d.height) * self->vp_zoom) + 2 - self->vp_y);
    }

    if (vx0 < vx1 && vy0 < vy1) {
      cairo_rectangle_int_t upd = { vx0, vy0, vx1 - vx0, vy1 - vy0 };
      render_doc_rect_to_viewport (self, upd.x, upd.y, upd.width, upd.height);
      upload_viewport_texture (self, &upd);
    }
  }

  return self->vp_texture;
}

static void
update_size_request (PaintCanvas *self)
{
  if (!self->document)
    return;
  int w = MAX (1, (int) lround (self->document->width * self->zoom));
  int h = MAX (1, (int) lround (self->document->height * self->zoom));
  gtk_widget_set_size_request (GTK_WIDGET (self), w, h);

  if (w > 4096 || h > 4096)
    gtk_widget_remove_css_class (GTK_WIDGET (self), "paint-canvas-shadow");
  else
    gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-shadow");

  gtk_widget_queue_resize (GTK_WIDGET (self));
}

static void
paint_canvas_measure (GtkWidget *widget,
                      GtkOrientation orientation,
                      int for_size G_GNUC_UNUSED,
                      int *minimum,
                      int *natural,
                      int *minimum_baseline,
                      int *natural_baseline)
{
  PaintCanvas *self = PAINT_CANVAS (widget);
  int size = 1;
  if (self->document) {
    double dim = (orientation == GTK_ORIENTATION_HORIZONTAL)
                   ? self->document->width
                   : self->document->height;
    size = MAX (1, (int) lround (dim * self->zoom));
  }
  *minimum = size;
  *natural = size;
  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}

static void
paint_canvas_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  PaintCanvas *self = PAINT_CANVAS (widget);
  if (!self->document)
    return;

  graphene_rect_t vis_rect = compute_visible_widget_rect (self);
  if (vis_rect.size.width <= 0.0f || vis_rect.size.height <= 0.0f)
    return;

  GdkTexture *tex = ensure_viewport_texture (self, &vis_rect);
  if (!tex)
    return;

  graphene_rect_t bounds;
  GskScalingFilter filter;
  if (self->vp_is_scaled) {
    bounds = GRAPHENE_RECT_INIT ((float) self->vp_x,
                                 (float) self->vp_y,
                                 (float) self->vp_w,
                                 (float) self->vp_h);
    filter = GSK_SCALING_FILTER_LINEAR;
  } else {
    float x0 = (float) (self->vp_x * self->zoom);
    float y0 = (float) (self->vp_y * self->zoom);
    float x1 = (float) ((self->vp_x + self->vp_w) * self->zoom);
    float y1 = (float) ((self->vp_y + self->vp_h) * self->zoom);
    bounds = GRAPHENE_RECT_INIT (x0, y0, x1 - x0, y1 - y0);
    filter = (self->zoom >= 2.0) ? GSK_SCALING_FILTER_NEAREST : GSK_SCALING_FILTER_LINEAR;
  }

  gtk_snapshot_append_scaled_texture (snapshot, tex, filter, &bounds);

  if (self->show_cursor_ring && self->has_hover && self->settings) {
    const PaintTool *active_tool = paint_tools_find (self->settings->active_tool_id);
    double px = self->hover_x * self->zoom;
    double py = self->hover_y * self->zoom;
    double radius = active_tool->uses_brush_size
                      ? MAX (2.0, (self->settings->size * self->zoom) / 2.0)
                      : 3.5;

    float pad = (float) (radius + 4.0);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT ((float) px - pad,
                                                 (float) py - pad,
                                                 pad * 2.0f,
                                                 pad * 2.0f);
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &bounds);
    cairo_set_line_width (cr, 1.5);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.85);
    cairo_arc (cr, px, py, radius + 0.75, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);

    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.75);
    cairo_arc (cr, px, py, radius, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);
    cairo_destroy (cr);
  }
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               double start_x,
               double start_y,
               gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->drawing_active = TRUE;
  self->drag_start_x = start_x;
  self->drag_start_y = start_y;
  self->last_drag_off_x = 0.0;
  self->last_drag_off_y = 0.0;

  double cx = start_x / self->zoom;
  double cy = start_y / self->zoom;
  self->has_hover = TRUE;
  self->hover_x = cx;
  self->hover_y = cy;

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  paint_document_begin_stroke (self->document, cx, cy, tool, self->settings);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_drag_update (GtkGestureDrag *gesture G_GNUC_UNUSED,
                double offset_x,
                double offset_y,
                gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  if (hypot (offset_x - self->last_drag_off_x, offset_y - self->last_drag_off_y) < 0.75)
    return;
  self->last_drag_off_x = offset_x;
  self->last_drag_off_y = offset_y;

  double cx = (self->drag_start_x + offset_x) / self->zoom;
  double cy = (self->drag_start_y + offset_y) / self->zoom;
  self->has_hover = TRUE;
  self->hover_x = cx;
  self->hover_y = cy;

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  paint_document_continue_stroke (self->document, cx, cy, tool, self->settings);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *gesture G_GNUC_UNUSED,
             double offset_x,
             double offset_y,
             gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  self->drawing_active = FALSE;
  double cx = (self->drag_start_x + offset_x) / self->zoom;
  double cy = (self->drag_start_y + offset_y) / self->zoom;

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  paint_document_end_stroke (self->document, cx, cy, tool, self->settings);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

static void
on_drag_cancel (GtkGesture *gesture G_GNUC_UNUSED,
                GdkEventSequence *sequence G_GNUC_UNUSED,
                gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (self->drawing_active) {
    self->drawing_active = FALSE;
    const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
    paint_document_end_stroke (self->document, self->hover_x, self->hover_y, tool, self->settings);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
on_motion (GtkEventControllerMotion *ctrl G_GNUC_UNUSED,
           double x,
           double y,
           gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->has_hover = TRUE;
  self->hover_x = x / self->zoom;
  self->hover_y = y / self->zoom;
  if (self->show_cursor_ring)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->has_hover = FALSE;
  if (self->show_cursor_ring)
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
paint_canvas_finalize (GObject *object)
{
  PaintCanvas *self = PAINT_CANVAS (object);
  invalidate_viewport_cache (self);
  g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
  g_clear_pointer (&self->document, paint_document_free);
  G_OBJECT_CLASS (paint_canvas_parent_class)->finalize (object);
}

static void
paint_canvas_class_init (PaintCanvasClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_canvas_finalize;

  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  widget_class->measure = paint_canvas_measure;
  widget_class->snapshot = paint_canvas_snapshot;

  signals[SIGNAL_CHANGED] = g_signal_new ("changed",
                                          G_TYPE_FROM_CLASS (klass),
                                          G_SIGNAL_RUN_FIRST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE, 0);
  signals[SIGNAL_ZOOM_CHANGED] = g_signal_new ("zoom-changed",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_FIRST,
                                               0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void
paint_canvas_init (PaintCanvas *self)
{
  self->zoom = 1.0;
  self->show_cursor_ring = TRUE;

  gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-frame");
  gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-shadow");
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (on_drag_cancel), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  g_signal_connect (motion, "leave", G_CALLBACK (on_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);
}

PaintCanvas *
paint_canvas_new (PaintDocument *doc, PaintToolSettings *settings)
{
  PaintCanvas *self = g_object_new (PAINT_TYPE_CANVAS, NULL);
  self->document = doc ? doc : paint_document_new (PAINT_DEFAULT_CANVAS_WIDTH,
                                                   PAINT_DEFAULT_CANVAS_HEIGHT);
  self->settings = settings;
  invalidate_viewport_cache (self);
  update_size_request (self);
  return self;
}

PaintDocument *
paint_canvas_get_document (PaintCanvas *self)
{
  return self->document;
}

void
paint_canvas_set_document (PaintCanvas *self, PaintDocument *doc)
{
  invalidate_viewport_cache (self);
  g_clear_pointer (&self->document, paint_document_free);
  self->document = doc;
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

PaintToolSettings *
paint_canvas_get_settings (PaintCanvas *self)
{
  return self->settings;
}

double
paint_canvas_get_zoom (PaintCanvas *self)
{
  return self->zoom;
}

double
paint_canvas_get_min_zoom (void)
{
  return ZOOM_STEPS[0];
}

double
paint_canvas_get_max_zoom (void)
{
  return ZOOM_STEPS[G_N_ELEMENTS (ZOOM_STEPS) - 1];
}

void
paint_canvas_set_zoom (PaintCanvas *self, double zoom)
{
  double min_z = ZOOM_STEPS[0];
  double max_z = ZOOM_STEPS[G_N_ELEMENTS (ZOOM_STEPS) - 1];
  double clamped = CLAMP (zoom, min_z, max_z);
  if (fabs (clamped - self->zoom) < 1e-4)
    return;
  self->zoom = clamped;
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_ZOOM_CHANGED], 0, self->zoom);
}

void
paint_canvas_zoom_in (PaintCanvas *self)
{
  for (gsize i = 0; i < G_N_ELEMENTS (ZOOM_STEPS); i++) {
    if (ZOOM_STEPS[i] > self->zoom + 1e-4) {
      paint_canvas_set_zoom (self, ZOOM_STEPS[i]);
      return;
    }
  }
}

void
paint_canvas_zoom_out (PaintCanvas *self)
{
  for (gssize i = (gssize) G_N_ELEMENTS (ZOOM_STEPS) - 1; i >= 0; i--) {
    if (ZOOM_STEPS[i] < self->zoom - 1e-4) {
      paint_canvas_set_zoom (self, ZOOM_STEPS[i]);
      return;
    }
  }
}

void
paint_canvas_zoom_reset (PaintCanvas *self)
{
  paint_canvas_set_zoom (self, 1.0);
}

gboolean
paint_canvas_undo (PaintCanvas *self)
{
  if (paint_document_undo (self->document)) {
    update_size_request (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }
  return FALSE;
}

gboolean
paint_canvas_redo (PaintCanvas *self)
{
  if (paint_document_redo (self->document)) {
    update_size_request (self);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    return TRUE;
  }
  return FALSE;
}

void
paint_canvas_clear (PaintCanvas *self)
{
  paint_document_clear (self->document);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_resize (PaintCanvas *self, int width, int height)
{
  paint_document_resize (self->document, width, height);
  update_size_request (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

void
paint_canvas_set_show_cursor_ring (PaintCanvas *self, gboolean show)
{
  self->show_cursor_ring = show;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
paint_canvas_check_viewport (PaintCanvas *self)
{
  if (!self->document || gtk_widget_get_width (GTK_WIDGET (self)) <= 0)
    return;
  graphene_rect_t vis = compute_visible_widget_rect (self);
  if (vis.size.width > 0.0f && vis.size.height > 0.0f &&
      !is_visible_covered_by_cache (self, &vis)) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}
