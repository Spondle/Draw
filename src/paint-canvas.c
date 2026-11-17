#include "paint-canvas.h"

#include <math.h>
#include <string.h>

#define MAX_UNDO_STEPS 40

static const double ZOOM_STEPS[] = {
  0.25, 0.33, 0.5, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0
};

typedef struct {
  int width;
  int height;
  int stride;
  GBytes *pixels;
} SurfaceSnapshot;

static void
surface_snapshot_free (gpointer ptr)
{
  SurfaceSnapshot *snap = ptr;
  if (!snap)
    return;
  g_clear_pointer (&snap->pixels, g_bytes_unref);
  g_free (snap);
}

static SurfaceSnapshot *
surface_snapshot_capture (cairo_surface_t *surface)
{
  cairo_surface_flush (surface);
  int w = cairo_image_surface_get_width (surface);
  int h = cairo_image_surface_get_height (surface);
  int stride = cairo_image_surface_get_stride (surface);
  const unsigned char *data = cairo_image_surface_get_data (surface);
  gsize byte_len = (gsize) stride * (gsize) h;

  SurfaceSnapshot *snap = g_new0 (SurfaceSnapshot, 1);
  snap->width = w;
  snap->height = h;
  snap->stride = stride;
  snap->pixels = g_bytes_new (data, byte_len);
  return snap;
}

static cairo_surface_t *
surface_snapshot_restore (const SurfaceSnapshot *snap)
{
  cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                         snap->width,
                                                         snap->height);
  gsize len = 0;
  const unsigned char *src = g_bytes_get_data (snap->pixels, &len);
  unsigned char *dst = cairo_image_surface_get_data (surface);
  int dst_stride = cairo_image_surface_get_stride (surface);

  if (dst_stride == snap->stride) {
    memcpy (dst, src, len);
  } else {
    int copy_bytes = MIN (dst_stride, snap->stride);
    for (int y = 0; y < snap->height; y++)
      memcpy (dst + y * dst_stride, src + y * snap->stride, (gsize) copy_bytes);
  }
  cairo_surface_mark_dirty (surface);
  return surface;
}

struct _PaintDocument {
  int width;
  int height;
  GdkRGBA background_color;
  cairo_surface_t *surface;
  GFile *file;
  gboolean modified;
  GPtrArray *undo_stack; /* element-type: SurfaceSnapshot* */
  GPtrArray *redo_stack; /* element-type: SurfaceSnapshot* */
};

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
  doc->undo_stack = g_ptr_array_new_with_free_func (surface_snapshot_free);
  doc->redo_stack = g_ptr_array_new_with_free_func (surface_snapshot_free);
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
  return doc;
}

void
paint_document_free (PaintDocument *doc)
{
  if (!doc)
    return;
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

static void
push_undo_state (PaintDocument *doc)
{
  g_ptr_array_add (doc->undo_stack, surface_snapshot_capture (doc->surface));
  if (doc->undo_stack->len > MAX_UNDO_STEPS)
    g_ptr_array_remove_index (doc->undo_stack, 0);
  g_ptr_array_set_size (doc->redo_stack, 0);
}

gboolean
paint_document_can_undo (const PaintDocument *doc)
{
  return doc->undo_stack->len > 0;
}

gboolean
paint_document_can_redo (const PaintDocument *doc)
{
  return doc->redo_stack->len > 0;
}

gboolean
paint_document_undo (PaintDocument *doc)
{
  if (doc->undo_stack->len == 0)
    return FALSE;

  g_ptr_array_add (doc->redo_stack, surface_snapshot_capture (doc->surface));
  guint last = doc->undo_stack->len - 1;
  const SurfaceSnapshot *snap = g_ptr_array_index (doc->undo_stack, last);
  cairo_surface_destroy (doc->surface);
  doc->surface = surface_snapshot_restore (snap);
  doc->width = cairo_image_surface_get_width (doc->surface);
  doc->height = cairo_image_surface_get_height (doc->surface);
  g_ptr_array_remove_index (doc->undo_stack, last);
  doc->modified = TRUE;
  return TRUE;
}

gboolean
paint_document_redo (PaintDocument *doc)
{
  if (doc->redo_stack->len == 0)
    return FALSE;

  g_ptr_array_add (doc->undo_stack, surface_snapshot_capture (doc->surface));
  guint last = doc->redo_stack->len - 1;
  const SurfaceSnapshot *snap = g_ptr_array_index (doc->redo_stack, last);
  cairo_surface_destroy (doc->surface);
  doc->surface = surface_snapshot_restore (snap);
  doc->width = cairo_image_surface_get_width (doc->surface);
  doc->height = cairo_image_surface_get_height (doc->surface);
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
  push_undo_state (doc);
  cairo_t *cr = cairo_create (doc->surface);
  tool->begin_stroke (tool, cr, x, y, settings);
  cairo_destroy (cr);
  doc->modified = TRUE;
}

void
paint_document_continue_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings)
{
  cairo_t *cr = cairo_create (doc->surface);
  tool->continue_stroke (tool, cr, x, y, settings);
  cairo_destroy (cr);
  doc->modified = TRUE;
}

