/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2005-2009 Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Street #330, Boston, MA 02111-1307, USA.
 */

#include <config.h>
#include <gtk/gtk.h>
#include "dlg-personalize-filters.h"
#include "gconf-utils.h"
#include "glib-utils.h"
#include "gtk-utils.h"
#include "gth-browser.h"
#include "gth-browser-actions-callbacks.h"
#include "gth-browser-actions-entries.h"
#include "gth-browser-ui.h"
#include "gth-duplicable.h"
#include "gth-enum-types.h"
#include "gth-file-list.h"
#include "gth-file-view.h"
#include "gth-file-selection.h"
#include "gth-filter.h"
#include "gth-filterbar.h"
#include "gth-folder-tree.h"
#include "gth-icon-cache.h"
#include "gth-image-preloader.h"
#include "gth-location-chooser.h"
#include "gth-main.h"
#include "gth-marshal.h"
#include "gth-metadata-provider.h"
#include "gth-preferences.h"
#include "gth-sidebar.h"
#include "gth-statusbar.h"
#include "gth-viewer-page.h"
#include "gth-window.h"
#include "gth-window-actions-callbacks.h"
#include "gth-window-actions-entries.h"
#include "gthumb-error.h"

#define GTH_BROWSER_CALLBACK(f) ((GthBrowserCallback) (f))
#define GO_BACK_HISTORY_POPUP "/GoBackHistoryPopup"
#define GO_FORWARD_HISTORY_POPUP "/GoForwardHistoryPopup"
#define GO_PARENT_POPUP "/GoParentPopup"
#define MAX_HISTORY_LENGTH 15
#define GCONF_NOTIFICATIONS 9
#define DEF_SIDEBAR_WIDTH 250
#define DEF_PROPERTIES_HEIGHT 128
#define DEF_THUMBNAIL_SIZE 128
#define LOAD_FILE_DELAY 150
#define HIDE_MOUSE_DELAY 1000
#define MOTION_THRESHOLD 0

typedef void (*GthBrowserCallback) (GthBrowser *, gboolean cancelled, gpointer user_data);

typedef enum {
	GTH_ACTION_GO_TO,
	GTH_ACTION_GO_INTO,
	GTH_ACTION_GO_BACK,
	GTH_ACTION_GO_FORWARD,
	GTH_ACTION_GO_UP,
	GTH_ACTION_LIST_CHILDREN,
	GTH_ACTION_VIEW
} GthAction;

enum {
	LOCATION_READY,
	LAST_SIGNAL
};

struct _GthBrowserPrivateData {
	/* UI staff */

	GtkUIManager      *ui;
	GtkActionGroup    *actions;
	GtkWidget         *statusbar;
	GtkWidget         *browser_toolbar;
	GtkWidget         *browser_container;
	GtkWidget         *browser_sidebar;
	GtkWidget         *location_chooser;
	GtkWidget         *folder_tree;
	GtkWidget         *history_list_popup_menu;
	GtkWidget         *folder_popup;
	GtkWidget         *file_list_popup;
	GtkWidget         *filterbar;
	GtkWidget         *file_list;
	GtkWidget         *list_extra_widget_container;
	GtkWidget         *list_extra_widget;
	GtkWidget         *file_properties;

	GtkWidget         *viewer_pane;
	GtkWidget         *viewer_sidebar;
	GtkWidget         *viewer_container;
	GtkWidget         *viewer_toolbar;
	GthViewerPage     *viewer_page;
	GthImagePreloader *image_preloader;

	GHashTable        *named_dialogs;
	GList             *toolbar_menu_buttons[GTH_BROWSER_N_PAGES];

	guint              browser_ui_merge_id;
	guint              viewer_ui_merge_id;

	/* Browser data */

	guint              help_message_cid;
	gulong             bookmarks_changed_id;
	gulong             folder_changed_id;
	gulong             file_renamed_id;
	gulong             metadata_changed_id;
	gulong             entry_points_changed_id;
	GFile             *location;
	GthFileData       *current_file;
	GthFileSource     *location_source;
	gboolean           activity_ref;
	GthIconCache      *menu_icon_cache;
	guint              cnxn_id[GCONF_NOTIFICATIONS];
	GthFileDataSort   *sort_type;
	gboolean           sort_inverse;
	gboolean           show_hidden_files;
	gboolean           fast_file_type;
	gboolean           closing;
	GthTask           *task;
	gulong             task_completed;
	GList             *load_data_queue;
	guint              load_file_timeout;

	/* fulscreen */

	gboolean           fullscreen;
	GtkWidget         *fullscreen_toolbar;
	guint              hide_mouse_timeout;
	guint              motion_signal;
	gdouble            last_mouse_x;
	gdouble            last_mouse_y;

	/* history */

	GList             *history;
	GList             *history_current;
};


static GthWindowClass *parent_class = NULL;
static guint gth_browser_signals[LAST_SIGNAL] = { 0 };
static GList *browser_list = NULL;


/* -- monitor_event_data -- */


typedef struct {
	int              ref;
	GthFileSource   *file_source;
	GFile           *parent;
	GthMonitorEvent  event;
	GthBrowser      *browser;
	gboolean         update_file_list;
	gboolean         update_folder_tree;
} MonitorEventData;


static MonitorEventData *
monitor_event_data_new (void)
{
	MonitorEventData *monitor_data;

	monitor_data = g_new0 (MonitorEventData, 1);
	monitor_data->ref = 1;

	return monitor_data;
}


G_GNUC_UNUSED
static MonitorEventData *
monitor_event_data_ref (MonitorEventData *monitor_data)
{
	monitor_data->ref++;
	return monitor_data;
}


static void
monitor_event_data_unref (MonitorEventData *monitor_data)
{
	monitor_data->ref--;

	if (monitor_data->ref > 0)
		return;

	g_object_unref (monitor_data->file_source);
	g_object_unref (monitor_data->parent);
	g_free (monitor_data);
}


/* -- gth_browser -- */


static void
_gth_browser_set_action_sensitive (GthBrowser  *browser,
				   const char  *action_name,
				   gboolean     sensitive)
{
	GtkAction *action;

	action = gtk_action_group_get_action (browser->priv->actions, action_name);
	g_object_set (action, "sensitive", sensitive, NULL);
}


static void
_gth_browser_set_action_active (GthBrowser  *browser,
				const char  *action_name,
				gboolean     active)
{
	GtkAction *action;

	action = gtk_action_group_get_action (browser->priv->actions, action_name);
	g_object_set (action, "active", active, NULL);
}


static void
activate_go_back_menu_item (GtkMenuItem *menuitem,
			    gpointer     data)
{
	GthBrowser *browser = data;

	gth_browser_go_back (browser, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menuitem), "steps")));
}


static void
activate_go_forward_menu_item (GtkMenuItem *menuitem,
			       gpointer     data)
{
	GthBrowser *browser = data;

	gth_browser_go_forward (browser, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menuitem), "steps")));
}


static void
activate_go_up_menu_item (GtkMenuItem *menuitem,
			  gpointer     data)
{
	GthBrowser *browser = data;

	gth_browser_go_up (browser, GPOINTER_TO_INT (g_object_get_data (G_OBJECT (menuitem), "steps")));
}


static void
activate_go_to_menu_item (GtkMenuItem *menuitem,
			  gpointer     data)
{
	GthBrowser *browser = data;
	GFile      *location;

	location = g_file_new_for_uri (g_object_get_data (G_OBJECT (menuitem), "uri"));
	gth_browser_go_to (browser, location);

	g_object_unref (location);
}


static void
_gth_browser_add_file_menu_item_full (GthBrowser *browser,
				      GtkWidget  *menu,
				      GFile      *file,
				      GIcon      *icon,
				      const char *display_name,
				      GthAction   action,
				      int         steps,
				      int         position)
{
	GdkPixbuf *pixbuf;
	GtkWidget *menu_item;

	pixbuf = gth_icon_cache_get_pixbuf (browser->priv->menu_icon_cache, icon);

	menu_item = gtk_image_menu_item_new_with_label (display_name);
	if (pixbuf != NULL)
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menu_item), gtk_image_new_from_pixbuf (pixbuf));
	else
		gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menu_item), gtk_image_new_from_stock (GTK_STOCK_OPEN, GTK_ICON_SIZE_MENU));
	gtk_widget_show (menu_item);
	if (position == -1)
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);
	else
		gtk_menu_shell_insert (GTK_MENU_SHELL (menu), menu_item, position);

	if (action == GTH_ACTION_GO_TO) {
		g_object_set_data_full (G_OBJECT (menu_item),
					"uri",
					g_file_get_uri (file),
					(GDestroyNotify) g_free);
		g_signal_connect (menu_item,
				  "activate",
				  G_CALLBACK (activate_go_to_menu_item),
			  	  browser);
	}
	else {
		g_object_set_data (G_OBJECT (menu_item),
				   "steps",
				   GINT_TO_POINTER (steps));
		if (action == GTH_ACTION_GO_BACK)
			g_signal_connect (menu_item,
					  "activate",
					  G_CALLBACK (activate_go_back_menu_item),
			  	  	  browser);
		else if (action == GTH_ACTION_GO_FORWARD)
			g_signal_connect (menu_item,
					  "activate",
					  G_CALLBACK (activate_go_forward_menu_item),
			  	  	  browser);
		else if (action == GTH_ACTION_GO_UP)
			g_signal_connect (menu_item,
					  "activate",
					  G_CALLBACK (activate_go_up_menu_item),
			  	  	  browser);
	}

	if (pixbuf != NULL)
		g_object_unref (pixbuf);
}


static void
_gth_browser_add_file_menu_item (GthBrowser *browser,
				 GtkWidget  *menu,
			 	 GFile      *file,
			 	 GthAction   action,
				 int         steps)
{
	GthFileSource *file_source;
	GFileInfo     *info;

	file_source = gth_main_get_file_source (file);
	info = gth_file_source_get_file_info (file_source, file);
	if (info != NULL) {
		_gth_browser_add_file_menu_item_full (browser,
						      menu,
						      file,
						      g_file_info_get_icon (info),
						      g_file_info_get_display_name (info),
						      action,
						      steps,
						      -1);
		g_object_unref (info);
	}
	g_object_unref (file_source);
}


static void
_gth_browser_update_parent_list (GthBrowser *browser)
{
	GtkWidget *menu;
	int        i;
	GFile     *parent;

	menu = gtk_ui_manager_get_widget (browser->priv->ui, GO_PARENT_POPUP);
	_gtk_container_remove_children (GTK_CONTAINER (menu), NULL, NULL);

	if (browser->priv->location == NULL)
		return;

	/* Update the parent list menu. */

	i = 0;
	parent = g_file_get_parent (browser->priv->location);
	while (parent != NULL) {
		GFile *parent_parent;

		_gth_browser_add_file_menu_item (browser,
						 menu,
						 parent,
						 GTH_ACTION_GO_UP,
						 ++i);

		parent_parent = g_file_get_parent (parent);
		g_object_unref (parent);
		parent = parent_parent;
	}
}


void
gth_browser_update_title (GthBrowser *browser)
{
	char    *uri = NULL;
	GString *title;

	switch (gth_window_get_current_page (GTH_WINDOW (browser))) {
	case GTH_BROWSER_PAGE_BROWSER:
		if (browser->priv->location != NULL)
			uri = g_file_get_uri (browser->priv->location);
		break;

	case GTH_BROWSER_PAGE_VIEWER:
		if (browser->priv->current_file != NULL)
			uri = g_file_get_uri (browser->priv->current_file->file);
		break;

	default:
		break;
	}

	title = g_string_new (NULL);
	if (uri != NULL) {
		g_string_append (title, uri);
		if (gth_browser_get_file_modified (browser)) {
			g_string_append (title, " ");
			g_string_append (title, _("[modified]"));
		}
	}
	else
		g_string_append (title, _("gthumb"));

	gtk_window_set_title (GTK_WINDOW (browser), title->str);

	g_string_free (title, TRUE);
}


void
gth_browser_update_sensitivity (GthBrowser *browser)
{
	GFile    *parent;
	gboolean  parent_available;
	gboolean  viewer_can_save;
	gboolean  modified;
	int       current_file_pos;
	int       n_files;
	int       n_selected;

	if (browser->priv->location != NULL)
		parent = g_file_get_parent (browser->priv->location);
	else
		parent = NULL;
	parent_available = (parent != NULL);
	_g_object_unref (parent);

	viewer_can_save = (browser->priv->location != NULL) && (browser->priv->viewer_page != NULL) && gth_viewer_page_can_save (GTH_VIEWER_PAGE (browser->priv->viewer_page));
	modified = gth_browser_get_file_modified (browser);

	if (browser->priv->current_file != NULL)
		current_file_pos = gth_file_store_find_visible (gth_browser_get_file_store (browser), browser->priv->current_file->file);
	else
		current_file_pos = -1;
	n_files = gth_file_store_n_visibles (gth_browser_get_file_store (browser));
	n_selected = gth_file_selection_get_n_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (browser)));

	_gth_browser_set_action_sensitive (browser, "File_Save", viewer_can_save && modified);
	_gth_browser_set_action_sensitive (browser, "File_SaveAs", viewer_can_save);
	_gth_browser_set_action_sensitive (browser, "File_Revert", viewer_can_save && modified);
	_gth_browser_set_action_sensitive (browser, "Go_Up", parent_available);
	_gth_browser_set_action_sensitive (browser, "Toolbar_Go_Up", parent_available);
	_gth_browser_set_action_sensitive (browser, "View_Stop", browser->priv->fullscreen || (browser->priv->activity_ref > 0));
	_gth_browser_set_action_sensitive (browser, "View_Prev", current_file_pos > 0);
	_gth_browser_set_action_sensitive (browser, "View_Next", (current_file_pos != -1) && (current_file_pos < n_files - 1));
	_gth_browser_set_action_sensitive (browser, "Edit_Metadata", n_selected > 0);

	gth_sidebar_update_sensitivity (GTH_SIDEBAR (browser->priv->viewer_sidebar));
	if (browser->priv->viewer_page != NULL)
		gth_viewer_page_update_sensitivity (browser->priv->viewer_page);

	gth_hook_invoke ("gth-browser-update-sensitivity", browser);
}


static void
_gth_browser_set_location (GthBrowser *browser,
			   GFile      *location)
{
	if (location == NULL)
		return;

	if (browser->priv->location != NULL)
		g_object_unref (browser->priv->location);
	browser->priv->location = g_file_dup (location);

	gth_browser_update_title (browser);
	_gth_browser_update_parent_list (browser);
	gth_browser_update_sensitivity (browser);

	g_signal_handlers_block_by_data (browser->priv->location_chooser, browser);
	gth_location_chooser_set_current (GTH_LOCATION_CHOOSER (browser->priv->location_chooser), browser->priv->location);
	g_signal_handlers_unblock_by_data (browser->priv->location_chooser, browser);
}


static void
_gth_browser_update_go_sensitivity (GthBrowser *browser)
{
	gboolean  sensitive;

	sensitive = (browser->priv->history_current != NULL) && (browser->priv->history_current->next != NULL);
	_gth_browser_set_action_sensitive (browser, "Go_Back", sensitive);
	_gth_browser_set_action_sensitive (browser, "Toolbar_Go_Back", sensitive);

	sensitive = (browser->priv->history_current != NULL) && (browser->priv->history_current->prev != NULL);
	_gth_browser_set_action_sensitive (browser, "Go_Forward", sensitive);
	_gth_browser_set_action_sensitive (browser, "Toolbar_Go_Forward", sensitive);
}


