/*
 * Copyright © 2010 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <netinet/in.h>
#include <unistd.h>

#include "gdk.h"
#include "gdkwayland.h"

#include "gdkwindow.h"
#include "gdkwindowimpl.h"
#include "gdkdisplay-wayland.h"
#include "gdkglcontext-wayland.h"
#include "gdkframeclockprivate.h"
#include "gdkprivate-wayland.h"
#include "gdkinternals.h"
#include "gdkdeviceprivate.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

enum {
  COMMITTED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

#define WINDOW_IS_TOPLEVEL_OR_FOREIGN(window) \
  (GDK_WINDOW_TYPE (window) != GDK_WINDOW_CHILD &&   \
   GDK_WINDOW_TYPE (window) != GDK_WINDOW_OFFSCREEN)

#define WINDOW_IS_TOPLEVEL(window)                   \
  (GDK_WINDOW_TYPE (window) != GDK_WINDOW_CHILD &&   \
   GDK_WINDOW_TYPE (window) != GDK_WINDOW_FOREIGN && \
   GDK_WINDOW_TYPE (window) != GDK_WINDOW_OFFSCREEN)

typedef struct _GdkWaylandWindow GdkWaylandWindow;
typedef struct _GdkWaylandWindowClass GdkWaylandWindowClass;

struct _GdkWaylandWindow
{
  GdkWindow parent;
};

struct _GdkWaylandWindowClass
{
  GdkWindowClass parent_class;
};

G_DEFINE_TYPE (GdkWaylandWindow, gdk_wayland_window, GDK_TYPE_WINDOW)

static void
gdk_wayland_window_class_init (GdkWaylandWindowClass *wayland_window_class)
{
}

static void
gdk_wayland_window_init (GdkWaylandWindow *wayland_window)
{
}

#define GDK_TYPE_WINDOW_IMPL_WAYLAND              (_gdk_window_impl_wayland_get_type ())
#define GDK_WINDOW_IMPL_WAYLAND(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), GDK_TYPE_WINDOW_IMPL_WAYLAND, GdkWindowImplWayland))
#define GDK_WINDOW_IMPL_WAYLAND_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GDK_TYPE_WINDOW_IMPL_WAYLAND, GdkWindowImplWaylandClass))
#define GDK_IS_WINDOW_IMPL_WAYLAND(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_WINDOW_IMPL_WAYLAND))
#define GDK_IS_WINDOW_IMPL_WAYLAND_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GDK_TYPE_WINDOW_IMPL_WAYLAND))
#define GDK_WINDOW_IMPL_WAYLAND_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GDK_TYPE_WINDOW_IMPL_WAYLAND, GdkWindowImplWaylandClass))

typedef struct _GdkWindowImplWayland GdkWindowImplWayland;
typedef struct _GdkWindowImplWaylandClass GdkWindowImplWaylandClass;

struct _GdkWindowImplWayland
{
  GdkWindowImpl parent_instance;

  GdkWindow *wrapper;

  struct {
    /* The wl_outputs that this window currently touches */
    GSList               *outputs;

    struct wl_surface    *wl_surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_popup     *xdg_popup;
    struct gtk_surface1  *gtk_surface;
    struct wl_subsurface *wl_subsurface;
    struct wl_egl_window *egl_window;
    struct wl_egl_window *dummy_egl_window;
  } display_server;

  EGLSurface egl_surface;
  EGLSurface dummy_egl_surface;

  unsigned int mapped : 1;
  unsigned int use_custom_surface : 1;
  unsigned int pending_buffer_attached : 1;
  unsigned int pending_commit : 1;
  unsigned int awaiting_frame : 1;
  unsigned int position_set : 1;
  GdkWindowTypeHint hint;
  GdkWindow *transient_for;

  cairo_surface_t *staging_cairo_surface;
  cairo_surface_t *committed_cairo_surface;
  cairo_surface_t *backfill_cairo_surface;

  int pending_buffer_offset_x;
  int pending_buffer_offset_y;

  gchar *title;

  struct {
    gboolean was_set;

    gchar *application_id;
    gchar *app_menu_path;
    gchar *menubar_path;
    gchar *window_object_path;
    gchar *application_object_path;
    gchar *unique_bus_name;
  } application;

  GdkGeometry geometry_hints;
  GdkWindowHints geometry_mask;

  GdkSeat *grab_input_seat;

  gint64 pending_frame_counter;
  guint32 scale;

  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;

  int initial_fullscreen_monitor;

  cairo_region_t *opaque_region;
  cairo_region_t *input_region;
  cairo_region_t *staged_updates_region;

  int saved_width;
  int saved_height;
};

struct _GdkWindowImplWaylandClass
{
  GdkWindowImplClass parent_class;
};

static void gdk_wayland_window_configure (GdkWindow *window,
                                          int        width,
                                          int        height,
                                          int        scale);

static void maybe_set_gtk_surface_dbus_properties (GdkWindow *window);
static void maybe_set_gtk_surface_modal (GdkWindow *window);

static void gdk_window_request_transient_parent_commit (GdkWindow *window);

GType _gdk_window_impl_wayland_get_type (void);

G_DEFINE_TYPE (GdkWindowImplWayland, _gdk_window_impl_wayland, GDK_TYPE_WINDOW_IMPL)

static void
_gdk_window_impl_wayland_init (GdkWindowImplWayland *impl)
{
  impl->scale = 1;
  impl->initial_fullscreen_monitor = -1;
  impl->saved_width = -1;
  impl->saved_height = -1;
}

/* Keep a list of orphaned dialogs (i.e. without parent) */
static GList *orphan_dialogs;

static void
_gdk_wayland_screen_add_orphan_dialog (GdkWindow *window)
{
  if (!g_list_find (orphan_dialogs, window))
    orphan_dialogs = g_list_prepend (orphan_dialogs, window);
}

static void
drop_cairo_surfaces (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  g_clear_pointer (&impl->staging_cairo_surface, cairo_surface_destroy);
  g_clear_pointer (&impl->backfill_cairo_surface, cairo_surface_destroy);

  /* We nullify this so if a buffer release comes in later, we won't
   * try to reuse that buffer since it's no longer suitable
   */
  impl->committed_cairo_surface = NULL;
}

static void
_gdk_wayland_window_save_size (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (window->state & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED))
    return;

  impl->saved_width = window->width - impl->margin_left - impl->margin_right;
  impl->saved_height = window->height - impl->margin_top - impl->margin_bottom;
}

static void
_gdk_wayland_window_clear_saved_size (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (window->state & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED))
    return;

  impl->saved_width = -1;
  impl->saved_height = -1;
}

/*
 * gdk_wayland_window_update_size:
 * @drawable: a #GdkDrawableImplWayland.
 *
 * Updates the state of the drawable (in particular the drawable's
 * cairo surface) when its size has changed.
 */
static void
gdk_wayland_window_update_size (GdkWindow *window,
                                int32_t    width,
                                int32_t    height,
                                int        scale)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkRectangle area;
  cairo_region_t *region;

  if ((window->width == width) &&
      (window->height == height) &&
      (impl->scale == scale))
    return;

  drop_cairo_surfaces (window);

  window->width = width;
  window->height = height;
  impl->scale = scale;

  if (impl->display_server.egl_window)
    {
      wl_egl_window_resize (impl->display_server.egl_window,
                            width * scale,
                            height * scale,
                            0, 0);
      wl_surface_set_buffer_scale (impl->display_server.wl_surface, scale);
    }

  area.x = 0;
  area.y = 0;
  area.width = window->width;
  area.height = window->height;

  region = cairo_region_create_rectangle (&area);
  _gdk_window_invalidate_for_expose (window, region);
  cairo_region_destroy (region);
}

GdkWindow *
_gdk_wayland_screen_create_root_window (GdkScreen *screen,
                                        int        width,
                                        int        height)
{
  GdkWaylandDisplay *wayland_display =
    GDK_WAYLAND_DISPLAY (gdk_screen_get_display (screen));
  GdkWindow *window;
  GdkWindowImplWayland *impl;

  window = _gdk_display_create_window (GDK_DISPLAY (wayland_display));
  window->impl = g_object_new (GDK_TYPE_WINDOW_IMPL_WAYLAND, NULL);
  window->impl_window = window;
  window->visual = gdk_screen_get_system_visual (screen);

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  impl->wrapper = GDK_WINDOW (window);
  if (wayland_display->compositor_version >= WL_SURFACE_HAS_BUFFER_SCALE &&
      gdk_screen_get_n_monitors (screen) > 0)
    impl->scale = gdk_screen_get_monitor_scale_factor (screen, 0);

  /* logical 1x1 fake buffer */
  impl->staging_cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                            impl->scale,
                                                            impl->scale);

  cairo_surface_set_device_scale (impl->staging_cairo_surface, impl->scale, impl->scale);

  window->window_type = GDK_WINDOW_ROOT;
  window->depth = 32;

  window->x = 0;
  window->y = 0;
  window->abs_x = 0;
  window->abs_y = 0;
  window->width = width;
  window->height = height;
  window->viewable = TRUE;

  /* see init_randr_support() in gdkscreen-wayland.c */
  window->event_mask = GDK_STRUCTURE_MASK;

  return window;
}

static const gchar *
get_default_title (void)
{
  const char *title;

  title = g_get_application_name ();
  if (!title)
    title = g_get_prgname ();
  if (!title)
    title = "";

  return title;
}

static void
fill_presentation_time_from_frame_time (GdkFrameTimings *timings,
                                        guint32          frame_time)
{
  /* The timestamp in a wayland frame is a msec time value that in some
   * way reflects the time at which the server started drawing the frame.
   * This is not useful from our perspective.
   *
   * However, for the DRM backend of Weston, on reasonably recent
   * Linux, we know that the time is the
   * clock_gettime (CLOCK_MONOTONIC) value at the vblank, and that
   * backend starts drawing immediately after receiving the vblank
   * notification. If we detect this, and make the assumption that the
   * compositor will finish drawing before the next vblank, we can
   * then determine the presentation time as the frame time we
   * received plus one refresh interval.
   *
   * If a backend is using clock_gettime(CLOCK_MONOTONIC), but not
   * picking values right at the vblank, then the presentation times
   * we compute won't be accurate, but not really worse than then
   * the alternative of not providing presentation times at all.
   *
   * The complexity here is dealing with the fact that we receive
   * only the low 32 bits of the CLOCK_MONOTONIC value in milliseconds.
   */
  gint64 now_monotonic = g_get_monotonic_time ();
  gint64 now_monotonic_msec = now_monotonic / 1000;
  uint32_t now_monotonic_low = (uint32_t)now_monotonic_msec;

  if (frame_time - now_monotonic_low < 1000 ||
      frame_time - now_monotonic_low > (uint32_t)-1000)
    {
      /* Timestamp we received is within one second of the current time.
       */
      gint64 last_frame_time = now_monotonic + (gint64)1000 * (gint32)(frame_time - now_monotonic_low);
      if ((gint32)now_monotonic_low < 0 && (gint32)frame_time > 0)
        last_frame_time += (gint64)1000 * G_GINT64_CONSTANT(0x100000000);
      else if ((gint32)now_monotonic_low > 0 && (gint32)frame_time < 0)
        last_frame_time -= (gint64)1000 * G_GINT64_CONSTANT(0x100000000);

      timings->presentation_time = last_frame_time + timings->refresh_interval;
    }
}

