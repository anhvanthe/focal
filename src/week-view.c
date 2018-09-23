/*
 * week-view.c
 * This file is part of focal, a calendar application for Linux
 * Copyright 2018 Oliver Giles and focal contributors.
 *
 * Focal is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Focal is distributed without any explicit or implied warranty.
 * You should have received a copy of the GNU General Public License
 * version 3 with focal. If not, see <http://www.gnu.org/licenses/>.
 */
#include <ctype.h>
#include <gtk/gtk.h>
#include <libical/ical.h>
#include <stdlib.h>
#include <string.h>

#include "week-view.h"

struct _EventWidget {
	Event* ev;
	// cached time values for faster drawing
	int minutes_from, minutes_to;
	// list pointer
	struct _EventWidget* next;
};
typedef struct _EventWidget EventWidget;

struct _WeekView {
	GtkDrawingArea drawing_area;
	int x, y, width, height;
	int scroll_pos;
	GtkAdjustment* adj;
	GSList* calendars;
	EventWidget* events_week[7];
	int current_week; // 1-based, note libical is 0-based
	int current_year;
	icaltimezone* current_tz;
	icaltime_span current_view;
	struct {
		gboolean visible;
		int dow;
		int minutes;
	} now;
};

enum {
	SIGNAL_EVENT_SELECTED,
	SIGNAL_DATE_RANGE_CHANGED,
	LAST_SIGNAL
};

static gint week_view_signals[LAST_SIGNAL] = {0};

enum {
	PROP_0,
	PROP_HADJUSTMENT,
	PROP_VADJUSTMENT,
	PROP_HSCROLL_POLICY,
	PROP_VSCROLL_POLICY,
};

#define HEADER_HEIGHT 35.5

// Implemented from GtkScrollable, causes the scroll bar to start below the header
static gboolean get_border(GtkScrollable* scrollable, GtkBorder* border)
{
	border->top = HEADER_HEIGHT;
	return TRUE;
}

static void week_view_scrollable_init(GtkScrollableInterface* iface)
{
	iface->get_border = get_border;
}

G_DEFINE_TYPE_WITH_CODE(WeekView, week_view, GTK_TYPE_DRAWING_AREA, G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, week_view_scrollable_init))

#define SIDEBAR_WIDTH 25.5
#define HALFHOUR_HEIGHT 30.0