static void
activate_clear_history_menu_item (GtkMenuItem *menuitem,
				  gpointer     data)
{
	gth_browser_clear_history ((GthBrowser *)data);
}


static void
_gth_browser_add_clear_history_menu_item (GthBrowser *browser,
					  GtkWidget  *menu)
{
	GtkWidget *menu_item;

	menu_item = gtk_separator_menu_item_new ();
	gtk_widget_show (menu_item);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);

	menu_item = gtk_image_menu_item_new_with_mnemonic (_("_Delete History"));
	gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (menu_item), gtk_image_new_from_stock (GTK_STOCK_CLEAR, GTK_ICON_SIZE_MENU));
	gtk_widget_show (menu_item);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), menu_item);

	g_signal_connect (menu_item,
			  "activate",
			  G_CALLBACK (activate_clear_history_menu_item),
		  	  browser);
}


static void
_gth_browser_update_history_list (GthBrowser *browser)
{
	GtkWidget *menu;
	GList     *scan;
	GtkWidget *separator;

	_gth_browser_update_go_sensitivity (browser);

	/* Update the back history menu. */

	menu = gtk_ui_manager_get_widget (browser->priv->ui, GO_BACK_HISTORY_POPUP);
	_gtk_container_remove_children (GTK_CONTAINER (menu), NULL, NULL);

	if ((browser->priv->history != NULL)
	    && (browser->priv->history_current->next != NULL))
	{
		int i;

		for (i = 0, scan = browser->priv->history_current->next;
		     scan && (i < MAX_HISTORY_LENGTH);
		     scan = scan->next)
		{
			_gth_browser_add_file_menu_item (browser,
							 menu,
							 scan->data,
							 GTH_ACTION_GO_BACK,
							 ++i);
		}
		if (i > 0)
			_gth_browser_add_clear_history_menu_item (browser, menu);
	}

	/* Update the forward history menu. */

	menu = gtk_ui_manager_get_widget (browser->priv->ui, GO_FORWARD_HISTORY_POPUP);
	_gtk_container_remove_children (GTK_CONTAINER (menu), NULL, NULL);

	if ((browser->priv->history != NULL)
	    && (browser->priv->history_current->prev != NULL))
	{
		int i;

		for (i = 0, scan = browser->priv->history_current->prev;
		     scan && (i < MAX_HISTORY_LENGTH);
		     scan = scan->prev)
		{
			_gth_browser_add_file_menu_item (browser,
							 menu,
							 scan->data,
							 GTH_ACTION_GO_FORWARD,
							 ++i);
		}
		if (i > 0)
			_gth_browser_add_clear_history_menu_item (browser, menu);
	}

	/* Update the history list in the go menu */

	separator = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Go/HistoryList");
	menu = gtk_widget_get_parent (separator);

	_gtk_container_remove_children (GTK_CONTAINER (menu), separator, NULL);

	if (browser->priv->history != NULL) {
		int i;

		for (i = 0, scan = browser->priv->history;
		     scan && (i < MAX_HISTORY_LENGTH);
		     scan = scan->next)
		{
			_gth_browser_add_file_menu_item (browser,
							 menu,
							 scan->data,
							 GTH_ACTION_GO_TO,
							 ++i);
		}
	}

	separator = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Go/BeforeHistoryList");
	gtk_widget_show (separator);
}


static void
_gth_browser_add_to_history (GthBrowser *browser,
			     GFile      *file)
{
	if (file == NULL)
		return;

	if ((browser->priv->history_current == NULL) || ! g_file_equal (file, browser->priv->history_current->data)) {
		browser->priv->history = g_list_prepend (browser->priv->history, g_object_ref (file));
		browser->priv->history_current = browser->priv->history;
	}
}


static void
_gth_browser_update_bookmark_list (GthBrowser *browser)
{
	GtkWidget      *menu;
	GtkWidget      *bookmark_list;
	GtkWidget      *bookmark_list_separator;
	GBookmarkFile  *bookmarks;
	char          **uris;
	gsize           length;
	int             i;

	bookmark_list = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Bookmarks/BookmarkList");
	menu = gtk_widget_get_parent (bookmark_list);

	_gtk_container_remove_children (GTK_CONTAINER (menu), bookmark_list, NULL);

	bookmarks = gth_main_get_default_bookmarks ();
	uris = g_bookmark_file_get_uris (bookmarks, &length);

	bookmark_list_separator = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Bookmarks/BookmarkListSeparator");
	if (length > 0)
		gtk_widget_show (bookmark_list_separator);
	else
		gtk_widget_hide (bookmark_list_separator);

	for (i = 0; uris[i] != NULL; i++) {
		GFile *file;

		file = g_file_new_for_uri (uris[i]);
		_gth_browser_add_file_menu_item (browser,
						 menu,
						 file,
						 GTH_ACTION_GO_TO,
						 i);

		g_object_unref (file);
	}

	g_strfreev (uris);
}


static void
_gth_browser_monitor_entry_points (GthBrowser *browser)
{
	GList *scan;

	for (scan = gth_main_get_all_file_sources (); scan; scan = scan->next) {
		GthFileSource *file_source = scan->data;
		gth_file_source_monitor_entry_points (file_source);
	}
}


static void
_gth_browser_update_entry_point_list (GthBrowser *browser)
{
	GtkWidget *separator1;
	GtkWidget *separator2;
	GtkWidget *menu;
	GList     *entry_points;
	GList     *scan;
	int        position;
	GFile     *root;

	separator1 = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Go/BeforeEntryPointList");
	separator2 = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Go/EntryPointList");
	menu = gtk_widget_get_parent (separator1);
	_gtk_container_remove_children (GTK_CONTAINER (menu), separator1, separator2);

	separator1 = separator2;
	separator2 = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar/Go/EntryPointListSeparator");
	_gtk_container_remove_children (GTK_CONTAINER (menu), separator1, separator2);

	position = 5;
	entry_points = gth_main_get_all_entry_points ();
	for (scan = entry_points; scan; scan = scan->next) {
		GthFileData  *file_data = scan->data;

		g_file_info_set_attribute_boolean (file_data->info, "gthumb::entry-point", TRUE);
		g_file_info_set_sort_order (file_data->info, position);
		_gth_browser_add_file_menu_item_full (browser,
						      menu,
						      file_data->file,
						      g_file_info_get_icon (file_data->info),
						      g_file_info_get_display_name (file_data->info),
						      GTH_ACTION_GO_TO,
						      0,
						      position++);
	}
	root = g_file_new_for_uri ("gthumb-vfs:///");
	gth_folder_tree_set_children (GTH_FOLDER_TREE (browser->priv->folder_tree), root, entry_points);

	g_object_unref (root);
	_g_object_list_unref (entry_points);
}


static GthTest *
_gth_browser_get_file_filter (GthBrowser *browser)
{
	GthTest *filterbar_test;
	GthTest *test;

	filterbar_test = gth_filterbar_get_test (GTH_FILTERBAR (browser->priv->filterbar));
	test = gth_main_add_general_filter (filterbar_test);

	_g_object_unref (filterbar_test);

	return test;
}


static void
_gth_browser_update_statusbar_list_info (GthBrowser *browser)
{
	GList *file_list;
	int    n_total;
	gsize  size_total;
	GList *scan;
	int    n_selected;
	gsize  size_selected;
	GList *selected;
	char  *size_total_formatted;
	char  *size_selected_formatted;
	char  *text_total;
	char  *text_selected;
	char  *text;

	/* total */

	file_list = gth_file_store_get_visibles (gth_browser_get_file_store (browser));
	n_total = 0;
	size_total = 0;
	for (scan = file_list; scan; scan = scan->next) {
		GthFileData *file_data = scan->data;

		n_total++;
		size_total += g_file_info_get_size (file_data->info);
	}
	_g_object_list_unref (file_list);

	/* selected */

	selected = gth_file_selection_get_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (browser)));
	file_list = gth_file_list_get_files (GTH_FILE_LIST (gth_browser_get_file_list (browser)), selected);

	n_selected = 0;
	size_selected = 0;
	for (scan = file_list; scan; scan = scan->next) {
		GthFileData *file_data = scan->data;

		n_selected++;
		size_selected += g_file_info_get_size (file_data->info);
	}
	_g_object_list_unref (file_list);
	_gtk_tree_path_list_free (selected);

	/**/

	size_total_formatted = g_format_size_for_display (size_total);
	size_selected_formatted = g_format_size_for_display (size_selected);
	text_total = g_strdup_printf (g_dngettext (NULL, "%d file (%s)", "%d files (%s)", n_total), n_total, size_total_formatted);
	text_selected = g_strdup_printf (g_dngettext (NULL, "%d file (%s)", "%d files (%s)", n_selected), n_selected, size_selected_formatted);
	text = g_strconcat (text_total,
			    ((n_selected == 0) ? NULL : ", "),
			    text_selected,
			    NULL);
	gth_statusbar_set_list_info (GTH_STATUSBAR (browser->priv->statusbar), text);

	g_free (text);
	g_free (text_selected);
	g_free (text_total);
	g_free (size_selected_formatted);
	g_free (size_total_formatted);
}


typedef struct {
	GthBrowser    *browser;
	GFile         *folder;
	GthAction      action;
	GList         *list;
	GList         *current;
	GFile         *entry_point;
	GthFileSource *file_source;
	GCancellable  *cancellable;
} LoadData;


static LoadData *
load_data_new (GthBrowser *browser,
	       GFile      *location,
	       GthAction   action,
	       GFile      *entry_point)
{
	LoadData *load_data;
	GFile    *file;

	load_data = g_new0 (LoadData, 1);
	load_data->browser = browser;
	load_data->folder = g_object_ref (location);
	load_data->action = action;
	load_data->cancellable = g_cancellable_new ();

	if (entry_point == NULL)
		return load_data;

	load_data->entry_point = g_object_ref (entry_point);
	load_data->file_source = gth_main_get_file_source (load_data->folder);

	file = g_object_ref (load_data->folder);
	load_data->list = g_list_prepend (NULL, g_object_ref (file));
	while (! g_file_equal (load_data->entry_point, file)) {
		GFile *parent;

		parent = g_file_get_parent (file);
		g_object_unref (file);
		file = parent;

		load_data->list = g_list_prepend (load_data->list, g_object_ref (file));
	}
	g_object_unref (file);
	load_data->current = NULL;

	browser->priv->load_data_queue = g_list_prepend (browser->priv->load_data_queue, load_data);

	return load_data;
}


static void
load_data_free (LoadData *data)
{
	data->browser->priv->load_data_queue = g_list_remove (data->browser->priv->load_data_queue, data);

	g_object_unref (data->folder);
	_g_object_unref (data->file_source);
	_g_object_list_unref (data->list);
	_g_object_unref (data->entry_point);
	g_object_unref (data->cancellable);
	g_free (data);
}


static void
load_data_cancel (LoadData *data)
{
	if (data->file_source != NULL)
		gth_file_source_cancel (data->file_source);
	g_cancellable_cancel (data->cancellable);
}


static void
load_data_done (LoadData *data,
		GError   *error)
{
	GthBrowser *browser = data->browser;

	{
		char *uri;

		uri = g_file_get_uri (data->folder);
		debug (DEBUG_INFO, "LOAD READY: %s [%s]\n", uri, (error == NULL ? "Ok" : "Error"));
		performance (DEBUG_INFO, "load done for %s", uri);

		g_free (uri);
	}

	browser->priv->activity_ref--;
	g_signal_emit (G_OBJECT (browser),
		       gth_browser_signals[LOCATION_READY],
		       0,
		       data->folder,
		       (error != NULL));

	if (error == NULL) {
		_g_object_unref (browser->priv->location_source);
		browser->priv->location_source = g_object_ref (data->file_source);
	}

	gth_hook_invoke ("gth-browser-load-location-after", browser, data->folder, error);

	if (error == NULL)
		return;

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_error_free (error);
		return;
	}

#if 0
	/* update the folder list */

	switch (data->action) {
	case GTH_ACTION_LIST_CHILDREN:
		gth_folder_tree_set_children (GTH_FOLDER_TREE (browser->priv->folder_tree), data->folder, NULL);
		break;
	default:
		break;
	}

	/* update the file list */

	switch (data->action) {
	case GTH_ACTION_VIEW:
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
	case GTH_ACTION_GO_INTO:
	case GTH_ACTION_GO_TO:
		gth_file_list_set_files (GTH_FILE_LIST (browser->priv->file_list), NULL, NULL);
		break;
	default:
		break;
	}
#endif

	gth_browser_update_sensitivity (browser);
	_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not load the position"), &error);
}


static void _gth_browser_load_ready_cb (GthFileSource *file_source, GList *files, GError *error, gpointer user_data);


static void
load_data_load_next_folder (LoadData *data)
{
	GthFolderTree *folder_tree;
	GFile         *folder_to_load;

	folder_tree = GTH_FOLDER_TREE (data->browser->priv->folder_tree);
	do {
		GtkTreePath *path;

		if (data->current == NULL)
			data->current = data->list;
		else
			data->current = data->current->next;

		folder_to_load = (GFile *) data->current->data;

		if (g_file_equal (folder_to_load, data->folder))
			break;

		path = gth_folder_tree_get_path (folder_tree, folder_to_load);
		if (path == NULL)
			break;

		if (! gth_folder_tree_is_loaded (folder_tree, path)) {
			gtk_tree_path_free (path);
			break;
		}

		if (! g_file_equal (folder_to_load, data->folder))
			gth_folder_tree_expand_row (folder_tree, path, FALSE);

		gtk_tree_path_free (path);
	}
	while (TRUE);

	gth_file_source_list (data->file_source,
			      folder_to_load,
			      eel_gconf_get_boolean (PREF_FAST_FILE_TYPE, TRUE) ? GTH_FILE_DATA_ATTRIBUTES_WITH_FAST_CONTENT_TYPE : GTH_FILE_DATA_ATTRIBUTES_WITH_CONTENT_TYPE,
			      _gth_browser_load_ready_cb,
			      data);
}