static void
read_back_cairo_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  cairo_t *cr;
  cairo_region_t *paint_region = NULL;

  if (!impl->backfill_cairo_surface)
    goto out;

  paint_region = cairo_region_copy (window->clip_region);
  cairo_region_subtract (paint_region, impl->staged_updates_region);

  if (cairo_region_is_empty (paint_region))
    goto out;

  cr = cairo_create (impl->staging_cairo_surface);
  cairo_set_source_surface (cr, impl->backfill_cairo_surface, 0, 0);
  gdk_cairo_region (cr, paint_region);
  cairo_clip (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_flush (impl->staging_cairo_surface);

out:
  g_clear_pointer (&paint_region, cairo_region_destroy);
  g_clear_pointer (&impl->staged_updates_region, cairo_region_destroy);
  g_clear_pointer (&impl->backfill_cairo_surface, cairo_surface_destroy);
}

static void
frame_callback (void               *data,
                struct wl_callback *callback,
                uint32_t            time)
{
  GdkWindow *window = data;
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkFrameClock *clock = gdk_window_get_frame_clock (window);
  GdkFrameTimings *timings;

  GDK_NOTE (EVENTS,
            g_message ("frame %p", window));

  wl_callback_destroy (callback);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  if (!impl->awaiting_frame)
    return;

  impl->awaiting_frame = FALSE;
  _gdk_frame_clock_thaw (clock);

  timings = gdk_frame_clock_get_timings (clock, impl->pending_frame_counter);
  impl->pending_frame_counter = 0;

  if (timings == NULL)
    return;

  timings->refresh_interval = 16667; /* default to 1/60th of a second */
  if (impl->display_server.outputs)
    {
      /* We pick a random output out of the outputs that the window touches
       * The rate here is in milli-hertz */
      int refresh_rate = _gdk_wayland_screen_get_output_refresh_rate (wayland_display->screen,
                                                                      impl->display_server.outputs->data);
      if (refresh_rate != 0)
        timings->refresh_interval = G_GINT64_CONSTANT(1000000000) / refresh_rate;
    }

  fill_presentation_time_from_frame_time (timings, time);

  timings->complete = TRUE;

#ifdef G_ENABLE_DEBUG
  if ((_gdk_debug_flags & GDK_DEBUG_FRAMES) != 0)
    _gdk_frame_clock_debug_print_timings (clock, timings);
#endif
}

static const struct wl_callback_listener frame_listener = {
  frame_callback
};

static void
on_frame_clock_before_paint (GdkFrameClock *clock,
                             GdkWindow     *window)
{
  GdkFrameTimings *timings = gdk_frame_clock_get_current_timings (clock);
  gint64 presentation_time;
  gint64 refresh_interval;

  gdk_frame_clock_get_refresh_info (clock,
                                    timings->frame_time,
                                    &refresh_interval, &presentation_time);

  if (presentation_time != 0)
    {
      /* Assume the algorithm used by the DRM backend of Weston - it
       * starts drawing at the next vblank after receiving the commit
       * for this frame, and presentation occurs at the vblank
       * after that.
       */
      timings->predicted_presentation_time = presentation_time + refresh_interval;
    }
  else
    {
      /* As above, but we don't actually know the phase of the vblank,
       * so just assume that we're half way through a refresh cycle.
       */
      timings->predicted_presentation_time = timings->frame_time + refresh_interval / 2 + refresh_interval;
    }
}

static void
on_frame_clock_after_paint (GdkFrameClock *clock,
                            GdkWindow     *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  struct wl_callback *callback;

  if (!impl->pending_commit)
    return;

  callback = wl_surface_frame (impl->display_server.wl_surface);
  wl_callback_add_listener (callback, &frame_listener, window);
  _gdk_frame_clock_freeze (clock);

  /* Before we commit a new buffer, make sure we've backfilled
   * undrawn parts from any old committed buffer
   */
  if (impl->pending_buffer_attached)
    read_back_cairo_surface (window);

  /* From this commit forward, we can't write to the buffer,
   * it's "live".  In the future, if we need to stage more changes
   * we have to allocate a new staging buffer and draw to it instead.
   *
   * Our one saving grace is if the compositor releases the buffer
   * before we need to stage any changes, then we can take it back and
   * use it again.
   */
  wl_surface_commit (impl->display_server.wl_surface);

  if (impl->pending_buffer_attached)
    impl->committed_cairo_surface = g_steal_pointer (&impl->staging_cairo_surface);

  impl->pending_buffer_attached = FALSE;
  impl->pending_commit = FALSE;
  impl->pending_frame_counter = gdk_frame_clock_get_frame_counter (clock);
  impl->awaiting_frame = TRUE;

  g_signal_emit (impl, signals[COMMITTED], 0);
}

static void
window_update_scale (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  guint32 scale;
  GSList *l;

  if (wayland_display->compositor_version < WL_SURFACE_HAS_BUFFER_SCALE)
    {
      /* We can't set the scale on this surface */
      return;
    }

  scale = 1;
  for (l = impl->display_server.outputs; l != NULL; l = l->next)
    {
      guint32 output_scale =
        _gdk_wayland_screen_get_output_scale (wayland_display->screen, l->data);
      scale = MAX (scale, output_scale);
    }

  /* Notify app that scale changed */
  gdk_wayland_window_configure (window, window->width, window->height, scale);
}

static void
on_monitors_changed (GdkScreen *screen,
                     GdkWindow *window)
{
  window_update_scale (window);
}


static void gdk_wayland_window_create_surface (GdkWindow *window);

void
_gdk_wayland_display_create_window_impl (GdkDisplay    *display,
                                         GdkWindow     *window,
                                         GdkWindow     *real_parent,
                                         GdkScreen     *screen,
                                         GdkEventMask   event_mask,
                                         GdkWindowAttr *attributes,
                                         gint           attributes_mask)
{
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (display);
  GdkWindowImplWayland *impl;
  GdkFrameClock *frame_clock;
  const char *title;

  impl = g_object_new (GDK_TYPE_WINDOW_IMPL_WAYLAND, NULL);
  window->impl = GDK_WINDOW_IMPL (impl);
  impl->wrapper = GDK_WINDOW (window);

  if (window->width > 65535)
    {
      g_warning ("Native Windows wider than 65535 pixels are not supported");
      window->width = 65535;
    }
  if (window->height > 65535)
    {
      g_warning ("Native Windows taller than 65535 pixels are not supported");
      window->height = 65535;
    }

  g_object_ref (window);

  /* More likely to be right than just assuming 1 */
  if (wayland_display->compositor_version >= WL_SURFACE_HAS_BUFFER_SCALE &&
      gdk_screen_get_n_monitors (screen) > 0)
    impl->scale = gdk_screen_get_monitor_scale_factor (screen, 0);

  impl->title = NULL;

  switch (GDK_WINDOW_TYPE (window))
    {
    case GDK_WINDOW_TOPLEVEL:
    case GDK_WINDOW_TEMP:
      if (attributes_mask & GDK_WA_TITLE)
        title = attributes->title;
      else
        title = get_default_title ();

      gdk_window_set_title (window, title);
      break;

    case GDK_WINDOW_CHILD:
    default:
      break;
    }

  gdk_wayland_window_create_surface (window);

  if (attributes_mask & GDK_WA_TYPE_HINT)
    gdk_window_set_type_hint (window, attributes->type_hint);

  frame_clock = gdk_window_get_frame_clock (window);

  g_signal_connect (frame_clock, "before-paint",
                    G_CALLBACK (on_frame_clock_before_paint), window);
  g_signal_connect (frame_clock, "after-paint",
                    G_CALLBACK (on_frame_clock_after_paint), window);

  g_signal_connect (screen, "monitors-changed",
                    G_CALLBACK (on_monitors_changed), window);
}

static void
gdk_wayland_window_attach_image (GdkWindow *window)
{
  GdkWaylandDisplay *display;
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  g_assert (_gdk_wayland_is_shm_surface (impl->staging_cairo_surface));

  /* Attach this new buffer to the surface */
  wl_surface_attach (impl->display_server.wl_surface,
                     _gdk_wayland_shm_surface_get_wl_buffer (impl->staging_cairo_surface),
                     impl->pending_buffer_offset_x,
                     impl->pending_buffer_offset_y);
  impl->pending_buffer_offset_x = 0;
  impl->pending_buffer_offset_y = 0;

  /* Only set the buffer scale if supported by the compositor */
  display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  if (display->compositor_version >= WL_SURFACE_HAS_BUFFER_SCALE)
    wl_surface_set_buffer_scale (impl->display_server.wl_surface, impl->scale);

  impl->pending_buffer_attached = TRUE;
  impl->pending_commit = TRUE;
}

static const cairo_user_data_key_t gdk_wayland_window_cairo_key;

static void
buffer_release_callback (void             *_data,
                         struct wl_buffer *wl_buffer)
{
  cairo_surface_t *cairo_surface = _data;
  GdkWindowImplWayland *impl = cairo_surface_get_user_data (cairo_surface, &gdk_wayland_window_cairo_key);

  g_return_if_fail (GDK_IS_WINDOW_IMPL_WAYLAND (impl));

  /* The released buffer isn't the latest committed one, we have no further
   * use for it, so clean it up.
   */
  if (impl->committed_cairo_surface != cairo_surface)
    {
      /* If this fails, then the surface buffer got reused before it was
       * released from the compositor
       */
      g_warn_if_fail (impl->staging_cairo_surface != cairo_surface);

      cairo_surface_destroy (cairo_surface);
      return;
    }

  if (impl->staged_updates_region != NULL)
    {
      /* If this fails, then we're tracking staged updates on a staging surface
       * that doesn't exist.
       */
      g_warn_if_fail (impl->staging_cairo_surface != NULL);

      /* If we've staged updates into a new buffer before the release for this
       * buffer came in, then we can't reuse this buffer, so unref it. It may still
       * be alive as a readback buffer though (via impl->backfill_cairo_surface).
       *
       * It's possible a staging surface was allocated but no updates were staged.
       * If that happened, clean up that staging surface now, since the old commit
       * buffer is available again, and reusing the old commit buffer for future
       * updates will save having to do a read back later.
       */
      if (!cairo_region_is_empty (impl->staged_updates_region))
        {
          g_clear_pointer (&impl->committed_cairo_surface, cairo_surface_destroy);
          return;
        }
      else
        {
          g_clear_pointer (&impl->staged_updates_region, cairo_region_destroy);
          g_clear_pointer (&impl->staging_cairo_surface, cairo_surface_destroy);
        }
    }

  /* Release came in, we haven't done any interim updates, so we can just use
   * the old committed buffer again.
   */
  impl->staging_cairo_surface = g_steal_pointer (&impl->committed_cairo_surface);
}

static const struct wl_buffer_listener buffer_listener = {
  buffer_release_callback
};

