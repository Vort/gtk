#include "config.h"
#include "gtkimageview.h"
#include "gtktypebuiltins.h"
#include "gtkmain.h"
#include "gtkintl.h"
#include "gtkprivate.h"
#include "gtkrender.h"
#include "gtkgesture.h"
#include "gtkgesturerotate.h"
#include "gtkgesturezoom.h"
#include "gtkscrollable.h"
#include "gtkmarshalers.h"
#include "gtkadjustment.h"
#include <gdk/gdkcairo.h>

#include <cairo-gobject.h>
#include <math.h>


#define DEG_TO_RAD(x) (((x) / 360.0) * (2 * M_PI))
#define RAD_TO_DEG(x) ((x / (2.0 * M_PI) * 360.0))

#define TRANSITION_DURATION (150.0 * 1000.0)

struct _GtkImageViewPrivate
{
  double   scale;
  double   angle;
  gboolean snap_angle;
  gboolean fit_allocation;
  gboolean scale_set;
  int      scale_factor;
  gboolean rotate_gesture_enabled;
  gboolean zoom_gesture_enabled;

  GtkGesture *rotate_gesture;
  GtkGesture *zoom_gesture;

  /* GtkScrollable stuff */
  GtkAdjustment       *hadjustment;
  GtkAdjustment       *vadjustment;
  GtkScrollablePolicy  hscroll_policy;
  GtkScrollablePolicy  vscroll_policy;

  gboolean                is_animation;
  GdkPixbufAnimation     *source_animation;
  GdkPixbufAnimationIter *source_animation_iter;
  cairo_surface_t        *image_surface;
  int                     surface_width;
  int                     surface_height;
  int                     animation_timeout;

  /* Transitions */
  gint64 angle_transition_start;
  double transition_start_angle;
  double transition_end_angle;
};

// XXX animate image size changes!
//
// XXX Keep track of the inital width/height and use a non-image cairo surface
// XXX Look for memory leaks

enum
{
  PROP_SCALE = 1,
  PROP_SCALE_SET,
  PROP_ANGLE,
  PROP_ROTATE_GESTURE_ENABLED,
  PROP_ZOOM_GESTURE_ENABLED,
  PROP_SNAP_ANGLE,
  PROP_FIT_ALLOCATION,
  LAST_WIDGET_PROPERTY,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY,

  LAST_PROPERTY
};

enum
{
  PREPARE_IMAGE,

  LAST_SIGNAL
};

static GParamSpec *widget_props[LAST_WIDGET_PROPERTY] = { NULL, };
static int         widget_signals[LAST_SIGNAL]        = { 0 };


G_DEFINE_TYPE_WITH_CODE (GtkImageView, gtk_image_view, GTK_TYPE_WIDGET,
                         G_ADD_PRIVATE (GtkImageView)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

typedef struct _LoadTaskData LoadTaskData;


// @ask How to solve this more elegantly?
struct _LoadTaskData
{
  int scale_factor;
  gpointer source;
};

static void
free_load_task_data (LoadTaskData *data)
{
  g_clear_object ((&data->source));
}


static void
gtk_image_view_init (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkStyleContext *sc = gtk_widget_get_style_context ((GtkWidget *)image_view);

  priv->scale = 1.0;
  priv->angle = 0.0;
  priv->surface_width = -1;
  priv->surface_height = -1;
  priv->snap_angle = FALSE;
  priv->fit_allocation = FALSE;
  priv->scale_set = FALSE;
  priv->rotate_gesture_enabled = TRUE;
  priv->zoom_gesture_enabled = TRUE;
  priv->rotate_gesture = gtk_gesture_rotate_new ((GtkWidget *)image_view);
  priv->zoom_gesture = gtk_gesture_zoom_new ((GtkWidget *)image_view);

  gtk_style_context_add_class (sc, GTK_STYLE_CLASS_BACKGROUND);
}

/* Prototypes {{{ */
static void gtk_image_view_update_surface (GtkImageView    *image_view,
                                           const GdkPixbuf *frame,
                                           int              scale_factor);

static void adjustment_value_changed_cb (GtkAdjustment *adjustment,
                                         gpointer       user_data);


/* }}} */


static GdkPixbuf *
gtk_image_view_get_current_frame (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  g_assert (priv->source_animation);

  if (priv->is_animation)
    return gdk_pixbuf_animation_iter_get_pixbuf (priv->source_animation_iter);
  else
    return gdk_pixbuf_animation_get_static_image (priv->source_animation);
}


static gboolean
gtk_image_view_update_animation (gpointer user_data)
{
  GtkImageView *image_view = user_data;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  gdk_pixbuf_animation_iter_advance (priv->source_animation_iter, NULL);
  gtk_image_view_update_surface (image_view,
                                 gtk_image_view_get_current_frame (image_view),
                                 priv->scale_factor);

  return priv->is_animation;
}


static void
gtk_image_view_start_animation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int delay_ms;

  g_assert (priv->is_animation);

  delay_ms = gdk_pixbuf_animation_iter_get_delay_time (priv->source_animation_iter);

  priv->animation_timeout = g_timeout_add (delay_ms, gtk_image_view_update_animation, image_view);
}