static void
load_data_continue (LoadData *data,
		    GList    *loaded_files)
{
	GthBrowser  *browser = data->browser;
	GList       *files;
	GFile       *loaded_folder;
	GtkTreePath *path;
	GthTest     *filter;

	if ((data->action != GTH_ACTION_LIST_CHILDREN)
	    && ! g_file_equal (data->folder, data->browser->priv->location))
	{
		load_data_done (data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, ""));
		load_data_free (data);
		return;
	}

	if (! browser->priv->show_hidden_files) {
		GList *scan;

		files = NULL;
		for (scan = loaded_files; scan; scan = scan->next) {
			GthFileData *file_data = scan->data;

			if (! g_file_info_get_is_hidden (file_data->info))
				files = g_list_prepend (files, file_data);
		}
		files = g_list_reverse (files);
	}
	else
		files = g_list_copy (loaded_files);

	loaded_folder = (GFile *) data->current->data;
	gth_folder_tree_set_children (GTH_FOLDER_TREE (browser->priv->folder_tree), loaded_folder, files);
	path = gth_folder_tree_get_path (GTH_FOLDER_TREE (browser->priv->folder_tree), loaded_folder);
	if ((path != NULL) && ! g_file_equal (loaded_folder, data->folder))
		gth_folder_tree_expand_row (GTH_FOLDER_TREE (browser->priv->folder_tree), path, FALSE);

	if (! g_file_equal (loaded_folder, data->folder)) {
		gtk_tree_path_free (path);
		g_list_free (files);

		load_data_load_next_folder (data);
		return;
	}

	load_data_done (data, NULL);

	switch (data->action) {
	case GTH_ACTION_VIEW:
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
	case GTH_ACTION_GO_TO:
		if (path != NULL) {
			gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (browser->priv->folder_tree), path, NULL, FALSE, .0, .0);
			gth_folder_tree_select_path (GTH_FOLDER_TREE (browser->priv->folder_tree), path);
		}
		break;
	default:
		break;
	}

	switch (data->action) {
	case GTH_ACTION_VIEW:
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
	case GTH_ACTION_GO_INTO:
	case GTH_ACTION_GO_TO:
		filter = _gth_browser_get_file_filter (browser);
		gth_file_list_set_filter (GTH_FILE_LIST (browser->priv->file_list), filter);
		gth_file_list_set_files (GTH_FILE_LIST (browser->priv->file_list), data->file_source, files);
		g_object_unref (filter);
		break;
	default:
		break;
	}

	gth_browser_update_sensitivity (browser);
	_gth_browser_update_statusbar_list_info (browser);

	gth_file_source_monitor_directory (browser->priv->location_source,
					   browser->priv->location,
					   TRUE);

	if (path != NULL)
		gtk_tree_path_free (path);
	load_data_free (data);
	g_list_free (files);
}


static void
metadata_ready_cb (GList    *files,
		   GError   *error,
		   gpointer  user_data)
{
	LoadData *load_data = user_data;

	if (error != NULL) {
		load_data_done (load_data, error);
		load_data_free (load_data);
		return;
	}

	load_data_continue (load_data, files);
}


static void
load_data_ready (LoadData *data,
		 GList    *files,
		 GError   *error)
{
	if (error != NULL) {
		load_data_done (data, error);
		load_data_free (data);
	}
	else if (g_file_equal ((GFile *) data->current->data, data->folder))
		/* FIXME: make the metadata attribute list automatic, based
		 * on the data required to filter and view the file list. */
		_g_query_metadata_async (files,
					 "file::*,comment::*",
					 data->cancellable,
					 metadata_ready_cb,
				 	 data);
	else
		load_data_continue (data, files);
}


static void
_gth_browser_load_ready_cb (GthFileSource *file_source,
			    GList         *files,
			    GError        *error,
			    gpointer       user_data)
{
	load_data_ready ((LoadData *) user_data, files, error);
}


#if 0
static void
_gth_browser_print_history (GthBrowser *browser)
{
	GList *scan;

	g_print ("history:\n");
	for (scan = browser->priv->history; scan; scan = scan->next) {
		GFile *file = scan->data;
		char  *uri;

		uri = g_file_get_uri (file);
		g_print (" %s%s\n", (browser->priv->history_current == scan) ? "*" : " ", uri);

		g_free (uri);
	}
}
#endif


static void
_gth_browser_cancel (GthBrowser *browser)
{
	if (browser->priv->load_file_timeout != 0) {
		g_source_remove (browser->priv->load_file_timeout);
		browser->priv->load_file_timeout = 0;
	}

	g_list_foreach (browser->priv->load_data_queue, (GFunc) load_data_cancel, NULL);
}


static GFile *
get_nearest_entry_point (GFile *file)
{
	GList *list;
	GList *scan;
	GList *entries;
	char  *nearest_uri;
	int    max_len;
	GFile *nearest;

	entries = NULL;
	list = gth_main_get_all_entry_points ();
	for (scan = list; scan; scan = scan->next) {
		GthFileData *entry_point = scan->data;

		if (g_file_equal (file, entry_point->file) || g_file_has_prefix (file, entry_point->file))
			entries = g_list_prepend (entries, g_file_get_uri (entry_point->file));
	}

	nearest_uri = NULL;
	max_len = 0;
	for (scan = entries; scan; scan = scan->next) {
		char *entry_uri = scan->data;
		int   entry_len = strlen (entry_uri);

		if (entry_len > max_len) {
			nearest_uri = entry_uri;
			max_len = entry_len;
		}
	}

	nearest = NULL;
	if (nearest_uri != NULL)
		nearest = g_file_new_for_uri (nearest_uri);

	_g_string_list_free (entries);
	_g_object_list_unref (list);

	return nearest;
}


static void
_gth_browser_load (GthBrowser *browser,
		   GFile      *location,
		   GthAction   action)
{
	LoadData *load_data;
	GFile    *entry_point;

	_gth_browser_cancel (browser);

	switch (action) {
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
	case GTH_ACTION_GO_TO:
		if (browser->priv->location_source != NULL) {
			gth_file_source_monitor_directory (browser->priv->location_source,
							   browser->priv->location,
							   FALSE);
			_g_object_unref (browser->priv->location_source);
			browser->priv->location_source = NULL;
		}
		break;
	default:
		break;
	}

	entry_point = get_nearest_entry_point (location);
	load_data = load_data_new (browser, location, action, entry_point);

	gth_hook_invoke ("gth-browser-load-location-before", browser, load_data->folder);
	browser->priv->activity_ref++;

	if (entry_point == NULL) {
		GError *error;
		char   *uri;

		uri = g_file_get_uri (location);
		error = g_error_new (GTHUMB_ERROR, 0, _("No suitable module found for %s"), uri);
		load_data_ready (load_data, NULL, error);

		g_free (uri);

		return;
	}

	switch (action) {
	case GTH_ACTION_LIST_CHILDREN:
		gth_folder_tree_loading_children (GTH_FOLDER_TREE (browser->priv->folder_tree), location);
		break;
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
	case GTH_ACTION_GO_TO:
	case GTH_ACTION_VIEW:
		gth_file_list_clear (GTH_FILE_LIST (browser->priv->file_list), _("Getting folder listing..."));
		break;
	default:
		break;
	}

	if (load_data->file_source == NULL) {
		GError *error;
		char   *uri;

		uri = g_file_get_uri (load_data->folder);
		error = g_error_new (GTHUMB_ERROR, 0, _("No suitable module found for %s"), uri);
		load_data_ready (load_data, NULL, error);

		g_free (uri);

		return;
	}

	switch (load_data->action) {
	case GTH_ACTION_GO_INTO:
	case GTH_ACTION_GO_TO:
		_gth_browser_set_location (browser, load_data->folder);
		_gth_browser_add_to_history (browser, browser->priv->location);
		_gth_browser_update_history_list (browser);
		break;
	case GTH_ACTION_GO_BACK:
	case GTH_ACTION_GO_FORWARD:
		_gth_browser_set_location (browser, load_data->folder);
		_gth_browser_update_history_list (browser);
		break;
	case GTH_ACTION_VIEW:
		_gth_browser_set_location (browser, load_data->folder);
		_gth_browser_add_to_history (browser, browser->priv->location);
		_gth_browser_update_history_list (browser);
		break;
	default:
		break;
	}

	{
		char *uri;

		uri = g_file_get_uri (load_data->folder);

		debug (DEBUG_INFO, "LOAD: %s\n", uri);
		performance (DEBUG_INFO, "loading %s", uri);

		g_free (uri);
	}

	gth_browser_update_sensitivity (browser);
	gth_browser_set_list_extra_widget (browser, NULL);
	load_data_load_next_folder (load_data);

	g_object_unref (entry_point);
}


/* -- _gth_browser_ask_whether_to_save -- */


typedef struct {
	GthBrowser         *browser;
	GthBrowserCallback  callback;
	gpointer            user_data;
} AskSaveData;


static void
ask_whether_to_save__done (AskSaveData *data,
			   gboolean     cancelled)
{
	if (cancelled)
		g_file_info_set_attribute_boolean (data->browser->priv->current_file->info, "file::is-modified", TRUE);
	if (data->callback != NULL)
		(*data->callback) (data->browser, cancelled, data->user_data);
	g_free (data);
}


static void
ask_whether_to_save__file_saved_cb (GthViewerPage *viewer_page,
				    GthFileData   *file_data,
				    GError        *error,
				    gpointer       user_data)
{
	AskSaveData *data = user_data;
	gboolean     error_occurred;

	error_occurred = error != NULL;
	if (error != NULL)
		_gtk_error_dialog_from_gerror_show (GTK_WINDOW (data->browser), _("Could not save the file"), &error);
	ask_whether_to_save__done (data, error_occurred);
}


enum {
	RESPONSE_SAVE,
	RESPONSE_NO_SAVE,
};


static void
ask_whether_to_save__response_cb (GtkWidget   *dialog,
				  int          response_id,
				  AskSaveData *data)
{
	gtk_widget_destroy (dialog);

	if (response_id == RESPONSE_SAVE)
		gth_viewer_page_save (data->browser->priv->viewer_page,
				      NULL,
				      ask_whether_to_save__file_saved_cb,
				      data);
	else
		ask_whether_to_save__done (data, response_id == GTK_RESPONSE_CANCEL);
}


static void
_gth_browser_ask_whether_to_save (GthBrowser         *browser,
				  GthBrowserCallback  callback,
				  gpointer            user_data)
{
	AskSaveData *data;
	char        *title;
	GtkWidget   *d;

	data = g_new0 (AskSaveData, 1);
	data->browser = browser;
	data->callback = callback;
	data->user_data = user_data;

	title = g_strdup_printf (_("Save changes to file '%s'?"), g_file_info_get_display_name (browser->priv->current_file->info));
	d = _gtk_message_dialog_new (GTK_WINDOW (browser),
				     GTK_DIALOG_MODAL,
				     GTK_STOCK_DIALOG_QUESTION,
				     title,
				     _("If you don't save, changes to the file will be permanently lost."),
				     _("Do _Not Save"), RESPONSE_NO_SAVE,
				     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				     GTK_STOCK_SAVE, RESPONSE_SAVE,
				     NULL);
	g_signal_connect (G_OBJECT (d),
			  "response",
			  G_CALLBACK (ask_whether_to_save__response_cb),
			  data);
	gtk_widget_show (d);

	g_free (title);
}


/* -- _gth_browser_close -- */