static void
gdk_wayland_window_ensure_cairo_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  /* If we are drawing using OpenGL then we only need a logical 1x1 surface. */
  if (impl->display_server.egl_window)
    {
      if (impl->staging_cairo_surface &&
          _gdk_wayland_is_shm_surface (impl->staging_cairo_surface))
        cairo_surface_destroy (impl->staging_cairo_surface);

      impl->staging_cairo_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                                impl->scale,
                                                                impl->scale);
      cairo_surface_set_device_scale (impl->staging_cairo_surface,
                                      impl->scale, impl->scale);
    }
  else if (!impl->staging_cairo_surface)
    {
      GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_window_get_display (impl->wrapper));
      struct wl_buffer *buffer;

      impl->staging_cairo_surface = _gdk_wayland_display_create_shm_surface (display_wayland,
                                                                             impl->wrapper->width,
                                                                             impl->wrapper->height,
                                                                             impl->scale);
      cairo_surface_set_user_data (impl->staging_cairo_surface,
                                   &gdk_wayland_window_cairo_key,
                                   g_object_ref (impl),
                                   (cairo_destroy_func_t)
                                   g_object_unref);
      buffer = _gdk_wayland_shm_surface_get_wl_buffer (impl->staging_cairo_surface);
      wl_buffer_add_listener (buffer, &buffer_listener, impl->staging_cairo_surface);
    }
}

/* The cairo surface returned here uses a memory segment that's shared
 * with the display server.  This is not a temporary buffer that gets
 * copied to the display server, but the actual buffer the display server
 * will ultimately end up sending to the GPU. At the time this happens
 * impl->committed_cairo_surface gets set to impl->staging_cairo_surface, and
 * impl->staging_cairo_surface gets nullified.
 */
static cairo_surface_t *
gdk_wayland_window_ref_cairo_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (impl->wrapper))
    return NULL;

  gdk_wayland_window_ensure_cairo_surface (window);

  cairo_surface_reference (impl->staging_cairo_surface);

  return impl->staging_cairo_surface;
}

static cairo_surface_t *
gdk_wayland_window_create_similar_image_surface (GdkWindow *     window,
                                                 cairo_format_t  format,
                                                 int             width,
                                                 int             height)
{
  return cairo_image_surface_create (format, width, height);
}

static gboolean
gdk_window_impl_wayland_begin_paint (GdkWindow *window)
{
  gdk_wayland_window_ensure_cairo_surface (window);

  return FALSE;
}

static void
gdk_window_impl_wayland_end_paint (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  cairo_rectangle_int_t rect;
  int i, n;

  if (!window->current_paint.use_gl &&
      !cairo_region_is_empty (window->current_paint.region))
    {
      gdk_wayland_window_attach_image (window);

      /* If there's a committed buffer pending, then track which
       * updates are staged until the next frame, so we can back
       * fill the unstaged parts of the staging buffer with the
       * last frame.
       */
      if (impl->committed_cairo_surface != NULL)
        {
          if (impl->staged_updates_region == NULL)
            {
              impl->staged_updates_region = cairo_region_copy (window->current_paint.region);
              impl->backfill_cairo_surface = cairo_surface_reference (impl->committed_cairo_surface);
            }
          else
            {
              cairo_region_union (impl->staged_updates_region, window->current_paint.region);
            }
        }

      n = cairo_region_num_rectangles (window->current_paint.region);
      for (i = 0; i < n; i++)
        {
          cairo_region_get_rectangle (window->current_paint.region, i, &rect);
          wl_surface_damage (impl->display_server.wl_surface, rect.x, rect.y, rect.width, rect.height);
        }

      impl->pending_commit = TRUE;
    }
}

static gboolean
gdk_window_impl_wayland_beep (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWaylandDisplay *display_wayland =
    GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));

  if (!display_wayland->gtk_shell)
    return FALSE;

  gtk_shell1_system_bell (display_wayland->gtk_shell,
                          impl->display_server.gtk_surface);

  return TRUE;
}

static void
gdk_window_impl_wayland_finalize (GObject *object)
{
  GdkWindowImplWayland *impl;

  g_return_if_fail (GDK_IS_WINDOW_IMPL_WAYLAND (object));

  impl = GDK_WINDOW_IMPL_WAYLAND (object);

  g_free (impl->title);

  g_free (impl->application.application_id);
  g_free (impl->application.app_menu_path);
  g_free (impl->application.menubar_path);
  g_free (impl->application.window_object_path);
  g_free (impl->application.application_object_path);
  g_free (impl->application.unique_bus_name);

  g_clear_pointer (&impl->opaque_region, cairo_region_destroy);
  g_clear_pointer (&impl->input_region, cairo_region_destroy);
  g_clear_pointer (&impl->staged_updates_region, cairo_region_destroy);

  G_OBJECT_CLASS (_gdk_window_impl_wayland_parent_class)->finalize (object);
}

static void
gdk_wayland_window_configure (GdkWindow *window,
                              int        width,
                              int        height,
                              int        scale)
{
  GdkDisplay *display;
  GdkEvent *event;

  event = gdk_event_new (GDK_CONFIGURE);
  event->configure.window = g_object_ref (window);
  event->configure.send_event = FALSE;
  event->configure.width = width;
  event->configure.height = height;

  gdk_wayland_window_update_size (window, width, height, scale);
  _gdk_window_update_size (window);

  display = gdk_window_get_display (window);
  _gdk_wayland_display_deliver_event (display, event);
}

static void
gdk_wayland_window_sync_parent (GdkWindow *window,
                                GdkWindow *parent)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindowImplWayland *impl_parent = NULL;
  struct xdg_surface *parent_surface;

  if (!impl->display_server.xdg_surface)
    return;

  if (impl->transient_for)
    impl_parent = GDK_WINDOW_IMPL_WAYLAND (impl->transient_for->impl);
  else if (parent)
    impl_parent = GDK_WINDOW_IMPL_WAYLAND (parent->impl);

  if (impl_parent)
    {
      /* XXX: Is this correct? */
      if (!impl_parent->display_server.wl_surface)
        return;

      parent_surface = impl_parent->display_server.xdg_surface;
    }
  else
    parent_surface = NULL;

  xdg_surface_set_parent (impl->display_server.xdg_surface, parent_surface);
}

static void
gdk_wayland_window_update_dialogs (GdkWindow *window)
{
  GList *l;

  if (!orphan_dialogs)
    return;

  for (l = orphan_dialogs; l; l = l->next)
    {
      GdkWindow *w = l->data;
      GdkWindowImplWayland *impl;

      if (!GDK_IS_WINDOW_IMPL_WAYLAND(w->impl))
        continue;

      impl = GDK_WINDOW_IMPL_WAYLAND (w->impl);
      if (w == window)
	continue;
      if (impl->hint != GDK_WINDOW_TYPE_HINT_DIALOG)
        continue;
      if (impl->transient_for)
        continue;

      /* Update the parent relationship only for dialogs without transients */
      gdk_wayland_window_sync_parent (w, window);
    }
}

static void
gdk_wayland_window_sync_title (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.xdg_surface)
    return;

  if (!impl->title)
    return;

  xdg_surface_set_title (impl->display_server.xdg_surface, impl->title);
}

static void
gdk_wayland_window_sync_margin (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  gint x, y, width, height;

  if (!impl->display_server.xdg_surface)
    return;

  x = impl->margin_left;
  y = impl->margin_top;
  width = window->width - (impl->margin_left + impl->margin_right);
  height = window->height - (impl->margin_top + impl->margin_bottom);

  xdg_surface_set_window_geometry (impl->display_server.xdg_surface, x, y, width, height);
}

static struct wl_region *
wl_region_from_cairo_region (GdkWaylandDisplay *display,
                             cairo_region_t    *region)
{
  struct wl_region *wl_region;
  int i, n_rects;

  wl_region = wl_compositor_create_region (display->compositor);
  if (wl_region == NULL)
    return NULL;

  n_rects = cairo_region_num_rectangles (region);
  for (i = 0; i < n_rects; i++)
    {
      cairo_rectangle_int_t rect;
      cairo_region_get_rectangle (region, i, &rect);
      wl_region_add (wl_region, rect.x, rect.y, rect.width, rect.height);
    }

  return wl_region;
}

static void
gdk_wayland_window_sync_opaque_region (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  struct wl_region *wl_region = NULL;

  if (!impl->display_server.wl_surface)
    return;

  if (impl->opaque_region != NULL)
    wl_region = wl_region_from_cairo_region (GDK_WAYLAND_DISPLAY (gdk_window_get_display (window)),
                                             impl->opaque_region);

  wl_surface_set_opaque_region (impl->display_server.wl_surface, wl_region);

  if (wl_region != NULL)
    wl_region_destroy (wl_region);
}

static void
gdk_wayland_window_sync_input_region (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  struct wl_region *wl_region = NULL;

  if (!impl->display_server.wl_surface)
    return;

  if (impl->input_region != NULL)
    wl_region = wl_region_from_cairo_region (GDK_WAYLAND_DISPLAY (gdk_window_get_display (window)),
                                             impl->input_region);

  wl_surface_set_input_region (impl->display_server.wl_surface, wl_region);

  if (wl_region != NULL)
    wl_region_destroy (wl_region);
}

static void
surface_enter (void              *data,
               struct wl_surface *wl_surface,
               struct wl_output  *output)
{
  GdkWindow *window = GDK_WINDOW (data);
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  GDK_NOTE (EVENTS,
            g_message ("surface enter, window %p output %p", window, output));

  impl->display_server.outputs = g_slist_prepend (impl->display_server.outputs, output);

  window_update_scale (window);
}

static void
surface_leave (void              *data,
               struct wl_surface *wl_surface,
               struct wl_output  *output)
{
  GdkWindow *window = GDK_WINDOW (data);
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  GDK_NOTE (EVENTS,
            g_message ("surface leave, window %p output %p", window, output));

  impl->display_server.outputs = g_slist_remove (impl->display_server.outputs, output);

  window_update_scale (window);
}

static const struct wl_surface_listener surface_listener = {
  surface_enter,
  surface_leave
};

static void
on_parent_surface_committed (GdkWindowImplWayland *parent_impl,
                             GdkWindow            *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  g_signal_handlers_disconnect_by_func (parent_impl,
                                        (gpointer) on_parent_surface_committed,
                                        window);

  wl_subsurface_set_desync (impl->display_server.wl_subsurface);
}

static void
gdk_wayland_window_create_subsurface (GdkWindow *window)
{
  GdkWindowImplWayland *impl, *parent_impl = NULL;
  GdkWaylandDisplay *display_wayland;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.wl_surface)
    return; /* Bail out, surface and subsurface will be created later when shown */

  if (impl->display_server.wl_subsurface)
    return;

  if (impl->transient_for)
    parent_impl = GDK_WINDOW_IMPL_WAYLAND (impl->transient_for->impl);

  if (parent_impl)
    {
      display_wayland = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
      impl->display_server.wl_subsurface =
        wl_subcompositor_get_subsurface (display_wayland->subcompositor,
                                         impl->display_server.wl_surface, parent_impl->display_server.wl_surface);
      wl_subsurface_set_position (impl->display_server.wl_subsurface, window->x, window->y);

      /* In order to synchronize the initial position with the initial frame
       * content, wait with making the subsurface desynchronized until after
       * the parent was committed.
       */
      g_signal_connect_object (parent_impl, "committed",
                               G_CALLBACK (on_parent_surface_committed),
                               window, 0);
      gdk_window_request_transient_parent_commit (window);
    }
}