static void
gtk_image_view_stop_animation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->animation_timeout != 0)
    {
      g_assert (priv->is_animation);
      g_source_remove (priv->animation_timeout);
      priv->animation_timeout = 0;
    }
}


static gboolean
frameclock_cb (GtkWidget     *widget,
               GdkFrameClock *frame_clock,
               gpointer       user_data)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private ((GtkImageView *)widget);
  gint64 now = gdk_frame_clock_get_frame_time (frame_clock);

  double t = (now - priv->angle_transition_start) / TRANSITION_DURATION;

  double new_angle = (priv->transition_end_angle - priv->transition_start_angle) * t;

  priv->angle = priv->transition_start_angle + new_angle;

  if (priv->fit_allocation)
    gtk_widget_queue_draw (widget);
  else
    gtk_widget_queue_resize (widget);

  if (t >= 1.0)
    {
      priv->angle = priv->transition_end_angle;
      return FALSE;
    }

  return TRUE;
}


static void
gtk_image_view_animate_to_angle (GtkImageView *image_view,
                                 double        start_angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  /* target angle is priv->angle! */

  priv->transition_start_angle = start_angle;
  priv->transition_end_angle   = priv->angle;
  priv->angle_transition_start = gdk_frame_clock_get_frame_time (gtk_widget_get_frame_clock ((GtkWidget *)image_view));
  gtk_widget_add_tick_callback ((GtkWidget *)image_view, frameclock_cb, NULL, NULL);
}

static void
gtk_image_view_do_snapping (GtkImageView *image_view,
                            double        angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int new_angle;

  g_assert (priv->snap_angle);

  /* Snap to angles of 0, 90, 180 and 270 degrees */

  new_angle = (int) ((angle) / 90.0) * 90;

  if (new_angle != priv->angle)
    {
      double old_angle = priv->angle;
      priv->angle = new_angle;
      /* XXX Make this conditional */
      gtk_image_view_animate_to_angle (image_view,
                                       old_angle);
    }

  priv->angle = new_angle;
}

static void
gtk_image_view_compute_bounding_box (GtkImageView *image_view,
                                     int          *width,
                                     int          *height,
                                     double       *scale_out)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkAllocation alloc;
  int image_width;
  int image_height;
  int bb_width  = 0;
  int bb_height = 0;
  double upper_right_degrees;
  double upper_left_degrees;
  double r;
  int upper_right_x, upper_right_y;
  int upper_left_x, upper_left_y;
  double scale;


  if (!priv->image_surface)
    {
      *width  = 0;
      *height = 0;
      return;
    }

  gtk_widget_get_allocation ((GtkWidget *)image_view, &alloc);
  image_width  = priv->surface_width;
  image_height = priv->surface_height;

  upper_right_degrees = DEG_TO_RAD (priv->angle) + atan ((double)image_height / (double)image_width);
  upper_left_degrees  = DEG_TO_RAD (priv->angle) + atan ((double)image_height / -(double)image_width);
  r = sqrtf ((image_width / 2) * (image_width / 2) + (image_height / 2) * (image_height / 2));

  upper_right_x = r * cos (upper_right_degrees);
  upper_right_y = r * sin (upper_right_degrees);

  upper_left_x = r * cos (upper_left_degrees);
  upper_left_y = r * sin (upper_left_degrees);


  //
  bb_width  = MAX (fabs (upper_right_x), fabs (upper_left_x)) * 2;
  bb_height = MAX (fabs (upper_right_y), fabs (upper_left_y)) * 2;


  if (!priv->scale_set)
    {
      double scale_x = (double)alloc.width / (double)bb_width;
      double scale_y = (double)alloc.height / (double)bb_height;

      scale = MIN (MIN (scale_x, scale_y), 1.0);
    }
  else
    {
      scale = priv->scale;
    }

  if (scale_out)
    *scale_out = scale;

  if (priv->fit_allocation)
    {

      // XXX We probably don't want to do that here since it will be called fairly often.
      priv->scale = scale;
      g_object_notify_by_pspec ((GObject *)image_view,
                                widget_props[PROP_SCALE]);

      *width  = bb_width * scale;
      *height = bb_height * scale;
    }
  else
    {
      *width  = bb_width * scale;
      *height = bb_height * scale;
    }
}