static void
_gth_browser_close_final_step (gpointer user_data)
{
	GthBrowser      *browser = user_data;
	gboolean        last_window;
	GdkWindowState  state;
	gboolean        maximized;

	browser_list = g_list_remove (browser_list, browser);
	last_window = gth_window_get_n_windows () == 1;

	/* Save visualization options only if the window is not maximized. */

	state = gdk_window_get_state (GTK_WIDGET (browser)->window);
	maximized = (state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
	if (! maximized && GTK_WIDGET_VISIBLE (browser)) {
		int width, height;

		gdk_drawable_get_size (GTK_WIDGET (browser)->window, &width, &height);
		eel_gconf_set_integer (PREF_UI_WINDOW_WIDTH, width);
		eel_gconf_set_integer (PREF_UI_WINDOW_HEIGHT, height);
	}

	eel_gconf_set_integer (PREF_UI_BROWSER_SIDEBAR_WIDTH, gtk_paned_get_position (GTK_PANED (browser->priv->browser_container)));
	eel_gconf_set_integer (PREF_UI_VIEWER_SIDEBAR_WIDTH, gtk_paned_get_position (GTK_PANED (browser->priv->viewer_pane)));
	eel_gconf_set_integer (PREF_UI_PROPERTIES_HEIGHT, gtk_paned_get_position (GTK_PANED (browser->priv->browser_sidebar)));

	/**/

	gth_hook_invoke ("gth-browser-close", browser);

	if (last_window) {
		if (eel_gconf_get_boolean (PREF_GO_TO_LAST_LOCATION, TRUE)
		    && (browser->priv->location != NULL))
		{
			char *uri;

			uri = g_file_get_uri (browser->priv->location);
			eel_gconf_set_path (PREF_STARTUP_LOCATION, uri);

			g_free (uri);
		}

		eel_gconf_set_string (PREF_SORT_TYPE, browser->priv->sort_type->name);
		eel_gconf_set_boolean (PREF_SORT_INVERSE, browser->priv->sort_inverse);

		gth_hook_invoke ("gth-browser-close-last-window", browser);
	}

	if (browser->priv->folder_popup != NULL)
		gtk_widget_destroy (browser->priv->folder_popup);
	if (browser->priv->file_list_popup != NULL)
		gtk_widget_destroy (browser->priv->file_list_popup);

	gtk_widget_destroy (GTK_WIDGET (browser));
}


static void
_gth_browser_close_step3 (gpointer user_data)
{
	GthBrowser *browser = user_data;

	gth_file_list_cancel (GTH_FILE_LIST (browser->priv->file_list), _gth_browser_close_final_step, browser);
}


static void
_gth_browser_real_close (GthBrowser *browser)
{
	int i;

	/* remove gconf notifications */

	for (i = 0; i < GCONF_NOTIFICATIONS; i++)
		if (browser->priv->cnxn_id[i] != 0)
			eel_gconf_notification_remove (browser->priv->cnxn_id[i]);

	/* disconnect from the monitor */

	g_signal_handler_disconnect (gth_main_get_default_monitor (),
				     browser->priv->bookmarks_changed_id);
	g_signal_handler_disconnect (gth_main_get_default_monitor (),
				     browser->priv->folder_changed_id);
	g_signal_handler_disconnect (gth_main_get_default_monitor (),
				     browser->priv->file_renamed_id);
	g_signal_handler_disconnect (gth_main_get_default_monitor (),
				     browser->priv->metadata_changed_id);
	g_signal_handler_disconnect (gth_main_get_default_monitor (),
				     browser->priv->entry_points_changed_id);

	/* cancel async operations */

	browser->priv->closing = TRUE;

	_gth_browser_cancel (browser);

	if ((browser->priv->task != NULL) && gth_task_is_running (browser->priv->task))
		gth_task_cancel (browser->priv->task);
	else
		_gth_browser_close_step3 (browser);
}


static void
close__file_saved_cb (GthBrowser *browser,
		      gboolean    cancelled,
		      gpointer    user_data)
{
	if (! cancelled)
		_gth_browser_real_close (browser);
}


static void
_gth_browser_close (GthWindow *window)
{
	GthBrowser *browser = (GthBrowser *) window;

	if (gth_browser_get_file_modified (browser))
		_gth_browser_ask_whether_to_save (browser,
						  close__file_saved_cb,
						  NULL);
	else
		_gth_browser_real_close (browser);
}


static void
_gth_browser_update_viewer_ui (GthBrowser *browser,
			       int         page)
{
	if (page == GTH_BROWSER_PAGE_VIEWER) {
		GError *error = NULL;

		if (browser->priv->viewer_ui_merge_id != 0)
			return;
		browser->priv->viewer_ui_merge_id = gtk_ui_manager_add_ui_from_string (browser->priv->ui, viewer_ui_info, -1, &error);
		if (browser->priv->viewer_ui_merge_id == 0) {
			g_warning ("ui building failed: %s", error->message);
			g_clear_error (&error);
		}
	}
	else if (browser->priv->viewer_ui_merge_id != 0) {
		gtk_ui_manager_remove_ui (browser->priv->ui, browser->priv->viewer_ui_merge_id);
		browser->priv->viewer_ui_merge_id = 0;
	}

	if (browser->priv->viewer_page != NULL) {
		if (page == GTH_BROWSER_PAGE_VIEWER)
			gth_viewer_page_show (browser->priv->viewer_page);
		else
			gth_viewer_page_hide (browser->priv->viewer_page);
	}
}


static void
_gth_browser_update_browser_ui (GthBrowser *browser,
				int         page)
{
	if (page == GTH_BROWSER_PAGE_BROWSER) {
		GError *error = NULL;

		if (browser->priv->browser_ui_merge_id != 0)
			return;
		browser->priv->browser_ui_merge_id = gtk_ui_manager_add_ui_from_string (browser->priv->ui, browser_ui_info, -1, &error);
		if (browser->priv->browser_ui_merge_id == 0) {
			g_warning ("ui building failed: %s", error->message);
			g_clear_error (&error);
		}
	}
	else if (browser->priv->browser_ui_merge_id != 0) {
		gtk_ui_manager_remove_ui (browser->priv->ui, browser->priv->browser_ui_merge_id);
		browser->priv->browser_ui_merge_id = 0;
	}
}


static void
_gth_browser_set_current_page (GthWindow *window,
			       int        page)
{
	GthBrowser *browser = (GthBrowser *) window;

	GTH_WINDOW_CLASS (parent_class)->set_current_page (window, page);

	_gth_browser_update_viewer_ui (browser, page);
	_gth_browser_update_browser_ui (browser, page);

	gth_hook_invoke ("gth-browser-set-current-page", browser);

	gth_browser_update_title (browser);
}


static void
gth_browser_init (GthBrowser *browser)
{
	int i;

	browser->priv = g_new0 (GthBrowserPrivateData, 1);
	browser->priv->menu_icon_cache = gth_icon_cache_new_for_widget (GTK_WIDGET (browser), GTK_ICON_SIZE_MENU);
	browser->priv->named_dialogs = g_hash_table_new (g_str_hash, g_str_equal);

	for (i = 0; i < GCONF_NOTIFICATIONS; i++)
		browser->priv->cnxn_id[i] = 0;
}


static void
gth_browser_finalize (GObject *object)
{
	GthBrowser *browser = GTH_BROWSER (object);

	if (browser->priv != NULL) {
		_g_object_unref (browser->priv->location_source);
		_g_object_unref (browser->priv->location);
		_g_object_unref (browser->priv->current_file);
		_g_object_unref (browser->priv->viewer_page);
		_g_object_unref (browser->priv->image_preloader);
		_g_object_list_unref (browser->priv->history);
		gth_icon_cache_free (browser->priv->menu_icon_cache);
		g_hash_table_unref (browser->priv->named_dialogs);
		g_free (browser->priv);
		browser->priv = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gth_browser_class_init (GthBrowserClass *class)
{
	GObjectClass   *gobject_class;
	GthWindowClass *window_class;

	parent_class = g_type_class_peek_parent (class);

	gobject_class = G_OBJECT_CLASS (class);
	gobject_class->finalize = gth_browser_finalize;

	window_class = GTH_WINDOW_CLASS (class);
	window_class->close = _gth_browser_close;
	window_class->set_current_page = _gth_browser_set_current_page;

	/* signals */

	gth_browser_signals[LOCATION_READY] =
		g_signal_new ("location-ready",
			      G_TYPE_FROM_CLASS (class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GthBrowserClass, location_ready),
			      NULL, NULL,
			      gth_marshal_VOID__OBJECT_BOOLEAN,
			      G_TYPE_NONE,
			      2,
			      G_TYPE_OBJECT,
			      G_TYPE_BOOLEAN);
}


GType
gth_browser_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo type_info = {
			sizeof (GthBrowserClass),
			NULL,
			NULL,
			(GClassInitFunc) gth_browser_class_init,
			NULL,
			NULL,
			sizeof (GthBrowser),
			0,
			(GInstanceInitFunc) gth_browser_init
		};

		type = g_type_register_static (GTH_TYPE_WINDOW,
					       "GthBrowser",
					       &type_info,
					       0);
	}

	return type;
}


static void
menu_item_select_cb (GtkMenuItem *proxy,
		     GthBrowser  *browser)
{
	GtkAction *action;
	char      *message;

	action = g_object_get_data (G_OBJECT (proxy),  "gtk-action");
	g_return_if_fail (action != NULL);

	g_object_get (G_OBJECT (action), "tooltip", &message, NULL);
	if (message != NULL) {
		gtk_statusbar_push (GTK_STATUSBAR (browser->priv->statusbar),
				    browser->priv->help_message_cid,
				    message);
		g_free (message);
	}
}


static void
menu_item_deselect_cb (GtkMenuItem *proxy,
		       GthBrowser  *browser)
{
	gtk_statusbar_pop (GTK_STATUSBAR (browser->priv->statusbar),
			   browser->priv->help_message_cid);
}


static void
disconnect_proxy_cb (GtkUIManager *manager,
		     GtkAction    *action,
		     GtkWidget    *proxy,
		     GthBrowser   *browser)
{
	if (GTK_IS_MENU_ITEM (proxy)) {
		g_signal_handlers_disconnect_by_func
			(proxy, G_CALLBACK (menu_item_select_cb), browser);
		g_signal_handlers_disconnect_by_func
			(proxy, G_CALLBACK (menu_item_deselect_cb), browser);
	}
}


static void
connect_proxy_cb (GtkUIManager *manager,
		  GtkAction    *action,
		  GtkWidget    *proxy,
		  GthBrowser   *browser)
{
	if (GTK_IS_MENU_ITEM (proxy)) {
		g_signal_connect (proxy, "select",
				  G_CALLBACK (menu_item_select_cb), browser);
		g_signal_connect (proxy, "deselect",
				  G_CALLBACK (menu_item_deselect_cb), browser);
	}
}


static void
folder_tree_open_cb (GthFolderTree *folder_tree,
		     GFile         *file,
		     GthBrowser    *browser)
{
	gth_browser_go_to (browser, file);
}


static void
folder_tree_open_parent_cb (GthFolderTree *folder_tree,
			    GthBrowser    *browser)
{
	gth_browser_go_up (browser, 1);
}


static void
folder_tree_list_children_cb (GthFolderTree *folder_tree,
			      GFile         *file,
			      GthBrowser    *browser)
{
	_gth_browser_load (browser, file, GTH_ACTION_LIST_CHILDREN);
}


static void
folder_tree_load_cb (GthFolderTree *folder_tree,
		     GFile         *file,
		     GthBrowser    *browser)
{
	_gth_browser_load (browser, file, GTH_ACTION_VIEW);
}


static void
folder_tree_folder_popup_cb (GthFolderTree *folder_tree,
			     GFile         *file,
			     guint          time,
			     gpointer       user_data)
{
	GthBrowser    *browser = user_data;
	gboolean       sensitive;
	GthFileSource *file_source;

	sensitive = (file != NULL);
	_gth_browser_set_action_sensitive (browser, "Folder_Open", sensitive);
	_gth_browser_set_action_sensitive (browser, "Folder_OpenInNewWindow", sensitive);

	if (file != NULL)
		file_source = gth_main_get_file_source (file);
	else
		file_source = NULL;
	gth_hook_invoke ("gth-browser-folder-tree-popup-before", browser, file_source, file);
	gtk_ui_manager_ensure_update (browser->priv->ui);

	gtk_menu_popup (GTK_MENU (browser->priv->folder_popup),
			NULL,
			NULL,
			NULL,
			NULL,
			3,
			(guint32) time);

	_g_object_unref (file_source);
}


static void
file_source_rename_ready_cb (GObject  *object,
			     GError   *error,
			     gpointer  user_data)
{
	GthBrowser *browser = user_data;

	g_object_unref (object);

	if (error != NULL) {
		_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not change name"), &error);
		return;
	}
}


static void
folder_tree_rename_cb (GthFolderTree *folder_tree,
		       GFile         *file,
		       const char    *new_name,
		       GthBrowser    *browser)
{
	GFile  *parent;
	char   *uri;
	char   *new_basename;
	GFile  *new_file;
	GError *error = NULL;

	parent = g_file_get_parent (file);
	uri = g_file_get_uri (file);
	new_basename = g_strconcat (new_name, _g_uri_get_file_extension (uri), NULL);
	new_file = g_file_get_child_for_display_name (parent, new_basename, &error);

	if (new_file == NULL)
		_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not change name"), &error);
	else {
		GthFileSource *file_source;

		file_source = gth_main_get_file_source (file);
		gth_file_source_rename (file_source, file, new_file, file_source_rename_ready_cb, browser);
	}

	g_object_unref (new_file);
	g_free (new_basename);
	g_free (uri);
	g_object_unref (parent);
}


static void
location_changed_cb (GthLocationChooser *chooser,
		     GthBrowser         *browser)
{
	gth_browser_go_to (browser, gth_location_chooser_get_current (chooser));
}


static void
filterbar_changed_cb (GthFilterbar *filterbar,
		      GthBrowser   *browser)
{
	GthTest *filter;

	filter = _gth_browser_get_file_filter (browser);
	gth_file_list_set_filter (GTH_FILE_LIST (browser->priv->file_list), filter);
	g_object_unref (filter);

	_gth_browser_update_statusbar_list_info (browser);
	gth_browser_update_sensitivity (browser);
}


static void
filterbar_personalize_cb (GthFilterbar *filterbar,
			  GthBrowser   *browser)
{
	dlg_personalize_filters (browser);
}


static void
bookmarks_changed_cb (GthMonitor *monitor,
		      GthBrowser *browser)
{
	_gth_browser_update_bookmark_list (browser);
}


static void
file_attributes_ready_cb (GthFileSource *file_source,
			  GList         *files,
			  GError        *error,
			  gpointer       user_data)
{
	MonitorEventData *monitor_data = user_data;
	GthBrowser       *browser = monitor_data->browser;

	if (error != NULL) {
		monitor_event_data_unref (monitor_data);
		g_clear_error (&error);
		return;
	}

	if (monitor_data->event == GTH_MONITOR_EVENT_CREATED) {
		if (monitor_data->update_folder_tree)
			gth_folder_tree_add_children (GTH_FOLDER_TREE (browser->priv->folder_tree), monitor_data->parent, files);
		if (monitor_data->update_file_list) {
			gth_file_list_add_files (GTH_FILE_LIST (browser->priv->file_list), files);
			gth_file_list_update_files (GTH_FILE_LIST (browser->priv->file_list), files);
		}
	}
	else if (monitor_data->event == GTH_MONITOR_EVENT_CHANGED) {
		if (monitor_data->update_folder_tree)
			gth_folder_tree_update_children (GTH_FOLDER_TREE (browser->priv->folder_tree), monitor_data->parent, files);
		if (monitor_data->update_file_list)
			gth_file_list_update_files (GTH_FILE_LIST (browser->priv->file_list), files);
	}

	if (browser->priv->current_file != NULL) {
		GList *link;

		link = gth_file_data_list_find_file (files, browser->priv->current_file->file);
		if (link != NULL) {
			GthFileData *file_data = link->data;
			gth_browser_load_file (browser, file_data, FALSE);
		}
	}

	_gth_browser_update_statusbar_list_info (browser);
	gth_browser_update_sensitivity (browser);

	monitor_event_data_unref (monitor_data);
}


static void
folder_changed_cb (GthMonitor      *monitor,
		   GFile           *parent,
		   GList           *list,
		   GthMonitorEvent  event,
		   GthBrowser      *browser)
{
	GtkTreePath *path;
	gboolean     update_folder_tree;
	gboolean     update_file_list;

	if ((event == GTH_MONITOR_EVENT_DELETED) && (_g_file_list_find_file (list, browser->priv->location) != NULL)) {
		gth_browser_go_to (browser, parent);
		return;
	}

#if 0
{
	GList *scan;
	g_print ("folder changed: %s [%s]\n", g_file_get_uri (parent), _g_enum_type_get_value (GTH_TYPE_MONITOR_EVENT, event)->value_nick);
	for (scan = list; scan; scan = scan->next)
		g_print ("   %s\n", g_file_get_uri (scan->data));
}
#endif

	path = gth_folder_tree_get_path (GTH_FOLDER_TREE (browser->priv->folder_tree), parent);
	update_folder_tree = (g_file_equal (parent, gth_folder_tree_get_root (GTH_FOLDER_TREE (browser->priv->folder_tree)))
			      || ((path != NULL) && gtk_tree_view_row_expanded (GTK_TREE_VIEW (browser->priv->folder_tree), path)));

	update_file_list = g_file_equal (parent, browser->priv->location);
	if (! update_file_list && (event == GTH_MONITOR_EVENT_CHANGED)) {
		GthFileStore *file_store;
		GList        *scan;

		file_store = (GthFileStore *) gth_file_view_get_model (GTH_FILE_VIEW (gth_file_list_get_view (GTH_FILE_LIST (browser->priv->file_list))));
		for (scan = list; scan; scan = scan->next) {
			if (gth_file_store_find (file_store, (GFile *) scan->data) >= 0) {
				update_file_list = TRUE;
				break;
			}
		}
	}

	if (update_folder_tree || update_file_list) {
		MonitorEventData *monitor_data;
		gboolean          current_file_deleted = FALSE;
		GthFileData      *new_file = NULL;

		switch (event) {
		case GTH_MONITOR_EVENT_CREATED:
		case GTH_MONITOR_EVENT_CHANGED:
			monitor_data = monitor_event_data_new ();
			monitor_data->file_source = gth_main_get_file_source (parent);
			monitor_data->parent = g_file_dup (parent);
			monitor_data->event = event;
			monitor_data->browser = browser;
			monitor_data->update_file_list = update_file_list;
			monitor_data->update_folder_tree = update_folder_tree;
			gth_file_source_read_attributes (monitor_data->file_source,
						 	 list,
						 	 eel_gconf_get_boolean (PREF_FAST_FILE_TYPE, TRUE) ? GTH_FILE_DATA_ATTRIBUTES_WITH_FAST_CONTENT_TYPE : GTH_FILE_DATA_ATTRIBUTES_WITH_CONTENT_TYPE,
						 	 file_attributes_ready_cb,
						 	 monitor_data);
			break;

		case GTH_MONITOR_EVENT_DELETED:
			if (browser->priv->current_file != NULL) {
				GList *link;

				link = _g_file_list_find_file (list, browser->priv->current_file->file);
				if (link != NULL) {
					GthFileStore *file_store;
					int           pos;

					current_file_deleted = TRUE;
					file_store = gth_browser_get_file_store (browser);
					pos = gth_file_store_find_visible (file_store, browser->priv->current_file->file);
					new_file = gth_file_store_get_file_at_pos (file_store, pos + 1);
					if (new_file == NULL)
						new_file = gth_file_store_get_file_at_pos (file_store, pos - 1);
					if (new_file != NULL)
						new_file = g_object_ref (new_file);
				}
			}

			if (update_folder_tree)
				gth_folder_tree_delete_children (GTH_FOLDER_TREE (browser->priv->folder_tree), parent, list);

			if (update_file_list) {
				if (current_file_deleted)
					g_signal_handlers_block_by_data (gth_browser_get_file_list_view (browser), browser);
				gth_file_list_delete_files (GTH_FILE_LIST (browser->priv->file_list), list);
				if (current_file_deleted)
					g_signal_handlers_unblock_by_data (gth_browser_get_file_list_view (browser), browser);
			}

			if (current_file_deleted) {
				gth_browser_load_file (browser, new_file, FALSE);
				_g_object_unref (new_file);
			}

			_gth_browser_update_statusbar_list_info (browser);
			gth_browser_update_sensitivity (browser);
			break;
		}
	}

	gtk_tree_path_free (path);
}


static void
file_renamed_cb (GthMonitor *monitor,
		 GFile      *file,
		 GFile      *new_file,
		 GthBrowser *browser)
{
	GthFileSource *file_source;
	GFileInfo     *info;
	GthFileData   *file_data;
	GList         *list;

	file_source = gth_main_get_file_source (new_file);
	info = gth_file_source_get_file_info (file_source, new_file);
	file_data = gth_file_data_new (new_file, info);
	gth_folder_tree_update_child (GTH_FOLDER_TREE (browser->priv->folder_tree), file, file_data);

	list = g_list_prepend (NULL, file_data);
	gth_file_list_update_files (GTH_FILE_LIST (browser->priv->file_list), list);

	if (g_file_equal (file, browser->priv->location))
		_gth_browser_set_location (browser, new_file);

	g_list_free (list);
	g_object_unref (file_data);
	g_object_unref (info);
	g_object_unref (file_source);
}


static void
_gth_browser_update_statusbar_file_info (GthBrowser *browser)
{
	const char  *image_size;
	const char  *file_date;
	const char  *file_size;
	GthMetadata *metadata;
	char        *text;

	if (browser->priv->current_file == NULL) {
		gth_statusbar_set_primary_text (GTH_STATUSBAR (browser->priv->statusbar), "");
		gth_statusbar_set_secondary_text (GTH_STATUSBAR (browser->priv->statusbar), "");
		return;
	}

	image_size = g_file_info_get_attribute_string (browser->priv->current_file->info, "image::size");
	metadata = (GthMetadata *) g_file_info_get_attribute_object (browser->priv->current_file->info, "Embedded::Image::DateTime");
	if (metadata != NULL)
		file_date = gth_metadata_get_formatted (metadata);
	else
		file_date = g_file_info_get_attribute_string (browser->priv->current_file->info, "file::display-mtime");
	file_size = g_file_info_get_attribute_string (browser->priv->current_file->info, "file::display-size");

	if (gth_browser_get_file_modified (browser))
		text = g_strdup_printf ("%s - %s", image_size, _("Modified"));
	else if (image_size != NULL)
		text = g_strdup_printf ("%s - %s - %s",	image_size, file_size, file_date);
	else
		text = g_strdup_printf ("%s - %s", file_size, file_date);

	gth_statusbar_set_primary_text (GTH_STATUSBAR (browser->priv->statusbar), text);

	g_free (text);
}


static void
metadata_changed_cb (GthMonitor  *monitor,
		     GthFileData *file_data,
		     GthBrowser  *browser)
{
	if (browser->priv->current_file == NULL)
		return;

	if (! g_file_equal (browser->priv->current_file->file, file_data->file))
		return;

	if (file_data->info != browser->priv->current_file->info)
		g_file_info_copy_into (file_data->info, browser->priv->current_file->info);

	gth_sidebar_set_file (GTH_SIDEBAR (browser->priv->file_properties), browser->priv->current_file);
	gth_sidebar_set_file (GTH_SIDEBAR (browser->priv->viewer_sidebar), browser->priv->current_file);

	_gth_browser_update_statusbar_file_info (browser);
	gth_browser_update_title (browser);
	gth_browser_update_sensitivity (browser);

}


static void
entry_points_changed_cb (GthMonitor *monitor,
			 GthBrowser *browser)
{
	_gth_browser_update_entry_point_list (browser);
}


static void
pref_general_filter_changed (GConfClient *client,
			     guint        cnxn_id,
			     GConfEntry  *entry,
			     gpointer     user_data)
{
	GthBrowser *browser = user_data;
	GthTest    *filter;

	filter = _gth_browser_get_file_filter (browser);
	gth_file_list_set_filter (GTH_FILE_LIST (browser->priv->file_list), filter);
	g_object_unref (filter);
}


static gboolean
gth_file_list_button_press_cb  (GtkWidget      *widget,
				GdkEventButton *event,
				gpointer        user_data)
{
	 GthBrowser *browser = user_data;

	 if ((event->type == GDK_BUTTON_PRESS) && (event->button == 3)) {
		gtk_menu_popup (GTK_MENU (browser->priv->file_list_popup),
				NULL,
				NULL,
				NULL,
				NULL,
				event->button,
				event->time);
	 	return TRUE;
	 }

	 return FALSE;
}


static void
gth_file_view_selection_changed_cb (GtkIconView *iconview,
				    gpointer     user_data)
{
	GthBrowser *browser = user_data;
	int         n_selected;

	gth_browser_update_sensitivity (browser);
	_gth_browser_update_statusbar_list_info (browser);

	if (gth_window_get_current_page (GTH_WINDOW (browser)) != GTH_BROWSER_PAGE_BROWSER)
		return;

	n_selected = gth_file_selection_get_n_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (browser)));
	if (n_selected == 1) {
		GList *items;
		GList *file_list = NULL;

		items = gth_file_selection_get_selected (GTH_FILE_SELECTION (gth_browser_get_file_list_view (browser)));
		file_list = gth_file_list_get_files (GTH_FILE_LIST (gth_browser_get_file_list (browser)), items);
		gth_browser_load_file (browser, (GthFileData *) file_list->data, FALSE);

		_g_object_list_unref (file_list);
		_gtk_tree_path_list_free (items);
	}
	else
		gth_browser_load_file (browser, NULL, FALSE);
}