static void
gdk_wayland_window_create_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));

  impl->display_server.wl_surface = wl_compositor_create_surface (display_wayland->compositor);
  wl_surface_add_listener (impl->display_server.wl_surface, &surface_listener, window);

  gdk_wayland_window_sync_opaque_region (window);
  gdk_wayland_window_sync_input_region (window);
}

static void
xdg_surface_configure (void               *data,
                       struct xdg_surface *xdg_surface,
                       int32_t             width,
                       int32_t             height,
                       struct wl_array    *states,
                       uint32_t            serial)
{
  GdkWindow *window = GDK_WINDOW (data);
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindowState new_state = 0;
  gboolean maximized_or_fullscreen;
  uint32_t *p;

  wl_array_for_each (p, states)
    {
      uint32_t state = *p;
      switch (state)
        {
        case XDG_SURFACE_STATE_FULLSCREEN:
          new_state |= GDK_WINDOW_STATE_FULLSCREEN;
          break;
        case XDG_SURFACE_STATE_MAXIMIZED:
          new_state |= GDK_WINDOW_STATE_MAXIMIZED;
          break;
        case XDG_SURFACE_STATE_ACTIVATED:
          new_state |= GDK_WINDOW_STATE_FOCUSED;
          break;
        case XDG_SURFACE_STATE_RESIZING:
          break;
        default:
          /* Unknown state */
          break;
        }
    }

   maximized_or_fullscreen =
       new_state & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN);

    /* According to xdg_shell, an xdg_surface.configure with size 0x0
     * should be interpreted as that it is up to the client to set a
     * size.
     *
     * When transitioning from maximize or fullscreen state, this means
     * the client should configure its size back to what it was before
     * being maximize or fullscreen.
     */
   if (width == 0 && height == 0 && !maximized_or_fullscreen)
    {
      width = impl->saved_width;
      height = impl->saved_height;
    }

   if (width > 0 && height > 0)
    {
      GdkWindowHints geometry_mask = impl->geometry_mask;

      /* Ignore size increments for maximized/fullscreen windows */
      if (maximized_or_fullscreen)
        geometry_mask &= ~GDK_HINT_RESIZE_INC;

      gdk_window_constrain_size (&impl->geometry_hints,
                                 geometry_mask,
                                 width + impl->margin_left + impl->margin_right,
                                 height + impl->margin_top + impl->margin_bottom,
                                 &width,
                                 &height);

      /* Save size for next time we get 0x0 */
      _gdk_wayland_window_save_size (window);

      gdk_wayland_window_configure (window, width, height, impl->scale);
    }

  GDK_NOTE (EVENTS,
            g_message ("configure, window %p %dx%d,%s%s%s",
                       window, width, height,
                       (new_state & GDK_WINDOW_STATE_FULLSCREEN) ? " fullscreen" : "",
                       (new_state & GDK_WINDOW_STATE_MAXIMIZED) ? " maximized" : "",
                       (new_state & GDK_WINDOW_STATE_FOCUSED) ? " focused" : ""));

  _gdk_set_window_state (window, new_state);
  gdk_wayland_window_sync_margin (window);
  xdg_surface_ack_configure (xdg_surface, serial);
  if (impl->hint != GDK_WINDOW_TYPE_HINT_DIALOG &&
      new_state & GDK_WINDOW_STATE_FOCUSED)
    gdk_wayland_window_update_dialogs (window);
}

static void
xdg_surface_close (void               *data,
                   struct xdg_surface *xdg_surface)
{
  GdkWindow *window = GDK_WINDOW (data);
  GdkDisplay *display;
  GdkEvent *event;

  GDK_NOTE (EVENTS,
            g_message ("close %p", window));

  event = gdk_event_new (GDK_DELETE);
  event->any.window = g_object_ref (window);
  event->any.send_event = TRUE;

  display = gdk_window_get_display (window);
  _gdk_wayland_display_deliver_event (display, event);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  xdg_surface_configure,
  xdg_surface_close,
};

static void
gdk_wayland_window_create_xdg_surface (GdkWindow *window)
{
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  const gchar *app_id;
  GdkScreen *screen = gdk_window_get_screen (window);
  struct wl_output *fullscreen_output = NULL;
  if (impl->initial_fullscreen_monitor >= 0 &&
      impl->initial_fullscreen_monitor < gdk_screen_get_n_monitors (screen))
      fullscreen_output = _gdk_wayland_screen_get_wl_output (screen, impl->initial_fullscreen_monitor);

  impl->display_server.xdg_surface = xdg_shell_get_xdg_surface (display_wayland->xdg_shell, impl->display_server.wl_surface);
  xdg_surface_add_listener (impl->display_server.xdg_surface, &xdg_surface_listener, window);

  gdk_wayland_window_sync_parent (window, NULL);
  gdk_wayland_window_sync_title (window);
  gdk_wayland_window_sync_margin (window);

  if (window->state & GDK_WINDOW_STATE_MAXIMIZED)
    xdg_surface_set_maximized (impl->display_server.xdg_surface);
  if (window->state & GDK_WINDOW_STATE_FULLSCREEN)
    xdg_surface_set_fullscreen (impl->display_server.xdg_surface, fullscreen_output);

  app_id = g_get_prgname ();
  if (app_id == NULL)
    app_id = gdk_get_program_class ();

  xdg_surface_set_app_id (impl->display_server.xdg_surface, app_id);

  maybe_set_gtk_surface_dbus_properties (window);
  maybe_set_gtk_surface_modal (window);

  if (impl->hint == GDK_WINDOW_TYPE_HINT_DIALOG)
    _gdk_wayland_screen_add_orphan_dialog (window);
}

static void
xdg_popup_done (void             *data,
                struct xdg_popup *xdg_popup)
{
  GdkWindow *window = GDK_WINDOW (data);

  GDK_NOTE (EVENTS,
            g_message ("done %p", window));

  gdk_window_hide (window);
}

static const struct xdg_popup_listener xdg_popup_listener = {
  xdg_popup_done,
};

/**
 * gdk_wayland_window_get_fake_root_coords:
 * @window: A #GdkWindow
 * @x_out: (out): The X offset of this window
 * @y_out: (out): The Y offset of this window
 *
 * Wayland does not have a global coordinate space shared between
 * surfaces. In fact, for regular toplevels, we have no idea where
 * our surfaces are positioned, relatively.
 *
 * However, there are some cases like popups and subsurfaces where
 * we do have some amount of control over the placement of our
 * window, and we can semi-accurately control the x/y position of
 * these windows, if they are relative to another surface.
 *
 * GTK+ loves to position "relative" popups like menus in root
 * window coordinates, since it was built for display servers that
 * have queryable absolute coordinate spaces. In these cases, GTK+
 * might ask for the root coordinates of a widget window, add a
 * few values, and then call gdk_window_move() with that absolute
 * value.
 *
 * In Wayland, we have to "reverse-engineer" this use, and figure
 * out the root coordinates from the relative position, and the
 * relative position from the root coordinates.
 *
 * We invent a coordinate space called the "fake root coordinate"
 * space in which a toplevel is always at 0,0, and all popups are
 * relative to that space.
 *
 * gdk_wayland_window_get_fake_root_coords() gives you the
 * position of a #GdkWindow in "fake root" coordinates.
 */
static void
gdk_wayland_window_get_fake_root_coords (GdkWindow *window,
                                         gint      *x_out,
                                         gint      *y_out)
{
  gint x_offset = 0, y_offset = 0;

  while (window)
    {
      GdkWindowImplWayland *impl;

      x_offset += window->x;
      y_offset += window->y;

      impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
      window = impl->transient_for;
    }

  *x_out = x_offset;
  *y_out = y_offset;
}

static void
gdk_wayland_window_create_xdg_popup (GdkWindow      *window,
                                     GdkWindow      *parent,
                                     struct wl_seat *seat)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindowImplWayland *parent_impl = GDK_WINDOW_IMPL_WAYLAND (parent->impl);
  GdkSeat *gdk_seat;
  guint32 serial;
  int x, y;
  int parent_x, parent_y;

  if (!impl->display_server.wl_surface)
    return;

  if (!parent_impl->display_server.wl_surface)
    return;

  gdk_seat = gdk_display_get_default_seat (GDK_DISPLAY (display));
  gdk_wayland_window_get_fake_root_coords (parent, &parent_x, &parent_y);

  x = window->x - parent_x;
  y = window->y - parent_y;
  serial = _gdk_wayland_seat_get_last_implicit_grab_serial (gdk_seat, NULL);

  impl->display_server.xdg_popup = xdg_shell_get_xdg_popup (display->xdg_shell,
                                             impl->display_server.wl_surface,
                                             parent_impl->display_server.wl_surface,
                                             seat, serial,
                                             x, y);

  xdg_popup_add_listener (impl->display_server.xdg_popup, &xdg_popup_listener, window);
}

static struct wl_seat *
find_grab_input_seat (GdkWindow *window, GdkWindow *transient_for)
{
  GdkWindow *attached_grab_window;
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindowImplWayland *tmp_impl;

  /* Use the device that was used for the grab as the device for
   * the popup window setup - so this relies on GTK+ taking the
   * grab before showing the popup window.
   */
  if (impl->grab_input_seat)
    return gdk_wayland_seat_get_wl_seat (impl->grab_input_seat);

  /* HACK: GtkMenu grabs a special window known as the "grab transfer window"
   * and then transfers the grab over to the correct window later. Look for
   * this window when taking the grab to know it's correct.
   *
   * See: associate_menu_grab_transfer_window in gtkmenu.c
   */
  attached_grab_window = g_object_get_data (G_OBJECT (window), "gdk-attached-grab-window");
  if (attached_grab_window)
    {
      tmp_impl = GDK_WINDOW_IMPL_WAYLAND (attached_grab_window->impl);
      if (tmp_impl->grab_input_seat)
        return gdk_wayland_seat_get_wl_seat (tmp_impl->grab_input_seat);
    }

  while (transient_for)
    {
      tmp_impl = GDK_WINDOW_IMPL_WAYLAND (transient_for->impl);

      if (tmp_impl->grab_input_seat)
        return gdk_wayland_seat_get_wl_seat (tmp_impl->grab_input_seat);

      transient_for = tmp_impl->transient_for;
    }

  return NULL;
}

static struct wl_seat *
find_default_input_seat (GdkWindow *window)
{
  GdkDisplay *display;
  GdkSeat *seat;

  display = gdk_window_get_display (window);
  seat = gdk_display_get_default_seat (display);

  return gdk_wayland_seat_get_wl_seat (seat);
}

static gboolean
should_be_mapped (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  /* Don't map crazy temp that GTK+ uses for internal X11 shenanigans. */
  if (window->window_type == GDK_WINDOW_TEMP && window->x < 0 && window->y < 0)
    return FALSE;

  if (impl->hint == GDK_WINDOW_TYPE_HINT_DND)
    return FALSE;

  return TRUE;
}

