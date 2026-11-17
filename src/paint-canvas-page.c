#include "paint-canvas-page.h"

#include <math.h>

static const int BRUSH_PRESETS[] = { 2, 6, 12, 24, 48 };

enum {
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PaintCanvasPage {
  AdwBin parent_instance;

  PaintToolSettings *settings; /* unowned */
  PaintCanvas *canvas;
  GtkOverlay *overlay;
  GtkScrolledWindow *scrolled;

  GtkWidget *osd_pill;
  GtkWidget *undo_btn;
  GtkWidget *redo_btn;
  GtkWidget *size_btn;
  GtkScale *size_scale;
  GtkColorDialogButton *color_btn;
  GHashTable *tool_buttons; /* char* -> GtkToggleButton* */

  double pan_start_h;
  double pan_start_v;
  gboolean syncing;
};

G_DEFINE_FINAL_TYPE (PaintCanvasPage, paint_canvas_page, ADW_TYPE_BIN)

static void
on_canvas_changed (PaintCanvas *canvas G_GNUC_UNUSED, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  paint_canvas_page_sync_controls (self);
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
on_canvas_zoom_changed (PaintCanvas *canvas G_GNUC_UNUSED,
                        double zoom G_GNUC_UNUSED,
                        gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
on_tool_toggled (GtkToggleButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing || !gtk_toggle_button_get_active (btn))
    return;
  const char *tool_id = g_object_get_data (G_OBJECT (btn), "paint-tool-id");
  if (tool_id) {
    self->settings->active_tool_id = tool_id;
    gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
  }
}

static void
on_size_scale_changed (GtkRange *range, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  double val = gtk_range_get_value (range);
  self->settings->size = val;
  g_autofree char *label = g_strdup_printf ("%d px", (int) lround (val));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), label);
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

static void
on_preset_clicked (GtkButton *btn, gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  int val = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (btn), "paint-preset-px"));
  paint_canvas_page_set_brush_size (self, (double) val);
}

static void
on_color_changed (GObject *obj,
                  GParamSpec *pspec G_GNUC_UNUSED,
                  gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  if (self->syncing)
    return;
  const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (obj));
  if (rgba)
    self->settings->color = *rgba;
}

static gboolean
on_scroll (GtkEventControllerScroll *ctrl,
           double dx G_GNUC_UNUSED,
           double dy,
           gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (ctrl));
  if (state & GDK_CONTROL_MASK) {
    if (dy < 0)
      paint_canvas_zoom_in (self->canvas);
    else if (dy > 0)
      paint_canvas_zoom_out (self->canvas);
    return TRUE;
  }
  return FALSE;
}

static void
on_pan_begin (GtkGestureDrag *gesture G_GNUC_UNUSED,
              double start_x G_GNUC_UNUSED,
              double start_y G_GNUC_UNUSED,
              gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  self->pan_start_h = gtk_adjustment_get_value (gtk_scrolled_window_get_hadjustment (self->scrolled));
  self->pan_start_v = gtk_adjustment_get_value (gtk_scrolled_window_get_vadjustment (self->scrolled));
}

static void
on_pan_update (GtkGestureDrag *gesture G_GNUC_UNUSED,
               double offset_x,
               double offset_y,
               gpointer user_data)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (user_data);
  gtk_adjustment_set_value (gtk_scrolled_window_get_hadjustment (self->scrolled),
                            self->pan_start_h - offset_x);
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scrolled),
                            self->pan_start_v - offset_y);
}

static GtkPopover *
build_size_popover (PaintCanvasPage *self)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (box, 10);
  gtk_widget_set_margin_bottom (box, 10);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  GtkWidget *heading = gtk_label_new ("Brush Size");
  gtk_widget_add_css_class (heading, "heading");
  gtk_widget_set_halign (heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), heading);

  GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1.0, 96.0, 1.0);
  gtk_widget_set_size_request (scale, 190, -1);
  gtk_range_set_value (GTK_RANGE (scale), self->settings->size);
  gtk_scale_set_draw_value (GTK_SCALE (scale), TRUE);
  gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_RIGHT);
  g_signal_connect (scale, "value-changed", G_CALLBACK (on_size_scale_changed), self);
  gtk_box_append (GTK_BOX (box), scale);
  self->size_scale = GTK_SCALE (scale);

  GtkWidget *presets = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_halign (presets, GTK_ALIGN_CENTER);
  for (gsize i = 0; i < G_N_ELEMENTS (BRUSH_PRESETS); i++) {
    int px = BRUSH_PRESETS[i];
    g_autofree char *lbl = g_strdup_printf ("%d", px);
    GtkWidget *btn = gtk_button_new_with_label (lbl);
    gtk_widget_add_css_class (btn, "flat");
    g_object_set_data (G_OBJECT (btn), "paint-preset-px", GINT_TO_POINTER (px));
    g_signal_connect (btn, "clicked", G_CALLBACK (on_preset_clicked), self);
    gtk_box_append (GTK_BOX (presets), btn);
  }
  gtk_box_append (GTK_BOX (box), presets);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  return GTK_POPOVER (pop);
}

