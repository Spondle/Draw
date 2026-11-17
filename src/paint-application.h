#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PAINT_APP_ID "io.github.spondle.Paint"
#define PAINT_APP_NAME "Paint"
#define PAINT_APP_VERSION "0.1.0"

#define PAINT_TYPE_APPLICATION (paint_application_get_type ())
G_DECLARE_FINAL_TYPE (PaintApplication, paint_application, PAINT, APPLICATION, AdwApplication)

PaintApplication *paint_application_new (void);

G_END_DECLS