static gboolean
should_map_as_popup (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  /* Ideally, popup would be temp windows with a parent and grab */
  if (GDK_WINDOW_TYPE (window) == GDK_WINDOW_TEMP)
    {
      /* If a temp window has a parent and a grab, we can use a popup */
      if (impl->transient_for)
        {
          if (impl->grab_input_seat)
            return TRUE;
        }
      else
        g_message ("Window %p is a temporary window without parent, "
                   "application will not be able to position it on screen.",
                   window);
    }

  /* Yet we need to keep the window type hint tests for compatibility */
  switch (impl->hint)
    {
    case GDK_WINDOW_TYPE_HINT_POPUP_MENU:
    case GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU:
    case GDK_WINDOW_TYPE_HINT_COMBO:
      return TRUE;

    case GDK_WINDOW_TYPE_HINT_UTILITY:
      if (GDK_WINDOW_TYPE (window) != GDK_WINDOW_TEMP)
        return TRUE;
      break;

    default:
      break;
    }

  return FALSE;
}

static gboolean
should_map_as_subsurface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_TYPE (window) == GDK_WINDOW_SUBSURFACE)
    return TRUE;

  if (GDK_WINDOW_TYPE (window) != GDK_WINDOW_TEMP)
    return FALSE;

  /* if we want a popup, we do not want a subsurface */
  if (should_map_as_popup (window))
    return FALSE;

  if (impl->transient_for)
    {
      GdkWindowImplWayland *impl_parent;

      impl_parent = GDK_WINDOW_IMPL_WAYLAND (impl->transient_for->impl);
      /* subsurface require that the parent is mapped */
      if (impl_parent->mapped)
        return TRUE;
      else
        g_warning ("Couldn't map window %p as susburface because its parent is not mapped.",
                   window);

    }

  return FALSE;
}

/* Get the window that can be used as a parent for a popup, i.e. a xdg_surface
 * or xdg_popup. If the window is not, traverse up the transiency parents until
 * we find one.
 */
static GdkWindow *
get_popup_parent (GdkWindow *window)
{
  while (window)
    {
      GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

      if (impl->display_server.xdg_popup || impl->display_server.xdg_surface)
        return window;

      window = impl->transient_for;
    }

  return NULL;
}

static void
gdk_wayland_window_map (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindow *transient_for = NULL;

  if (!should_be_mapped (window))
    return;

  if (impl->mapped || impl->use_custom_surface)
    return;

  if (should_map_as_subsurface (window))
    {
      if (impl->transient_for)
        gdk_wayland_window_create_subsurface (window);
      else
        g_warning ("Couldn't map window %p as susburface yet because it doesn't have a parent",
                   window);
    }
  else if (should_map_as_popup (window))
    {
      gboolean create_fallback = FALSE;
      struct wl_seat *grab_input_seat;

      /* Popup menus can appear without a transient parent, which means they
       * cannot be positioned properly on Wayland. This attempts to guess the
       * surface they should be positioned with by finding the surface beneath
       * the device that created the grab for the popup window.
       */
      if (!impl->transient_for && impl->hint == GDK_WINDOW_TYPE_HINT_POPUP_MENU)
        {
          GdkDevice *grab_device = NULL;

          /* The popup menu window is not the grabbed window. This may mean
           * that a "transfer window" (see gtkmenu.c) is used, and we need
           * to find that window to get the grab device. If so is the case
           * the "transfer window" can be retrieved via the
           * "gdk-attached-grab-window" associated data field.
           */
          if (!impl->grab_input_seat)
            {
              GdkWindow *attached_grab_window =
                g_object_get_data (G_OBJECT (window),
                                   "gdk-attached-grab-window");
              if (attached_grab_window)
                {
                  GdkWindowImplWayland *attached_impl =
                    GDK_WINDOW_IMPL_WAYLAND (attached_grab_window->impl);
                  grab_device = gdk_seat_get_pointer (attached_impl->grab_input_seat);
                  transient_for =
                    gdk_device_get_window_at_position (grab_device,
                                                       NULL, NULL);
                }
            }
          else
            {
              grab_device = gdk_seat_get_pointer (impl->grab_input_seat);
              transient_for =
                gdk_device_get_window_at_position (grab_device, NULL, NULL);
            }

          if (transient_for)
            transient_for = get_popup_parent (gdk_window_get_effective_toplevel (transient_for));

          /* If the position was not explicitly set, start the popup at the
           * position of the device that holds the grab.
           */
          if (!impl->position_set && grab_device)
            gdk_window_get_device_position (transient_for, grab_device,
                                            &window->x, &window->y, NULL);
        }
      else
        transient_for = get_popup_parent (impl->transient_for);

      if (!transient_for)
        {
          g_warning ("Couldn't map as window %p as popup because it doesn't have a parent",
                     window);

          create_fallback = TRUE;
        }
      else
        {
          grab_input_seat = find_grab_input_seat (window, transient_for);

          if (!grab_input_seat)
            {
              g_warning ("No grabbed seat found, using the default one in "
                         "order to map popup window %p. You may find oddities "
                         "ahead, gdk_seat_grab() should be used to "
                         "simultaneously grab input and show this popup",
                         window);
              grab_input_seat = find_default_input_seat (window);
            }
        }

      if (!create_fallback)
        {
          gdk_wayland_window_create_xdg_popup (window,
                                               transient_for,
                                               grab_input_seat);
        }
      else
        {
          gdk_wayland_window_create_xdg_surface (window);
        }
    }
  else
    {
      gdk_wayland_window_create_xdg_surface (window);
    }

  impl->mapped = TRUE;
}

static void
gdk_wayland_window_show (GdkWindow *window,
                         gboolean   already_mapped)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.wl_surface)
    gdk_wayland_window_create_surface (window);

  gdk_wayland_window_map (window);

  _gdk_make_event (window, GDK_MAP, NULL, FALSE);

  if (impl->staging_cairo_surface &&
      _gdk_wayland_is_shm_surface (impl->staging_cairo_surface))
    gdk_wayland_window_attach_image (window);
}

static void
unmap_subsurface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindowImplWayland *parent_impl;

  g_return_if_fail (impl->display_server.wl_subsurface);
  g_return_if_fail (impl->transient_for);

  parent_impl = GDK_WINDOW_IMPL_WAYLAND (impl->transient_for->impl);
  wl_subsurface_destroy (impl->display_server.wl_subsurface);
  g_signal_handlers_disconnect_by_func (parent_impl,
                                        (gpointer) on_parent_surface_committed,
                                        window);
  impl->display_server.wl_subsurface = NULL;
}

static void
gdk_wayland_window_hide_surface (GdkWindow *window)
{
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (impl->display_server.wl_surface)
    {
      if (impl->dummy_egl_surface)
        {
          eglDestroySurface (display_wayland->egl_display, impl->dummy_egl_surface);
          impl->dummy_egl_surface = NULL;
        }

      if (impl->display_server.dummy_egl_window)
        {
          wl_egl_window_destroy (impl->display_server.dummy_egl_window);
          impl->display_server.dummy_egl_window = NULL;
        }

      if (impl->egl_surface)
        {
          eglDestroySurface (display_wayland->egl_display, impl->egl_surface);
          impl->egl_surface = NULL;
        }

      if (impl->display_server.egl_window)
        {
          wl_egl_window_destroy (impl->display_server.egl_window);
          impl->display_server.egl_window = NULL;
        }

      if (impl->display_server.xdg_surface)
        {
          xdg_surface_destroy (impl->display_server.xdg_surface);
          impl->display_server.xdg_surface = NULL;
        }
      else if (impl->display_server.xdg_popup)
        {
          xdg_popup_destroy (impl->display_server.xdg_popup);
          impl->display_server.xdg_popup = NULL;
        }

      if (impl->display_server.wl_subsurface)
        unmap_subsurface (window);

      if (impl->awaiting_frame)
        {
          GdkFrameClock *frame_clock;

          impl->awaiting_frame = FALSE;
          frame_clock = gdk_window_get_frame_clock (window);
          if (frame_clock)
            _gdk_frame_clock_thaw (frame_clock);
        }

      if (impl->display_server.gtk_surface)
        {
          gtk_surface1_destroy (impl->display_server.gtk_surface);
          impl->display_server.gtk_surface = NULL;
        }

      wl_surface_destroy (impl->display_server.wl_surface);
      impl->display_server.wl_surface = NULL;

      g_slist_free (impl->display_server.outputs);
      impl->display_server.outputs = NULL;

      if (impl->hint == GDK_WINDOW_TYPE_HINT_DIALOG && !impl->transient_for)
        orphan_dialogs = g_list_remove (orphan_dialogs, window);
    }

  _gdk_wayland_window_clear_saved_size (window);
  impl->pending_commit = FALSE;
  impl->mapped = FALSE;
}

static void
gdk_wayland_window_hide (GdkWindow *window)
{
  gdk_wayland_window_hide_surface (window);
  _gdk_window_clear_update_area (window);
}

static void
gdk_window_wayland_withdraw (GdkWindow *window)
{
  if (!window->destroyed)
    {
      if (GDK_WINDOW_IS_MAPPED (window))
        gdk_synthesize_window_state (window, 0, GDK_WINDOW_STATE_WITHDRAWN);

      g_assert (!GDK_WINDOW_IS_MAPPED (window));

      gdk_wayland_window_hide_surface (window);
    }
}

static void
gdk_window_wayland_set_events (GdkWindow    *window,
                               GdkEventMask  event_mask)
{
  GDK_WINDOW (window)->event_mask = event_mask;
}

static GdkEventMask
gdk_window_wayland_get_events (GdkWindow *window)
{
  if (GDK_WINDOW_DESTROYED (window))
    return 0;
  else
    return GDK_WINDOW (window)->event_mask;
}

static void
gdk_window_wayland_raise (GdkWindow *window)
{
}

static void
gdk_window_wayland_lower (GdkWindow *window)
{
}

static void
gdk_window_wayland_restack_under (GdkWindow *window,
                                  GList     *native_siblings)
{
}

static void
gdk_window_wayland_restack_toplevel (GdkWindow *window,
                                     GdkWindow *sibling,
                                     gboolean   above)
{
}

static void
gdk_window_request_transient_parent_commit (GdkWindow *window)
{
  GdkWindowImplWayland *window_impl, *impl;
  GdkFrameClock *frame_clock;

  window_impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!window_impl->transient_for)
    return;

  impl = GDK_WINDOW_IMPL_WAYLAND (window_impl->transient_for->impl);

  if (!impl->display_server.wl_surface || impl->pending_commit)
    return;

  frame_clock = gdk_window_get_frame_clock (window_impl->transient_for);

  if (!frame_clock)
    return;

  impl->pending_commit = TRUE;
  gdk_frame_clock_request_phase (frame_clock,
                                 GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);
}

static void
gdk_window_wayland_move_resize (GdkWindow *window,
                                gboolean   with_move,
                                gint       x,
                                gint       y,
                                gint       width,
                                gint       height)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (with_move)
    {
      /* Each toplevel has in its own "root" coordinate system */
      if (GDK_WINDOW_TYPE (window) != GDK_WINDOW_TOPLEVEL)
        {
          window->x = x;
          window->y = y;
          impl->position_set = 1;

          if (impl->display_server.wl_subsurface)
            {
              wl_subsurface_set_position (impl->display_server.wl_subsurface, x, y);
              gdk_window_request_transient_parent_commit (window);
            }
        }
    }

  /* If this function is called with width and height = -1 then that means
   * just move the window - don't update its size
   */
  if (width > 0 && height > 0)
    gdk_wayland_window_configure (window, width, height, impl->scale);
}