static void
gth_file_view_item_activated_cb (GtkIconView *iconview,
				 GtkTreePath *path,
				 gpointer     user_data)
{
	GthBrowser *browser = user_data;
	GList      *list;
	GList      *files;

	list = g_list_prepend (NULL, path);
	files = gth_file_list_get_files (GTH_FILE_LIST (browser->priv->file_list), list);

	if (files != NULL)
		gth_browser_load_file (browser, (GthFileData *) files->data, TRUE);

	_g_object_list_unref (files);
	g_list_free (list);
}


static void
add_browser_toolbar_menu_buttons (GthBrowser *browser)
{
	int          tool_pos;
	GtkToolItem *tool_item;
	GtkAction   *action;

	tool_pos = 0;

	/* toolbar back button */

	tool_item = gtk_menu_tool_button_new_from_stock (GTK_STOCK_GO_BACK);
	gtk_menu_tool_button_set_menu (GTK_MENU_TOOL_BUTTON (tool_item),
				       gtk_ui_manager_get_widget (browser->priv->ui, GO_BACK_HISTORY_POPUP));
	gtk_tool_item_set_homogeneous (tool_item, TRUE);
	gtk_widget_set_tooltip_text (GTK_WIDGET (tool_item), _("Go to the previous visited location"));
	gtk_menu_tool_button_set_arrow_tooltip_text (GTK_MENU_TOOL_BUTTON (tool_item), _("View the list of visited locations"));

	action = gtk_action_new ("Toolbar_Go_Back", NULL, NULL, GTK_STOCK_GO_BACK);
	g_object_set (action, "is_important", TRUE, NULL);
	g_signal_connect (action,
			  "activate",
			  G_CALLBACK (gth_browser_activate_action_go_back),
			  browser);
	gtk_action_connect_proxy (action, GTK_WIDGET (tool_item));
	gtk_action_group_add_action (browser->priv->actions, action);

	gtk_widget_show (GTK_WIDGET (tool_item));
	gtk_toolbar_insert (GTK_TOOLBAR (browser->priv->browser_toolbar), tool_item, tool_pos++);

	/* toolbar forward button */

	tool_item = gtk_menu_tool_button_new_from_stock (GTK_STOCK_GO_FORWARD);
	gtk_menu_tool_button_set_menu (GTK_MENU_TOOL_BUTTON (tool_item),
				       gtk_ui_manager_get_widget (browser->priv->ui, GO_FORWARD_HISTORY_POPUP));
	gtk_tool_item_set_homogeneous (tool_item, TRUE);
	gtk_widget_set_tooltip_text (GTK_WIDGET (tool_item), _("Go to the next visited location"));
	gtk_menu_tool_button_set_arrow_tooltip_text (GTK_MENU_TOOL_BUTTON (tool_item), _("View the list of visited locations"));

	action = gtk_action_new ("Toolbar_Go_Forward", NULL, NULL, GTK_STOCK_GO_FORWARD);
	g_object_set (action, "is_important", TRUE, NULL);
	g_signal_connect (action,
			  "activate",
			  G_CALLBACK (gth_browser_activate_action_go_forward),
			  browser);
	gtk_action_connect_proxy (action, GTK_WIDGET (tool_item));
	gtk_action_group_add_action (browser->priv->actions, action);

	gtk_widget_show (GTK_WIDGET (tool_item));
	gtk_toolbar_insert (GTK_TOOLBAR (browser->priv->browser_toolbar), tool_item, tool_pos++);

	/* toolbar up button */

	tool_item = gtk_menu_tool_button_new_from_stock (GTK_STOCK_GO_UP);
	gtk_menu_tool_button_set_menu (GTK_MENU_TOOL_BUTTON (tool_item),
				       gtk_ui_manager_get_widget (browser->priv->ui, GO_PARENT_POPUP));
	gtk_tool_item_set_homogeneous (tool_item, TRUE);
	gtk_widget_set_tooltip_text (GTK_WIDGET (tool_item), _("Go up one level"));
	gtk_menu_tool_button_set_arrow_tooltip_text (GTK_MENU_TOOL_BUTTON (tool_item), _("View the list of upper locations"));

	action = gtk_action_new ("Toolbar_Go_Up", NULL, NULL, GTK_STOCK_GO_UP);
	g_object_set (action, "is_important", FALSE, NULL);
	g_signal_connect (action,
			  "activate",
			  G_CALLBACK (gth_browser_activate_action_go_up),
			  browser);
	gtk_action_connect_proxy (action, GTK_WIDGET (tool_item));
	gtk_action_group_add_action (browser->priv->actions, action);

	gtk_widget_show (GTK_WIDGET (tool_item));
	gtk_toolbar_insert (GTK_TOOLBAR (browser->priv->browser_toolbar), tool_item, tool_pos++);
}


static void
_gth_browser_construct_step2 (gpointer data)
{
	GthBrowser *browser = data;

	_gth_browser_update_bookmark_list (browser);
	_gth_browser_monitor_entry_points (browser);

	/* force an update to load the correct icons */
	gth_monitor_file_entry_points_changed (gth_main_get_default_monitor ());
}


static void
_gth_browser_update_toolbar_style (GthBrowser *browser)
{
	GthToolbarStyle toolbar_style;
	GtkToolbarStyle prop = GTK_TOOLBAR_BOTH;

	toolbar_style = gth_pref_get_real_toolbar_style ();

	switch (toolbar_style) {
	case GTH_TOOLBAR_STYLE_TEXT_BELOW:
		prop = GTK_TOOLBAR_BOTH;
		break;
	case GTH_TOOLBAR_STYLE_TEXT_BESIDE:
		prop = GTK_TOOLBAR_BOTH_HORIZ;
		break;
	case GTH_TOOLBAR_STYLE_ICONS:
		prop = GTK_TOOLBAR_ICONS;
		break;
	case GTH_TOOLBAR_STYLE_TEXT:
		prop = GTK_TOOLBAR_TEXT;
		break;
	default:
		break;
	}

	gtk_toolbar_set_style (GTK_TOOLBAR (browser->priv->browser_toolbar), prop);
	gtk_toolbar_set_style (GTK_TOOLBAR (browser->priv->viewer_toolbar), prop);
}


static void
pref_ui_toolbar_style_changed (GConfClient *client,
			       guint        cnxn_id,
			       GConfEntry  *entry,
			       gpointer     user_data)
{
	GthBrowser *browser = user_data;
	_gth_browser_update_toolbar_style (browser);
}


static void
_gth_browser_set_toolbar_visibility (GthBrowser *browser,
				    gboolean    visible)
{
	g_return_if_fail (browser != NULL);

	_gth_browser_set_action_active (browser, "View_Toolbar", visible);
	if (visible) {
		gtk_widget_show (browser->priv->browser_toolbar);
		gtk_widget_show (browser->priv->viewer_toolbar);
	}
	else {
		gtk_widget_hide (browser->priv->browser_toolbar);
		gtk_widget_hide (browser->priv->viewer_toolbar);
	}
}


static void
pref_ui_toolbar_visible_changed (GConfClient *client,
				 guint        cnxn_id,
				 GConfEntry  *entry,
				 gpointer     user_data)
{
	GthBrowser *browser = user_data;
	_gth_browser_set_toolbar_visibility (browser, gconf_value_get_bool (gconf_entry_get_value (entry)));
}


static void
_gth_browser_set_statusbar_visibility  (GthBrowser *browser,
				       gboolean    visible)
{
	g_return_if_fail (browser != NULL);

	_gth_browser_set_action_active (browser, "View_Statusbar", visible);
	if (visible)
		gtk_widget_show (browser->priv->statusbar);
	else
		gtk_widget_hide (browser->priv->statusbar);
}


static void
pref_ui_statusbar_visible_changed (GConfClient *client,
				   guint        cnxn_id,
				   GConfEntry  *entry,
				   gpointer     user_data)
{
	GthBrowser *browser = user_data;
	_gth_browser_set_statusbar_visibility (browser, gconf_value_get_bool (gconf_entry_get_value (entry)));
}


static void
pref_show_hidden_files_changed (GConfClient *client,
				guint        cnxn_id,
				GConfEntry  *entry,
				gpointer     user_data)
{
	GthBrowser *browser = user_data;
	gboolean    show_hidden_files;

	show_hidden_files = eel_gconf_get_boolean (PREF_SHOW_HIDDEN_FILES, FALSE);
	if (show_hidden_files == browser->priv->show_hidden_files)
		return;

	_gth_browser_set_action_active (browser, "View_ShowHiddenFiles", show_hidden_files);
	browser->priv->show_hidden_files = show_hidden_files;
	gth_folder_tree_reset_loaded (GTH_FOLDER_TREE (browser->priv->folder_tree));
	gth_browser_reload (browser);
}


static void
pref_fast_file_type_changed (GConfClient *client,
			     guint        cnxn_id,
			     GConfEntry  *entry,
			     gpointer     user_data)
{
	GthBrowser *browser = user_data;

	browser->priv->fast_file_type = eel_gconf_get_boolean (PREF_FAST_FILE_TYPE, TRUE);
	gth_browser_reload (browser);
}


static void
pref_thumbnail_size_changed (GConfClient *client,
			     guint        cnxn_id,
			     GConfEntry  *entry,
			     gpointer     user_data)
{
	GthBrowser *browser = user_data;

	gth_file_list_set_thumb_size (GTH_FILE_LIST (browser->priv->file_list), eel_gconf_get_integer (PREF_THUMBNAIL_SIZE, DEF_THUMBNAIL_SIZE));
	gth_browser_reload (browser);
}


static gboolean
_gth_browser_realize (GtkWidget *browser,
		      gpointer  *data)
{
	gth_hook_invoke ("gth-browser-realize", browser);

	return FALSE;
}


static gboolean
_gth_browser_unrealize (GtkWidget *browser,
			gpointer  *data)
{
	gth_hook_invoke ("gth-browser-unrealize", browser);

	return FALSE;
}


