/*
 * Copyright © 2016 Endless Mobile Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Matthew Watson <mattdangerw@gmail.com>
 */

#include "gtkprogresstrackerprivate.h"
#include "gtkcsseasevalueprivate.h"

#include <math.h>

void
gtk_progress_tracker_start (GtkProgressTracker *tracker,
                            guint64 duration,
                            gint64 delay,
                            gdouble iteration_count)
{
  tracker->is_running = TRUE;
  tracker->last_frame_time = 0;
  tracker->duration = duration;
  tracker->iteration = - delay / (gdouble) duration;
  tracker->iteration_count = iteration_count;
}

void
gtk_progress_tracker_finish (GtkProgressTracker *tracker)
{
  tracker->is_running = FALSE;
}

void
gtk_progress_tracker_next_frame (GtkProgressTracker *tracker,
                                 guint64 frame_time)
{
  gdouble delta;

  if (!tracker->is_running)
    return;

  if (tracker->last_frame_time == 0)
    {
      tracker->last_frame_time = frame_time;
      return;
    }

  if (frame_time < tracker->last_frame_time)
    {
      g_warning ("Progress tracker frame set backwards, ignoring\n");
      return;
    }

  delta = (frame_time - tracker->last_frame_time) / (gdouble) tracker->duration;
  tracker->last_frame_time = frame_time;
  tracker->iteration += delta;
}

GtkProgressState
gtk_progress_tracker_get_state (GtkProgressTracker *tracker)
{
  if (!tracker->is_running || tracker->iteration > tracker->iteration_count)
    return GTK_PROGRESS_STATE_AFTER;
  if (tracker->iteration < 0)
    return GTK_PROGRESS_STATE_BEFORE;
  return GTK_PROGRESS_STATE_DURING;
}

gdouble
gtk_progress_tracker_get_iteration (GtkProgressTracker *tracker)
{
  return tracker->is_running ? tracker->iteration : 1.0;
}

gint
gtk_progress_tracker_get_iteration_cycle (GtkProgressTracker *tracker)
{
  gdouble iteration;

  if (!tracker->is_running)
    return 0;

  iteration = CLAMP (tracker->iteration, 0.0, tracker->iteration_count);

  if (iteration == 0.0)
    return 0;

  return (gint) ceil (iteration) - 1;
}

gdouble
gtk_progress_tracker_get_progress (GtkProgressTracker *tracker,
                                   gboolean reversed)
{
  gdouble progress, iteration;

  if (!tracker->is_running)
    return reversed ? 0.0 : 1.0;

  iteration = CLAMP (tracker->iteration, 0.0, tracker->iteration_count);

  if (iteration == 0.0)
    progress = 0.0;
  else
    {
      progress = fmod (iteration, 1.0);
      if (progress == 0.0)
        progress = 1.0;
    }

  return reversed ? 1.0 - progress : progress;
}

/* From clutter-easing.c, based on Robert Penner's
 * infamous easing equations, MIT license.
 */
static gdouble
ease_out_cubic (gdouble t)
{
  gdouble p = t - 1;
  return p * p * p + 1;
}

gdouble
gtk_progress_tracker_get_ease (GtkProgressTracker *tracker,
                               GtkCssValue *ease,
                               gboolean reversed)
{
  gdouble progress = gtk_progress_tracker_get_progress (tracker, reversed);

  /* XXX: We've combined our css easing and non css easing here in the same
   * function. Would be nice to open up csseasevalue so you could create one
   * without a css parser, and use the same easings across the entire codebase */
  if (ease == NULL)
    return ease_out_cubic (progress);
  return _gtk_css_ease_value_transform (ease, progress);
}