static void week_view_draw(WeekView* wv, cairo_t* cr)
{
	const int num_days = 7;
	const double dashes[] = {1.0};

	double dark = 0.3, med = 0.65, light = 0.85;
	cairo_set_source_rgb(cr, dark, dark, dark);
	cairo_set_line_width(cr, 1.0);

	const int first_visible_halfhour = wv->scroll_pos / HALFHOUR_HEIGHT + 1;

	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

	cairo_set_font_size(cr, 12);

	for (int hh = first_visible_halfhour;; ++hh) {
		double y = wv->y + HEADER_HEIGHT + hh * HALFHOUR_HEIGHT - wv->scroll_pos;
		if (y > wv->y + wv->height)
			break;
		if (hh % 2 == 0) {
			cairo_set_source_rgb(cr, med, med, med);
			cairo_set_dash(cr, NULL, 0, 0);
			cairo_move_to(cr, wv->x, y);
			cairo_rel_line_to(cr, wv->width, 0);
			cairo_stroke(cr);
			// draw hour labels
			char hourlabel[8];
			cairo_move_to(cr, wv->x + 5, y + 12);
			sprintf(hourlabel, "%02d", hh / 2);
			cairo_set_source_rgb(cr, dark, dark, dark);
			cairo_show_text(cr, hourlabel);
		} else {
			cairo_set_source_rgb(cr, light, light, light);
			cairo_set_dash(cr, dashes, 1, 0);
			cairo_move_to(cr, wv->x + SIDEBAR_WIDTH, y);
			cairo_rel_line_to(cr, wv->width, 0);
			cairo_stroke(cr);
		}
	}

	// draw vertical lines for days
	const int day_width = (double) (wv->width - SIDEBAR_WIDTH) / num_days;
	cairo_set_dash(cr, NULL, 0, 0);
	time_t t = wv->current_view.start;
	for (int d = 0; d < num_days; ++d, t += 60 * 60 * 24) {
		double x = wv->x + SIDEBAR_WIDTH + d * day_width;

		char daylabel[16];
		struct tm* firstday = localtime(&t);
		strftime(daylabel, 16, "%e", firstday);
		cairo_move_to(cr, x + 5, wv->y + HEADER_HEIGHT - 5);
		cairo_set_font_size(cr, 22);
		cairo_set_source_rgb(cr, dark, dark, dark);
		cairo_show_text(cr, daylabel);

		strftime(daylabel, 16, "%a", firstday);
		for (char* p = daylabel; *p; ++p)
			*p = toupper(*p);
		cairo_move_to(cr, x + 35, wv->y + HEADER_HEIGHT - 5);
		cairo_set_font_size(cr, 11);
		cairo_show_text(cr, daylabel);

		cairo_set_source_rgb(cr, med, med, med);
		cairo_move_to(cr, x, wv->y + HEADER_HEIGHT);
		cairo_rel_line_to(cr, 0, wv->height);
		cairo_stroke(cr);
	}
	// top bar
	cairo_move_to(cr, wv->x, wv->y + HEADER_HEIGHT);
	cairo_rel_line_to(cr, wv->width, 0);
	cairo_set_source_rgb(cr, dark, dark, dark);
	cairo_stroke(cr);

	PangoLayout* layout = pango_cairo_create_layout(cr);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

	PangoFontDescription* font_desc = pango_font_description_from_string("sans 9");
	pango_layout_set_font_description(layout, font_desc);
	pango_font_description_free(font_desc);

	// draw events
	for (int d = 0; d < num_days; ++d) {
		for (EventWidget* tmp = wv->events_week[d]; tmp; tmp = tmp->next) {
			double yminutescale = HALFHOUR_HEIGHT / 30.0; //(cal->height - header_height) / (60.0 * num_hours_displayed);
			double yfrom = tmp->minutes_from * yminutescale + wv->y + HEADER_HEIGHT - wv->scroll_pos;
			double yto = tmp->minutes_to * yminutescale + wv->y + HEADER_HEIGHT - wv->scroll_pos;
			double x = wv->x + SIDEBAR_WIDTH + d * day_width;
			GdkRGBA* color = event_get_color(tmp->ev);
			cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha - (event_get_dirty(tmp->ev) ? 0.3 : 0.0));
			cairo_rectangle(cr, x + 1, yfrom + 1, day_width - 2, yto - yfrom - 2);
			cairo_fill(cr);

			pango_layout_set_width(layout, PANGO_SCALE * (day_width - 8));
			pango_layout_set_height(layout, PANGO_SCALE * (yto - yfrom - 2));
			pango_layout_set_text(layout, event_get_summary(tmp->ev), -1);

			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, x + 3, yfrom + 1);
			pango_cairo_show_layout(cr, layout);
		}
	}
	g_object_unref(layout);

	if (wv->now.visible) {
		double nowY = wv->y + HEADER_HEIGHT + wv->now.minutes * HALFHOUR_HEIGHT / 30 - wv->scroll_pos;
		cairo_set_source_rgb(cr, 1, 0, 0);
		cairo_set_dash(cr, NULL, 0, 0);
		cairo_move_to(cr, wv->x + SIDEBAR_WIDTH + wv->now.dow * day_width, nowY);
		cairo_rel_line_to(cr, day_width, 0);
		cairo_stroke(cr);
	}
}

static gboolean on_draw_event(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	week_view_draw(wv, cr);
	return FALSE;
}