static void
_gth_browser_construct (GthBrowser *browser)
{
	GError         *error = NULL;
	GtkWidget      *vbox;
	GtkWidget      *scrolled_window;
	GtkWidget      *menubar;
	char           *general_filter;
	int             i;

	gtk_window_set_default_size (GTK_WINDOW (browser),
				     eel_gconf_get_integer (PREF_UI_WINDOW_WIDTH, DEFAULT_UI_WINDOW_WIDTH),
				     eel_gconf_get_integer (PREF_UI_WINDOW_HEIGHT, DEFAULT_UI_WINDOW_HEIGHT));

	/* ui actions */

	browser->priv->actions = gtk_action_group_new ("Actions");
	gtk_action_group_set_translation_domain (browser->priv->actions, NULL);
	gtk_action_group_add_actions (browser->priv->actions,
				      gth_window_action_entries,
				      gth_window_action_entries_size,
				      browser);
	gtk_action_group_add_actions (browser->priv->actions,
				      gth_browser_action_entries,
				      gth_browser_action_entries_size,
				      browser);
	gtk_action_group_add_toggle_actions (browser->priv->actions,
					     gth_browser_action_toggle_entries,
					     gth_browser_action_toggle_entries_size,
					     browser);

	browser->priv->ui = gtk_ui_manager_new ();
	g_signal_connect (browser->priv->ui,
			  "connect_proxy",
			  G_CALLBACK (connect_proxy_cb),
			  browser);
	g_signal_connect (browser->priv->ui,
			  "disconnect_proxy",
			  G_CALLBACK (disconnect_proxy_cb),
			  browser);

	gtk_ui_manager_insert_action_group (browser->priv->ui, browser->priv->actions, 0);
	gtk_window_add_accel_group (GTK_WINDOW (browser), gtk_ui_manager_get_accel_group (browser->priv->ui));

	if (! gtk_ui_manager_add_ui_from_string (browser->priv->ui, fixed_ui_info, -1, &error)) {
		g_message ("building menus failed: %s", error->message);
		g_error_free (error);
	}

	/* -- image page -- */

	/* toolbar */

	browser->priv->viewer_toolbar = gtk_ui_manager_get_widget (browser->priv->ui, "/ViewerToolBar");
	gtk_toolbar_set_show_arrow (GTK_TOOLBAR (browser->priv->viewer_toolbar), TRUE);
	gtk_widget_show (browser->priv->viewer_toolbar);
	gth_window_attach_toolbar (GTH_WINDOW (browser), GTH_BROWSER_PAGE_VIEWER, browser->priv->viewer_toolbar);

	/* content */

	browser->priv->viewer_pane = gtk_hpaned_new ();
	gtk_paned_set_position (GTK_PANED (browser->priv->viewer_pane), eel_gconf_get_integer (PREF_UI_VIEWER_SIDEBAR_WIDTH, DEFAULT_UI_WINDOW_WIDTH - DEF_SIDEBAR_WIDTH));
	gtk_widget_show (browser->priv->viewer_pane);
	gth_window_attach_content (GTH_WINDOW (browser), GTH_BROWSER_PAGE_VIEWER, browser->priv->viewer_pane);

	browser->priv->viewer_container = gtk_alignment_new (0.0, 0.0, 1.0, 1.0);
	gtk_widget_show (browser->priv->viewer_container);
	gtk_paned_pack1 (GTK_PANED (browser->priv->viewer_pane), browser->priv->viewer_container, TRUE, TRUE);

	browser->priv->viewer_sidebar = gth_sidebar_new ("file-tools");
	gtk_paned_pack2 (GTK_PANED (browser->priv->viewer_pane), browser->priv->viewer_sidebar, FALSE, TRUE);

	/* -- browser page -- */

	/* menus */

	menubar = gtk_ui_manager_get_widget (browser->priv->ui, "/MenuBar");
#ifdef USE_MACOSMENU
	{
		GtkWidget *widget;

		ige_mac_menu_install_key_handler ();
		ige_mac_menu_set_menu_bar (GTK_MENU_SHELL (menubar));
		gtk_widget_hide (menubar);
		widget = gtk_ui_manager_get_widget(ui, "/MenuBar/File/Close");
		if (widget != NULL) {
			ige_mac_menu_set_quit_menu_item (GTK_MENU_ITEM (widget));
		}
		widget = gtk_ui_manager_get_widget(ui, "/MenuBar/Help/About");
		if (widget != NULL) {
			ige_mac_menu_add_app_menu_item  (ige_mac_menu_add_app_menu_group (),
			GTK_MENU_ITEM (widget),
			NULL);
		}
		widget = gtk_ui_manager_get_widget(ui, "/MenuBar/Edit/Preferences");
			if (widget != NULL) {
			ige_mac_menu_add_app_menu_item  (ige_mac_menu_add_app_menu_group (),
			GTK_MENU_ITEM (widget),
			NULL);
		}
	}
#endif
	gth_window_attach (GTH_WINDOW (browser), menubar, GTH_WINDOW_MENUBAR);
	browser->priv->folder_popup = gtk_ui_manager_get_widget (browser->priv->ui, "/FolderListPopup");

	/* toolbar */

	browser->priv->browser_toolbar = gtk_ui_manager_get_widget (browser->priv->ui, "/ToolBar");
	gtk_toolbar_set_show_arrow (GTK_TOOLBAR (browser->priv->browser_toolbar), TRUE);
	gth_window_attach_toolbar (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER, browser->priv->browser_toolbar);
	add_browser_toolbar_menu_buttons (browser);

	/* statusbar */

	browser->priv->statusbar = gth_statusbar_new ();
	gtk_statusbar_set_has_resize_grip (GTK_STATUSBAR (browser->priv->statusbar), TRUE);
	browser->priv->help_message_cid = gtk_statusbar_get_context_id (GTK_STATUSBAR (browser->priv->statusbar), "gth_help_message");
	_gth_browser_set_statusbar_visibility (browser, eel_gconf_get_boolean (PREF_UI_STATUSBAR_VISIBLE, TRUE));
	gth_window_attach (GTH_WINDOW (browser), browser->priv->statusbar, GTH_WINDOW_STATUSBAR);

	/* main content */

	browser->priv->browser_container = gtk_hpaned_new ();
	gtk_paned_set_position (GTK_PANED (browser->priv->browser_container), eel_gconf_get_integer (PREF_UI_BROWSER_SIDEBAR_WIDTH, DEF_SIDEBAR_WIDTH));
	gtk_widget_show (browser->priv->browser_container);
	gth_window_attach_content (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER, browser->priv->browser_container);

	/* the browser sidebar */

	browser->priv->browser_sidebar = gtk_vpaned_new ();
	gtk_paned_set_position (GTK_PANED (browser->priv->browser_sidebar), eel_gconf_get_integer (PREF_UI_PROPERTIES_HEIGHT, DEF_PROPERTIES_HEIGHT));
	gtk_widget_show (browser->priv->browser_sidebar);
	gtk_paned_pack1 (GTK_PANED (browser->priv->browser_container), browser->priv->browser_sidebar, FALSE, TRUE);

	/* the box that contains the location and the folder list.  */

	vbox = gtk_vbox_new (FALSE, 6);
	gtk_widget_show (vbox);
	gtk_paned_pack1 (GTK_PANED (browser->priv->browser_sidebar), vbox, TRUE, TRUE);

	/* the location combobox */

	browser->priv->location_chooser = gth_location_chooser_new ();
	gtk_widget_show (browser->priv->location_chooser);
	gtk_box_pack_start (GTK_BOX (vbox), browser->priv->location_chooser, FALSE, FALSE, 0);

	g_signal_connect (browser->priv->location_chooser,
			  "changed",
			  G_CALLBACK (location_changed_cb),
			  browser);

	/* the folder list */

	scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
					GTK_POLICY_AUTOMATIC,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled_window),
					     GTK_SHADOW_ETCHED_IN);
	gtk_widget_show (scrolled_window);

	browser->priv->folder_tree = gth_folder_tree_new ("gthumb-vfs:///");
	gtk_widget_show (browser->priv->folder_tree);

	gtk_container_add (GTK_CONTAINER (scrolled_window), browser->priv->folder_tree);
	gtk_box_pack_start (GTK_BOX (vbox), scrolled_window, TRUE, TRUE, 0);

	g_signal_connect (browser->priv->folder_tree,
			  "open",
			  G_CALLBACK (folder_tree_open_cb),
			  browser);
	g_signal_connect (browser->priv->folder_tree,
			  "open_parent",
			  G_CALLBACK (folder_tree_open_parent_cb),
			  browser);
	g_signal_connect (browser->priv->folder_tree,
			  "list_children",
			  G_CALLBACK (folder_tree_list_children_cb),
			  browser);
	g_signal_connect (browser->priv->folder_tree,
			  "load",
			  G_CALLBACK (folder_tree_load_cb),
			  browser);
	g_signal_connect (browser->priv->folder_tree,
			  "folder_popup",
			  G_CALLBACK (folder_tree_folder_popup_cb),
			  browser);
	g_signal_connect (browser->priv->folder_tree,
			  "rename",
			  G_CALLBACK (folder_tree_rename_cb),
			  browser);

	/* the file property box */

	browser->priv->file_properties = gth_sidebar_new ("file-list-tools");
	gtk_widget_hide (browser->priv->file_properties);
	gtk_paned_pack2 (GTK_PANED (browser->priv->browser_sidebar), browser->priv->file_properties, FALSE, TRUE);

	/* the box that contains the file list and the filter bar.  */

	vbox = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (vbox);
	gtk_paned_pack2 (GTK_PANED (browser->priv->browser_container), vbox, TRUE, TRUE);

	/* the list extra widget container */

	browser->priv->list_extra_widget_container = gtk_vbox_new (FALSE, 0);
	gtk_widget_show (browser->priv->list_extra_widget_container);
	gtk_box_pack_start (GTK_BOX (vbox), browser->priv->list_extra_widget_container, FALSE, FALSE, 0);

	/* the file list */

	browser->priv->file_list = gth_file_list_new ();
	gth_browser_set_sort_order (browser,
				    gth_main_get_sort_type (eel_gconf_get_string (PREF_SORT_TYPE, "file::mtime")),
				    FALSE);
	gth_browser_enable_thumbnails (browser, eel_gconf_get_boolean (PREF_SHOW_THUMBNAILS, TRUE));
	gth_file_list_set_thumb_size (GTH_FILE_LIST (browser->priv->file_list), eel_gconf_get_integer (PREF_THUMBNAIL_SIZE, DEF_THUMBNAIL_SIZE));
	gtk_widget_show (browser->priv->file_list);
	gtk_box_pack_start (GTK_BOX (vbox), browser->priv->file_list, TRUE, TRUE, 0);

	g_signal_connect (G_OBJECT (browser->priv->file_list),
			  "button_press_event",
			  G_CALLBACK (gth_file_list_button_press_cb),
			  browser);
	g_signal_connect (G_OBJECT (gth_file_list_get_view (GTH_FILE_LIST (browser->priv->file_list))),
			  "selection_changed",
			  G_CALLBACK (gth_file_view_selection_changed_cb),
			  browser);
	g_signal_connect (G_OBJECT (gth_file_list_get_view (GTH_FILE_LIST (browser->priv->file_list))),
			  "item_activated",
			  G_CALLBACK (gth_file_view_item_activated_cb),
			  browser);

	browser->priv->file_list_popup = gtk_ui_manager_get_widget (browser->priv->ui, "/FileListPopup");

	/* the filter bar */

	general_filter = eel_gconf_get_string (PREF_GENERAL_FILTER, DEFAULT_GENERAL_FILTER);
	browser->priv->filterbar = gth_filterbar_new (general_filter);
	g_free (general_filter);

	gth_browser_show_filterbar (browser, eel_gconf_get_boolean (PREF_UI_FILTERBAR_VISIBLE, TRUE));
	gtk_box_pack_end (GTK_BOX (vbox), browser->priv->filterbar, FALSE, FALSE, 0);

	g_signal_connect (browser->priv->filterbar,
			  "changed",
			  G_CALLBACK (filterbar_changed_cb),
			  browser);
	g_signal_connect (browser->priv->filterbar,
			  "personalize",
			  G_CALLBACK (filterbar_personalize_cb),
			  browser);

	/* the image preloader */

	browser->priv->image_preloader = gth_image_preloader_new ();

	/**/

	browser->priv->bookmarks_changed_id =
		g_signal_connect (gth_main_get_default_monitor (),
				  "bookmarks-changed",
				  G_CALLBACK (bookmarks_changed_cb),
				  browser);
	browser->priv->folder_changed_id =
		g_signal_connect (gth_main_get_default_monitor (),
				  "folder-changed",
				  G_CALLBACK (folder_changed_cb),
				  browser);
	browser->priv->file_renamed_id =
		g_signal_connect (gth_main_get_default_monitor (),
				  "file-renamed",
				  G_CALLBACK (file_renamed_cb),
				  browser);
	browser->priv->metadata_changed_id =
		g_signal_connect (gth_main_get_default_monitor (),
				  "metadata-changed",
				  G_CALLBACK (metadata_changed_cb),
				  browser);
	browser->priv->entry_points_changed_id =
		g_signal_connect (gth_main_get_default_monitor (),
				  "entry-points-changed",
				  G_CALLBACK (entry_points_changed_cb),
				  browser);

	/* init browser data */

	_gth_browser_set_toolbar_visibility (browser, eel_gconf_get_boolean (PREF_UI_TOOLBAR_VISIBLE, TRUE));
	_gth_browser_update_toolbar_style (browser);
	_gth_browser_update_entry_point_list (browser);

	browser->priv->show_hidden_files = eel_gconf_get_boolean (PREF_SHOW_HIDDEN_FILES, FALSE);
	_gth_browser_set_action_active (browser, "View_ShowHiddenFiles", browser->priv->show_hidden_files);

	browser->priv->fast_file_type = eel_gconf_get_boolean (PREF_FAST_FILE_TYPE, TRUE);

	gth_hook_invoke ("gth-browser-construct", browser);

	g_signal_connect (browser, "realize", G_CALLBACK (_gth_browser_realize), NULL);
	g_signal_connect (browser, "unrealize", G_CALLBACK (_gth_browser_unrealize), NULL);

	performance (DEBUG_INFO, "window initialized");

	/* gconf notifications */

	i = 0;
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_GENERAL_FILTER,
					   pref_general_filter_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_UI_TOOLBAR_STYLE,
					   pref_ui_toolbar_style_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   "/desktop/gnome/interface/toolbar_style",
					   pref_ui_toolbar_style_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_UI_TOOLBAR_VISIBLE,
					   pref_ui_toolbar_visible_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_UI_STATUSBAR_VISIBLE,
					   pref_ui_statusbar_visible_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_SHOW_HIDDEN_FILES,
					   pref_show_hidden_files_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_FAST_FILE_TYPE,
					   pref_fast_file_type_changed,
					   browser);
	browser->priv->cnxn_id[i++] = eel_gconf_notification_add (
					   PREF_THUMBNAIL_SIZE,
					   pref_thumbnail_size_changed,
					   browser);

	gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER);

	call_when_idle (_gth_browser_construct_step2, browser);
}