static void
gtk_image_view_update_adjustments (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (!priv->hadjustment && !priv->vadjustment)
    return;

  if (!priv->image_surface)
    {
      gtk_adjustment_configure (priv->vadjustment, 0, 0, 0, 0, 0, 0);
      gtk_adjustment_configure (priv->hadjustment, 0, 0, 0, 0, 0, 0);
      return;
    }



  if (priv->fit_allocation)
    {
      gtk_adjustment_set_upper (priv->vadjustment, 0);
      gtk_adjustment_set_upper (priv->hadjustment, 0);
    }
  else
    {
      int width, height;
      gtk_image_view_compute_bounding_box (image_view,
                                           &width,
                                           &height,
                                           NULL);
      gtk_adjustment_set_upper (priv->hadjustment, width);
      gtk_adjustment_set_upper (priv->vadjustment, height);

    }


  gtk_adjustment_set_page_size (priv->hadjustment,
                                gtk_widget_get_allocated_width ((GtkWidget *)image_view));
  gtk_adjustment_set_page_size (priv->vadjustment,
                                gtk_widget_get_allocated_height ((GtkWidget *)image_view));

}

static gboolean
gtk_image_view_draw (GtkWidget *widget, cairo_t *ct)
{
  GtkImageView *image_view = (GtkImageView *)widget;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GtkStyleContext *sc = gtk_widget_get_style_context (widget);
  GtkAllocation alloc;
  int draw_x;
  int draw_y;
  int draw_width;
  int draw_height;
  int image_width;
  int image_height;
  double scale = 0.0;

  gtk_widget_get_allocation (widget, &alloc);


  gtk_render_background (sc, ct, 0, 0, alloc.width, alloc.height);
  gtk_render_frame      (sc, ct, 0, 0, alloc.width, alloc.height);

  if (!priv->image_surface)
    return GDK_EVENT_PROPAGATE;

  gtk_image_view_compute_bounding_box (image_view, &draw_width, &draw_height, &scale);

  image_width  = priv->surface_width  * scale;
  image_height = priv->surface_height * scale;

  if (image_width == 0 || image_height == 0)
    return GDK_EVENT_PROPAGATE;


  draw_x = (alloc.width  - draw_width) / 2;
  draw_y = (alloc.height - draw_height) / 2;


#if 1
  {
    cairo_save (ct);
    cairo_set_source_rgba (ct, 0.7, 0.7, 0.7, 1);
    cairo_rectangle (ct, (alloc.width - draw_width) / 2, (alloc.height - draw_height) / 2, draw_width, draw_height);
    cairo_fill (ct);
    cairo_set_source_rgba (ct, 0, 0, 0, 1);
    cairo_rectangle (ct, (alloc.width - draw_width) / 2, (alloc.height - draw_height) / 2, draw_width, draw_height);
    cairo_stroke (ct);
    cairo_restore (ct);
  }

  {
    /*cairo_save (ct);*/
    /*cairo_set_source_rgba (ct, 1, 0, 0, 1);*/
    /*cairo_rectangle (ct, ((alloc.width - image_width) / 2)   / scale,*/
                         /*((alloc.height - image_height) / 2) / scale,*/
                         /*cairo_image_surface_get_width (priv->image_surface),*/
                         /*cairo_image_surface_get_height (priv->image_surface));*/
    /*cairo_stroke (ct);*/
    /*cairo_restore (ct);*/
  }
#endif




  cairo_save (ct);
  cairo_rectangle (ct, draw_x, draw_y, draw_width, draw_height);

  /* Handle the h/vadjustments, but keep the image centered in all cases */

  /*if (priv->hadjustment &&*/
      /*gtk_adjustment_get_page_size (priv->hadjustment) < draw_width)*/
    /*draw_x = -gtk_adjustment_get_value (priv->hadjustment);*/
  /*else*/
    draw_x = (alloc.width - image_width) / 2;


  /*if (priv->vadjustment &&*/
      /*gtk_widget_get_allocated_height (widget) < draw_height)*/
    /*draw_y = -gtk_adjustment_get_value (priv->vadjustment);*/
  /*else*/
    draw_y = (alloc.height - image_height) / 2;

    if (priv->vadjustment)
      draw_y -= gtk_adjustment_get_value (priv->vadjustment);



  /* Rotate around the center */
  cairo_translate (ct, draw_x + (image_width / 2.0), draw_y + (image_height / 2.0));
  cairo_rotate (ct, DEG_TO_RAD (priv->angle));
  cairo_translate (ct, -draw_x - (image_width / 2.0), - draw_y - (image_height / 2.0));


  cairo_scale (ct, scale, scale);
  cairo_set_source_surface (ct, priv->image_surface, draw_x / scale, draw_y / scale);
  cairo_pattern_set_filter (cairo_get_source (ct), CAIRO_FILTER_FAST);
  cairo_fill (ct);
  cairo_restore (ct);

  return GDK_EVENT_PROPAGATE;
}