static gboolean on_press_event(GtkWidget* widget, GdkEventButton* event, gpointer data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	if (event->button == GDK_BUTTON_PRIMARY) {
		// look for collisions
		if (event->x < SIDEBAR_WIDTH || event->y < HEADER_HEIGHT)
			return TRUE;

		int dow = 7 * (event->x - SIDEBAR_WIDTH) / (wv->width - SIDEBAR_WIDTH);
		int minutesAt = (event->y - HEADER_HEIGHT + wv->scroll_pos) * 30 / HALFHOUR_HEIGHT;

		EventWidget* tmp;
		for (tmp = wv->events_week[dow]; tmp; tmp = tmp->next) {
			if (tmp->minutes_from < minutesAt && minutesAt < tmp->minutes_to) {
				break;
			}
		}

		GdkRectangle rect;
		if (tmp) {
			rect.width = (wv->width - SIDEBAR_WIDTH) / 7;
			rect.x = dow * rect.width + SIDEBAR_WIDTH;
			rect.y = HEADER_HEIGHT + (tmp->minutes_from - wv->scroll_pos) * HALFHOUR_HEIGHT / 30;
			rect.height = (tmp->minutes_to - tmp->minutes_from) * HALFHOUR_HEIGHT / 30;
			g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, tmp->ev, &rect);
		} else if (event->type == GDK_2BUTTON_PRESS) {
			// double-click: request to create an event
			// dtstart: round down to closest quarter-hour
			time_t at = wv->current_view.start + dow * 24 * 3600 + 15 * (minutesAt / 15) * 60;
			icaltimetype dtstart = icaltime_from_timet_with_zone(at, FALSE, wv->current_tz);
			// https://github.com/libical/libical/blob/master/src/test/timezones.c#L96
			dtstart.zone = wv->current_tz;
			icaltimetype dtend = dtstart;
			// duration: default event is 30min long
			icaltime_adjust(&dtend, 0, 0, 30, 0);
			Event* ev = event_new("New Event", dtstart, dtend, wv->current_tz);

			/* Assumes a calendar is loaded, chooses the first in the list. TODO something smarter? */
			event_set_calendar(ev, wv->calendars->data);

			rect.width = (wv->width - SIDEBAR_WIDTH) / 7;
			rect.x = dow * rect.width + SIDEBAR_WIDTH;
			rect.y = HEADER_HEIGHT + (dtstart.hour * 60 + dtstart.minute - wv->scroll_pos) * HALFHOUR_HEIGHT / 30;
			rect.height = (dtend.hour * 60 + dtend.minute - dtstart.hour * 60 - dtstart.minute) * HALFHOUR_HEIGHT / 30;

			week_view_add_event(wv, ev);
			gtk_widget_queue_draw((GtkWidget*) wv);
			g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, ev, &rect);
		} else {
			// deselect
			g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, NULL);
		}
	}
	return TRUE;
}

static void adjustment_changed(GtkAdjustment* adjustment, WeekView* wv)
{
	wv->scroll_pos = gtk_adjustment_get_value(adjustment);
	gtk_widget_queue_draw(GTK_WIDGET(wv));
}

static void set_vadjustment(WeekView* wv, GtkAdjustment* adjustment)
{
	if (!adjustment)
		return;

	// this function should only be called once with a real adjustment
	g_assert_null(wv->adj);

	wv->adj = g_object_ref_sink(adjustment);
	g_signal_connect(adjustment, "value-changed", G_CALLBACK(adjustment_changed), wv);
}

