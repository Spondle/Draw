#include "paint-canvas.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MAX_UNDO_STEPS 40
#define UNDO_TILE_SIZE 256
#define VIEWPORT_OVERSCAN 256
#define MIN_VP_CACHE_DIM 1024
#define MAX_OVERVIEW_DIM 2048

static const double ZOOM_STEPS[] = {
  0.05, 0.06, 0.08, 0.10, 0.125, 0.16, 0.20, 0.25, 0.33, 0.40, 0.50, 0.60,
  0.75, 0.90, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0
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
  gboolean pre_stroke_modified;
  double stroke_start_x;
  double stroke_start_y;
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

static void
compute_stroke_bounds (double x0,
                       double y0,
                       double x1,
                       double y1,
                       double stroke_size,
                       int *rx,
                       int *ry,
                       int *rw,
                       int *rh)
{
  int pad = (int) ceil (stroke_size / 2.0) + 4;
  double min_x = MIN (x0, x1);
  double max_x = MAX (x0, x1);
  double min_y = MIN (y0, y1);
  double max_y = MAX (y0, y1);

  *rx = (int) floor (min_x) - pad;
  *ry = (int) floor (min_y) - pad;
  *rw = (int) ceil (max_x - min_x) + pad * 2 + 2;
  *rh = (int) ceil (max_y - min_y) + pad * 2 + 2;
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

  doc->pre_stroke_modified = doc->modified;
  doc->stroke_start_x = x;
  doc->stroke_start_y = y;
  doc->prev_stroke_x = x;
  doc->prev_stroke_y = y;
  doc->prev2_stroke_x = x;
  doc->prev2_stroke_y = y;

  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (x, y, x, y, settings->size, &rx, &ry, &rw, &rh);

    doc->active_stroke_undo = undo_step_new_tiled (doc->width, doc->height);
    undo_step_capture_rect (doc->active_stroke_undo, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->begin_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);
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
  if (tool->draw_preview) {
    cairo_t *cr = cairo_create (doc->surface);
    tool->continue_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    return;
  }

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
  if (tool->draw_preview) {
    int rx, ry, rw, rh;
    double stroke_size = tool->uses_brush_size ? settings->size : 1.0;
    compute_stroke_bounds (doc->stroke_start_x, doc->stroke_start_y, x, y,
                           stroke_size, &rx, &ry, &rw, &rh);

    UndoStep *step = undo_step_new_tiled (doc->width, doc->height);
    undo_step_capture_rect (step, doc->surface, rx, ry, rw, rh);

    cairo_t *cr = cairo_create (doc->surface);
    tool->end_stroke (tool, cr, x, y, settings);
    cairo_destroy (cr);
    mark_rect_dirty (doc, rx, ry, rw, rh);

    push_undo_step (doc, step);
    doc->modified = TRUE;
    return;
  }

  if (tool->uses_brush_size) {
    int rx, ry, rw, rh;
    compute_stroke_bounds (doc->prev_stroke_x, doc->prev_stroke_y, x, y,
                           settings->size, &rx, &ry, &rw, &rh);

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
paint_document_cancel_stroke (PaintDocument *doc)
{
  if (!doc || !doc->active_stroke_undo)
    return;

  UndoStep *step = doc->active_stroke_undo;
  doc->active_stroke_undo = NULL;

  for (guint i = 0; i < step->tiles->len; i++) {
    const UndoTile *t = g_ptr_array_index (step->tiles, i);
    restore_rect_tile (doc->surface, t);
    mark_rect_dirty (doc, t->x, t->y, t->w, t->h);
  }
  cairo_surface_mark_dirty (doc->surface);
  undo_step_free (step);
  doc->modified = doc->pre_stroke_modified;
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
 * PaintCanvas (2-Tier Single-Texture Cache: Overview < 50% + 1:1 Viewport >= 50%)
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

  /* Tier 1 (>= 50% zoom, or >= 30% half-res on >4096 canvases): overscanned viewport texture */
  cairo_surface_t *vp_surface;
  GdkTexture *vp_texture;
  cairo_surface_t *vp_doc_surface;
  gboolean vp_valid;
  int vp_div;
  int vp_x;
  int vp_y;
  int vp_w;
  int vp_h;
  int vp_surf_w;
  int vp_surf_h;
  gboolean vp_all_dirty;
  gboolean vp_has_dirty_rect;
  cairo_rectangle_int_t vp_dirty_rect;

  /* Tier 2 (overview): box-downsampled <= 2048x2048 overview texture */
  cairo_surface_t *ov_surface;
  GdkTexture *ov_texture;
  cairo_surface_t *ov_doc_surface;
  int ov_div;
  int ov_w;
  int ov_h;
  gboolean ov_all_dirty;
  gboolean ov_has_dirty_rect;
  cairo_rectangle_int_t ov_dirty_rect;

  double zoom;
  gboolean has_hover;
  double hover_x;
  double hover_y;
  gboolean drawing_active;
  gboolean touch_stroke_pending;
  double drag_start_x;
  double drag_start_y;
  double last_drag_off_x;
  double last_drag_off_y;
  double preview_end_x;
  double preview_end_y;
  gboolean show_cursor_ring;
};

G_DEFINE_FINAL_TYPE (PaintCanvas, paint_canvas, GTK_TYPE_WIDGET)

static void
invalidate_viewport_cache (PaintCanvas *self)
{
  self->vp_valid = FALSE;
  self->vp_doc_surface = NULL;
  self->vp_div = 0;
  self->vp_surf_w = 0;
  self->vp_surf_h = 0;
  self->vp_all_dirty = FALSE;
  self->vp_has_dirty_rect = FALSE;
  g_clear_object (&self->vp_texture);

  self->ov_doc_surface = NULL;
  self->ov_all_dirty = FALSE;
  self->ov_has_dirty_rect = FALSE;
  g_clear_object (&self->ov_texture);
}

static void
expand_dirty_rect (gboolean *all_dirty,
                   gboolean *has_rect,
                   cairo_rectangle_int_t *accum,
                   const cairo_rectangle_int_t *r)
{
  if (*all_dirty)
    return;
  if (!*has_rect) {
    *accum = *r;
    *has_rect = TRUE;
  } else {
    int x0 = MIN (accum->x, r->x);
    int y0 = MIN (accum->y, r->y);
    int x1 = MAX (accum->x + accum->width, r->x + r->width);
    int y1 = MAX (accum->y + accum->height, r->y + r->height);
    *accum = (cairo_rectangle_int_t) { x0, y0, x1 - x0, y1 - y0 };
  }
}

static void
propagate_document_dirty (PaintCanvas *self)
{
  PaintDocument *doc = self->document;
  if (!doc)
    return;

  if (doc->all_dirty) {
    self->vp_all_dirty = TRUE;
    self->vp_has_dirty_rect = FALSE;
    self->ov_all_dirty = TRUE;
    self->ov_has_dirty_rect = FALSE;
    doc->all_dirty = FALSE;
    doc->has_dirty_rect = FALSE;
  } else if (doc->has_dirty_rect) {
    expand_dirty_rect (&self->vp_all_dirty, &self->vp_has_dirty_rect,
                       &self->vp_dirty_rect, &doc->dirty_rect);
    expand_dirty_rect (&self->ov_all_dirty, &self->ov_has_dirty_rect,
                       &self->ov_dirty_rect, &doc->dirty_rect);
    doc->has_dirty_rect = FALSE;
  }
}

static GdkTexture *
upload_surface_texture (cairo_surface_t *surface,
                        GdkTexture *old_texture,
                        int w,
                        int h,
                        const cairo_rectangle_int_t *update_rect)
{
  cairo_surface_flush (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);
  g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) h);

#if GTK_CHECK_VERSION (4, 16, 0)
  if (update_rect && old_texture &&
      update_rect->width > 0 && update_rect->height > 0) {
    cairo_region_t *region = cairo_region_create_rectangle (update_rect);
    g_autoptr (GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new ();
    gdk_memory_texture_builder_set_width (builder, w);
    gdk_memory_texture_builder_set_height (builder, h);
    gdk_memory_texture_builder_set_format (builder, GDK_MEMORY_DEFAULT);
    gdk_memory_texture_builder_set_stride (builder, (gsize) stride);
    gdk_memory_texture_builder_set_bytes (builder, bytes);
    gdk_memory_texture_builder_set_update_texture (builder, old_texture);
    gdk_memory_texture_builder_set_update_region (builder, region);

    GdkTexture *updated = gdk_memory_texture_builder_build (builder);
    cairo_region_destroy (region);
    return updated;
  }
#else
  (void) update_rect;
  (void) old_texture;
#endif

  return gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
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

static void box_downsample_rect (cairo_surface_t *src_surf,
                                 cairo_surface_t *dst_surf,
                                 int div,
                                 int src_ox,
                                 int src_oy,
                                 int dx0,
                                 int dy0,
                                 int dw,
                                 int dh);

static int
target_vp_div (const PaintCanvas *self)
{
  if (!self->document)
    return 0;
  if (self->zoom >= 0.50)
    return 1;
  int max_dim = MAX (self->document->width, self->document->height);
  if (self->zoom >= 0.30 && max_dim > MAX_OVERVIEW_DIM * 2)
    return 2;
  return 0;
}

static gboolean
is_visible_covered_by_vp_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  int want_div = target_vp_div (self);
  if (want_div <= 0 || !self->vp_valid || !self->vp_texture || !self->vp_surface ||
      !self->document || self->vp_doc_surface != self->document->surface ||
      self->vp_div != want_div) {
    return FALSE;
  }

  int req_x0 = MAX (0, (int) floor (vis->origin.x / self->zoom));
  int req_y0 = MAX (0, (int) floor (vis->origin.y / self->zoom));
  int req_x1 = MIN (self->document->width,
                    (int) ceil ((vis->origin.x + vis->size.width) / self->zoom));
  int req_y1 = MIN (self->document->height,
                    (int) ceil ((vis->origin.y + vis->size.height) / self->zoom));

  return req_x0 >= self->vp_x &&
         req_y0 >= self->vp_y &&
         req_x1 <= self->vp_x + self->vp_w &&
         req_y1 <= self->vp_y + self->vp_h;
}

static void
rebuild_vp_cache (PaintCanvas *self, const graphene_rect_t *vis)
{
  PaintDocument *doc = self->document;
  int div = target_vp_div (self);
  if (div <= 0)
    return;

  int pad = CLAMP ((int) ceil ((double) VIEWPORT_OVERSCAN / self->zoom), 64, 256);
  int req_x0 = MAX (0, (int) floor (vis->origin.x / self->zoom) - pad);
  int req_y0 = MAX (0, (int) floor (vis->origin.y / self->zoom) - pad);
  int req_x1 = MIN (doc->width, (int) ceil ((vis->origin.x + vis->size.width) / self->zoom) + pad);
  int req_y1 = MIN (doc->height, (int) ceil ((vis->origin.y + vis->size.height) / self->zoom) + pad);

  req_x0 = (req_x0 / div) * div;
  req_y0 = (req_y0 / div) * div;

  /* Ensure a minimum cache span so zooming 100% <-> 3200% doesn't re-copy */
  int min_w = MIN (doc->width, MIN_VP_CACHE_DIM * div);
  int min_h = MIN (doc->height, MIN_VP_CACHE_DIM * div);
  if (req_x1 - req_x0 < min_w) {
    int cx = (req_x0 + req_x1) / 2;
    int max_x0 = MAX (0, ((doc->width - min_w) / div) * div);
    req_x0 = CLAMP (((cx - min_w / 2) / div) * div, 0, max_x0);
    req_x1 = MIN (doc->width, req_x0 + min_w);
  }
  if (req_y1 - req_y0 < min_h) {
    int cy = (req_y0 + req_y1) / 2;
    int max_y0 = MAX (0, ((doc->height - min_h) / div) * div);
    req_y0 = CLAMP (((cy - min_h / 2) / div) * div, 0, max_y0);
    req_y1 = MIN (doc->height, req_y0 + min_h);
  }

  int cw = MAX (1, req_x1 - req_x0);
  int ch = MAX (1, req_y1 - req_y0);
  int sw = MAX (1, (cw + div - 1) / div);
  int sh = MAX (1, (ch + div - 1) / div);

  if (!self->vp_surface ||
      cairo_image_surface_get_width (self->vp_surface) != sw ||
      cairo_image_surface_get_height (self->vp_surface) != sh) {
    g_clear_pointer (&self->vp_surface, cairo_surface_destroy);
    self->vp_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sw, sh);
    g_clear_object (&self->vp_texture);
  }

  self->vp_div = div;
  self->vp_x = req_x0;
  self->vp_y = req_y0;
  self->vp_w = cw;
  self->vp_h = ch;
  self->vp_surf_w = sw;
  self->vp_surf_h = sh;
  self->vp_doc_surface = doc->surface;
  self->vp_valid = TRUE;
  self->vp_all_dirty = FALSE;
  self->vp_has_dirty_rect = FALSE;

  box_downsample_rect (doc->surface, self->vp_surface, div, req_x0, req_y0, 0, 0, sw, sh);
  GdkTexture *tex = upload_surface_texture (self->vp_surface, NULL, sw, sh, NULL);
  g_clear_object (&self->vp_texture);
  self->vp_texture = tex;
}

static GdkTexture *
ensure_vp_texture (PaintCanvas *self, const graphene_rect_t *vis)
{
  if (!is_visible_covered_by_vp_cache (self, vis)) {
    rebuild_vp_cache (self, vis);
    return self->vp_texture;
  }

  int div = self->vp_div;
  if (self->vp_all_dirty) {
    self->vp_all_dirty = FALSE;
    self->vp_has_dirty_rect = FALSE;
    box_downsample_rect (self->document->surface, self->vp_surface, div,
                         self->vp_x, self->vp_y, 0, 0, self->vp_surf_w, self->vp_surf_h);
    GdkTexture *tex = upload_surface_texture (
        self->vp_surface, NULL, self->vp_surf_w, self->vp_surf_h, NULL);
    g_clear_object (&self->vp_texture);
    self->vp_texture = tex;
    return self->vp_texture;
  }

  if (self->vp_has_dirty_rect) {
    cairo_rectangle_int_t d = self->vp_dirty_rect;
    self->vp_has_dirty_rect = FALSE;

    int cx0 = MAX (self->vp_x, d.x);
    int cy0 = MAX (self->vp_y, d.y);
    int cx1 = MIN (self->vp_x + self->vp_w, d.x + d.width);
    int cy1 = MIN (self->vp_y + self->vp_h, d.y + d.height);

    if (cx0 < cx1 && cy0 < cy1) {
      int vx0 = (cx0 - self->vp_x) / div;
      int vy0 = (cy0 - self->vp_y) / div;
      int vx1 = MIN (self->vp_surf_w, (cx1 - self->vp_x + div - 1) / div);
      int vy1 = MIN (self->vp_surf_h, (cy1 - self->vp_y + div - 1) / div);

      if (vx0 < vx1 && vy0 < vy1) {
        cairo_rectangle_int_t upd = { vx0, vy0, vx1 - vx0, vy1 - vy0 };
        box_downsample_rect (self->document->surface, self->vp_surface, div,
                             self->vp_x, self->vp_y, upd.x, upd.y, upd.width, upd.height);
        GdkTexture *next = upload_surface_texture (
            self->vp_surface, self->vp_texture, self->vp_surf_w, self->vp_surf_h, &upd);
        g_set_object (&self->vp_texture, next);
        g_object_unref (next);
      }
    }
  }

  return self->vp_texture;
}

static uint32_t
downsample_clamped_pixel (const uint32_t *src_px,
                          int src_stride_px,
                          int sx0,
                          int sx1,
                          int sy0,
                          int sy1)
{
  uint32_t sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
  uint32_t count = 0;
  for (int sy = sy0; sy < sy1; sy++) {
    const uint32_t *srow = src_px + (gsize) sy * (gsize) src_stride_px;
    for (int sx = sx0; sx < sx1; sx++) {
      uint32_t px = srow[sx];
      sum_a += (px >> 24) & 0xFFU;
      sum_r += (px >> 16) & 0xFFU;
      sum_g += (px >> 8) & 0xFFU;
      sum_b += px & 0xFFU;
      count++;
    }
  }
  if (count == 0)
    return 0;
  uint32_t half = count >> 1;
  return (((sum_a + half) / count) << 24) |
         (((sum_r + half) / count) << 16) |
         (((sum_g + half) / count) << 8) |
         ((sum_b + half) / count);
}

static void
box_downsample_rect (cairo_surface_t *src_surf,
                     cairo_surface_t *dst_surf,
                     int div,
                     int src_ox,
                     int src_oy,
                     int dx0,
                     int dy0,
                     int dw,
                     int dh)
{
  cairo_surface_flush (src_surf);
  int src_w = cairo_image_surface_get_width (src_surf);
  int src_h = cairo_image_surface_get_height (src_surf);
  int src_stride = cairo_image_surface_get_stride (src_surf);
  const unsigned char *src_data = cairo_image_surface_get_data (src_surf);

  int dst_stride = cairo_image_surface_get_stride (dst_surf);
  unsigned char *dst_data = cairo_image_surface_get_data (dst_surf);

  if (div == 1) {
    gsize row_bytes = (gsize) dw * 4;
    int sx = src_ox + dx0;
    for (int row = 0; row < dh; row++) {
      int sy = src_oy + dy0 + row;
      const unsigned char *srow = src_data + (gsize) sy * (gsize) src_stride + (gsize) sx * 4;
      unsigned char *drow = dst_data + (gsize) (dy0 + row) * (gsize) dst_stride + (gsize) dx0 * 4;
      memcpy (drow, srow, row_bytes);
    }
    cairo_surface_mark_dirty_rectangle (dst_surf, dx0, dy0, dw, dh);
    return;
  }

  const uint32_t *src_px = (const uint32_t *) src_data;
  uint32_t *dst_px = (uint32_t *) dst_data;
  int src_stride_px = src_stride / 4;
  int dst_stride_px = dst_stride / 4;

  /* Safe for 16-bit lane SWAR accumulation up to div=8 (max 64 * 255 = 16320 < 65535) */
  int shift = (div == 2) ? 2 : (div == 4) ? 4 : (div == 8) ? 6 : 0;
  int full_x_end = MAX (0, (src_w - src_ox) / div);
  int full_y_end = MAX (0, (src_h - src_oy) / div);

  for (int dy = dy0; dy < dy0 + dh; dy++) {
    int sy0 = src_oy + dy * div;
    int sy1 = MIN (src_h, sy0 + div);
    uint32_t *drow = dst_px + (gsize) dy * (gsize) dst_stride_px;
    gboolean full_y = (shift > 0) && (dy < full_y_end);
    int dx = dx0;
    int fast_end = full_y ? MIN (dx0 + dw, full_x_end) : dx0;

    if (div == 4 && full_y) {
      const uint32_t *r0 = src_px + (gsize) sy0 * (gsize) src_stride_px;
      const uint32_t *r1 = r0 + src_stride_px;
      const uint32_t *r2 = r1 + src_stride_px;
      const uint32_t *r3 = r2 + src_stride_px;
      for (; dx < fast_end; dx++) {
        int sx0 = src_ox + dx * 4;
        uint32_t rb = 0x00080008U;
        uint32_t ag = 0x00080008U;
        for (int k = 0; k < 4; k++) {
          uint32_t p0 = r0[sx0 + k], p1 = r1[sx0 + k], p2 = r2[sx0 + k], p3 = r3[sx0 + k];
          rb += (p0 & 0x00FF00FFU) + (p1 & 0x00FF00FFU) + (p2 & 0x00FF00FFU) + (p3 & 0x00FF00FFU);
          ag += ((p0 >> 8) & 0x00FF00FFU) + ((p1 >> 8) & 0x00FF00FFU) +
                ((p2 >> 8) & 0x00FF00FFU) + ((p3 >> 8) & 0x00FF00FFU);
        }
        drow[dx] = ((rb >> 4) & 0x00FF00FFU) | (((ag >> 4) & 0x00FF00FFU) << 8);
      }
    } else if (shift > 0 && full_y) {
      uint32_t bias = 0x00010001U * (1U << (shift - 1));
      for (; dx < fast_end; dx++) {
        int sx0 = src_ox + dx * div;
        uint32_t rb = bias;
        uint32_t ag = bias;
        const uint32_t *row = src_px + (gsize) sy0 * (gsize) src_stride_px + sx0;
        for (int ky = 0; ky < div; ky++, row += src_stride_px) {
          for (int kx = 0; kx < div; kx++) {
            uint32_t p = row[kx];
            rb += p & 0x00FF00FFU;
            ag += (p >> 8) & 0x00FF00FFU;
          }
        }
        drow[dx] = ((rb >> shift) & 0x00FF00FFU) | (((ag >> shift) & 0x00FF00FFU) << 8);
      }
    }

    for (; dx < dx0 + dw; dx++) {
      int sx0 = src_ox + dx * div;
      int sx1 = MIN (src_w, sx0 + div);
      drow[dx] = downsample_clamped_pixel (src_px, src_stride_px, sx0, sx1, sy0, sy1);
    }
  }
  cairo_surface_mark_dirty_rectangle (dst_surf, dx0, dy0, dw, dh);
}

static double
get_widget_device_scale (GtkWidget *widget)
{
  GtkNative *native = gtk_widget_get_native (widget);
  if (native) {
    GdkSurface *surface = gtk_native_get_surface (native);
    if (surface) {
      double s = gdk_surface_get_scale (surface);
      if (s > 0.0)
        return s;
    }
  }
  int sf = gtk_widget_get_scale_factor (widget);
  return sf > 0 ? (double) sf : 1.0;
}

static GdkTexture *
ensure_overview_texture (PaintCanvas *self, double device_scale)
{
  PaintDocument *doc = self->document;
  if (!doc || !doc->surface)
    return NULL;

  int max_ov_dim = (device_scale > 1.25) ? MAX_OVERVIEW_DIM * 2 : MAX_OVERVIEW_DIM;
  int max_dim = MAX (doc->width, doc->height);
  int div = 1;
  while (max_dim / div > max_ov_dim)
    div *= 2;
  int ow = MAX (1, (doc->width + div - 1) / div);
  int oh = MAX (1, (doc->height + div - 1) / div);

  if (!self->ov_texture || !self->ov_surface ||
      self->ov_doc_surface != doc->surface ||
      self->ov_div != div || self->ov_w != ow || self->ov_h != oh) {
    if (!self->ov_surface ||
        cairo_image_surface_get_width (self->ov_surface) != ow ||
        cairo_image_surface_get_height (self->ov_surface) != oh) {
      g_clear_pointer (&self->ov_surface, cairo_surface_destroy);
      self->ov_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ow, oh);
    }
    self->ov_div = div;
    self->ov_w = ow;
    self->ov_h = oh;
    self->ov_doc_surface = doc->surface;
    self->ov_all_dirty = FALSE;
    self->ov_has_dirty_rect = FALSE;

    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0, 0, 0, ow, oh);
    GdkTexture *tex = upload_surface_texture (self->ov_surface, NULL, ow, oh, NULL);
    g_clear_object (&self->ov_texture);
    self->ov_texture = tex;
    return self->ov_texture;
  }

  if (self->ov_all_dirty) {
    self->ov_all_dirty = FALSE;
    self->ov_has_dirty_rect = FALSE;
    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0, 0, 0, ow, oh);
    GdkTexture *tex = upload_surface_texture (self->ov_surface, NULL, ow, oh, NULL);
    g_clear_object (&self->ov_texture);
    self->ov_texture = tex;
    return self->ov_texture;
  }

  if (self->ov_has_dirty_rect) {
    cairo_rectangle_int_t d = self->ov_dirty_rect;
    self->ov_has_dirty_rect = FALSE;

    int dx0 = CLAMP (d.x / div, 0, ow - 1);
    int dy0 = CLAMP (d.y / div, 0, oh - 1);
    int dx1 = CLAMP ((d.x + d.width + div - 1) / div, dx0 + 1, ow);
    int dy1 = CLAMP ((d.y + d.height + div - 1) / div, dy0 + 1, oh);

    cairo_rectangle_int_t upd = { dx0, dy0, dx1 - dx0, dy1 - dy0 };
    box_downsample_rect (doc->surface, self->ov_surface, div, 0, 0,
                         upd.x, upd.y, upd.width, upd.height);
    GdkTexture *next = upload_surface_texture (
        self->ov_surface, self->ov_texture, ow, oh, &upd);
    g_set_object (&self->ov_texture, next);
    g_object_unref (next);
  }

  return self->ov_texture;
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

  propagate_document_dirty (self);

  double device_scale = get_widget_device_scale (widget);
  double eff_zoom = self->zoom * device_scale;
  const PaintTool *active_tool = self->settings
                                   ? paint_tools_find (self->settings->active_tool_id)
                                   : NULL;
  gboolean has_preview = self->drawing_active &&
                         !self->touch_stroke_pending &&
                         active_tool &&
                         active_tool->draw_preview != NULL;

  int active_div = 1;
  GskScalingFilter canvas_filter = GSK_SCALING_FILTER_LINEAR;
  graphene_rect_t vis_rect = compute_visible_widget_rect (self);

  if (target_vp_div (self) == 0) {
    GdkTexture *tex = ensure_overview_texture (self, device_scale);
    if (!tex)
      return;
    active_div = self->ov_div;
    float bw = (float) (self->ov_w * self->ov_div * self->zoom);
    float bh = (float) (self->ov_h * self->ov_div * self->zoom);
    if (bw <= 0.0f || bh <= 0.0f)
      return;
    gboolean trilinear = (!self->drawing_active || has_preview) &&
                         (eff_zoom * self->ov_div < 0.5);
    canvas_filter = trilinear ? GSK_SCALING_FILTER_TRILINEAR : GSK_SCALING_FILTER_LINEAR;
    gtk_snapshot_append_scaled_texture (snapshot,
                                        tex,
                                        canvas_filter,
                                        &GRAPHENE_RECT_INIT (0.0f, 0.0f, bw, bh));
  } else {
    if (vis_rect.size.width <= 0.0f || vis_rect.size.height <= 0.0f)
      return;
    GdkTexture *tex = ensure_vp_texture (self, &vis_rect);
    if (!tex)
      return;
    active_div = self->vp_div;

    float x0 = (float) (self->vp_x * self->zoom);
    float y0 = (float) (self->vp_y * self->zoom);
    float x1 = (float) ((self->vp_x + self->vp_surf_w * self->vp_div) * self->zoom);
    float y1 = (float) ((self->vp_y + self->vp_surf_h * self->vp_div) * self->zoom);
    double cache_eff_zoom = eff_zoom / (double) self->vp_div;
    canvas_filter = (fabs (cache_eff_zoom - 1.0) < 1e-4 || cache_eff_zoom >= 1.5)
                      ? GSK_SCALING_FILTER_NEAREST
                      : GSK_SCALING_FILTER_LINEAR;
    gtk_snapshot_append_scaled_texture (snapshot,
                                        tex,
                                        canvas_filter,
                                        &GRAPHENE_RECT_INIT (x0, y0, x1 - x0, y1 - y0));
  }

  if (has_preview) {
    double x0 = self->drag_start_x / self->zoom;
    double y0 = self->drag_start_y / self->zoom;
    double x1 = self->preview_end_x;
    double y1 = self->preview_end_y;

    int rx, ry, rw, rh;
    double stroke_size = active_tool->uses_brush_size ? self->settings->size : 1.0;
    compute_stroke_bounds (x0, y0, x1, y1, stroke_size, &rx, &ry, &rw, &rh);

    int vis_x0 = MAX (0, (int) floor (vis_rect.origin.x / self->zoom) - 4);
    int vis_y0 = MAX (0, (int) floor (vis_rect.origin.y / self->zoom) - 4);
    int vis_x1 = MIN (self->document->width,
                      (int) ceil ((vis_rect.origin.x + vis_rect.size.width) / self->zoom) + 4);
    int vis_y1 = MIN (self->document->height,
                      (int) ceil ((vis_rect.origin.y + vis_rect.size.height) / self->zoom) + 4);

    int cx0 = MAX (vis_x0, rx);
    int cy0 = MAX (vis_y0, ry);
    int cx1 = MIN (vis_x1, rx + rw);
    int cy1 = MIN (vis_y1, ry + rh);

    if (cx0 < cx1 && cy0 < cy1) {
      int div = MAX (1, active_div);
      int ax0 = (cx0 / div) * div;
      int ay0 = (cy0 / div) * div;
      int ax1 = MIN (self->document->width, ((cx1 + div - 1) / div) * div);
      int ay1 = MIN (self->document->height, ((cy1 + div - 1) / div) * div);
      int sw = MAX (1, (ax1 - ax0 + div - 1) / div);
      int sh = MAX (1, (ay1 - ay0 + div - 1) / div);

      cairo_surface_t *psurf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, sw, sh);
      cairo_t *cr = cairo_create (psurf);
      if (div > 1)
        cairo_scale (cr, 1.0 / (double) div, 1.0 / (double) div);
      cairo_translate (cr, -(double) ax0, -(double) ay0);
      active_tool->draw_preview (active_tool, cr, x0, y0, x1, y1, self->settings);
      cairo_destroy (cr);

      cairo_surface_flush (psurf);
      int stride = cairo_image_surface_get_stride (psurf);
      unsigned char *data = cairo_image_surface_get_data (psurf);
      g_autoptr (GBytes) bytes = g_bytes_new (data, (gsize) stride * (gsize) sh);
      g_autoptr (GdkTexture) ptex = gdk_memory_texture_new (sw, sh, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
      cairo_surface_destroy (psurf);

      float px0 = (float) (ax0 * self->zoom);
      float py0 = (float) (ay0 * self->zoom);
      float px1 = (float) ((ax0 + sw * div) * self->zoom);
      float py1 = (float) ((ay0 + sh * div) * self->zoom);
      if (div > 1) {
        float cw = (float) (self->document->width * self->zoom);
        float ch = (float) (self->document->height * self->zoom);
        gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (0.0f, 0.0f, cw, ch));
      }
      gtk_snapshot_append_scaled_texture (snapshot,
                                          ptex,
                                          canvas_filter,
                                          &GRAPHENE_RECT_INIT (px0, py0, px1 - px0, py1 - py0));
      if (div > 1)
        gtk_snapshot_pop (snapshot);
    }
  }

  if (self->show_cursor_ring && self->has_hover && active_tool) {
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

static gboolean
is_touch_gesture (GtkGesture *gesture)
{
  GdkEventSequence *seq = gtk_gesture_single_get_current_sequence (GTK_GESTURE_SINGLE (gesture));
  GdkEvent *ev = gtk_gesture_get_last_event (gesture, seq);
  if (ev) {
    GdkEventType et = gdk_event_get_event_type (ev);
    if (et == GDK_TOUCH_BEGIN || et == GDK_TOUCH_UPDATE ||
        et == GDK_TOUCH_END || et == GDK_TOUCH_CANCEL) {
      return TRUE;
    }
  }
  GdkDevice *dev = gtk_gesture_get_device (gesture);
  return dev && gdk_device_get_source (dev) == GDK_SOURCE_TOUCHSCREEN;
}

static void
resolve_drag_point (PaintCanvas *self,
                    GtkGestureDrag *gesture,
                    const PaintTool *tool,
                    double offset_x,
                    double offset_y,
                    double *out_cx,
                    double *out_cy)
{
  double cx = (self->drag_start_x + offset_x) / self->zoom;
  double cy = (self->drag_start_y + offset_y) / self->zoom;
  if (tool->snap_point) {
    GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
    if (state & GDK_SHIFT_MASK)
      tool->snap_point (self->drag_start_x / self->zoom,
                        self->drag_start_y / self->zoom,
                        &cx,
                        &cy);
  }
  *out_cx = cx;
  *out_cy = cy;
}

static void
on_drag_begin (GtkGestureDrag *gesture,
               double start_x,
               double start_y,
               gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gboolean is_touch = is_touch_gesture (GTK_GESTURE (gesture));

  /* Leave touch sequences unclaimed so parent CAPTURE zoom gesture can see a 2nd finger */
  if (!is_touch)
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->drawing_active = TRUE;
  self->touch_stroke_pending = is_touch;
  self->drag_start_x = start_x;
  self->drag_start_y = start_y;
  self->last_drag_off_x = 0.0;
  self->last_drag_off_y = 0.0;

  double cx = start_x / self->zoom;
  double cy = start_y / self->zoom;
  self->has_hover = !is_touch;
  self->hover_x = cx;
  self->hover_y = cy;
  self->preview_end_x = cx;
  self->preview_end_y = cy;

  if (!is_touch) {
    const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
    paint_document_begin_stroke (self->document, cx, cy, tool, self->settings);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    if (!tool->draw_preview)
      g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
on_drag_update (GtkGestureDrag *gesture,
                double offset_x,
                double offset_y,
                gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  if (self->touch_stroke_pending) {
    if (!tool->uses_brush_size)
      return;
    if (hypot (offset_x, offset_y) < 6.0)
      return;
    self->touch_stroke_pending = FALSE;
    paint_document_begin_stroke (self->document,
                                 self->drag_start_x / self->zoom,
                                 self->drag_start_y / self->zoom,
                                 tool,
                                 self->settings);
    if (!tool->draw_preview)
      g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
  } else if (hypot (offset_x - self->last_drag_off_x, offset_y - self->last_drag_off_y) < 0.75) {
    return;
  }

  self->last_drag_off_x = offset_x;
  self->last_drag_off_y = offset_y;

  double cx, cy;
  resolve_drag_point (self, gesture, tool, offset_x, offset_y, &cx, &cy);
  self->hover_x = cx;
  self->hover_y = cy;
  self->preview_end_x = cx;
  self->preview_end_y = cy;

  paint_document_continue_stroke (self->document, cx, cy, tool, self->settings);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_end (GtkGestureDrag *gesture,
             double offset_x,
             double offset_y,
             gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  if (!self->drawing_active)
    return;

  self->drawing_active = FALSE;
  const PaintTool *tool = paint_tools_find (self->settings->active_tool_id);
  if (self->touch_stroke_pending) {
    self->touch_stroke_pending = FALSE;
    paint_document_begin_stroke (self->document,
                                 self->drag_start_x / self->zoom,
                                 self->drag_start_y / self->zoom,
                                 tool,
                                 self->settings);
  }

  double cx, cy;
  resolve_drag_point (self, gesture, tool, offset_x, offset_y, &cx, &cy);
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
    if (self->touch_stroke_pending) {
      self->touch_stroke_pending = FALSE;
    } else {
      paint_document_cancel_stroke (self->document);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
    }
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
  g_clear_pointer (&self->ov_surface, cairo_surface_destroy);
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
  if (target_vp_div (self) == 0 && self->ov_texture &&
      self->ov_doc_surface == self->document->surface) {
    return;
  }
  graphene_rect_t vis = compute_visible_widget_rect (self);
  if (vis.size.width > 0.0f && vis.size.height > 0.0f &&
      !is_visible_covered_by_vp_cache (self, &vis)) {
    gtk_widget_queue_draw (GTK_WIDGET (self));
  }
}