static void
gdk_window_wayland_set_background (GdkWindow       *window,
                                   cairo_pattern_t *pattern)
{
}

static gboolean
gdk_window_wayland_reparent (GdkWindow *window,
                             GdkWindow *new_parent,
                             gint       x,
                             gint       y)
{
  return FALSE;
}

static void
gdk_window_wayland_set_device_cursor (GdkWindow *window,
                                      GdkDevice *device,
                                      GdkCursor *cursor)
{
  g_return_if_fail (GDK_IS_WINDOW (window));
  g_return_if_fail (GDK_IS_DEVICE (device));

  if (!GDK_WINDOW_DESTROYED (window))
    GDK_DEVICE_GET_CLASS (device)->set_window_cursor (device, window, cursor);
}

static void
gdk_window_wayland_get_geometry (GdkWindow *window,
                                 gint      *x,
                                 gint      *y,
                                 gint      *width,
                                 gint      *height)
{
  if (!GDK_WINDOW_DESTROYED (window))
    {
      if (x)
        *x = window->x;
      if (y)
        *y = window->y;
      if (width)
        *width = window->width;
      if (height)
        *height = window->height;
    }
}

static void
gdk_window_wayland_get_root_coords (GdkWindow *window,
                                    gint       x,
                                    gint       y,
                                    gint      *root_x,
                                    gint      *root_y)
{
  gint x_offset, y_offset;

  gdk_wayland_window_get_fake_root_coords (window, &x_offset, &y_offset);

  if (root_x)
    *root_x = x_offset + x;

  if (root_y)
    *root_y = y_offset + y;
}

static gboolean
gdk_window_wayland_get_device_state (GdkWindow       *window,
                                     GdkDevice       *device,
                                     gdouble         *x,
                                     gdouble         *y,
                                     GdkModifierType *mask)
{
  gboolean return_val;

  g_return_val_if_fail (window == NULL || GDK_IS_WINDOW (window), FALSE);

  return_val = TRUE;

  if (!GDK_WINDOW_DESTROYED (window))
    {
      GdkWindow *child;

      GDK_DEVICE_GET_CLASS (device)->query_state (device, window,
                                                  NULL, &child,
                                                  NULL, NULL,
                                                  x, y, mask);
      return_val = (child != NULL);
    }

  return return_val;
}

static void
gdk_window_wayland_shape_combine_region (GdkWindow            *window,
                                         const cairo_region_t *shape_region,
                                         gint                  offset_x,
                                         gint                  offset_y)
{
}

static void
gdk_window_wayland_input_shape_combine_region (GdkWindow            *window,
                                               const cairo_region_t *shape_region,
                                               gint                  offset_x,
                                               gint                  offset_y)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  g_clear_pointer (&impl->input_region, cairo_region_destroy);

  if (shape_region)
    {
      impl->input_region = cairo_region_copy (shape_region);
      cairo_region_translate (impl->input_region, offset_x, offset_y);
    }

  gdk_wayland_window_sync_input_region (window);
}

static void
gdk_wayland_window_destroy (GdkWindow *window,
                            gboolean   recursing,
                            gboolean   foreign_destroy)
{
  g_return_if_fail (GDK_IS_WINDOW (window));

  /* Wayland windows can't be externally destroyed; we may possibly
   * eventually want to use this path at display close-down
   */
  g_return_if_fail (!foreign_destroy);

  gdk_wayland_window_hide_surface (window);
  drop_cairo_surfaces (window);
}

static void
gdk_window_wayland_destroy_foreign (GdkWindow *window)
{
}

static cairo_region_t *
gdk_wayland_window_get_shape (GdkWindow *window)
{
  return NULL;
}

static cairo_region_t *
gdk_wayland_window_get_input_shape (GdkWindow *window)
{
  return NULL;
}

static void
gdk_wayland_window_focus (GdkWindow *window,
                          guint32    timestamp)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.gtk_surface)
    return;

  /* We didn't have an event to fetch a time from, meaning we have nothing valid
   * to send. This should rather be translated to a 'needs-attention' request or
   * something.
   */
  if (timestamp == GDK_CURRENT_TIME)
    return;

  gtk_surface1_present (impl->display_server.gtk_surface, timestamp);
}

static void
gdk_wayland_window_set_type_hint (GdkWindow         *window,
                                  GdkWindowTypeHint  hint)
{
  GdkWindowImplWayland *impl;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  impl->hint = hint;
}

static GdkWindowTypeHint
gdk_wayland_window_get_type_hint (GdkWindow *window)
{
  GdkWindowImplWayland *impl;

  if (GDK_WINDOW_DESTROYED (window))
    return GDK_WINDOW_TYPE_HINT_NORMAL;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  return impl->hint;
}

static void
gdk_wayland_window_init_gtk_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWaylandDisplay *display =
    GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));

  if (impl->display_server.gtk_surface != NULL)
    return;
  if (impl->display_server.xdg_surface == NULL)
    return;
  if (display->gtk_shell == NULL)
    return;

  impl->display_server.gtk_surface =
    gtk_shell1_get_gtk_surface (display->gtk_shell,
                                impl->display_server.wl_surface);
}

static void
maybe_set_gtk_surface_modal (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  gdk_wayland_window_init_gtk_surface (window);
  if (impl->display_server.gtk_surface == NULL)
    return;

  if (window->modal_hint)
    gtk_surface1_set_modal (impl->display_server.gtk_surface);
  else
    gtk_surface1_unset_modal (impl->display_server.gtk_surface);

}

static void
gdk_wayland_window_set_modal_hint (GdkWindow *window,
                                   gboolean   modal)
{
  window->modal_hint = modal;
  maybe_set_gtk_surface_modal (window);
}

static void
gdk_wayland_window_set_skip_taskbar_hint (GdkWindow *window,
                                          gboolean   skips_taskbar)
{
}

static void
gdk_wayland_window_set_skip_pager_hint (GdkWindow *window,
                                        gboolean   skips_pager)
{
}

static void
gdk_wayland_window_set_urgency_hint (GdkWindow *window,
                                     gboolean   urgent)
{
}

static void
gdk_wayland_window_set_geometry_hints (GdkWindow         *window,
                                       const GdkGeometry *geometry,
                                       GdkWindowHints     geom_mask)
{
  GdkWindowImplWayland *impl;

  if (GDK_WINDOW_DESTROYED (window) ||
      !WINDOW_IS_TOPLEVEL_OR_FOREIGN (window))
    return;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  impl->geometry_hints = *geometry;
  impl->geometry_mask = geom_mask;
}

static void
gdk_wayland_window_set_title (GdkWindow   *window,
                              const gchar *title)
{
  GdkWindowImplWayland *impl;
  g_return_if_fail (title != NULL);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  g_free (impl->title);
  impl->title = g_strdup (title);

  gdk_wayland_window_sync_title (window);
}

static void
gdk_wayland_window_set_role (GdkWindow   *window,
                             const gchar *role)
{
}

static void
gdk_wayland_window_set_startup_id (GdkWindow   *window,
                                   const gchar *startup_id)
{
}

static gboolean
check_transient_for_loop (GdkWindow *window,
                          GdkWindow *parent)
{
  while (parent)
    {
      GdkWindowImplWayland *impl;

      if (!GDK_IS_WINDOW_IMPL_WAYLAND(parent->impl))
        return FALSE;

      impl = GDK_WINDOW_IMPL_WAYLAND (parent->impl);
      if (impl->transient_for == window)
        return TRUE;
      parent = impl->transient_for;
    }
  return FALSE;
}

static void
gdk_wayland_window_set_transient_for (GdkWindow *window,
                                      GdkWindow *parent)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkWindow *previous_parent;

  if (check_transient_for_loop (window, parent))
    {
      g_warning ("Setting %p transient for %p would create a loop", window, parent);
      return;
    }

  if (impl->display_server.wl_subsurface)
    unmap_subsurface (window);

  previous_parent = impl->transient_for;
  impl->transient_for = parent;

  if (impl->hint == GDK_WINDOW_TYPE_HINT_DIALOG)
    {
      if (!parent)
        _gdk_wayland_screen_add_orphan_dialog (window);
      else if (!previous_parent)
        orphan_dialogs = g_list_remove (orphan_dialogs, window);
    }
  gdk_wayland_window_sync_parent (window, NULL);
  if (should_map_as_subsurface (window) &&
      parent && gdk_window_is_visible (window))
    gdk_wayland_window_create_subsurface (window);
}

static void
gdk_wayland_window_get_frame_extents (GdkWindow    *window,
                                      GdkRectangle *rect)
{
  gdk_wayland_window_get_fake_root_coords (window, &rect->x, &rect->y);
  rect->width = window->width;
  rect->height = window->height;
}

static void
gdk_wayland_window_set_override_redirect (GdkWindow *window,
                                          gboolean   override_redirect)
{
}

static void
gdk_wayland_window_set_accept_focus (GdkWindow *window,
                                     gboolean   accept_focus)
{
}

static void
gdk_wayland_window_set_focus_on_map (GdkWindow *window,
                                     gboolean focus_on_map)
{
}

static void
gdk_wayland_window_set_icon_list (GdkWindow *window,
                                  GList     *pixbufs)
{
}

static void
gdk_wayland_window_set_icon_name (GdkWindow   *window,
                                  const gchar *name)
{
  if (GDK_WINDOW_DESTROYED (window))
    return;
}

static void
gdk_wayland_window_iconify (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window) ||
      !WINDOW_IS_TOPLEVEL_OR_FOREIGN (window))
    return;

  if (!impl->display_server.xdg_surface)
    return;

  xdg_surface_set_minimized (impl->display_server.xdg_surface);
}

static void
gdk_wayland_window_deiconify (GdkWindow *window)
{
  if (GDK_WINDOW_DESTROYED (window) ||
      !WINDOW_IS_TOPLEVEL_OR_FOREIGN (window))
    return;

  if (GDK_WINDOW_IS_MAPPED (window))
    gdk_window_show (window);
  else
    /* Flip our client side flag, the real work happens on map. */
    gdk_synthesize_window_state (window, GDK_WINDOW_STATE_ICONIFIED, 0);
}

static void
gdk_wayland_window_stick (GdkWindow *window)
{
}

static void
gdk_wayland_window_unstick (GdkWindow *window)
{
}

static void
gdk_wayland_window_maximize (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  _gdk_wayland_window_save_size (window);
  if (impl->display_server.xdg_surface)
    xdg_surface_set_maximized (impl->display_server.xdg_surface);
  else
    gdk_synthesize_window_state (window, 0, GDK_WINDOW_STATE_MAXIMIZED);
}

static void
gdk_wayland_window_unmaximize (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  if (impl->display_server.xdg_surface)
    xdg_surface_unset_maximized (impl->display_server.xdg_surface);
  else
    gdk_synthesize_window_state (window, GDK_WINDOW_STATE_MAXIMIZED, 0);
}

static void
gdk_wayland_window_fullscreen_on_monitor (GdkWindow *window, gint monitor)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  GdkScreen *screen = gdk_window_get_screen (window);
  struct wl_output *fullscreen_output = 
    _gdk_wayland_screen_get_wl_output (screen, monitor);
  
  if (GDK_WINDOW_DESTROYED (window))
    return;

  _gdk_wayland_window_save_size (window);
  if (impl->display_server.xdg_surface)
    xdg_surface_set_fullscreen (impl->display_server.xdg_surface, fullscreen_output);
  else {
    gdk_synthesize_window_state (window, 0, GDK_WINDOW_STATE_FULLSCREEN);
    impl->initial_fullscreen_monitor = monitor;
  }
}

