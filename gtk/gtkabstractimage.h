/*  Copyright 2016 Timm Bäder
 *
 * GTK+ is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GTK+; see the file COPYING.  If not,
 * see <http://www.gnu.org/licenses/>.
 */

#ifndef __GTK_ABSTRACT_IMAGE_H__
#define __GTK_ABSTRACT_IMAGE_H__

#if !defined (__GTK_H_INSIDE__) && !defined (GTK_COMPILATION)
#error "Only <gtk/gtk.h> can be included directly."
#endif

#include <gtk/gtkwidget.h>

G_BEGIN_DECLS

typedef struct _GtkAbstractImage GtkAbstractImage;
typedef struct _GtkAbstractImageClass GtkAbstractImageClass;

#define GTK_TYPE_ABSTRACT_IMAGE           (gtk_abstract_image_get_type ())
#define GTK_ABSTRACT_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImage))
#define GTK_ABSTRACT_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImageClass))
#define GTK_IS_ABSTRACT_IMAGE(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_ABSTRACT_IMAGE))
#define GTK_IS_ABSTRACT_IMAGE_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_ABSTRACT_IMAGE))
#define GTK_ABSTRACT_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_ABSTRACT_IMAGE, GtkAbstractImageClass))


struct _GtkAbstractImage
{
  GObject parent;
};


struct _GtkAbstractImageClass
{
  GObjectClass parent_class;
  int    (*get_width) (GtkAbstractImage *image);
  int    (*get_height) (GtkAbstractImage *image);
  int    (*get_scale_factor) (GtkAbstractImage *image);
  void   (*draw) (GtkAbstractImage *image, cairo_t *ct);

  void   (*changed) (GtkAbstractImage image);
};

GDK_AVAILABLE_IN_3_20
GType gtk_abstract_image_get_type (void) G_GNUC_CONST;

int gtk_abstract_image_get_width (GtkAbstractImage *image);

int gtk_abstract_image_get_height (GtkAbstractImage *image);

void gtk_abstract_image_draw (GtkAbstractImage *image, cairo_t *ct);

int gtk_abstract_image_get_scale_factor (GtkAbstractImage *image);

/* ------------------------------------------------------------------------------------ */

typedef struct _GtkPixbufImage GtkPixbufImage;
typedef struct _GtkPixbufImageClass GtkPixbufImageClass;

#define GTK_TYPE_PIXBUF_IMAGE           (gtk_pixbuf_image_get_type ())
#define GTK_PIXBUF_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImage))
#define GTK_PIXBUF_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImageClass))
#define GTK_IS_PIXBUF_IMAGE(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_PIXBUF_IMAGE))
#define GTK_IS_PIXBUF_IMAGE_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_PIXBUF_IMAGE))
#define GTK_PIXBUF_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_PIXBUF_IMAGE, GtkPixbufImageClass))

struct _GtkPixbufImage
{
  GtkAbstractImage parent;
  GdkPixbuf *pixbuf;
  int scale_factor;
};

struct _GtkPixbufImageClass
{
  GtkAbstractImageClass parent_class;
};

GDK_AVAILABLE_IN_3_20
GType gtk_pixbuf_image_get_type (void) G_GNUC_CONST;

GDK_AVAILABLE_IN_3_20
GtkPixbufImage *gtk_pixbuf_image_new (const char *path, int scale_factor);


/* ------------------------------------------------------------------------------------ */


typedef struct _GtkPixbufAnimationImage GtkPixbufAnimationImage;
typedef struct _GtkPixbufAnimationImageClass GtkPixbufAnimationImageClass;

#define GTK_TYPE_PIXBUF_ANIMATION_IMAGE           (gtk_pixbuf_animation_image_get_type ())
#define GTK_PIXBUF_ANIMATION_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_PIXBUF_ANIMATION_IMAGE, GtkPixbufAnimationImage))
#define GTK_PIXBUF_ANIMATION_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_PIXBUF_ANIMATION_IMAGE, GtkPixbufAnimationImageClass))
#define GTK_IS_PIXBUX_ANIMATION_IMAGE(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_PIXBUF_ANIMATION_IMAGE))
#define GTK_IS_PIXBUX_ANIMATION_IMAGE_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_PIXBUF_ANIMATION_IMAGE))
#define GTK_PIXBUF_ANIMATION_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_PIXBUF_ANIMATION_IMAGE, GtkPixbufAnimationImageClass))

struct _GtkPixbufAnimationImage
{
  GtkAbstractImage parent;
  GdkPixbufAnimation *animation;
  GdkPixbufAnimationIter *iter;
  cairo_surface_t *frame;
  int scale_factor;
  int delay_ms;
  guint timeout_id;
};

struct _GtkPixbufAnimationImageClass
{
  GtkAbstractImageClass parent_class;
};

GDK_AVAILABLE_IN_3_20
GType gtk_pixbuf_animation_image_get_type (void) G_GNUC_CONST;

GDK_AVAILABLE_IN_3_20
GtkPixbufAnimationImage *gtk_pixbuf_animation_image_new (GdkPixbufAnimation *animation,
                                                         int scale_factor);


/* ------------------------------------------------------------------------------------ */

typedef struct _GtkSurfaceImage GtkSurfaceImage;
typedef struct _GtkSurfaceImageClass GtkSurfaceImageClass;


#define GTK_TYPE_SURFACE_IMAGE           (gtk_surface_image_get_type ())
#define GTK_SURFACE_IMAGE(obj)           (G_TYPE_CHECK_INSTANCE_CAST (obj, GTK_TYPE_SURFACE_IMAGE, GtkSurfaceImage))
#define GTK_SURFACE_IMAGE_CLASS(cls)     (G_TYPE_CHECK_CLASS_CAST (cls, GTK_TYPE_SURFACE_IMAGE, GtkSurfaceImageClass))
#define GTK_IS_SURFACE_IMAGE(obj)        (G_TYPE_CHECK_INSTANCE_TYPE (obj, GTK_TYPE_SURFACE_IMAGE))
#define GTK_IS_SURFACE_IMAGE_CLASS(obj)  (G_TYPE_CHECK_CLASS_TYPE (obj, GTK_TYPE_SURFACE_IMAGE))
#define GTK_SURFACE_IMAGE_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GTK_TYPE_SURFACE_IMAGE, GtkSurfaceImageClass))

struct _GtkSurfaceImage
{
  GtkAbstractImage parent;
  cairo_surface_t *surface;
};

struct _GtkSurfaceImageClass
{
  GtkAbstractImageClass parent_class;
};

GDK_AVAILABLE_IN_3_20
GType gtk_surface_image_get_type (void) G_GNUC_CONST;

GtkSurfaceImage *gtk_surface_image_new (cairo_surface_t *surface);

G_END_DECLS

#endif