static void
build_ui (PaintCanvasPage *self)
{
  self->overlay = GTK_OVERLAY (gtk_overlay_new ());
  adw_bin_set_child (ADW_BIN (self), GTK_WIDGET (self->overlay));

  self->scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_kinetic_scrolling (self->scrolled, FALSE);
  gtk_scrolled_window_set_propagate_natural_width (self->scrolled, FALSE);
  gtk_scrolled_window_set_propagate_natural_height (self->scrolled, FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (self->scrolled), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self->scrolled), TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (self->scrolled), "paint-workspace");

  GtkWidget *container = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (container, TRUE);
  gtk_widget_set_vexpand (container, TRUE);
  gtk_widget_set_halign (container, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (container, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top (container, 16);
  gtk_widget_set_margin_bottom (container, 72);
  gtk_widget_set_margin_start (container, 24);
  gtk_widget_set_margin_end (container, 24);
  gtk_box_append (GTK_BOX (container), GTK_WIDGET (self->canvas));

  gtk_scrolled_window_set_child (self->scrolled, container);
  gtk_overlay_set_child (self->overlay, GTK_WIDGET (self->scrolled));

  GtkEventController *scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), scroll);

  GtkGesture *pan = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (pan), GDK_BUTTON_MIDDLE);
  g_signal_connect (pan, "drag-begin", G_CALLBACK (on_pan_begin), self);
  g_signal_connect (pan, "drag-update", G_CALLBACK (on_pan_update), self);
  gtk_widget_add_controller (GTK_WIDGET (self->scrolled), GTK_EVENT_CONTROLLER (pan));

  /* Floating OSD pill */
  GtkWidget *pill = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (pill, "paint-osd-pill");
  gtk_widget_set_halign (pill, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (pill, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (pill, 16);

  GtkWidget *undo_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->undo_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
  gtk_widget_add_css_class (self->undo_btn, "flat");
  gtk_widget_set_tooltip_text (self->undo_btn, "Undo (Ctrl+Z)");
  g_signal_connect_swapped (self->undo_btn, "clicked", G_CALLBACK (paint_canvas_undo), self->canvas);

  self->redo_btn = gtk_button_new_from_icon_name ("edit-redo-symbolic");
  gtk_widget_add_css_class (self->redo_btn, "flat");
  gtk_widget_set_tooltip_text (self->redo_btn, "Redo (Ctrl+Shift+Z)");
  g_signal_connect_swapped (self->redo_btn, "clicked", G_CALLBACK (paint_canvas_redo), self->canvas);

  gtk_box_append (GTK_BOX (undo_box), self->undo_btn);
  gtk_box_append (GTK_BOX (undo_box), self->redo_btn);
  gtk_box_append (GTK_BOX (pill), undo_box);

  gtk_box_append (GTK_BOX (pill), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  GtkWidget *tools_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gsize n_tools = 0;
  const PaintTool *tools = paint_tools_get_all (&n_tools);
  GtkToggleButton *first_btn = NULL;
  for (gsize i = 0; i < n_tools; i++) {
    GtkWidget *btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (btn), tools[i].icon_name);
    gtk_widget_add_css_class (btn, "flat");
    gtk_widget_set_tooltip_text (btn, tools[i].tooltip);
    if (!first_btn)
      first_btn = GTK_TOGGLE_BUTTON (btn);
    else
      gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (btn), first_btn);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn),
                                  g_strcmp0 (tools[i].id, self->settings->active_tool_id) == 0);
    g_object_set_data (G_OBJECT (btn), "paint-tool-id", (gpointer) tools[i].id);
    g_signal_connect (btn, "toggled", G_CALLBACK (on_tool_toggled), self);
    gtk_box_append (GTK_BOX (tools_box), btn);
    g_hash_table_insert (self->tool_buttons, (gpointer) tools[i].id, btn);
  }
  gtk_box_append (GTK_BOX (pill), tools_box);

  gtk_box_append (GTK_BOX (pill), gtk_separator_new (GTK_ORIENTATION_VERTICAL));

  self->size_btn = gtk_menu_button_new ();
  gtk_widget_add_css_class (self->size_btn, "flat");
  gtk_widget_add_css_class (self->size_btn, "paint-size-button");
  g_autofree char *size_lbl = g_strdup_printf ("%d px", (int) lround (self->settings->size));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), size_lbl);
  gtk_widget_set_tooltip_text (self->size_btn, "Brush Size ([ / ])");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->size_btn), GTK_WIDGET (build_size_popover (self)));
  gtk_box_append (GTK_BOX (pill), self->size_btn);

  GtkColorDialog *color_dialog = gtk_color_dialog_new ();
  gtk_color_dialog_set_title (color_dialog, "Select Brush Color");
  gtk_color_dialog_set_with_alpha (color_dialog, TRUE);
  self->color_btn = GTK_COLOR_DIALOG_BUTTON (gtk_color_dialog_button_new (color_dialog));
  gtk_color_dialog_button_set_rgba (self->color_btn, &self->settings->color);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->color_btn), "Brush Color");
  g_signal_connect (self->color_btn, "notify::rgba", G_CALLBACK (on_color_changed), self);
  gtk_box_append (GTK_BOX (pill), GTK_WIDGET (self->color_btn));

  self->osd_pill = pill;
  gtk_overlay_add_overlay (self->overlay, pill);

  g_signal_connect (self->canvas, "changed", G_CALLBACK (on_canvas_changed), self);
  g_signal_connect (self->canvas, "zoom-changed", G_CALLBACK (on_canvas_zoom_changed), self);
}