static void set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec)
{
	if (prop_id == PROP_HADJUSTMENT) {
		// ignored, horizontal scrolling not supported
	} else if (prop_id == PROP_VADJUSTMENT) {
		set_vadjustment(FOCAL_WEEK_VIEW(object), g_value_get_object(value));
	} else {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec)
{
	if (prop_id == PROP_HADJUSTMENT) {
		// set to NULL, horizontal scrolling not supported
		g_value_set_object(value, NULL);
	} else if (prop_id == PROP_VADJUSTMENT) {
		g_value_set_object(value, FOCAL_WEEK_VIEW(object)->adj);
	} else if (prop_id == PROP_HSCROLL_POLICY || prop_id == PROP_VSCROLL_POLICY) {
		g_value_set_enum(value, GTK_SCROLL_MINIMUM);
	} else {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void week_view_dispose(GObject* gobject)
{
	WeekView* wv = FOCAL_WEEK_VIEW(gobject);
	g_clear_object(&wv->adj);
}

static void week_view_finalize(GObject* gobject)
{
	WeekView* wv = FOCAL_WEEK_VIEW(gobject);
	icaltimezone_free(wv->current_tz, TRUE);
	g_slist_free(wv->calendars);
	for (int i = 0; i < 7; ++i) {
		EventWidget* ew = wv->events_week[i];
		while (ew) {
			EventWidget* next = ew->next;
			free(ew);
			ew = next;
		}
	}
}

static void week_view_class_init(WeekViewClass* klass)
{
	GObjectClass* goc = (GObjectClass*) klass;

	goc->set_property = set_property;
	goc->get_property = get_property;
	goc->dispose = week_view_dispose;
	goc->finalize = week_view_finalize;
	g_object_class_override_property(goc, PROP_HADJUSTMENT, "hadjustment");
	g_object_class_override_property(goc, PROP_VADJUSTMENT, "vadjustment");
	g_object_class_override_property(goc, PROP_HSCROLL_POLICY, "hscroll-policy");
	g_object_class_override_property(goc, PROP_VSCROLL_POLICY, "vscroll-policy");

	week_view_signals[SIGNAL_EVENT_SELECTED] = g_signal_new("event-selected", G_TYPE_FROM_CLASS(goc), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
	week_view_signals[SIGNAL_DATE_RANGE_CHANGED] = g_signal_new("date-range-changed", G_TYPE_FROM_CLASS(goc), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void on_size_allocate(GtkWidget* widget, GdkRectangle* allocation, gpointer user_data)
{
	WeekView* wv = FOCAL_WEEK_VIEW(widget);
	g_assert_nonnull(wv->adj);

	wv->width = allocation->width;
	wv->height = allocation->height;

	gtk_adjustment_configure(wv->adj,
							 wv->scroll_pos,
							 0,
							 24 * 2 * HALFHOUR_HEIGHT + HEADER_HEIGHT,
							 0.1 * wv->height,
							 0.9 * wv->height,
							 wv->height);
}

static void week_view_init(WeekView* wv)
{
	wv->scroll_pos = 410;

	gtk_widget_set_events((GtkWidget*) wv, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

	g_signal_connect(G_OBJECT(wv), "size-allocate", G_CALLBACK(on_size_allocate), NULL);
	g_signal_connect(G_OBJECT(wv), "draw", G_CALLBACK(on_draw_event), NULL);
	g_signal_connect(G_OBJECT(wv), "button-press-event", G_CALLBACK(on_press_event), NULL);
}

static void update_current_time(WeekView* wv)
{
	struct tm ld;
	time_t now = time(NULL);
	localtime_r(&now, &ld);
	wv->now.minutes = ld.tm_hour * 60 + ld.tm_min;
	wv->now.dow = ld.tm_wday;
}

static gboolean timer_update_current_time(gpointer user_data)
{
	update_current_time(FOCAL_WEEK_VIEW(user_data));
	gtk_widget_queue_draw(GTK_WIDGET(user_data));
	return TRUE;
}

// update the displayed date range based on current week and year
void update_view_span(WeekView* wv)
{
	// based on algorithm from https://en.wikipedia.org/wiki/ISO_week_date
	int wd_4jan = icaltime_day_of_week(icaltime_from_day_of_year(4, wv->current_year));
	int tmp = wv->current_week * 7 + ICAL_SUNDAY_WEEKDAY - (wd_4jan + 3); // First day of week
	if (tmp < 1) {
		tmp += icaltime_days_in_year(--wv->current_year);
	} else if (tmp > icaltime_days_in_year(wv->current_year)) {
		tmp -= icaltime_days_in_year(wv->current_year++);
	}
	icaltimetype start = icaltime_from_day_of_year(tmp, wv->current_year);
	start.hour = 0;
	start.minute = 0;
	start.second = 0;
	start.is_date = FALSE;
	start.zone = wv->current_tz;
	icaltimetype until = start;
	icaltime_adjust(&until, 7, 0, 0, 0);
	wv->current_view = icaltime_span_new(start, until, 0);
}

GtkWidget* week_view_new()
{
	WeekView* cw = g_object_new(FOCAL_TYPE_WEEK_VIEW, NULL);

	char* zoneinfo_link = realpath("/etc/localtime", NULL);
	cw->current_tz = icaltimezone_get_builtin_timezone(zoneinfo_link + strlen("/usr/share/zoneinfo/"));
	free(zoneinfo_link);

	icaltimetype today = icaltime_today();
	cw->current_week = icaltime_week_number(today) + 1;
	cw->current_year = today.year;
	update_view_span(cw);

	update_current_time(cw);
	cw->now.visible = TRUE;
	g_timeout_add_seconds(120, &timer_update_current_time, cw);
	return (GtkWidget*) cw;
}

static void event_widget_set_extents(EventWidget* w, icaltimetype start, struct icaldurationtype dur)
{
	w->minutes_from = start.hour * 60 + start.minute;
	w->minutes_to = (start.hour + dur.hours) * 60 + start.minute + dur.minutes;
}

static void add_event_occurrence(Event* ev, icaltimetype next, struct icaldurationtype duration, gpointer user)
{
	WeekView* cw = FOCAL_WEEK_VIEW(user);

	// crude faster filter
	// TODO what if the week overlaps a year boundary?
	if (next.year < cw->current_year || next.year > cw->current_year)
		return;

	// exact check
	icaltime_span span = icaltime_span_new(next, icaltime_add(next, duration), 0);
	if (icaltime_span_overlaps(&span, &cw->current_view)) {
		int dow = icaltime_day_of_week(next) - 1;
		EventWidget* w = (EventWidget*) malloc(sizeof(EventWidget));
		w->ev = ev;
		event_widget_set_extents(w, next, duration);
		w->next = cw->events_week[dow];
		cw->events_week[dow] = w;
	}
}

static void add_event_from_calendar(gpointer user_data, Event* ev)
{
	WeekView* cw = FOCAL_WEEK_VIEW(user_data);
	event_each_recurrence(ev, cw->current_tz, add_event_occurrence, cw);
}

void week_view_add_event(WeekView* wv, Event* vevent)
{
	add_event_from_calendar(wv, vevent);
	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_remove_event(WeekView* wv, Event* ev)
{
	icaltimetype dtstart = event_get_dtstart(ev);
	const icaltimezone* tz = icaltime_get_timezone(dtstart);
	// convert to local time
	icaltimezone_convert_time(&dtstart, (icaltimezone*) tz, wv->current_tz);
	int dow = icaltime_day_of_week(dtstart) - 1;
	for (EventWidget** ew = &wv->events_week[dow]; *ew; ew = &(*ew)->next) {
		if ((*ew)->ev == ev) {
			EventWidget* next = (*ew)->next;
			free(*ew);
			*ew = next;
			break;
		}
	}
	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_add_calendar(WeekView* wv, Calendar* cal)
{
	wv->calendars = g_slist_append(wv->calendars, cal);
	calendar_each_event(cal, add_event_from_calendar, wv);
	gtk_widget_queue_draw((GtkWidget*) wv);
}

int week_view_get_current_week(WeekView* wv)
{
	return wv->current_week;
}

static int weeks_in_year(int year)
{
	int jan1_dow = icaltime_day_of_week(icaltime_from_day_of_year(1, year));
	if (jan1_dow == ICAL_THURSDAY_WEEKDAY || (jan1_dow == ICAL_WEDNESDAY_WEEKDAY && icaltime_is_leap_year(year)))
		return 53;
	else
		return 52;
}

static void week_view_populate_view(WeekView* wv)
{
	// clear all events
	for (int i = 0; i < 7; ++i) {
		for (EventWidget* p = wv->events_week[i]; p;) {
			EventWidget* next = p->next;
			free(p);
			p = next;
		}
		wv->events_week[i] = NULL;
	}

	update_view_span(wv);

	time_t now = time(NULL);
	icaltime_span icalnow = {now, now, FALSE};
	wv->now.visible = icaltime_span_contains(&icalnow, &wv->current_view);

	for (GSList* p = wv->calendars; p; p = p->next)
		calendar_each_event(FOCAL_CALENDAR(p->data), add_event_from_calendar, wv);

	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_remove_calendar(WeekView* wv, Calendar* cal)
{
	wv->calendars = g_slist_remove(wv->calendars, cal);
	week_view_populate_view(wv);
}

void week_view_previous(WeekView* wv)
{
	if (--wv->current_week == 0)
		wv->current_week = weeks_in_year(--wv->current_year) - 1;
	week_view_populate_view(wv);
	g_signal_emit(wv, week_view_signals[SIGNAL_DATE_RANGE_CHANGED], 0);
}

void week_view_next(WeekView* wv)
{
	wv->current_week = wv->current_week % weeks_in_year(wv->current_year) + 1;
	if (wv->current_week == 1)
		wv->current_year++;
	week_view_populate_view(wv);
	g_signal_emit(wv, week_view_signals[SIGNAL_DATE_RANGE_CHANGED], 0);
}

void week_view_refresh(WeekView* wv, Event* ev)
{
	// find corresponding EventWidget(s), there may be many if it's a recurring event
	for (int i = 0; i < 7; ++i) {
		for (EventWidget** ew = &wv->events_week[i]; *ew; ew = &(*ew)->next) {
			if ((*ew)->ev == ev) {
				// although dtstart may refer to a completely different day in the case
				// of a recurring event, here we assume the hour/minute is consistent and
				// so no need to go through all the recurrence rules again
				icaltimetype dtstart = event_get_dtstart(ev);
				struct icaldurationtype duration = event_get_duration(ev);
				event_widget_set_extents(*ew, dtstart, duration);
			}
		}
	}
	gtk_widget_queue_draw((GtkWidget*) wv);
}

void week_view_focus_event(WeekView* wv, Event* event)
{
	// The event might not be in the current view
	icaltimetype dt = event_get_dtstart(event);
	icaltimetype et = event_get_dtend(event);
	wv->current_week = icaltime_week_number(dt) + 1;
	wv->current_year = dt.year;
	week_view_populate_view(wv);
	g_signal_emit(wv, week_view_signals[SIGNAL_DATE_RANGE_CHANGED], 0);

	GdkRectangle rect;
	rect.width = (wv->width - SIDEBAR_WIDTH) / 7;
	rect.x = (icaltime_day_of_week(dt) - 1) * rect.width + SIDEBAR_WIDTH;
	rect.y = HEADER_HEIGHT + (dt.hour * 60 + dt.minute - wv->scroll_pos) * HALFHOUR_HEIGHT / 30;
	rect.height = (et.hour * 60 + et.minute - dt.hour * 60 - dt.minute) * HALFHOUR_HEIGHT / 30;
	g_signal_emit(wv, week_view_signals[SIGNAL_EVENT_SELECTED], 0, event, &rect);
}
