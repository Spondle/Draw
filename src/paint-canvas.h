#pragma once

#include <cairo.h>
#include <gtk/gtk.h>

#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_DEFAULT_CANVAS_WIDTH 640
#define PAINT_DEFAULT_CANVAS_HEIGHT 380

typedef struct _PaintDocument PaintDocument;

PaintDocument *paint_document_new (int width, int height);
PaintDocument *paint_document_new_transparent (int width, int height);
PaintDocument *paint_document_new_from_file (GFile *file, GError **error);
PaintDocument *paint_document_new_from_draft (GFile *draft_file,
                                              GFile *original_file,
                                              gboolean modified,
                                              GError **error);
void paint_document_free (PaintDocument *doc);

int paint_document_get_width (const PaintDocument *doc);
int paint_document_get_height (const PaintDocument *doc);
cairo_surface_t *paint_document_get_surface (const PaintDocument *doc);
GFile *paint_document_get_file (const PaintDocument *doc);
gboolean paint_document_get_modified (const PaintDocument *doc);
void paint_document_set_modified (PaintDocument *doc, gboolean modified);
gboolean paint_document_has_transparent_background (const PaintDocument *doc);

gboolean paint_document_can_undo (const PaintDocument *doc);
gboolean paint_document_can_redo (const PaintDocument *doc);
gboolean paint_document_undo (PaintDocument *doc);
gboolean paint_document_redo (PaintDocument *doc);

void paint_document_begin_stroke (PaintDocument *doc,
                                  double x,
                                  double y,
                                  const PaintTool *tool,
                                  PaintToolSettings *settings);
void paint_document_continue_stroke (PaintDocument *doc,
                                     double x,
                                     double y,
                                     const PaintTool *tool,
                                     PaintToolSettings *settings);
void paint_document_end_stroke (PaintDocument *doc,
                                double x,
                                double y,
                                const PaintTool *tool,
                                PaintToolSettings *settings);
void paint_document_cancel_stroke (PaintDocument *doc);

void paint_document_clear (PaintDocument *doc);
void paint_document_clear_transparent (PaintDocument *doc);
void paint_document_resize (PaintDocument *doc, int new_width, int new_height);
gboolean paint_document_save_to_file (PaintDocument *doc, GFile *file, GError **error);

#define PAINT_TYPE_CANVAS (paint_canvas_get_type ())
G_DECLARE_FINAL_TYPE (PaintCanvas, paint_canvas, PAINT, CANVAS, GtkWidget)

PaintCanvas *paint_canvas_new (PaintDocument *doc, PaintToolSettings *settings);

PaintDocument *paint_canvas_get_document (PaintCanvas *self);
void paint_canvas_set_document (PaintCanvas *self, PaintDocument *doc);
PaintToolSettings *paint_canvas_get_settings (PaintCanvas *self);

double paint_canvas_get_zoom (PaintCanvas *self);
double paint_canvas_get_min_zoom (void);
double paint_canvas_get_max_zoom (void);
void paint_canvas_set_zoom (PaintCanvas *self, double zoom);
void paint_canvas_zoom_in (PaintCanvas *self);
void paint_canvas_zoom_out (PaintCanvas *self);
void paint_canvas_zoom_reset (PaintCanvas *self);

gboolean paint_canvas_undo (PaintCanvas *self);
gboolean paint_canvas_redo (PaintCanvas *self);
void paint_canvas_clear (PaintCanvas *self);
void paint_canvas_clear_transparent (PaintCanvas *self);
void paint_canvas_resize (PaintCanvas *self, int width, int height);
void paint_canvas_set_show_cursor_ring (PaintCanvas *self, gboolean show);
void paint_canvas_check_viewport (PaintCanvas *self);

G_END_DECLS