GtkWidget *
gth_browser_new (const char *uri)
{
	GthBrowser *browser;

	browser = (GthBrowser*) g_object_new (GTH_TYPE_BROWSER, "n-pages", GTH_BROWSER_N_PAGES, NULL);
	_gth_browser_construct (browser);
	browser_list = g_list_prepend (browser_list, browser);

	if (uri != NULL) {
		GFile *location;

		location = g_file_new_for_uri ((uri != NULL) ? uri : gth_pref_get_startup_location ());
		gth_browser_go_to (browser, location);
		g_object_unref (location);
	}

	return (GtkWidget*) browser;
}


GFile *
gth_browser_get_location (GthBrowser *browser)
{
	return browser->priv->location;
}


GthFileData *
gth_browser_get_current_file (GthBrowser *browser)
{
	return browser->priv->current_file;
}


gboolean
gth_browser_get_file_modified (GthBrowser *browser)
{
	if (browser->priv->current_file == NULL)
		return FALSE;
	else
		return g_file_info_get_attribute_boolean (browser->priv->current_file->info, "file::is-modified");
}


void
gth_browser_go_to (GthBrowser *browser,
		   GFile      *location)
{
	gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER);
	_gth_browser_load (browser, location, GTH_ACTION_GO_TO);
}


void
gth_browser_go_back (GthBrowser *browser,
		     int         steps)
{
	GList *new_current;

	new_current = browser->priv->history_current;
	while ((new_current != NULL) && (steps-- > 0))
		new_current = new_current->next;

	if (new_current == NULL)
		return;

	browser->priv->history_current = new_current;
	_gth_browser_load (browser, (GFile*) browser->priv->history_current->data, GTH_ACTION_GO_BACK);
}


void
gth_browser_go_forward (GthBrowser *browser,
			int         steps)
{
	GList *new_current;

	new_current = browser->priv->history_current;
	while ((new_current != NULL) && (steps-- > 0))
		new_current = new_current->prev;

	if (new_current == NULL)
		return;

	browser->priv->history_current = new_current;
	_gth_browser_load (browser, (GFile *) browser->priv->history_current->data, GTH_ACTION_GO_FORWARD);
}


void
gth_browser_go_up (GthBrowser *browser,
		   int         steps)
{
	GFile *parent;

	if (browser->priv->location == NULL)
		return;

	parent = g_object_ref (browser->priv->location);
	while ((steps-- > 0) && (parent != NULL)) {
		GFile *parent_parent;

		parent_parent = g_file_get_parent (parent);
		g_object_unref (parent);
		parent = parent_parent;
	}

	if (parent != NULL) {
		gth_browser_go_to (browser, parent);
		g_object_unref (parent);
	}
}


void
gth_browser_go_home (GthBrowser *browser)
{
	GFile *location;

	location = g_file_new_for_uri (gth_pref_get_startup_location ());
	gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER);
	gth_browser_go_to (browser, location);

	g_object_unref (location);
}


void
gth_browser_clear_history (GthBrowser *browser)
{
	_g_object_list_unref (browser->priv->history);
	browser->priv->history = NULL;
	browser->priv->history_current = NULL;

	_gth_browser_add_to_history (browser, browser->priv->location);
	_gth_browser_update_history_list (browser);
}


void
gth_browser_set_dialog (GthBrowser *browser,
			const char *dialog_name,
			GtkWidget  *dialog)
{
	g_hash_table_insert (browser->priv->named_dialogs, (gpointer) dialog_name, dialog);
}


GtkWidget *
gth_browser_get_dialog (GthBrowser *browser,
			const char *dialog_name)
{
	return g_hash_table_lookup (browser->priv->named_dialogs, dialog_name);
}


GtkUIManager *
gth_browser_get_ui_manager (GthBrowser *browser)
{
	return browser->priv->ui;
}


GtkWidget *
gth_browser_get_statusbar (GthBrowser *browser)
{
	return browser->priv->statusbar;
}


GtkWidget *
gth_browser_get_file_list (GthBrowser *browser)
{
	return browser->priv->file_list;
}


GtkWidget *
gth_browser_get_file_list_view (GthBrowser *browser)
{
	return gth_file_list_get_view (GTH_FILE_LIST (browser->priv->file_list));
}


GthFileSource *
gth_browser_get_location_source (GthBrowser *browser)
{
	return browser->priv->location_source;
}


GthFileStore *
gth_browser_get_file_store (GthBrowser *browser)
{
	return GTH_FILE_STORE (gth_file_view_get_model (GTH_FILE_VIEW (gth_browser_get_file_list_view (browser))));
}


GtkWidget *
gth_browser_get_folder_tree (GthBrowser *browser)
{
	return browser->priv->folder_tree;
}


void
gth_browser_get_sort_order (GthBrowser        *browser,
			    GthFileDataSort **sort_type,
			    gboolean         *inverse)
{
	if (sort_type != NULL)
		*sort_type = browser->priv->sort_type;
	if (inverse != NULL)
		*inverse = browser->priv->sort_inverse;
}


void
gth_browser_set_sort_order (GthBrowser      *browser,
			    GthFileDataSort *sort_type,
			    gboolean         inverse)
{
	if (sort_type == NULL) {
		gth_browser_set_sort_order (browser,
					    gth_main_get_sort_type ("file::mtime"),
					    inverse);
		return;
	}

	browser->priv->sort_type = sort_type;
	browser->priv->sort_inverse = inverse;

	gth_file_list_set_sort_func (GTH_FILE_LIST (browser->priv->file_list),
				     browser->priv->sort_type->cmp_func,
				     browser->priv->sort_inverse);
}


void
gth_browser_stop (GthBrowser *browser)
{
	if (browser->priv->fullscreen)
		gth_browser_unfullscreen (browser);

	_gth_browser_cancel (browser);
	gth_browser_update_sensitivity (browser);

	if ((browser->priv->task != NULL) && gth_task_is_running (browser->priv->task))
		gth_task_cancel (browser->priv->task);
}


void
gth_browser_reload (GthBrowser *browser)
{
	gth_browser_go_to (browser, browser->priv->location);
}


static void
task_completed_cb (GthTask    *task,
		   GError     *error,
		   GthBrowser *browser)
{
	browser->priv->activity_ref--;

	g_signal_handler_disconnect (browser->priv->task, browser->priv->task_completed);

	if (! browser->priv->closing) {
		gth_browser_update_sensitivity (browser);
		if (error != NULL) {
			if (! g_error_matches (error, GTH_TASK_ERROR, GTH_TASK_ERROR_CANCELLED))
				_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not perfom the operation"), &error);
			else
				g_error_free (error);
		}
	}

	g_object_unref (browser->priv->task);
	browser->priv->task = NULL;

	if (browser->priv->closing)
		_gth_browser_close_step3 (browser);
}


void
gth_browser_exec_task (GthBrowser *browser,
		       GthTask    *task)
{
	g_return_if_fail (task != NULL);

	if (browser->priv->task != NULL)
		gth_task_cancel (task);

	browser->priv->task = g_object_ref (task);
	browser->priv->task_completed = g_signal_connect (task,
							  "completed",
							  G_CALLBACK (task_completed_cb),
							  browser);
	browser->priv->activity_ref++;
	gth_browser_update_sensitivity (browser);
	gth_task_exec (browser->priv->task);
}


void
gth_browser_set_list_extra_widget (GthBrowser *browser,
				   GtkWidget  *widget)
{
	if (browser->priv->list_extra_widget != NULL) {
		gtk_container_remove (GTK_CONTAINER (browser->priv->list_extra_widget_container), browser->priv->list_extra_widget);
		browser->priv->list_extra_widget = NULL;
	}

	if (widget != NULL) {
		browser->priv->list_extra_widget = widget;
		gtk_container_add (GTK_CONTAINER (browser->priv->list_extra_widget_container), browser->priv->list_extra_widget);
	}
}


GtkWidget *
gth_browser_get_list_extra_widget (GthBrowser *browser)
{
	return browser->priv->list_extra_widget;
}


void
gth_browser_set_viewer_widget (GthBrowser *browser,
			       GtkWidget  *widget)
{
	_gtk_container_remove_children (GTK_CONTAINER (browser->priv->viewer_container), NULL, NULL);
	if (widget != NULL)
		gtk_container_add (GTK_CONTAINER (browser->priv->viewer_container), widget);
}


GtkWidget *
gth_browser_get_viewer_widget (GthBrowser *browser)
{
	GtkWidget *child = NULL;
	GList     *children;

	children = gtk_container_get_children (GTK_CONTAINER (browser->priv->viewer_container));
	if (children != NULL)
		child = children->data;
	g_list_free (children);

	return child;
}


GtkWidget *
gth_browser_get_viewer_page (GthBrowser *browser)
{
	return (GtkWidget *) browser->priv->viewer_page;
}


GtkWidget *
gth_browser_get_viewer_sidebar (GthBrowser *browser)
{
	return browser->priv->viewer_sidebar;
}


static gboolean
view_focused_image (GthBrowser *browser)
{
	GthFileView *view;
	int          pos;
	GthFileData *focused_file;

	if (browser->priv->current_file == NULL)
		return FALSE;

	view = GTH_FILE_VIEW (gth_browser_get_file_list_view (browser));
	pos = gth_file_view_get_cursor (view);
	if (pos == -1)
		return FALSE;

	focused_file = gth_file_store_get_file_at_pos (GTH_FILE_STORE (gth_file_view_get_model (view)), pos);
	if (focused_file == NULL)
		return FALSE;

	return ! g_file_equal (browser->priv->current_file->file, focused_file->file);
}


gboolean
gth_browser_show_next_image (GthBrowser *browser,
			     gboolean    skip_broken,
			     gboolean    only_selected)
{
	GthFileView *view;
	int          pos;

	view = GTH_FILE_VIEW (gth_browser_get_file_list_view (browser));

	if (browser->priv->current_file == NULL) {
		pos = gth_file_list_next_file (GTH_FILE_LIST (browser->priv->file_list), -1, skip_broken, only_selected, TRUE);
	}
	else if (view_focused_image (browser)) {
		pos = gth_file_view_get_cursor (view);
		if (pos < 0)
			pos = gth_file_list_next_file (GTH_FILE_LIST (browser->priv->file_list), -1, skip_broken, only_selected, TRUE);
	}
	else {
		pos = gth_file_store_find_visible (gth_browser_get_file_store (browser), browser->priv->current_file->file);
		pos = gth_file_list_next_file (GTH_FILE_LIST (browser->priv->file_list), pos, skip_broken, only_selected, FALSE);
	}

	if (pos >= 0) {
		GthFileData *file_data;

		file_data = gth_file_store_get_file_at_pos (GTH_FILE_STORE (gth_file_view_get_model (view)), pos);
		gth_browser_load_file (browser, file_data, TRUE);
	}

	return (pos >= 0);
}


gboolean
gth_browser_show_prev_image (GthBrowser *browser,
			     gboolean    skip_broken,
			     gboolean    only_selected)
{
	GthFileView *view;
	int          pos;

	view = GTH_FILE_VIEW (gth_browser_get_file_list_view (browser));

	if (browser->priv->current_file == NULL) {
		pos = gth_file_list_prev_file (GTH_FILE_LIST (browser->priv->file_list), -1, skip_broken, only_selected, TRUE);
	}
	else if (view_focused_image (browser)) {
		pos = gth_file_view_get_cursor (view);
		if (pos < 0)
			pos = gth_file_list_prev_file (GTH_FILE_LIST (browser->priv->file_list), -1, skip_broken, only_selected, TRUE);
	}
	else {
		pos = gth_file_store_find_visible (gth_browser_get_file_store (browser), browser->priv->current_file->file);
		pos = gth_file_list_prev_file (GTH_FILE_LIST (browser->priv->file_list), pos, skip_broken, only_selected, FALSE);
	}

	if (pos >= 0) {
		GthFileData *file_data;

		file_data = gth_file_store_get_file_at_pos (GTH_FILE_STORE (gth_file_view_get_model (view)), pos);
		gth_browser_load_file (browser, file_data, TRUE);
	}

	return (pos >= 0);
}


gboolean
gth_browser_show_first_image (GthBrowser *browser,
			      gboolean    skip_broken,
			      gboolean    only_selected)
{
	int          pos;
	GthFileView *view;
	GthFileData *file_data;

	pos = gth_file_list_first_file (GTH_FILE_LIST (browser->priv->file_list), skip_broken, only_selected);
	if (pos < 0)
		return FALSE;

	view = GTH_FILE_VIEW (gth_browser_get_file_list_view (browser));
	file_data = gth_file_store_get_file_at_pos (GTH_FILE_STORE (gth_file_view_get_model (view)), pos);
	gth_browser_load_file (browser, file_data, TRUE);

	return TRUE;
}


gboolean
gth_browser_show_last_image (GthBrowser *browser,
			     gboolean    skip_broken,
			     gboolean    only_selected)
{
	int          pos;
	GthFileView *view;
	GthFileData *file_data;

	pos = gth_file_list_last_file (GTH_FILE_LIST (browser->priv->file_list), skip_broken, only_selected);
	if (pos < 0)
		return FALSE;

	view = GTH_FILE_VIEW (gth_browser_get_file_list_view (browser));
	file_data = gth_file_store_get_file_at_pos (GTH_FILE_STORE (gth_file_view_get_model (view)), pos);
	gth_browser_load_file (browser, file_data, TRUE);

	return TRUE;
}


static void
file_metadata_ready_cb (GList    *files,
			GError   *error,
			gpointer  user_data)
{
	GthBrowser *browser = user_data;

	gth_sidebar_set_file (GTH_SIDEBAR (browser->priv->file_properties), browser->priv->current_file);
	gth_sidebar_set_file (GTH_SIDEBAR (browser->priv->viewer_sidebar), browser->priv->current_file);
	_gth_browser_update_statusbar_file_info (browser);

	if (browser->priv->viewer_page != NULL)
		gth_viewer_page_view (browser->priv->viewer_page, browser->priv->current_file);

	gth_browser_update_title (browser);
	gth_browser_update_sensitivity (browser);

	if (browser->priv->location == NULL) {
		GFile *parent;

		parent = g_file_get_parent (browser->priv->current_file->file);
		_gth_browser_load (browser, parent, GTH_ACTION_GO_TO);
		g_object_unref (parent);
	}
}


/* -- gth_browser_load_file -- */


static void
_gth_browser_make_file_visible (GthBrowser  *browser,
				GthFileData *file_data)
{
	int            file_pos;
	GtkWidget     *view;
	GthVisibility  visibility;

	file_pos = gth_file_store_find_visible (GTH_FILE_STORE (gth_browser_get_file_store (browser)), file_data->file);
	if (file_pos < 0)
		return;

	view = gth_browser_get_file_list_view (browser);
	g_signal_handlers_block_by_func (gth_browser_get_file_list_view (browser), gth_file_view_selection_changed_cb, browser);
	gth_file_selection_unselect_all (GTH_FILE_SELECTION (view));
	gth_file_selection_select (GTH_FILE_SELECTION (view), file_pos);
	gth_file_view_set_cursor (GTH_FILE_VIEW (view), file_pos);
	g_signal_handlers_unblock_by_func (gth_browser_get_file_list_view (browser), gth_file_view_selection_changed_cb, browser);

	visibility = gth_file_view_get_visibility (GTH_FILE_VIEW (view), file_pos);
	if (visibility != GTH_VISIBILITY_FULL) {
		double align;

		switch (visibility) {
		case GTH_VISIBILITY_NONE:
		case GTH_VISIBILITY_FULL:
		case GTH_VISIBILITY_PARTIAL:
			align = 0.5;
			break;

		case GTH_VISIBILITY_PARTIAL_TOP:
			align = 0.0;
			break;

		case GTH_VISIBILITY_PARTIAL_BOTTOM:
			align = 1.0;
			break;
		}
		gth_file_view_scroll_to (GTH_FILE_VIEW (view), file_pos, align);
	}
}