void
paint_document_end_stroke (PaintDocument *doc,
                           double x,
                           double y,
                           const PaintTool *tool,
                           PaintToolSettings *settings)
{
  cairo_t *cr = cairo_create (doc->surface);
  tool->end_stroke (tool, cr, x, y, settings);
  cairo_destroy (cr);
  doc->modified = TRUE;
}

void
paint_document_clear (PaintDocument *doc)
{
  push_undo_state (doc);
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
}

void
paint_document_resize (PaintDocument *doc, int new_width, int new_height)
{
  int w = MAX (1, new_width);
  int h = MAX (1, new_height);
  if (w == doc->width && h == doc->height)
    return;

  push_undo_state (doc);
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
 * PaintCanvas (GtkDrawingArea)
 * ============================================================================ */

enum {
  SIGNAL_CHANGED,
  SIGNAL_ZOOM_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PaintCanvas {
  GtkDrawingArea parent_instance;

  PaintDocument *document;
  PaintToolSettings *settings; /* unowned shared settings */
  double zoom;
  gboolean has_hover;
  double hover_x;
  double hover_y;
  gboolean drawing_active;
  double drag_start_x;
  double drag_start_y;
};

G_DEFINE_FINAL_TYPE (PaintCanvas, paint_canvas, GTK_TYPE_DRAWING_AREA)

static void
update_size_request (PaintCanvas *self)
{
  if (!self->document)
    return;
  int w = MAX (1, (int) lround (self->document->width * self->zoom));
  int h = MAX (1, (int) lround (self->document->height * self->zoom));
  gtk_widget_set_size_request (GTK_WIDGET (self), w, h);
}

static void
on_draw (GtkDrawingArea *area,
         cairo_t *cr,
         int width G_GNUC_UNUSED,
         int height G_GNUC_UNUSED,
         gpointer user_data G_GNUC_UNUSED)
{
  PaintCanvas *self = PAINT_CANVAS (area);
  if (!self->document)
    return;

  cairo_save (cr);
  cairo_scale (cr, self->zoom, self->zoom);
  cairo_set_source_surface (cr, self->document->surface, 0, 0);
  cairo_pattern_t *pattern = cairo_get_source (cr);
  cairo_pattern_set_filter (pattern,
                            self->zoom >= 3.0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_restore (cr);

  if (self->has_hover && self->settings) {
    double px = self->hover_x * self->zoom;
    double py = self->hover_y * self->zoom;
    double radius = MAX (2.0, (self->settings->size * self->zoom) / 2.0);

    cairo_save (cr);
    cairo_set_line_width (cr, 1.5);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.85);
    cairo_arc (cr, px, py, radius + 0.75, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);

    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.75);
    cairo_arc (cr, px, py, radius, 0.0, 2.0 * G_PI);
    cairo_stroke (cr);
    cairo_restore (cr);
  }
}

static void
on_drag_begin (GtkGestureDrag *gesture G_GNUC_UNUSED,
               double start_x,
               double start_y,
               gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  gtk_widget_grab_focus (GTK_WIDGET (self));
  self->drawing_active = TRUE;
  self->drag_start_x = start_x;
  self->drag_start_y = start_y;

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
on_motion (GtkEventControllerMotion *ctrl G_GNUC_UNUSED,
           double x,
           double y,
           gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->has_hover = TRUE;
  self->hover_x = x / self->zoom;
  self->hover_y = y / self->zoom;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *ctrl G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvas *self = PAINT_CANVAS (user_data);
  self->has_hover = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
paint_canvas_finalize (GObject *object)
{
  PaintCanvas *self = PAINT_CANVAS (object);
  g_clear_pointer (&self->document, paint_document_free);
  G_OBJECT_CLASS (paint_canvas_parent_class)->finalize (object);
}

static void
paint_canvas_class_init (PaintCanvasClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_canvas_finalize;

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

  gtk_widget_add_css_class (GTK_WIDGET (self), "paint-canvas-frame");
  gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_CENTER);
  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self), on_draw, NULL, NULL);

  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
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
  self->document = doc ? doc : paint_document_new (1280, 720);
  self->settings = settings;
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