static void
gdk_wayland_window_fullscreen (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  impl->initial_fullscreen_monitor = -1;

  _gdk_wayland_window_save_size (window);
  if (impl->display_server.xdg_surface)
    xdg_surface_set_fullscreen (impl->display_server.xdg_surface, NULL);
  else
    gdk_synthesize_window_state (window, 0, GDK_WINDOW_STATE_FULLSCREEN);
}

static void
gdk_wayland_window_unfullscreen (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  
  if (GDK_WINDOW_DESTROYED (window))
    return;

  impl->initial_fullscreen_monitor = -1;

  if (impl->display_server.xdg_surface)
    xdg_surface_unset_fullscreen (impl->display_server.xdg_surface);
  else
    gdk_synthesize_window_state (window, GDK_WINDOW_STATE_FULLSCREEN, 0);
}

static void
gdk_wayland_window_set_keep_above (GdkWindow *window, gboolean setting)
{
}

static void
gdk_wayland_window_set_keep_below (GdkWindow *window, gboolean setting)
{
}

static GdkWindow *
gdk_wayland_window_get_group (GdkWindow *window)
{
  return NULL;
}

static void
gdk_wayland_window_set_group (GdkWindow *window,
                              GdkWindow *leader)
{
}

static void
gdk_wayland_window_set_decorations (GdkWindow       *window,
                                    GdkWMDecoration  decorations)
{
}

static gboolean
gdk_wayland_window_get_decorations (GdkWindow       *window,
                                    GdkWMDecoration *decorations)
{
  return FALSE;
}

static void
gdk_wayland_window_set_functions (GdkWindow     *window,
                                  GdkWMFunction  functions)
{
}

static void
gdk_wayland_window_begin_resize_drag (GdkWindow     *window,
                                      GdkWindowEdge  edge,
                                      GdkDevice     *device,
                                      gint           button,
                                      gint           root_x,
                                      gint           root_y,
                                      guint32        timestamp)
{
  GdkWindowImplWayland *impl;
  GdkEventSequence *sequence;
  uint32_t resize_edges, serial;

  if (GDK_WINDOW_DESTROYED (window) ||
      !WINDOW_IS_TOPLEVEL_OR_FOREIGN (window))
    return;

  switch (edge)
    {
    case GDK_WINDOW_EDGE_NORTH_WEST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_TOP_LEFT;
      break;

    case GDK_WINDOW_EDGE_NORTH:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_TOP;
      break;

    case GDK_WINDOW_EDGE_NORTH_EAST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_TOP_RIGHT;
      break;

    case GDK_WINDOW_EDGE_WEST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_LEFT;
      break;

    case GDK_WINDOW_EDGE_EAST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_RIGHT;
      break;

    case GDK_WINDOW_EDGE_SOUTH_WEST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_BOTTOM_LEFT;
      break;

    case GDK_WINDOW_EDGE_SOUTH:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_BOTTOM;
      break;

    case GDK_WINDOW_EDGE_SOUTH_EAST:
      resize_edges = XDG_SURFACE_RESIZE_EDGE_BOTTOM_RIGHT;
      break;

    default:
      g_warning ("gdk_window_begin_resize_drag: bad resize edge %d!", edge);
      return;
    }

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.xdg_surface)
    return;

  serial = _gdk_wayland_seat_get_last_implicit_grab_serial (gdk_device_get_seat (device),
                                                            &sequence);

  xdg_surface_resize (impl->display_server.xdg_surface,
                      gdk_wayland_device_get_wl_seat (device),
                      serial, resize_edges);

  if (sequence)
    gdk_wayland_device_unset_touch_grab (device, sequence);

  /* This is needed since Wayland will absorb all the pointer events after the
   * above function - FIXME: Is this always safe..?
   */
  gdk_seat_ungrab (gdk_device_get_seat (device));
}

static void
gdk_wayland_window_begin_move_drag (GdkWindow *window,
                                    GdkDevice *device,
                                    gint       button,
                                    gint       root_x,
                                    gint       root_y,
                                    guint32    timestamp)
{
  GdkWindowImplWayland *impl;
  GdkEventSequence *sequence;
  uint32_t serial;

  if (GDK_WINDOW_DESTROYED (window) ||
      !WINDOW_IS_TOPLEVEL (window))
    return;

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.xdg_surface)
    return;

  serial = _gdk_wayland_seat_get_last_implicit_grab_serial (gdk_device_get_seat (device),
                                                            &sequence);
  xdg_surface_move (impl->display_server.xdg_surface,
                    gdk_wayland_device_get_wl_seat (device),
                    serial);
  if (sequence)
    gdk_wayland_device_unset_touch_grab (device, sequence);

  /* This is needed since Wayland will absorb all the pointer events after the
   * above function - FIXME: Is this always safe..?
   */
  gdk_seat_ungrab (gdk_device_get_seat (device));
}

static void
gdk_wayland_window_set_opacity (GdkWindow *window,
                                gdouble    opacity)
{
}

static void
gdk_wayland_window_set_composited (GdkWindow *window,
                                   gboolean   composited)
{
}

static void
gdk_wayland_window_destroy_notify (GdkWindow *window)
{
  if (!GDK_WINDOW_DESTROYED (window))
    {
      if (GDK_WINDOW_TYPE (window) != GDK_WINDOW_FOREIGN)
        g_warning ("GdkWindow %p unexpectedly destroyed", window);

      _gdk_window_destroy (window, TRUE);
    }

  g_object_unref (window);
}

static void
gdk_wayland_window_sync_rendering (GdkWindow *window)
{
}

static gboolean
gdk_wayland_window_simulate_key (GdkWindow       *window,
                                 gint             x,
                                 gint             y,
                                 guint            keyval,
                                 GdkModifierType  modifiers,
                                 GdkEventType     key_pressrelease)
{
  return FALSE;
}

static gboolean
gdk_wayland_window_simulate_button (GdkWindow       *window,
                                    gint             x,
                                    gint             y,
                                    guint            button,
                                    GdkModifierType  modifiers,
                                    GdkEventType     button_pressrelease)
{
  return FALSE;
}

static gboolean
gdk_wayland_window_get_property (GdkWindow   *window,
                                 GdkAtom      property,
                                 GdkAtom      type,
                                 gulong       offset,
                                 gulong       length,
                                 gint         pdelete,
                                 GdkAtom     *actual_property_type,
                                 gint        *actual_format_type,
                                 gint        *actual_length,
                                 guchar     **data)
{
  return FALSE;
}

static void
gdk_wayland_window_change_property (GdkWindow    *window,
                                    GdkAtom       property,
                                    GdkAtom       type,
                                    gint          format,
                                    GdkPropMode   mode,
                                    const guchar *data,
                                    gint          nelements)
{
  if (property == gdk_atom_intern_static_string ("GDK_SELECTION"))
    gdk_wayland_selection_store (window, type, mode, data, nelements * (format / 8));
}

static void
gdk_wayland_window_delete_property (GdkWindow *window,
                                    GdkAtom    property)
{
}

static gint
gdk_wayland_window_get_scale_factor (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return 1;

  return impl->scale;
}

static void
gdk_wayland_window_set_opaque_region (GdkWindow      *window,
                                      cairo_region_t *region)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (GDK_WINDOW_DESTROYED (window))
    return;

  g_clear_pointer (&impl->opaque_region, cairo_region_destroy);
  impl->opaque_region = cairo_region_reference (region);
  gdk_wayland_window_sync_opaque_region (window);
}

static void
gdk_wayland_window_set_shadow_width (GdkWindow *window,
                                     int        left,
                                     int        right,
                                     int        top,
                                     int        bottom)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  gint new_width, new_height;

  if (GDK_WINDOW_DESTROYED (window))
    return;

  /* Reconfigure window to keep the same window geometry */
  new_width = window->width -
    (impl->margin_left + impl->margin_right) + (left + right);
  new_height = window->height -
    (impl->margin_top + impl->margin_bottom) + (top + bottom);
  gdk_wayland_window_configure (window, new_width, new_height, impl->scale);

  impl->margin_left = left;
  impl->margin_right = right;
  impl->margin_top = top;
  impl->margin_bottom = bottom;
  gdk_wayland_window_sync_margin (window);
}

static gboolean
gdk_wayland_window_show_window_menu (GdkWindow *window,
                                     GdkEvent  *event)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  struct wl_seat *seat;
  GdkWaylandDevice *device;
  double x, y;

  switch (event->type)
    {
    case GDK_BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
    case GDK_TOUCH_BEGIN:
    case GDK_TOUCH_END:
      break;
    default:
      return FALSE;
    }

  if (!impl->display_server.xdg_surface)
    return FALSE;

  device = GDK_WAYLAND_DEVICE (gdk_event_get_device (event));
  seat = gdk_wayland_device_get_wl_seat (GDK_DEVICE (device));
  gdk_event_get_coords (event, &x, &y);

  xdg_surface_show_window_menu (impl->display_server.xdg_surface,
                                seat,
                                _gdk_wayland_device_get_implicit_grab_serial (device, event),
                                x, y);
  return TRUE;
}