static void
_gth_browser_deactivate_viewer_page (GthBrowser *browser)
{
	if (browser->priv->viewer_page != NULL) {
		gth_viewer_page_deactivate (browser->priv->viewer_page);
		gtk_ui_manager_ensure_update (browser->priv->ui);
		gth_browser_set_viewer_widget (browser, NULL);
		g_object_unref (browser->priv->viewer_page);
		browser->priv->viewer_page = NULL;
	}
}


static void
_gth_browser_load_file (GthBrowser  *browser,
			GthFileData *file_data,
			gboolean     view)
{
	GPtrArray *viewer_pages;
	int       i;
	GList    *files;

	if (file_data == NULL) {
		_gth_browser_deactivate_viewer_page (browser);
		_g_object_unref (browser->priv->current_file);
		browser->priv->current_file = NULL;

		gtk_widget_hide (browser->priv->file_properties);

		_gth_browser_update_statusbar_file_info (browser);
		gth_browser_update_title (browser);
		gth_browser_update_sensitivity (browser);

		return;
	}
	else
		gtk_widget_show (browser->priv->file_properties);

	g_object_ref (file_data);
	_g_object_unref (browser->priv->current_file);
	browser->priv->current_file = gth_file_data_dup (file_data);
	g_object_unref (file_data);

	_gth_browser_make_file_visible (browser, browser->priv->current_file);

	viewer_pages = gth_main_get_object_set ("viewer-page");
	for (i = viewer_pages->len - 1; i >= 0; i--) {
		GthViewerPage *registered_viewer_page;

		registered_viewer_page = g_ptr_array_index (viewer_pages, i);
		if (gth_viewer_page_can_view (registered_viewer_page, browser->priv->current_file)) {
			if ((browser->priv->viewer_page != NULL) && (G_OBJECT_TYPE (registered_viewer_page) != G_OBJECT_TYPE (browser->priv->viewer_page))) {
				gth_viewer_page_deactivate (browser->priv->viewer_page);
				gtk_ui_manager_ensure_update (browser->priv->ui);
				gth_browser_set_viewer_widget (browser, NULL);
				g_object_unref (browser->priv->viewer_page);
				browser->priv->viewer_page = NULL;
			}
			if (browser->priv->viewer_page == NULL) {
				browser->priv->viewer_page = g_object_new (G_OBJECT_TYPE (registered_viewer_page), NULL);
				gth_viewer_page_activate (browser->priv->viewer_page, browser);
				gtk_ui_manager_ensure_update (browser->priv->ui);
			}
			break;
		}
	}

	if (view) {
		gth_viewer_page_show (browser->priv->viewer_page);
		if (browser->priv->fullscreen) {
			gth_viewer_page_fullscreen (browser->priv->viewer_page, TRUE);
			gth_viewer_page_show_pointer (browser->priv->viewer_page, FALSE);
		}
		gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_VIEWER);
	}

	files = g_list_prepend (NULL, browser->priv->current_file);
	_g_query_metadata_async (files,
				 "*",
				 NULL,
				 file_metadata_ready_cb,
			 	 browser);

	g_list_free (files);
}


typedef struct {
	int          ref;
	GthBrowser  *browser;
	GthFileData *file_data;
	gboolean     view;
} LoadFileData;


static LoadFileData *
load_file_data_new (GthBrowser  *browser,
		    GthFileData *file_data,
		    gboolean     view)
{
	LoadFileData *data;

	data = g_new0 (LoadFileData, 1);
	data->ref = 1;
	data->browser = browser;
	if (file_data != NULL)
		data->file_data = g_object_ref (file_data);
	data->view = view;

	return data;
}


static void
load_file_data_ref (LoadFileData *data)
{
	data->ref++;
}


static void
load_file_data_unref (LoadFileData *data)
{
	if (--data->ref > 0)
		return;
	_g_object_unref (data->file_data);
	g_free (data);
}


static void
load_file__previuos_file_saved_cb (GthBrowser *browser,
				   gboolean    cancelled,
				   gpointer    user_data)
{
	LoadFileData *data = user_data;

	if (! cancelled)
		_gth_browser_load_file (data->browser, data->file_data, data->view);

	load_file_data_unref (data);
}


static gboolean
load_file_delayed_cb (gpointer user_data)
{
	LoadFileData *data = user_data;
	GthBrowser   *browser = data->browser;

	load_file_data_ref (data);

	g_source_remove (browser->priv->load_file_timeout);
	browser->priv->load_file_timeout = 0;

	if (gth_browser_get_file_modified (browser)) {
		load_file_data_ref (data);
		_gth_browser_ask_whether_to_save (browser,
						  load_file__previuos_file_saved_cb,
						  data);
	}
	else
		_gth_browser_load_file (data->browser, data->file_data, data->view);

	load_file_data_unref (data);

	return FALSE;
}


void
gth_browser_load_file (GthBrowser  *browser,
		       GthFileData *file_data,
		       gboolean     view)
{
	LoadFileData *data;

	data = load_file_data_new (browser, file_data, view);

	if (browser->priv->load_file_timeout != 0)
		g_source_remove (browser->priv->load_file_timeout);
	browser->priv->load_file_timeout =
			g_timeout_add_full (G_PRIORITY_DEFAULT,
					    LOAD_FILE_DELAY,
					    load_file_delayed_cb,
					    data,
					    (GDestroyNotify) load_file_data_unref);
}


void
gth_browser_show_viewer_properties (GthBrowser *browser,
				    gboolean    show)
{
	_gth_browser_set_action_active (browser, "Viewer_Properties", show);

	if (show) {
		_gth_browser_set_action_active (browser, "Viewer_Tools", FALSE);
		gtk_widget_show (browser->priv->viewer_sidebar);
		gth_sidebar_show_properties (GTH_SIDEBAR (browser->priv->viewer_sidebar));
	}
	else
		gtk_widget_hide (browser->priv->viewer_sidebar);
}


void
gth_browser_show_viewer_tools (GthBrowser *browser,
			       gboolean    show)
{
	_gth_browser_set_action_active (browser, "Viewer_Tools", show);

	if (show) {
		_gth_browser_set_action_active (browser, "Viewer_Properties", FALSE);
		gtk_widget_show (browser->priv->viewer_sidebar);
		gth_sidebar_show_tools (GTH_SIDEBAR (browser->priv->viewer_sidebar));
	}
	else
		gtk_widget_hide (browser->priv->viewer_sidebar);
}


/* -- gth_browser_load_location -- */


typedef struct {
	GthFileSource *file_source;
	GFile         *location;
	GthBrowser    *browser;
} LoadLocationData;


static void
load_location_data_free (LoadLocationData *data)
{
	g_object_unref (data->location);
	g_object_unref (data->file_source);
	g_free (data);
}


static void
load_file_attributes_ready_cb (GthFileSource *file_source,
			       GList         *files,
			       GError        *error,
			       gpointer       user_data)
{
	LoadLocationData *data = user_data;
	GthBrowser       *browser = data->browser;

	if (error == NULL) {
		GthFileData *file_data;

		file_data = files->data;
		if (g_file_info_get_file_type (file_data->info) == G_FILE_TYPE_REGULAR) {
			GFile *parent;

			parent = g_file_get_parent (file_data->file);
			if ((browser->priv->location != NULL) && ! g_file_equal (parent, browser->priv->location)) {
				/* set location to NULL to force a folder reload */
				_g_object_unref (browser->priv->location);
				browser->priv->location = NULL;
			}

			gth_browser_load_file (browser, file_data, TRUE);

			g_object_unref (parent);
		}
		else if (g_file_info_get_file_type (file_data->info) == G_FILE_TYPE_DIRECTORY) {
			gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_BROWSER);
			gth_browser_go_to (browser, file_data->file);
		}
		else {
			GError *error;
			char   *uri;

			uri = g_file_get_uri (data->location);
			error = g_error_new (GTHUMB_ERROR, 0, _("File type not supported %s"), uri);
			_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not load the position"), &error);

			g_free (uri);
		}
	}
	else
		_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not load the position"), &error);

	load_location_data_free (data);
}


void
gth_browser_load_location (GthBrowser *browser,
		  	   GFile      *location)
{
	LoadLocationData *data;
	GList            *list;

	data = g_new0 (LoadLocationData, 1);
	data->browser = browser;
	data->location = g_object_ref (location);
	data->file_source = gth_main_get_file_source (data->location);
	if (data->file_source == NULL) {
		GError *error;
		char   *uri;

		uri = g_file_get_uri (data->location);
		error = g_error_new (GTHUMB_ERROR, 0, _("No suitable module found for %s"), uri);
		_gtk_error_dialog_from_gerror_show (GTK_WINDOW (browser), _("Could not load the position"), &error);

		g_free (uri);
	}

	list = g_list_prepend (NULL, g_object_ref (data->location));
	gth_file_source_read_attributes (data->file_source,
					 list,
					 eel_gconf_get_boolean (PREF_FAST_FILE_TYPE, TRUE) ? GTH_FILE_DATA_ATTRIBUTES_WITH_FAST_CONTENT_TYPE : GTH_FILE_DATA_ATTRIBUTES_WITH_CONTENT_TYPE,
					 load_file_attributes_ready_cb,
					 data);

	_g_object_list_unref (list);
}


void
gth_browser_enable_thumbnails (GthBrowser *browser,
			       gboolean    show)
{
	gth_file_list_enable_thumbs (GTH_FILE_LIST (browser->priv->file_list), show);
	_gth_browser_set_action_active (browser, "View_Thumbnails", show);
	eel_gconf_set_boolean (PREF_SHOW_THUMBNAILS, show);
}


void
gth_browser_show_filterbar (GthBrowser *browser,
			    gboolean    show)
{
	if (show)
		gtk_widget_show (browser->priv->filterbar);
	else
		gtk_widget_hide (browser->priv->filterbar);
	_gth_browser_set_action_active (browser, "View_Filterbar", show);
	eel_gconf_set_boolean (PREF_UI_FILTERBAR_VISIBLE, show);
}


gpointer
gth_browser_get_image_preloader (GthBrowser *browser)
{
	return g_object_ref (browser->priv->image_preloader);
}


static void
_gth_browser_create_fullscreen_toolbar (GthBrowser *browser)
{
	GdkScreen *screen;

	if (browser->priv->fullscreen_toolbar != NULL)
		return;

	browser->priv->fullscreen_toolbar = gtk_window_new (GTK_WINDOW_POPUP);

	screen = gtk_widget_get_screen (GTK_WIDGET (browser));
	gtk_window_set_screen (GTK_WINDOW (browser->priv->fullscreen_toolbar), screen);
	gtk_window_set_default_size (GTK_WINDOW (browser->priv->fullscreen_toolbar), gdk_screen_get_width (screen), -1);
	gtk_container_set_border_width (GTK_CONTAINER (browser->priv->fullscreen_toolbar), 0);

	gtk_container_add (GTK_CONTAINER (browser->priv->fullscreen_toolbar), gtk_ui_manager_get_widget (browser->priv->ui, "/Fullscreen_ToolBar"));
}


static gboolean
hide_mouse_pointer_cb (gpointer data)
{
	GthBrowser *browser = data;
	int         x, y, w, h, px, py;

	gdk_window_get_pointer (browser->priv->fullscreen_toolbar->window, &px, &py, 0);
	gdk_window_get_geometry (browser->priv->fullscreen_toolbar->window, &x, &y, &w, &h, NULL);

	if ((px >= x) && (px <= x + w) && (py >= y) && (py <= y + h))
		return FALSE;

	gtk_widget_hide (browser->priv->fullscreen_toolbar);
	if (browser->priv->viewer_page != NULL)
		gth_viewer_page_show_pointer (GTH_VIEWER_PAGE (browser->priv->viewer_page), FALSE);

	browser->priv->hide_mouse_timeout = 0;

	return FALSE;
}


static gboolean
fullscreen_motion_notify_event_cb (GtkWidget      *widget,
				   GdkEventMotion *event,
				   gpointer        data)
{
	GthBrowser *browser = data;

	if (browser->priv->last_mouse_x == 0.0)
		browser->priv->last_mouse_x = event->x;
	if (browser->priv->last_mouse_y == 0.0)
		browser->priv->last_mouse_y = event->y;

	if ((abs (browser->priv->last_mouse_x - event->x) > MOTION_THRESHOLD) || (abs (browser->priv->last_mouse_y - event->y) > MOTION_THRESHOLD))
		if (! GTK_WIDGET_VISIBLE (browser->priv->fullscreen_toolbar)) {
			gtk_widget_show (browser->priv->fullscreen_toolbar);
			if (browser->priv->viewer_page != NULL)
				gth_viewer_page_show_pointer (GTH_VIEWER_PAGE (browser->priv->viewer_page), TRUE);
		}

	if (browser->priv->hide_mouse_timeout != 0)
		g_source_remove (browser->priv->hide_mouse_timeout);
	browser->priv->hide_mouse_timeout = g_timeout_add (HIDE_MOUSE_DELAY,
							   hide_mouse_pointer_cb,
							   browser);

	browser->priv->last_mouse_x = event->x;
	browser->priv->last_mouse_y = event->y;

	return FALSE;
}


void
gth_browser_fullscreen (GthBrowser *browser)
{
	browser->priv->fullscreen = ! browser->priv->fullscreen;

	if (! browser->priv->fullscreen) {
		gth_browser_unfullscreen (browser);
		return;
	}

	if (browser->priv->current_file == NULL)
		gth_browser_show_first_image (browser, FALSE, FALSE);

	_gth_browser_create_fullscreen_toolbar (browser);

	gth_browser_show_viewer_properties (browser, FALSE);
	gtk_window_fullscreen (GTK_WINDOW (browser));
	gth_window_set_current_page (GTH_WINDOW (browser), GTH_BROWSER_PAGE_VIEWER);
	gth_window_show_only_content (GTH_WINDOW (browser), TRUE);
	if (browser->priv->viewer_page != NULL) {
		gth_viewer_page_show (browser->priv->viewer_page);
		gth_viewer_page_fullscreen (browser->priv->viewer_page, TRUE);
		gth_viewer_page_show_pointer (browser->priv->viewer_page, FALSE);
	}

	gth_browser_update_sensitivity (browser);

	browser->priv->last_mouse_x = 0.0;
	browser->priv->last_mouse_y = 0.0;
	browser->priv->motion_signal = g_signal_connect (browser,
							 "motion_notify_event",
							 G_CALLBACK (fullscreen_motion_notify_event_cb),
							 browser);
}


void
gth_browser_unfullscreen (GthBrowser *browser)
{
	browser->priv->fullscreen = FALSE;

	gtk_widget_hide (browser->priv->fullscreen_toolbar);
	gtk_window_unfullscreen (GTK_WINDOW (browser));
	gth_window_show_only_content (GTH_WINDOW (browser), FALSE);
	if (browser->priv->viewer_page != NULL)
		gth_viewer_page_fullscreen (browser->priv->viewer_page, FALSE);

	gth_browser_update_sensitivity (browser);

	if (browser->priv->motion_signal != 0)
		g_signal_handler_disconnect (browser, browser->priv->motion_signal);
}