/* Property Getter/Setter {{{ */
static void
gtk_image_view_set_hadjustment (GtkImageView  *image_view,
                                GtkAdjustment *hadjustment)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->hadjustment && priv->hadjustment == hadjustment)
    return;

  if (priv->hadjustment)
    {
      g_signal_handlers_disconnect_by_func (priv->hadjustment, adjustment_value_changed_cb, image_view);
      g_object_unref (priv->hadjustment);
    }


  if (hadjustment)
    {
      g_signal_connect ((GObject *)hadjustment, "value-changed",
                        (GCallback) adjustment_value_changed_cb, image_view);
      priv->hadjustment = g_object_ref_sink (hadjustment);
    }
  else
    {
      priv->hadjustment = hadjustment;
    }

}

static void
gtk_image_view_set_vadjustment (GtkImageView  *image_view,
                                GtkAdjustment *vadjustment)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->vadjustment == vadjustment)
    return;

  if (priv->vadjustment)
    {
      g_signal_handlers_disconnect_by_func (priv->vadjustment, adjustment_value_changed_cb, image_view);
      g_object_unref (priv->vadjustment);
    }

  if (vadjustment)
    {
      g_signal_connect ((GObject *)vadjustment, "value-changed",
                        (GCallback) adjustment_value_changed_cb, image_view);
      priv->vadjustment = g_object_ref_sink (vadjustment);
    }
  else
    {
      priv->vadjustment = vadjustment;
    }
}



/**
 * gtk_image_view_set_scale:
 * @image_view: A #GtkImageView
 * @scale: The new scale value
 *
 * Sets the value of the #scale property. This will cause the
 * #scale-set property to be set to #TRUE as well.
 *
 * If #fit-allocation is #TRUE, it will be set to #FALSE, and @image_view
 * will be resized to the image's current size, taking the new scale into
 * account.
 */
void
gtk_image_view_set_scale (GtkImageView *image_view,
                          double        scale)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  priv->scale = scale;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_SCALE]);
  priv->scale_set = TRUE;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_SCALE_SET]);

  if (priv->fit_allocation)
    {
      priv->fit_allocation = FALSE;
      g_object_notify_by_pspec ((GObject *)image_view,
                                widget_props[PROP_FIT_ALLOCATION]);
    }

  gtk_image_view_update_adjustments (image_view);

  gtk_widget_queue_resize ((GtkWidget *)image_view);
}

double
gtk_image_view_get_scale (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->scale;
}



void
gtk_image_view_set_angle (GtkImageView *image_view,
                          double        angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  if (priv->snap_angle)
    gtk_image_view_do_snapping (image_view, angle);
  else
    priv->angle = angle;

  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_ANGLE]);


  gtk_image_view_update_adjustments (image_view);

  if (priv->fit_allocation)
    gtk_widget_queue_draw ((GtkWidget *)image_view);
  else
    gtk_widget_queue_resize ((GtkWidget *)image_view);
}

double
gtk_image_view_get_angle (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->angle;
}



/**
 * gtk_image_view_set_snap_angle:
 * @image_view: A #GtkImageView
 * @snap_angle: The new value of the #snap-angle property
 *
 * Setting #snap-angle to #TRUE will cause @image_view's  angle to
 * be snapped to 90° steps. Setting the #angle property will cause it to
 * be set to the lower 90° step, e.g. setting #angle to 359 will cause
 * the new value to be 270.
 */
void
gtk_image_view_set_snap_angle (GtkImageView *image_view,
                               gboolean     snap_angle)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  snap_angle = !!snap_angle;

  if (snap_angle == priv->snap_angle)
    return;

  priv->snap_angle = snap_angle;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_SNAP_ANGLE]);

  if (priv->snap_angle)
    gtk_image_view_do_snapping (image_view, priv->angle);
}

gboolean
gtk_image_view_get_snap_angle (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->snap_angle;
}



/**
 * gtk_image_view_set_fit_allocation:
 * @image_view: A #GtkImageView
 * @fit_allocation: The new value of the #fit-allocation property.
 *
 * Setting #fit-allocation to #TRUE will cause the image to be scaled
 * to the widget's allocation, unless it would cause the image to be
 * scaled up.
 *
 * Setting #fit-allocation will have the side effect of setting
 * #scale-set set to #FALSE, thus giving the #GtkImageView the control
 * over the image's scale. Additionally, if the new #fit-allocation
 * value is #FALSE, the scale will be reset to 1.0 and the #GtkImageView
 * will be resized to take at least the image's real size.
 */