static void
paint_canvas_page_finalize (GObject *object)
{
  PaintCanvasPage *self = PAINT_CANVAS_PAGE (object);
  g_clear_pointer (&self->tool_buttons, g_hash_table_unref);
  G_OBJECT_CLASS (paint_canvas_page_parent_class)->finalize (object);
}

static void
paint_canvas_page_class_init (PaintCanvasPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = paint_canvas_page_finalize;

  signals[SIGNAL_STATE_CHANGED] = g_signal_new ("state-changed",
                                                G_TYPE_FROM_CLASS (klass),
                                                G_SIGNAL_RUN_FIRST,
                                                0, NULL, NULL, NULL,
                                                G_TYPE_NONE, 0);
}

static void
paint_canvas_page_init (PaintCanvasPage *self)
{
  self->tool_buttons = g_hash_table_new (g_str_hash, g_str_equal);
}

PaintCanvasPage *
paint_canvas_page_new (PaintDocument *doc, PaintToolSettings *settings)
{
  PaintCanvasPage *self = g_object_new (PAINT_TYPE_CANVAS_PAGE, NULL);
  self->settings = settings;
  self->canvas = paint_canvas_new (doc, settings);
  build_ui (self);
  paint_canvas_page_sync_controls (self);
  return self;
}

PaintCanvas *
paint_canvas_page_get_canvas (PaintCanvasPage *self)
{
  return self->canvas;
}

void
paint_canvas_page_select_tool (PaintCanvasPage *self, const char *tool_id)
{
  GtkToggleButton *btn = g_hash_table_lookup (self->tool_buttons, tool_id);
  if (btn)
    gtk_toggle_button_set_active (btn, TRUE);
}

void
paint_canvas_page_set_brush_size (PaintCanvasPage *self, double size)
{
  double clamped = CLAMP (size, 1.0, 96.0);
  self->settings->size = clamped;
  self->syncing = TRUE;
  gtk_range_set_value (GTK_RANGE (self->size_scale), clamped);
  g_autofree char *lbl = g_strdup_printf ("%d px", (int) lround (clamped));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), lbl);
  self->syncing = FALSE;
  gtk_widget_queue_draw (GTK_WIDGET (self->canvas));
}

void
paint_canvas_page_adjust_brush_size (PaintCanvasPage *self, double delta)
{
  paint_canvas_page_set_brush_size (self, self->settings->size + delta);
}

void
paint_canvas_page_toggle_osd (PaintCanvasPage *self)
{
  gtk_widget_set_visible (self->osd_pill, !gtk_widget_get_visible (self->osd_pill));
}

void
paint_canvas_page_sync_controls (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  gtk_widget_set_sensitive (self->undo_btn, paint_document_can_undo (doc));
  gtk_widget_set_sensitive (self->redo_btn, paint_document_can_redo (doc));

  self->syncing = TRUE;
  GtkToggleButton *active_btn = g_hash_table_lookup (self->tool_buttons,
                                                     self->settings->active_tool_id);
  if (active_btn && !gtk_toggle_button_get_active (active_btn))
    gtk_toggle_button_set_active (active_btn, TRUE);

  gtk_range_set_value (GTK_RANGE (self->size_scale), self->settings->size);
  g_autofree char *lbl = g_strdup_printf ("%d px", (int) lround (self->settings->size));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->size_btn), lbl);
  gtk_color_dialog_button_set_rgba (self->color_btn, &self->settings->color);
  self->syncing = FALSE;
}

char *
paint_canvas_page_dup_title (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  GFile *file = paint_document_get_file (doc);
  g_autofree char *base = file ? g_file_get_basename (file) : g_strdup ("Untitled Canvas");
  if (paint_document_get_modified (doc))
    return g_strdup_printf ("• %s", base);
  return g_strdup (base);
}

char *
paint_canvas_page_dup_subtitle (PaintCanvasPage *self)
{
  PaintDocument *doc = paint_canvas_get_document (self->canvas);
  int w = paint_document_get_width (doc);
  int h = paint_document_get_height (doc);
  int zoom_pct = (int) lround (paint_canvas_get_zoom (self->canvas) * 100.0);
  GFile *file = paint_document_get_file (doc);
  if (file) {
    g_autoptr (GFile) parent = g_file_get_parent (file);
    g_autofree char *parent_path = parent ? g_file_get_path (parent) : NULL;
    if (parent_path)
      return g_strdup_printf ("%s · %d × %d · %d%%", parent_path, w, h, zoom_pct);
  }
  return g_strdup_printf ("%d × %d · %d%%", w, h, zoom_pct);
}
