/* GDK - The GIMP Drawing Kit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GTK+ Team and others 1997-1999.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/.
 */

#ifndef __GDK_WINDOW_WIN32_H__
#define __GDK_WINDOW_WIN32_H__

#include "gdk/win32/gdkprivate-win32.h"
#include "gdk/gdkwindowimpl.h"
#include "gdk/gdkcursor.h"

#include <windows.h>

G_BEGIN_DECLS

/* Window implementation for Win32
 */

typedef struct _GdkWindowImplWin32 GdkWindowImplWin32;
typedef struct _GdkWindowImplWin32Class GdkWindowImplWin32Class;

#define GDK_TYPE_WINDOW_IMPL_WIN32              (_gdk_window_impl_win32_get_type ())
#define GDK_WINDOW_IMPL_WIN32(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), GDK_TYPE_WINDOW_IMPL_WIN32, GdkWindowImplWin32))
#define GDK_WINDOW_IMPL_WIN32_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GDK_TYPE_WINDOW_IMPL_WIN32, GdkWindowImplWin32Class))
#define GDK_IS_WINDOW_IMPL_WIN32(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_WINDOW_IMPL_WIN32))
#define GDK_IS_WINDOW_IMPL_WIN32_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GDK_TYPE_WINDOW_IMPL_WIN32))
#define GDK_WINDOW_IMPL_WIN32_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GDK_TYPE_WINDOW_IMPL_WIN32, GdkWindowImplWin32Class))

enum _GdkW32WindowDragOp
{
  GDK_WIN32_DRAGOP_NONE = 0,
  GDK_WIN32_DRAGOP_RESIZE,
  GDK_WIN32_DRAGOP_MOVE,
  GDK_WIN32_DRAGOP_COUNT
};

typedef enum _GdkW32WindowDragOp GdkW32WindowDragOp;

struct _GdkW32DragMoveResizeContext
{
  /* The kind of drag-operation going on. */
  GdkW32WindowDragOp op;

  /* The edge that was grabbed for resizing. Not used for moving. */
  GdkWindowEdge      edge;

  /* The device used to initiate the op.
   * We grab it at the beginning and ungrab it at the end.
   */
  GdkDevice         *device;

  /* The button pressed down to initiate the op.
   * The op will be canceled only when *this* button
   * is released.
   */
  gint               button;

  /* Initial cursor position when the operation began.
   * Current cursor position is subtracted from it to find how far
   * to move window border(s).
   */
  gint               start_root_x;
  gint               start_root_y;

  /* Initial window rectangle (position and size).
   * The window is resized/moved relative to this (see start_root_*).
   */
  RECT               start_rect;

  /* Not used */
  guint32            timestamp;

  /* TRUE if during the next redraw we should call SetWindowPos() to push
   * the window size and poistion to the native window.
   */
  gboolean           native_move_resize_pending;

  /* The cursor we should use while the operation is running. */
  GdkCursor         *cursor;
};

typedef struct _GdkW32DragMoveResizeContext GdkW32DragMoveResizeContext;

struct _GdkWindowImplWin32
{
  GdkWindowImpl parent_instance;

  GdkWindow *wrapper;
  HANDLE handle;

  gint8 toplevel_window_type;

  GdkCursor *cursor;
  HICON   hicon_big;
  HICON   hicon_small;

  /* Window size hints */
  gint hint_flags;
  GdkGeometry hints;

  GdkEventMask native_event_mask;

  GdkWindowTypeHint type_hint;

  GdkWindow *transient_owner;
  GSList    *transient_children;
  gint       num_transients;
  gboolean   changing_state;

  gint initial_x;
  gint initial_y;

  guint no_bg : 1;
  guint inhibit_configure : 1;
  guint override_redirect : 1;

  /* Set to TRUE if window is using true layered mode adjustments
   * via UpdateLayeredWindow().
   * Layered windows that get SetLayeredWindowAttributes() called
   * on them are not true layered windows.
   */
  guint layered : 1;

  /* GDK does not keep window contents around, it just draws new
   * stuff over the window where changes occurred.
   * cache_surface retains old window contents, because
   * UpdateLayeredWindow() doesn't do partial redraws.
   */
  cairo_surface_t *cache_surface;
  cairo_surface_t *cairo_surface;

  /* Unlike window-backed surfaces, DIB-backed surface
   * does not provide a way to query its size,
   * so we have to remember it ourselves.
   */
  gint             dib_width;
  gint             dib_height;

  /* If the client wants uniformly-transparent window,
   * we remember the opacity value here and apply it
   * during UpdateLayredWindow() call, for layered windows.
   */
  gdouble          layered_opacity;

  HDC              hdc;
  int              hdc_count;
  HBITMAP          saved_dc_bitmap; /* Original bitmap for dc */

  GdkW32DragMoveResizeContext drag_move_resize_context;

  /* Decorations set by gdk_window_set_decorations() or NULL if unset */
  GdkWMDecoration* decorations;

  /* No. of windows to force layered windows off */
  guint suppress_layered;
};

struct _GdkWindowImplWin32Class
{
  GdkWindowImplClass parent_class;
};

GType _gdk_window_impl_win32_get_type (void);

void  _gdk_win32_window_tmp_unset_bg  (GdkWindow *window,
				       gboolean   recurse);
void  _gdk_win32_window_tmp_reset_bg  (GdkWindow *window,
				       gboolean   recurse);

void  _gdk_win32_window_tmp_unset_parent_bg (GdkWindow *window);
void  _gdk_win32_window_tmp_reset_parent_bg (GdkWindow *window);

void  _gdk_win32_window_update_style_bits   (GdkWindow *window);

G_END_DECLS

#endif /* __GDK_WINDOW_WIN32_H__ */