static void
_gdk_window_impl_wayland_class_init (GdkWindowImplWaylandClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkWindowImplClass *impl_class = GDK_WINDOW_IMPL_CLASS (klass);

  object_class->finalize = gdk_window_impl_wayland_finalize;

  impl_class->ref_cairo_surface = gdk_wayland_window_ref_cairo_surface;
  impl_class->create_similar_image_surface = gdk_wayland_window_create_similar_image_surface;
  impl_class->show = gdk_wayland_window_show;
  impl_class->hide = gdk_wayland_window_hide;
  impl_class->withdraw = gdk_window_wayland_withdraw;
  impl_class->set_events = gdk_window_wayland_set_events;
  impl_class->get_events = gdk_window_wayland_get_events;
  impl_class->raise = gdk_window_wayland_raise;
  impl_class->lower = gdk_window_wayland_lower;
  impl_class->restack_under = gdk_window_wayland_restack_under;
  impl_class->restack_toplevel = gdk_window_wayland_restack_toplevel;
  impl_class->move_resize = gdk_window_wayland_move_resize;
  impl_class->set_background = gdk_window_wayland_set_background;
  impl_class->reparent = gdk_window_wayland_reparent;
  impl_class->set_device_cursor = gdk_window_wayland_set_device_cursor;
  impl_class->get_geometry = gdk_window_wayland_get_geometry;
  impl_class->get_root_coords = gdk_window_wayland_get_root_coords;
  impl_class->get_device_state = gdk_window_wayland_get_device_state;
  impl_class->shape_combine_region = gdk_window_wayland_shape_combine_region;
  impl_class->input_shape_combine_region = gdk_window_wayland_input_shape_combine_region;
  impl_class->destroy = gdk_wayland_window_destroy;
  impl_class->destroy_foreign = gdk_window_wayland_destroy_foreign;
  impl_class->get_shape = gdk_wayland_window_get_shape;
  impl_class->get_input_shape = gdk_wayland_window_get_input_shape;
  impl_class->begin_paint = gdk_window_impl_wayland_begin_paint;
  impl_class->end_paint = gdk_window_impl_wayland_end_paint;
  impl_class->beep = gdk_window_impl_wayland_beep;

  impl_class->focus = gdk_wayland_window_focus;
  impl_class->set_type_hint = gdk_wayland_window_set_type_hint;
  impl_class->get_type_hint = gdk_wayland_window_get_type_hint;
  impl_class->set_modal_hint = gdk_wayland_window_set_modal_hint;
  impl_class->set_skip_taskbar_hint = gdk_wayland_window_set_skip_taskbar_hint;
  impl_class->set_skip_pager_hint = gdk_wayland_window_set_skip_pager_hint;
  impl_class->set_urgency_hint = gdk_wayland_window_set_urgency_hint;
  impl_class->set_geometry_hints = gdk_wayland_window_set_geometry_hints;
  impl_class->set_title = gdk_wayland_window_set_title;
  impl_class->set_role = gdk_wayland_window_set_role;
  impl_class->set_startup_id = gdk_wayland_window_set_startup_id;
  impl_class->set_transient_for = gdk_wayland_window_set_transient_for;
  impl_class->get_frame_extents = gdk_wayland_window_get_frame_extents;
  impl_class->set_override_redirect = gdk_wayland_window_set_override_redirect;
  impl_class->set_accept_focus = gdk_wayland_window_set_accept_focus;
  impl_class->set_focus_on_map = gdk_wayland_window_set_focus_on_map;
  impl_class->set_icon_list = gdk_wayland_window_set_icon_list;
  impl_class->set_icon_name = gdk_wayland_window_set_icon_name;
  impl_class->iconify = gdk_wayland_window_iconify;
  impl_class->deiconify = gdk_wayland_window_deiconify;
  impl_class->stick = gdk_wayland_window_stick;
  impl_class->unstick = gdk_wayland_window_unstick;
  impl_class->maximize = gdk_wayland_window_maximize;
  impl_class->unmaximize = gdk_wayland_window_unmaximize;
  impl_class->fullscreen = gdk_wayland_window_fullscreen;
  impl_class->fullscreen_on_monitor = gdk_wayland_window_fullscreen_on_monitor;
  impl_class->unfullscreen = gdk_wayland_window_unfullscreen;
  impl_class->set_keep_above = gdk_wayland_window_set_keep_above;
  impl_class->set_keep_below = gdk_wayland_window_set_keep_below;
  impl_class->get_group = gdk_wayland_window_get_group;
  impl_class->set_group = gdk_wayland_window_set_group;
  impl_class->set_decorations = gdk_wayland_window_set_decorations;
  impl_class->get_decorations = gdk_wayland_window_get_decorations;
  impl_class->set_functions = gdk_wayland_window_set_functions;
  impl_class->begin_resize_drag = gdk_wayland_window_begin_resize_drag;
  impl_class->begin_move_drag = gdk_wayland_window_begin_move_drag;
  impl_class->set_opacity = gdk_wayland_window_set_opacity;
  impl_class->set_composited = gdk_wayland_window_set_composited;
  impl_class->destroy_notify = gdk_wayland_window_destroy_notify;
  impl_class->get_drag_protocol = _gdk_wayland_window_get_drag_protocol;
  impl_class->register_dnd = _gdk_wayland_window_register_dnd;
  impl_class->drag_begin = _gdk_wayland_window_drag_begin;
  impl_class->sync_rendering = gdk_wayland_window_sync_rendering;
  impl_class->simulate_key = gdk_wayland_window_simulate_key;
  impl_class->simulate_button = gdk_wayland_window_simulate_button;
  impl_class->get_property = gdk_wayland_window_get_property;
  impl_class->change_property = gdk_wayland_window_change_property;
  impl_class->delete_property = gdk_wayland_window_delete_property;
  impl_class->get_scale_factor = gdk_wayland_window_get_scale_factor;
  impl_class->set_opaque_region = gdk_wayland_window_set_opaque_region;
  impl_class->set_shadow_width = gdk_wayland_window_set_shadow_width;
  impl_class->show_window_menu = gdk_wayland_window_show_window_menu;
  impl_class->create_gl_context = gdk_wayland_window_create_gl_context;
  impl_class->invalidate_for_new_frame = gdk_wayland_window_invalidate_for_new_frame;

  signals[COMMITTED] = g_signal_new ("committed",
                                     G_TYPE_FROM_CLASS (object_class),
                                     G_SIGNAL_RUN_LAST,
                                     0,
                                     NULL, NULL, NULL,
                                     G_TYPE_NONE, 0);
}

void
_gdk_wayland_window_set_grab_seat (GdkWindow *window,
                                   GdkSeat   *seat)
{
  GdkWindowImplWayland *impl;

  g_return_if_fail (window != NULL);

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);
  impl->grab_input_seat = seat;
}

/**
 * gdk_wayland_window_get_wl_surface:
 * @window: (type GdkWaylandWindow): a #GdkWindow
 *
 * Returns the Wayland surface of a #GdkWindow.
 *
 * Returns: (transfer none): a Wayland wl_surface
 *
 * Since: 3.8
 */
struct wl_surface *
gdk_wayland_window_get_wl_surface (GdkWindow *window)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_WINDOW (window), NULL);

  return GDK_WINDOW_IMPL_WAYLAND (window->impl)->display_server.wl_surface;
}

static struct wl_egl_window *
gdk_wayland_window_get_wl_egl_window (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (impl->display_server.egl_window == NULL)
    {
      impl->display_server.egl_window =
        wl_egl_window_create (impl->display_server.wl_surface,
                              impl->wrapper->width * impl->scale,
                              impl->wrapper->height * impl->scale);
      wl_surface_set_buffer_scale (impl->display_server.wl_surface, impl->scale);
    }

  return impl->display_server.egl_window;
}

EGLSurface
gdk_wayland_window_get_egl_surface (GdkWindow *window,
                                    EGLConfig  config)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkWindowImplWayland *impl;
  struct wl_egl_window *egl_window;

  g_return_val_if_fail (GDK_IS_WAYLAND_WINDOW (window), NULL);

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (impl->egl_surface == NULL)
    {
      egl_window = gdk_wayland_window_get_wl_egl_window (window);

      impl->egl_surface =
        eglCreateWindowSurface (display->egl_display, config, egl_window, NULL);
    }

  return impl->egl_surface;
}

EGLSurface
gdk_wayland_window_get_dummy_egl_surface (GdkWindow *window,
                                          EGLConfig  config)
{
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (gdk_window_get_display (window));
  GdkWindowImplWayland *impl;

  g_return_val_if_fail (GDK_IS_WAYLAND_WINDOW (window), NULL);

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (impl->dummy_egl_surface == NULL)
    {
      impl->display_server.dummy_egl_window =
        wl_egl_window_create (impl->display_server.wl_surface, 1, 1);

      impl->dummy_egl_surface =
        eglCreateWindowSurface (display->egl_display, config, impl->display_server.dummy_egl_window, NULL);
    }

  return impl->dummy_egl_surface;
}


/**
 * gdk_wayland_window_set_use_custom_surface:
 * @window: (type GdkWaylandWindow): a #GdkWindow
 *
 * Marks a #GdkWindow as a custom Wayland surface. The application is
 * expected to register the surface as some type of surface using
 * some Wayland interface.
 *
 * A good example would be writing a panel or on-screen-keyboard as an
 * out-of-process helper - as opposed to having those in the compositor
 * process. In this case the underlying surface isn’t an xdg_shell
 * surface and the panel or OSK client need to identify the wl_surface
 * as a panel or OSK to the compositor. The assumption is that the
 * compositor will expose a private interface to the special client
 * that lets the client identify the wl_surface as a panel or such.
 *
 * This function should be called before a #GdkWindow is shown. This is
 * best done by connecting to the #GtkWidget::realize signal:
 *
 * |[<!-- language="C" -->
 *   static void
 *   widget_realize_cb (GtkWidget *widget)
 *   {
 *     GdkWindow *window;
 *     struct wl_surface *surface;
 *     struct input_panel_surface *ip_surface;
 *
 *     window = gtk_widget_get_window (widget);
 *     gdk_wayland_window_set_custom_surface (window);
 *
 *     surface = gdk_wayland_window_get_wl_surface (window);
 *     ip_surface = input_panel_get_input_panel_surface (input_panel, surface);
 *     input_panel_surface_set_panel (ip_surface);
 *   }
 *
 *   static void
 *   setup_window (GtkWindow *window)
 *   {
 *     g_signal_connect (window, "realize", G_CALLBACK (widget_realize_cb), NULL);
 *   }
 * ]|
 *
 * Since: 3.10
 */
void
gdk_wayland_window_set_use_custom_surface (GdkWindow *window)
{
  GdkWindowImplWayland *impl;

  g_return_if_fail (GDK_IS_WAYLAND_WINDOW (window));

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (!impl->display_server.wl_surface)
    gdk_wayland_window_create_surface (window);

  impl->use_custom_surface = TRUE;
}

static void
maybe_set_gtk_surface_dbus_properties (GdkWindow *window)
{
  GdkWindowImplWayland *impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  if (impl->application.was_set)
    return;

  if (impl->application.application_id == NULL &&
      impl->application.app_menu_path == NULL &&
      impl->application.menubar_path == NULL &&
      impl->application.window_object_path == NULL &&
      impl->application.application_object_path == NULL &&
      impl->application.unique_bus_name == NULL)
    return;

  gdk_wayland_window_init_gtk_surface (window);
  if (impl->display_server.gtk_surface == NULL)
    return;

  gtk_surface1_set_dbus_properties (impl->display_server.gtk_surface,
                                    impl->application.application_id,
                                    impl->application.app_menu_path,
                                    impl->application.menubar_path,
                                    impl->application.window_object_path,
                                    impl->application.application_object_path,
                                    impl->application.unique_bus_name);
  impl->application.was_set = TRUE;
}

void
gdk_wayland_window_set_dbus_properties_libgtk_only (GdkWindow  *window,
                                                    const char *application_id,
                                                    const char *app_menu_path,
                                                    const char *menubar_path,
                                                    const char *window_object_path,
                                                    const char *application_object_path,
                                                    const char *unique_bus_name)
{
  GdkWindowImplWayland *impl;

  g_return_if_fail (GDK_IS_WAYLAND_WINDOW (window));

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  impl->application.application_id = g_strdup (application_id);
  impl->application.app_menu_path = g_strdup (app_menu_path);
  impl->application.menubar_path = g_strdup (menubar_path);
  impl->application.window_object_path = g_strdup (window_object_path);
  impl->application.application_object_path =
    g_strdup (application_object_path);
  impl->application.unique_bus_name = g_strdup (unique_bus_name);

  maybe_set_gtk_surface_dbus_properties (window);
}

void
_gdk_wayland_window_offset_next_wl_buffer (GdkWindow *window,
                                           int        x,
                                           int        y)
{
  GdkWindowImplWayland *impl;

  g_return_if_fail (GDK_IS_WAYLAND_WINDOW (window));

  impl = GDK_WINDOW_IMPL_WAYLAND (window->impl);

  impl->pending_buffer_offset_x = x;
  impl->pending_buffer_offset_y = y;
}