void
gtk_image_view_set_fit_allocation (GtkImageView *image_view,
                                   gboolean      fit_allocation)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  fit_allocation = !!fit_allocation;

  if (fit_allocation == priv->fit_allocation)
    return;

  priv->fit_allocation = fit_allocation;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_FIT_ALLOCATION]);

  priv->scale_set = FALSE;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_SCALE_SET]);

  if (!priv->fit_allocation && !priv->scale_set)
    {
      priv->scale = 1.0;
      g_object_notify_by_pspec ((GObject *)image_view,
                                widget_props[PROP_SCALE]);
    }

  gtk_image_view_update_adjustments (image_view);

  gtk_widget_queue_resize ((GtkWidget *)image_view);
}

gboolean
gtk_image_view_get_fit_allocation (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->fit_allocation;
}



void
gtk_image_view_set_rotate_gesture_enabled (GtkImageView *image_view,
                                           gboolean      rotate_gesture_enabled)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  rotate_gesture_enabled = !!rotate_gesture_enabled;

  priv->rotate_gesture_enabled = rotate_gesture_enabled;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_ROTATE_GESTURE_ENABLED]);
}

gboolean
gtk_image_view_get_rotate_gesture_enabled (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->rotate_gesture_enabled;
}



void
gtk_image_view_set_zoom_gesture_enabled (GtkImageView *image_view,
                                         gboolean      zoom_gesture_enabled)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  zoom_gesture_enabled = !!zoom_gesture_enabled;

  priv->zoom_gesture_enabled = zoom_gesture_enabled;
  g_object_notify_by_pspec ((GObject *)image_view,
                            widget_props[PROP_ZOOM_GESTURE_ENABLED]);
}

gboolean
gtk_image_view_get_zoom_gesture_enabled (GtkImageView *image_view)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));

  return priv->zoom_gesture_enabled;
}
/* }}} */


/* GtkWidget API {{{ */

static void
gtk_image_view_realize (GtkWidget *widget)
{
  GtkAllocation allocation;
  GdkWindowAttr attributes = { 0, };
  GdkWindow *window;

  gtk_widget_get_allocation (widget, &allocation);
  gtk_widget_set_realized (widget, TRUE);

  attributes.x = allocation.x;
  attributes.y = allocation.y;
  attributes.width  = allocation.width;
  attributes.height = allocation.height;
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.event_mask = gtk_widget_get_events (widget) |
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_SMOOTH_SCROLL_MASK |
                          GDK_SCROLL_MASK;
  attributes.wclass = GDK_INPUT_OUTPUT;

  window = gdk_window_new (gtk_widget_get_parent_window (widget),
                           &attributes, GDK_WA_X | GDK_WA_Y);
  gdk_window_set_user_data (window, (GObject *) widget);
  gtk_widget_set_window (widget, window); /* Passes ownership */
}

static void
gtk_image_view_size_allocate (GtkWidget     *widget,
                              GtkAllocation *allocation)
{
  GtkImageView *image_view = (GtkImageView *)widget;

  gtk_widget_set_allocation (widget, allocation);

  if (gtk_widget_get_realized (widget))
    {
      gdk_window_move_resize (gtk_widget_get_window (widget),
                              allocation->x, allocation->y,
                              allocation->width, allocation->height);
    }

  gtk_image_view_update_adjustments (image_view);
}

static void
gtk_image_view_map (GtkWidget *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private ((GtkImageView *)widget);

  if (priv->is_animation)
    gtk_image_view_start_animation ((GtkImageView *)widget);

  GTK_WIDGET_CLASS (gtk_image_view_parent_class)->map (widget);
}

static void
gtk_image_view_unmap (GtkWidget *widget)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private ((GtkImageView *)widget);


  if (priv->is_animation)
    gtk_image_view_stop_animation ((GtkImageView *)widget);

  GTK_WIDGET_CLASS (gtk_image_view_parent_class)->unmap (widget);
}

static void
adjustment_value_changed_cb (GtkAdjustment *adjustment,
                             gpointer       user_data)
{
  GtkImageView *image_view = user_data;

  gtk_widget_queue_draw ((GtkWidget *)image_view);
}

static void
gtk_image_view_get_preferred_height (GtkWidget *widget,
                                     int       *minimal,
                                     int       *natural)
{
  GtkImageView *image_view  = (GtkImageView *)widget;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  int width, height;
  gtk_image_view_compute_bounding_box (image_view,
                                       &width,
                                       &height,
                                       NULL);


  if (priv->fit_allocation)
    {
      *minimal = 0;
      *natural = height;
    }
  else
    {
      *minimal = height;
      *natural = height;
    }
}

static void
gtk_image_view_get_preferred_width (GtkWidget *widget,
                                    int       *minimal,
                                    int       *natural)
{
  GtkImageView *image_view  = (GtkImageView *)widget;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int width, height;
  gtk_image_view_compute_bounding_box (image_view,
                                       &width,
                                       &height,
                                       NULL);

  if (priv->fit_allocation)
    {
      *minimal = 0;
      *natural = width;
    }
  else
    {
      *minimal = width;
      *natural = width;
    }

}
/* }}} */


/* GObject API {{{ */
static void
gtk_image_view_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)

{
  GtkImageView *image_view = (GtkImageView *) object;

  switch (prop_id)
    {
      case PROP_SCALE:
        gtk_image_view_set_scale (image_view, g_value_get_double (value));
        break;
      case PROP_ANGLE:
        gtk_image_view_set_angle (image_view, g_value_get_double (value));
        break;
      case PROP_SNAP_ANGLE:
        gtk_image_view_set_snap_angle (image_view, g_value_get_boolean (value));
        break;
      case PROP_FIT_ALLOCATION:
        gtk_image_view_set_fit_allocation (image_view, g_value_get_boolean (value));
        break;
      case PROP_HADJUSTMENT:
        gtk_image_view_set_hadjustment (image_view, g_value_get_object (value));
        break;
       case PROP_VADJUSTMENT:
        gtk_image_view_set_vadjustment (image_view, g_value_get_object (value));
        break;
      case PROP_HSCROLL_POLICY:
        ;
        break;
      case PROP_VSCROLL_POLICY:
        ;
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}


static void
gtk_image_view_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  GtkImageView *image_view  = (GtkImageView *)object;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  switch (prop_id)
    {
      case PROP_SCALE:
        g_value_set_double (value, priv->scale);
        break;
      case PROP_ANGLE:
        g_value_set_double (value, priv->angle);
        break;
      case PROP_SNAP_ANGLE:
        g_value_set_boolean (value, priv->snap_angle);
        break;
      case PROP_FIT_ALLOCATION:
        g_value_set_boolean (value, priv->fit_allocation);
        break;
      case PROP_HADJUSTMENT:
        g_value_set_object (value, priv->hadjustment);
        break;
      case PROP_VADJUSTMENT:
        g_value_set_object (value, priv->vadjustment);
        break;
      case PROP_HSCROLL_POLICY:
        ;
        break;
      case PROP_VSCROLL_POLICY:
        ;
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtk_image_view_finalize (GObject *object)
{
  GtkImageView *image_view  = (GtkImageView *)object;
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  gtk_image_view_stop_animation (image_view);

  g_clear_object (&priv->rotate_gesture);
  g_clear_object (&priv->zoom_gesture);

  g_clear_object (&priv->hadjustment);
  g_clear_object (&priv->vadjustment);

  if (priv->image_surface)
    cairo_surface_destroy (priv->image_surface);


  G_OBJECT_CLASS (gtk_image_view_parent_class)->finalize (object);
}

/* }}} GObject API */

static void
gtk_image_view_class_init (GtkImageViewClass *view_class)
{
  GObjectClass   *object_class = (GObjectClass *)view_class;
  GtkWidgetClass *widget_class = (GtkWidgetClass *)view_class;

  object_class->set_property = gtk_image_view_set_property;
  object_class->get_property = gtk_image_view_get_property;
  object_class->finalize     = gtk_image_view_finalize;

  widget_class->draw          = gtk_image_view_draw;
  widget_class->realize       = gtk_image_view_realize;
  widget_class->size_allocate = gtk_image_view_size_allocate;
  widget_class->map           = gtk_image_view_map;
  widget_class->unmap         = gtk_image_view_unmap;
  widget_class->get_preferred_width  = gtk_image_view_get_preferred_width;
  widget_class->get_preferred_height = gtk_image_view_get_preferred_height;

  widget_props[PROP_SCALE] = g_param_spec_double ("scale",
                                                  P_("Scale"),
                                                  P_("foobar scale"),
                                                  -G_MAXDOUBLE,
                                                  G_MAXDOUBLE,
                                                  0.0,
                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);

  widget_props[PROP_SCALE_SET] = g_param_spec_boolean ("scale-set",
                                                       P_("Foo"),
                                                       P_("fooar"),
                                                       TRUE,
                                                       GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);

  widget_props[PROP_ANGLE] = g_param_spec_double ("angle",
                                                  P_("angle"),
                                                  P_("angle"),
                                                  -G_MAXDOUBLE,
                                                  G_MAXDOUBLE,
                                                  0.0,
                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);

  widget_props[PROP_ROTATE_GESTURE_ENABLED] = g_param_spec_boolean ("rotate-gesture-enabled",
                                                                    P_("Foo"),
                                                                    P_("fooar"),
                                                                    TRUE,
                                                                    GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);
  widget_props[PROP_ZOOM_GESTURE_ENABLED] = g_param_spec_boolean ("zoom-gesture-enabled",
                                                                  P_("Foo"),
                                                                  P_("fooar"),
                                                                  TRUE,
                                                                  GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);

  widget_props[PROP_SNAP_ANGLE] = g_param_spec_boolean ("snap-angle",
                                                        P_("Foo"),
                                                        P_("fooar"),
                                                        FALSE,
                                                        GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);


  widget_props[PROP_FIT_ALLOCATION] = g_param_spec_boolean ("fit-allocation",
                                                            P_("Foo"),
                                                            P_("fooar"),
                                                            FALSE,
                                                            GTK_PARAM_READWRITE|G_PARAM_EXPLICIT_NOTIFY);




  widget_signals[PREPARE_IMAGE] = g_signal_new (I_("prepare-image"),
                                                G_TYPE_FROM_CLASS (object_class),
                                                G_SIGNAL_RUN_LAST,
                                                G_STRUCT_OFFSET (GtkImageViewClass, prepare_image),
                                                NULL, NULL,
                                                _gtk_marshal_VOID__VOID,
                                                G_TYPE_NONE, 0);

  g_object_class_install_properties (object_class, LAST_WIDGET_PROPERTY, widget_props);

  g_object_class_override_property (object_class, PROP_HADJUSTMENT,    "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT,    "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
}

GtkWidget *
gtk_image_view_new ()
{
  return g_object_new (GTK_TYPE_IMAGE_VIEW, NULL);
}


static void
gtk_image_view_replace_surface (GtkImageView    *image_view,
                                cairo_surface_t *surface,
                                int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);

  if (priv->image_surface)
    cairo_surface_destroy (priv->image_surface);

  priv->scale_factor = scale_factor;
  priv->image_surface = surface;
  cairo_surface_reference (priv->image_surface);
  priv->surface_width = cairo_image_surface_get_width (priv->image_surface);
  priv->surface_height = cairo_image_surface_get_height (priv->image_surface);
}

static void
gtk_image_view_update_surface (GtkImageView    *image_view,
                               const GdkPixbuf *frame,
                               int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  int new_width    = gdk_pixbuf_get_width (frame);
  int new_height   = gdk_pixbuf_get_height (frame);
  int widget_scale = gtk_widget_get_scale_factor ((GtkWidget *)image_view);
  gboolean resize  = TRUE;
  int real_width   = (new_width * scale_factor)  / widget_scale;
  int real_height  = (new_height * scale_factor) / widget_scale;

  if (!priv->image_surface ||
      priv->surface_width  != real_width ||
      priv->surface_height != real_height)
    {
      GdkWindow *window = gtk_widget_get_window ((GtkWidget *)image_view);
      int surface_scale = gtk_widget_get_scale_factor ((GtkWidget *)image_view);
      cairo_surface_t *new_surface = gdk_cairo_surface_create_from_pixbuf (frame,
                                                                           surface_scale,
                                                                           window);

      gtk_image_view_replace_surface (image_view,
                                      new_surface,
                                      surface_scale);
    }
  else
    {
      gdk_cairo_surface_paint_pixbuf (priv->image_surface, frame);
      resize = FALSE;
    }
  g_assert (priv->image_surface != NULL);

  g_signal_emit (image_view, widget_signals[PREPARE_IMAGE], 0, priv->image_surface);

  if (resize)
    gtk_widget_queue_resize ((GtkWidget *)image_view);
  else
    gtk_widget_queue_draw ((GtkWidget *)image_view);
}


static void
gtk_image_view_load_image_from_stream (GtkImageView *image_view,
                                       GInputStream *input_stream,
                                       int           scale_factor,
                                       GCancellable *cancellable,
                                       GError       *error)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  GdkPixbufAnimation *result;
  result = gdk_pixbuf_animation_new_from_stream (G_INPUT_STREAM (input_stream),
                                                 cancellable,
                                                 &error);

  g_object_unref (input_stream);
  if (error)
    {
      // XXX
      g_error ("error!");
    }
  else
    {
      if (priv->source_animation)
        {
          g_assert (priv->image_surface);
          // Cleanup old pixbufanimation, iter, surface, ...
          if (priv->is_animation)
            gtk_image_view_stop_animation (image_view);

        }
      /*g_task_return_pointer (task, result, g_object_unref);*/

      priv->is_animation = !gdk_pixbuf_animation_is_static_image (result);
      if (priv->is_animation)
        {
          priv->source_animation = result;
          priv->source_animation_iter = gdk_pixbuf_animation_get_iter (priv->source_animation,
                                                                       NULL);
          gtk_image_view_update_surface (image_view,
                                         gtk_image_view_get_current_frame (image_view),
                                         scale_factor);

          gtk_image_view_start_animation (image_view);
        }
      else
        {
          gtk_image_view_update_surface (image_view,
                                         gdk_pixbuf_animation_get_static_image (result),
                                         scale_factor);
        }
    }
}

static void
gtk_image_view_load_image_contents (GTask        *task,
                                    gpointer      source_object,
                                    gpointer      task_data,
                                    GCancellable *cancellable)
{
  GtkImageView *image_view = source_object;
  LoadTaskData *data = task_data;
  GFile *file = (GFile *)data->source;
  GError *error = NULL;
  GFileInputStream *in_stream;

  in_stream = g_file_read (file, cancellable, &error);

  if (error)
    {
      g_task_return_error (task, error);
      return;
    }


  gtk_image_view_load_image_from_stream (image_view,
                                         G_INPUT_STREAM (in_stream),
                                         data->scale_factor,
                                         cancellable,
                                         error);

  if (error)
    g_task_return_error (task, error);
}

static void
gtk_image_view_load_from_input_stream (GTask *task,
                                       gpointer source_object,
                                       gpointer task_data,
                                       GCancellable *cancellable)
{
  GtkImageView *image_view = source_object;
  LoadTaskData *data = task_data;
  GInputStream *in_stream = (GInputStream *)data->source;
  GError *error = NULL;

  gtk_image_view_load_image_from_stream (image_view,
                                         in_stream,
                                         data->scale_factor,
                                         cancellable,
                                         error);

  if (error)
    g_task_return_error (task, error);
}



void
gtk_image_view_load_from_file_async (GtkImageView        *image_view,
                                     GFile               *file,
                                     int                  scale_factor,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GTask *task;
  LoadTaskData *task_data;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (scale_factor > 0);

  task_data = g_slice_new (LoadTaskData);
  task_data->scale_factor = scale_factor;
  task_data->source = file;

  task = g_task_new (image_view, cancellable, callback, user_data);
  g_task_set_task_data (task, task_data, (GDestroyNotify)free_load_task_data);
  g_task_run_in_thread (task, gtk_image_view_load_image_contents);

  g_object_unref (task);
}
void
gtk_image_view_load_from_file_finish   (GtkImageView  *image_view,
                                        GAsyncResult  *result,
                                        GError       **error)
{
  g_return_if_fail (g_task_is_valid (result, image_view));
}




void
gtk_image_view_load_from_stream_async (GtkImageView        *image_view,
                                       GInputStream        *input_stream,
                                       int                  scale_factor,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
  GTask *task;
  LoadTaskData *task_data;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (G_IS_INPUT_STREAM (input_stream));
  g_return_if_fail (scale_factor > 0);

  task_data = g_slice_new (LoadTaskData);
  task_data->scale_factor = scale_factor;
  task_data->source = input_stream;

  task = g_task_new (image_view, cancellable, callback, user_data);
  g_task_set_task_data (task, task_data, (GDestroyNotify)free_load_task_data);
  g_task_run_in_thread (task, gtk_image_view_load_from_input_stream);

  g_object_unref (task);
}
void
gtk_image_view_load_from_stream_finish (GtkImageView  *image_view,
                                        GAsyncResult  *result,
                                        GError       **error)
{
  g_return_if_fail (g_task_is_valid (result, image_view));
}



void
gtk_image_view_set_pixbuf (GtkImageView    *image_view,
                           const GdkPixbuf *pixbuf,
                           int              scale_factor)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (GDK_IS_PIXBUF (pixbuf));


  if (priv->is_animation)
    {
      g_clear_object (&priv->source_animation);
      gtk_image_view_stop_animation (image_view);
    }

  gtk_image_view_update_surface (image_view, pixbuf, scale_factor);
}

void
gtk_image_view_set_surface (GtkImageView    *image_view,
                            cairo_surface_t *surface)
{
  GtkImageViewPrivate *priv = gtk_image_view_get_instance_private (image_view);
  double scale_x;
  double scale_y;
  g_return_if_fail (GTK_IS_IMAGE_VIEW (image_view));
  g_return_if_fail (surface != NULL);
  g_return_if_fail (cairo_surface_get_type (surface) == CAIRO_SURFACE_TYPE_IMAGE);


  cairo_surface_get_device_scale (surface, &scale_x, &scale_y);
  g_assert (scale_x == scale_y); /* XXX Legal? */

  gtk_image_view_replace_surface (image_view,
                                  surface,
                                  scale_x);

  if (priv->fit_allocation)
    gtk_widget_queue_draw ((GtkWidget *)image_view);
  else
    gtk_widget_queue_resize ((GtkWidget *)image_view);
}