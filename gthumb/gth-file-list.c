/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2008 Free Software Foundation, Inc.
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
#include <string.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include "glib-utils.h"
#include "gth-cell-renderer-thumbnail.h"
#include "gth-dumb-notebook.h"
#include "gth-empty-list.h"
#include "gth-file-list.h"
#include "gth-file-store.h"
#include "gth-icon-cache.h"
#include "gth-icon-view.h"
#include "gth-thumb-loader.h"
#include "gtk-utils.h"

#define DEFAULT_THUMBNAIL_SIZE 112
#define N_THUMBS_PER_NOTIFICATION 15
#define N_LOOKAHEAD 50
#define EMPTY (N_("(Empty)"))

typedef enum {
	GTH_FILE_LIST_OP_TYPE_SET_FILES,
	GTH_FILE_LIST_OP_TYPE_CLEAR_FILES,
	GTH_FILE_LIST_OP_TYPE_ADD_FILES,
	GTH_FILE_LIST_OP_TYPE_UPDATE_FILES,
	GTH_FILE_LIST_OP_TYPE_DELETE_FILES,
	GTH_FILE_LIST_OP_TYPE_SET_FILTER,
	GTH_FILE_LIST_OP_TYPE_SET_SORT_FUNC,
	GTH_FILE_LIST_OP_TYPE_ENABLE_THUMBS
	/*GTH_FILE_LIST_OP_TYPE_RENAME,
	GTH_FILE_LIST_OP_TYPE_SET_THUMBS_SIZE,*/
} GthFileListOpType;


typedef struct {
	GthFileListOpType    type;
	GthFileSource       *file_source;
	GtkTreeModel        *model;
	GthTest             *filter;
	GList               *file_list; /* GthFileData */
	GList               *files; /* GFile */
	GthFileDataCompFunc  cmp_func;
	gboolean             inverse_sort;
	char                *sval;
	int                  ival;
} GthFileListOp;


enum {
	FILE_POPUP,
	LAST_SIGNAL
};


enum {
	GTH_FILE_LIST_PANE_VIEW,
	GTH_FILE_LIST_PANE_MESSAGE
};

struct _GthFileListPrivateData
{
	GtkWidget       *notebook;
	GtkWidget       *view;
	GtkWidget       *message;
	GthIconCache    *icon_cache;
	GthFileSource   *file_source;
	gboolean         load_thumbs;
	int              thumb_size;
	gboolean         ignore_hidden_thumbs;
	GthThumbLoader  *thumb_loader;
	gboolean         update_thumb_in_view;
	int              thumb_pos;
	int              n_thumb;
	GthFileData     *thumb_fd;
	gboolean         loading_thumbs;
	gboolean         cancel;
	GList           *queue; /* list of GthFileListOp */
	GtkCellRenderer *thumbnail_renderer;
	GtkCellRenderer *text_renderer;

	DoneFunc         done_func;
	gpointer         done_func_data;
};


/* OPs */


static void _gth_file_list_exec_next_op (GthFileList *file_list);


static GthFileListOp *
gth_file_list_op_new (GthFileListOpType op_type)
{
	GthFileListOp *op;

	op = g_new0 (GthFileListOp, 1);
	op->type = op_type;

	return op;
}


static void
gth_file_list_op_free (GthFileListOp *op)
{
	switch (op->type) {
	case GTH_FILE_LIST_OP_TYPE_SET_FILES:
		g_object_unref (op->file_source);
		_g_object_list_unref (op->file_list);
		break;
	case GTH_FILE_LIST_OP_TYPE_CLEAR_FILES:
		g_free (op->sval);
		break;
	case GTH_FILE_LIST_OP_TYPE_ADD_FILES:
	case GTH_FILE_LIST_OP_TYPE_UPDATE_FILES:
		_g_object_list_unref (op->file_list);
		break;
	case GTH_FILE_LIST_OP_TYPE_DELETE_FILES:
		_g_object_list_unref (op->files);
		break;
	case GTH_FILE_LIST_OP_TYPE_SET_FILTER:
		g_object_unref (op->filter);
		break;
	default:
		break;
	}
	g_free (op);
}


static void
_gth_file_list_clear_queue (GthFileList *file_list)
{
	g_list_foreach (file_list->priv->queue, (GFunc) gth_file_list_op_free, NULL);
	g_list_free (file_list->priv->queue);
	file_list->priv->queue = NULL;
}


static void
_gth_file_list_remove_op (GthFileList       *file_list,
			  GthFileListOpType  op_type)
{
	GList *scan;

	scan = file_list->priv->queue;
	while (scan != NULL) {
		GthFileListOp *op = scan->data;

		if (op->type != op_type) {
			scan = scan->next;
			continue;
		}

		file_list->priv->queue = g_list_remove_link (file_list->priv->queue, scan);
		gth_file_list_op_free (op);
		g_list_free (scan);

		scan = file_list->priv->queue;
	}
}


static void
_gth_file_list_queue_op (GthFileList   *file_list,
			 GthFileListOp *op)
{
	if ((op->type == GTH_FILE_LIST_OP_TYPE_SET_FILES) || (op->type == GTH_FILE_LIST_OP_TYPE_CLEAR_FILES))
		_gth_file_list_clear_queue (file_list);
	if (op->type == GTH_FILE_LIST_OP_TYPE_SET_FILTER)
		_gth_file_list_remove_op (file_list, GTH_FILE_LIST_OP_TYPE_SET_FILTER);
	file_list->priv->queue = g_list_append (file_list->priv->queue, op);

	if (! file_list->priv->loading_thumbs)
		_gth_file_list_exec_next_op (file_list);
}


/* -- gth_file_list -- */


static GtkHBoxClass *parent_class = NULL;


static void
gth_file_list_finalize (GObject *object)
{
	GthFileList *file_list;

	file_list = GTH_FILE_LIST (object);

	if (file_list->priv != NULL) {
		if (file_list->priv->icon_cache != NULL)
			gth_icon_cache_free (file_list->priv->icon_cache);
		g_free (file_list->priv);
		file_list->priv = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}


static void
gth_file_list_class_init (GthFileListClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);
	object_class = (GObjectClass*) class;

	object_class->finalize = gth_file_list_finalize;
}


static void
gth_file_list_init (GthFileList *file_list)
{
	file_list->priv = g_new0 (GthFileListPrivateData, 1);

	file_list->priv->thumb_size = DEFAULT_THUMBNAIL_SIZE;
	file_list->priv->ignore_hidden_thumbs = FALSE;
	file_list->priv->load_thumbs = TRUE; /* FIXME: make this cnfigurable */
}


static void _gth_file_list_update_next_thumb (GthFileList *file_list);


static void
update_thumb_in_file_view (GthFileList *file_list)
{
	GthFileStore *file_store;
	GdkPixbuf    *pixbuf;

	file_store = (GthFileStore *) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));

	pixbuf = gth_thumb_loader_get_pixbuf (file_list->priv->thumb_loader);
	if (pixbuf != NULL)
		gth_file_store_queue_set (file_store,
					  gth_file_store_get_abs_pos (file_store, file_list->priv->thumb_pos),
					  NULL,
					  pixbuf,
					  FALSE,
					  NULL);

	if (file_list->priv->n_thumb % N_THUMBS_PER_NOTIFICATION == N_THUMBS_PER_NOTIFICATION - 1)
		gth_file_store_exec_set (file_store);
}


static void
thumb_loader_ready_cb (GthThumbLoader *tloader,
		       GError         *error,
		       gpointer        data)
{
	GthFileList *file_list = data;

	if (file_list->priv->thumb_fd != NULL) {
		if (error == NULL) {
			file_list->priv->thumb_fd->error = FALSE;
			file_list->priv->thumb_fd->thumb_created = TRUE;
			if (file_list->priv->update_thumb_in_view) {
				file_list->priv->thumb_fd->thumb_loaded = TRUE;
				update_thumb_in_file_view (file_list);
			}
		}
		else {
			file_list->priv->thumb_fd->error = TRUE;
			file_list->priv->thumb_fd->thumb_loaded = FALSE;
			file_list->priv->thumb_fd->thumb_created = FALSE;
		}
	}
	_gth_file_list_update_next_thumb (file_list);
}


static void
start_update_next_thumb (GthFileList *file_list)
{
	GthFileStore *file_store;

	if (file_list->priv->loading_thumbs)
		return;

	if (! file_list->priv->load_thumbs) {
		file_list->priv->loading_thumbs = FALSE;
		return;
	}

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	file_list->priv->n_thumb = -1;
	file_list->priv->loading_thumbs = TRUE;
	_gth_file_list_update_next_thumb (file_list);
}


static void
vadj_changed_cb (GtkAdjustment *adjustment,
		 gpointer       user_data)
{
	GthFileList *file_list = user_data;

	start_update_next_thumb (file_list);
}


static void
file_view_drag_data_get_cb (GtkWidget        *widget,
			    GdkDragContext   *drag_context,
			    GtkSelectionData *data,
			    guint             info,
			    guint             time,
			    gpointer          user_data)
{
	GthFileList  *file_list = user_data;
	GList        *items;
	GList        *files;
	GList        *scan;
	int           n_uris;
	char        **uris;
	int           i;

	items = gth_file_selection_get_selected (GTH_FILE_SELECTION (file_list->priv->view));
	files = gth_file_list_get_files (file_list, items);
	n_uris = g_list_length (files);
	uris = g_new (char *, n_uris + 1);
	for (scan = files, i = 0; scan; scan = scan->next, i++) {
		GthFileData *file_data = scan->data;
		uris[i] = g_file_get_uri (file_data->file);
	}
	uris[i] = NULL;

	gtk_selection_data_set_uris (data, uris);

	g_strfreev (uris);
	_g_object_list_unref (files);
	_gtk_tree_path_list_free (items);
}


static void
gth_file_list_construct (GthFileList *file_list)
{
	GtkWidget       *scrolled;
	GtkAdjustment   *vadj;
	GtkWidget       *viewport;
	GtkCellRenderer *renderer;
	GthFileStore    *model;
	GtkTargetList   *target_list;
	GtkTargetEntry  *targets;
	int              n_targets;

	/* thumbnail loader */

	file_list->priv->thumb_loader = gth_thumb_loader_new (file_list->priv->thumb_size, file_list->priv->thumb_size);
	g_signal_connect (G_OBJECT (file_list->priv->thumb_loader),
			  "ready",
			  G_CALLBACK (thumb_loader_ready_cb),
			  file_list);

	/* other data */

	file_list->priv->icon_cache = gth_icon_cache_new (gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (file_list))), file_list->priv->thumb_size / 2);

	/* the main notebook */

	file_list->priv->notebook = gth_dumb_notebook_new ();

	/* the message pane */

	viewport = gtk_viewport_new (NULL, NULL);
	gtk_viewport_set_shadow_type (GTK_VIEWPORT (viewport),
				      GTK_SHADOW_ETCHED_IN);

	file_list->priv->message = gth_empty_list_new (_(EMPTY));

	/* the file view */

	scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
					GTK_POLICY_NEVER,
					GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
					     GTK_SHADOW_ETCHED_IN);

	vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scrolled));
	g_signal_connect (G_OBJECT (vadj),
			  "changed",
			  G_CALLBACK (vadj_changed_cb),
			  file_list);
	g_signal_connect (G_OBJECT (vadj),
			  "value-changed",
			  G_CALLBACK (vadj_changed_cb),
			  file_list);

	model = gth_file_store_new ();
	file_list->priv->view = gth_icon_view_new_with_model (GTK_TREE_MODEL (model));
	g_object_unref (model);

	target_list = gtk_target_list_new (NULL, 0);
	gtk_target_list_add_uri_targets (target_list, 0);
	gtk_target_list_add_text_targets (target_list, 0);
	targets = gtk_target_table_new_from_list (target_list, &n_targets);
	gth_file_view_enable_drag_source (GTH_FILE_VIEW (file_list->priv->view),
					  GDK_BUTTON1_MASK,
					  targets,
					  n_targets,
					  GDK_ACTION_MOVE | GDK_ACTION_COPY);

	gtk_target_list_unref (target_list);
	gtk_target_table_free (targets, n_targets);

	g_signal_connect (G_OBJECT (file_list->priv->view),
			  "drag-data-get",
			  G_CALLBACK (file_view_drag_data_get_cb),
			  file_list);

	/* thumbnail */

	file_list->priv->thumbnail_renderer = renderer = gth_cell_renderer_thumbnail_new ();
	g_object_set (renderer,
		      "size", file_list->priv->thumb_size,
		      "yalign", 1.0,
		      NULL);
	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (file_list->priv->view), renderer, FALSE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (file_list->priv->view),
					renderer,
					"thumbnail", GTH_FILE_STORE_THUMBNAIL_COLUMN,
					"is_icon", GTH_FILE_STORE_IS_ICON_COLUMN,
					"file", GTH_FILE_STORE_FILE_COLUMN,
					NULL);

	/* text */

	file_list->priv->text_renderer = renderer = gtk_cell_renderer_text_new ();
	g_object_set (G_OBJECT (renderer),
		      "ellipsize", PANGO_ELLIPSIZE_END,
		      "alignment", PANGO_ALIGN_CENTER,
		      "width", file_list->priv->thumb_size + (8 * 2) /* FIXME: remove the numbers */,
		      "single-paragraph-mode", TRUE,
		      NULL);

	gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (file_list->priv->view), renderer, FALSE);
	gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (file_list->priv->view),
					renderer,
					"text", GTH_FILE_STORE_METADATA_COLUMN,
					NULL);

	/* pack the widgets together */

	gtk_widget_show (file_list->priv->view);
	gtk_container_add (GTK_CONTAINER (scrolled), file_list->priv->view);

	gtk_widget_show (scrolled);
	gtk_container_add (GTK_CONTAINER (file_list->priv->notebook), scrolled);

	gtk_widget_show (file_list->priv->message);
	gtk_container_add (GTK_CONTAINER (viewport), file_list->priv->message);

	gtk_widget_show (viewport);
	gtk_container_add (GTK_CONTAINER (file_list->priv->notebook), viewport);

	gtk_widget_show (file_list->priv->notebook);
	gtk_box_pack_start (GTK_BOX (file_list), file_list->priv->notebook, TRUE, TRUE, 0);
}


GType
gth_file_list_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo type_info = {
			sizeof (GthFileListClass),
			NULL,
			NULL,
			(GClassInitFunc) gth_file_list_class_init,
			NULL,
			NULL,
			sizeof (GthFileList),
			0,
			(GInstanceInitFunc) gth_file_list_init
		};

		type = g_type_register_static (GTK_TYPE_VBOX,
					       "GthFileList",
					       &type_info,
					       0);
	}

	return type;
}


GtkWidget*
gth_file_list_new (void)
{
	GtkWidget *widget;

	widget = GTK_WIDGET (g_object_new (GTH_TYPE_FILE_LIST, NULL));
	gth_file_list_construct (GTH_FILE_LIST (widget));

	return widget;
}


static void
_gth_file_list_thumb_cleanup (GthFileList *file_list)
{
	_g_object_unref (file_list->priv->thumb_fd);
	file_list->priv->thumb_fd = NULL;
}


static void
_gth_file_list_done (GthFileList *file_list)
{
	_gth_file_list_thumb_cleanup (file_list);
	file_list->priv->loading_thumbs = FALSE;
	file_list->priv->cancel = FALSE;
}


static void
cancel_step2 (gpointer user_data)
{
	GthFileList *file_list = user_data;

	_gth_file_list_done (file_list);

	if (file_list->priv->done_func)
		(file_list->priv->done_func) (file_list->priv->done_func_data);
}


void
gth_file_list_cancel (GthFileList *file_list,
		      DoneFunc     done_func,
		      gpointer     user_data)
{
	_gth_file_list_clear_queue (file_list);

	file_list->priv->done_func = done_func;
	file_list->priv->done_func_data = user_data;
	gth_thumb_loader_cancel (file_list->priv->thumb_loader, cancel_step2, file_list);
}


static void
gfl_clear_list (GthFileList *file_list,
		const char  *message)
{
	GthFileStore *file_store;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	gth_file_store_clear (file_store);

	gth_empty_list_set_text (GTH_EMPTY_LIST (file_list->priv->message), message);
	gth_dumb_notebook_show_child (GTH_DUMB_NOTEBOOK (file_list->priv->notebook), GTH_FILE_LIST_PANE_MESSAGE);
}


void
gth_file_list_clear (GthFileList *file_list,
		     const char  *message)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_CLEAR_FILES);
	op->sval = g_strdup (message != NULL ? message : _(EMPTY));
	_gth_file_list_queue_op (file_list, op);
}


static void
_gth_file_list_update_pane (GthFileList *file_list)
{
	GthFileStore *file_store;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));

	if (gth_file_store_n_visibles (file_store) > 0) {
		gth_dumb_notebook_show_child (GTH_DUMB_NOTEBOOK (file_list->priv->notebook), GTH_FILE_LIST_PANE_VIEW);
	}
	else {
		gth_empty_list_set_text (GTH_EMPTY_LIST (file_list->priv->message), _(EMPTY));
		gth_dumb_notebook_show_child (GTH_DUMB_NOTEBOOK (file_list->priv->notebook), GTH_FILE_LIST_PANE_MESSAGE);
	}
}


static void
gfl_add_files (GthFileList *file_list,
	       GList       *files)
{
	GthFileStore *file_store;
	GList        *scan;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));

	for (scan = files; scan; scan = scan->next) {
		GthFileData *fd = scan->data;
		GIcon       *icon;
		GdkPixbuf   *pixbuf = NULL;

		if (g_file_info_get_file_type (fd->info) != G_FILE_TYPE_REGULAR)
			continue;

		if (gth_file_store_find (file_store, fd->file) >= 0)
			continue;

		icon = g_file_info_get_icon (fd->info);
		pixbuf = gth_icon_cache_get_pixbuf (file_list->priv->icon_cache, icon);

		gth_file_store_queue_add (file_store,
					  fd,
					  pixbuf,
					  TRUE,
					  /* FIXME: make this user configurable */
					  g_file_info_get_attribute_string (fd->info, "standard::display-name"));

		if (pixbuf != NULL)
			g_object_unref (pixbuf);
	}

	gth_file_store_exec_add (file_store);
	_gth_file_list_update_pane (file_list);
}


void
gth_file_list_add_files (GthFileList *file_list,
			 GList       *files)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_ADD_FILES);
	op->file_list = _g_object_list_ref (files);
	_gth_file_list_queue_op (file_list, op);
}


static void
gfl_delete_files (GthFileList *file_list,
		  GList       *files)
{
	GthFileStore *file_store;
	GList        *scan;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	for (scan = files; scan; scan = scan->next) {
		GFile *file = scan->data;
		int    abs_pos;

		abs_pos = gth_file_store_find (file_store, file);
		if (abs_pos >= 0)
			gth_file_store_queue_remove (file_store, abs_pos);
	}
	gth_file_store_exec_remove (file_store);
	_gth_file_list_update_pane (file_list);
}


void
gth_file_list_delete_files (GthFileList *file_list,
			    GList       *files)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_DELETE_FILES);
	op->files = _g_object_list_ref (files);
	_gth_file_list_queue_op (file_list, op);
}


static void
gfl_update_files (GthFileList *file_list,
		  GList       *files)
{
	GthFileStore *file_store;
	GList        *scan;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	for (scan = files; scan; scan = scan->next) {
		GthFileData *fd = scan->data;
		int          abs_pos;

		abs_pos = gth_file_store_find (file_store, fd->file);
		if (abs_pos >= 0)
			gth_file_store_queue_set (file_store,
						  abs_pos,
						  fd,
						  NULL,
						  FALSE,
						  NULL);
	}
	gth_file_store_exec_set (file_store);
	_gth_file_list_update_pane (file_list);
}


void
gth_file_list_update_files (GthFileList *file_list,
			    GList       *files)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_UPDATE_FILES);
	op->file_list = _g_object_list_ref (files);
	_gth_file_list_queue_op (file_list, op);
}


static void
gfl_set_files (GthFileList   *file_list,
	       GthFileSource *file_source,
	       GList         *files)
{
	GthFileStore *file_store;

	if (file_list->priv->file_source != NULL) {
		g_object_unref (file_list->priv->file_source);
		file_list->priv->file_source = NULL;
	}
	if (file_source != NULL)
		file_list->priv->file_source = g_object_ref (file_source);

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	gth_file_store_clear (file_store);
	gfl_add_files (file_list, files);
}


void
gth_file_list_set_files (GthFileList   *file_list,
			GthFileSource *file_source,
			GList         *files)
{
	GthFileListOp *op;

	if (files == NULL) {
		op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_CLEAR_FILES);
		op->sval = g_strdup (_(EMPTY));
		_gth_file_list_queue_op (file_list, op);
	}
	else {
		op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_SET_FILES);
		op->file_source = g_object_ref (file_source);
		op->file_list = _g_object_list_ref (files);
		_gth_file_list_queue_op (file_list, op);
	}
}


GList *
gth_file_list_get_files (GthFileList *file_list,
			 GList       *items)
{
	GList        *list = NULL;
	GthFileStore *file_store;
	GList        *scan;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	for (scan = items; scan; scan = scan->next) {
		GtkTreePath *tree_path = scan->data;
		GtkTreeIter  iter;
		GthFileData *file_data;

		if (! gtk_tree_model_get_iter (GTK_TREE_MODEL (file_store), &iter, tree_path))
			continue;
		file_data = gth_file_store_get_file (file_store, &iter);
		if (file_data != NULL)
			list = g_list_prepend (list, g_object_ref (file_data));
	}

	return g_list_reverse (list);
}


static void
gfl_set_filter (GthFileList *file_list,
		GthTest     *filter)
{
	GthFileStore *file_store;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	if (file_store != NULL)
		gth_file_store_set_filter (file_store, filter);
	_gth_file_list_update_pane (file_list);
}


void
gth_file_list_set_filter (GthFileList *file_list,
			  GthTest     *filter)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_SET_FILTER);
	if (filter != NULL)
		op->filter = g_object_ref (filter);
	else
		op->filter = gth_test_new ();
	_gth_file_list_queue_op (file_list, op);
}


static void
gfl_set_sort_func (GthFileList         *file_list,
		   GthFileDataCompFunc  cmp_func,
		   gboolean             inverse_sort)
{
	GthFileStore *file_store;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	if (file_store != NULL)
		gth_file_store_set_sort_func (file_store, cmp_func, inverse_sort);
}


void
gth_file_list_set_sort_func (GthFileList         *file_list,
			     GthFileDataCompFunc  cmp_func,
			     gboolean             inverse_sort)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_SET_SORT_FUNC);
	op->cmp_func = cmp_func;
	op->inverse_sort = inverse_sort;
	_gth_file_list_queue_op (file_list, op);
}


static void
gfl_enable_thumbs (GthFileList *file_list,
		   gboolean     enable)
{
	GthFileStore *file_store;
	GList        *files, *scan;
	int           pos;

	file_list->priv->load_thumbs = enable;

	file_store = (GthFileStore*) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	files = gth_file_store_get_all (file_store);
	pos = 0;
	for (scan = files; scan; scan = scan->next) {
		GthFileData *fd = scan->data;
		GIcon       *icon;
		GdkPixbuf   *pixbuf = NULL;

		fd->thumb_loaded = FALSE;
		fd->thumb_created = FALSE;
		fd->error = FALSE;

		icon = g_file_info_get_icon (fd->info);
		pixbuf = gth_icon_cache_get_pixbuf (file_list->priv->icon_cache, icon);

		gth_file_store_set (file_store, pos, NULL, pixbuf, TRUE, NULL);

		if (pixbuf != NULL)
			g_object_unref (pixbuf);

		pos++;
	}
	_g_object_list_unref (files);

	start_update_next_thumb (file_list);
}


void
gth_file_list_enable_thumbs (GthFileList *file_list,
			     gboolean     enable)
{
	GthFileListOp *op;

	op = gth_file_list_op_new (GTH_FILE_LIST_OP_TYPE_ENABLE_THUMBS);
	op->ival = enable;
	_gth_file_list_queue_op (file_list, op);
}


void
gth_file_list_set_thumb_size (GthFileList *file_list,
			      int          size)
{
	file_list->priv->thumb_size = size;
	gth_thumb_loader_set_thumb_size (file_list->priv->thumb_loader, size, size);

	gth_icon_cache_free (file_list->priv->icon_cache);
	file_list->priv->icon_cache = gth_icon_cache_new (gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (file_list))), size / 2);

	g_object_set (file_list->priv->thumbnail_renderer,
		      "size", size,
		      NULL);
	g_object_set (file_list->priv->text_renderer,
		      "width", size + (8 * 2),
		      NULL);
}


GtkWidget *
gth_file_list_get_view (GthFileList *file_list)
{
	return file_list->priv->view;
}


/* thumbs */


static void
_gth_file_list_thumbs_completed (GthFileList *file_list)
{
	GthFileStore *file_store;

	file_store = (GthFileStore *) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));
	if (file_list->priv->n_thumb >= 0)
		gth_file_store_exec_set (file_store);

	_gth_file_list_done (file_list);
}


static void
_gth_file_list_update_current_thumb (GthFileList *file_list)
{
	gth_thumb_loader_set_file (file_list->priv->thumb_loader, file_list->priv->thumb_fd);
	gth_thumb_loader_load (file_list->priv->thumb_loader);
}


static gboolean
update_thumbs_stopped (gpointer callback_data)
{
	GthFileList *file_list = callback_data;

	file_list->priv->loading_thumbs = FALSE;
	_gth_file_list_exec_next_op (file_list);

	return FALSE;
}


static void
_gth_file_list_update_next_thumb (GthFileList *file_list)
{
	GthFileStore *file_store;
	int           pos;
	int           first_pos;
	int           last_pos;
	int           max_pos;
	GthFileData  *fd = NULL;
	GList        *list, *scan;
	int           new_pos = -1;

	if (file_list->priv->cancel || (file_list->priv->queue != NULL)) {
		g_idle_add (update_thumbs_stopped, file_list);
		return;
	}

	file_store = (GthFileStore *) gth_file_view_get_model (GTH_FILE_VIEW (file_list->priv->view));

	/* Find first visible undone. */

	first_pos = gth_file_view_get_first_visible (GTH_FILE_VIEW (file_list->priv->view));
	if (first_pos < 0)
		first_pos = 0;

	list = gth_file_store_get_visibles (file_store);
	max_pos = g_list_length (list) - 1;

	last_pos = gth_file_view_get_last_visible (GTH_FILE_VIEW (file_list->priv->view));
	if ((last_pos < 0) || (last_pos > max_pos))
		last_pos = max_pos;

	pos = first_pos;
	scan = g_list_nth (list, pos);
	if (scan == NULL) {
		_g_object_list_unref (list);
		_gth_file_list_thumbs_completed (file_list);
		return;
	}

	/* Find a not loaded thumbnail among the visible images. */

	while (pos <= last_pos) {
		fd = scan->data;
		if (! fd->thumb_loaded && ! fd->error) {
			new_pos = pos;
			break;
		}
		else {
			pos++;
			scan = scan->next;
		}
	}

	if (! file_list->priv->ignore_hidden_thumbs) {

		/* Find a not created thumbnail among the not-visible images. */

		/* start from the one after the last visible image... */

		if (new_pos == -1) {
			pos = last_pos + 1;
			scan = g_list_nth (list, pos);
			while (scan && ((pos - last_pos) <= N_LOOKAHEAD)) {
				fd = scan->data;
				if (! fd->thumb_created && ! fd->error) {
					new_pos = pos;
					break;
				}
				pos++;
				scan = scan->next;
			}
		}

		/* ...continue from the one before the first visible upward to
		 * the first one */

		if (new_pos == -1) {
			pos = first_pos - 1;
			scan = g_list_nth (list, pos);
			while (scan && ((first_pos - pos) <= N_LOOKAHEAD)) {
				fd = scan->data;
				if (! fd->thumb_created && ! fd->error) {
					new_pos = pos;
					break;
				}
				pos--;
				scan = scan->prev;
			}
		}
	}

	if (new_pos != -1)
		fd = g_object_ref (fd);

	_g_object_list_unref (list);

	if (new_pos == -1) {
		_gth_file_list_thumbs_completed (file_list);
		return;
	}

	/* We create thumbnail files for all images in the folder, but we only
	   load the visible ones (and N_LOOKAHEAD before and N_LOOKAHEAD after the visible range),
	   to minimize memory consumption in large folders. */
	file_list->priv->update_thumb_in_view = (new_pos >= (first_pos - N_LOOKAHEAD)) &&
						(new_pos <= (last_pos + N_LOOKAHEAD));
	file_list->priv->thumb_pos = new_pos;
	_g_object_unref (file_list->priv->thumb_fd);
	file_list->priv->thumb_fd = fd; /* already ref-ed above */
	file_list->priv->n_thumb++;

	_gth_file_list_update_current_thumb (file_list);
}


static void
_gth_file_list_exec_next_op (GthFileList *file_list)
{
	GList         *first;
	GthFileListOp *op;
	gboolean       exec_next_op = TRUE;

	if (file_list->priv->queue == NULL) {
		start_update_next_thumb (file_list);
		return;
	}

	first = file_list->priv->queue;
	file_list->priv->queue = g_list_remove_link (file_list->priv->queue, first);

	op = first->data;

	switch (op->type) {
	case GTH_FILE_LIST_OP_TYPE_SET_FILES:
		gfl_set_files (file_list, op->file_source, op->file_list);
		break;
	case GTH_FILE_LIST_OP_TYPE_ADD_FILES:
		gfl_add_files (file_list, op->file_list);
		break;
	case GTH_FILE_LIST_OP_TYPE_DELETE_FILES:
		gfl_delete_files (file_list, op->files);
		break;
	case GTH_FILE_LIST_OP_TYPE_UPDATE_FILES:
		gfl_update_files (file_list, op->file_list);
		break;
	case GTH_FILE_LIST_OP_TYPE_ENABLE_THUMBS:
		gfl_enable_thumbs (file_list, op->ival);
		exec_next_op = FALSE;
		break;
	case GTH_FILE_LIST_OP_TYPE_CLEAR_FILES:
		gfl_clear_list (file_list, op->sval);
		break;
	case GTH_FILE_LIST_OP_TYPE_SET_FILTER:
		gfl_set_filter (file_list, op->filter);
		break;
	case GTH_FILE_LIST_OP_TYPE_SET_SORT_FUNC:
		gfl_set_sort_func (file_list, op->cmp_func, op->inverse_sort);
		break;
	default:
		exec_next_op = FALSE;
		break;
	}

	gth_file_list_op_free (op);
	g_list_free (first);

	if (exec_next_op)
		_gth_file_list_exec_next_op (file_list);
}


int
gth_file_list_first_file (GthFileList *file_list,
			  gboolean     skip_broken,
			  gboolean     only_selected)
{
	GthFileView *view;
	GList       *files;
	GList       *scan;
	int          pos;

	view = GTH_FILE_VIEW (file_list->priv->view);
	files = gth_file_store_get_visibles (GTH_FILE_STORE (gth_file_view_get_model (view)));

	pos = 0;
	for (scan = files; scan; scan = scan->next, pos++) {
		GthFileData *file_data = scan->data;

		if (skip_broken && file_data->error)
			continue;
		if (only_selected && ! gth_file_selection_is_selected (GTH_FILE_SELECTION (view), pos))
			continue;

		return pos;
	}

	return -1;
}


int
gth_file_list_last_file (GthFileList *file_list,
			 gboolean     skip_broken,
			 gboolean     only_selected)
{
	GthFileView *view;
	GList       *files;
	GList       *scan;
	int          pos;

	view = GTH_FILE_VIEW (file_list->priv->view);
	files = gth_file_store_get_visibles (GTH_FILE_STORE (gth_file_view_get_model (view)));

	pos = g_list_length (files) - 1;
	if (pos < 0)
		return -1;

	for (scan = g_list_nth (files, pos); scan; scan = scan->prev, pos--) {
		GthFileData *file_data = scan->data;

		if (skip_broken && file_data->error)
			continue;
		if (only_selected && ! gth_file_selection_is_selected (GTH_FILE_SELECTION (view), pos))
			continue;

		return pos;
	}

	return -1;
}


int
gth_file_list_next_file (GthFileList *file_list,
			 int          pos,
			 gboolean     skip_broken,
			 gboolean     only_selected,
			 gboolean     wrap)
{
	GthFileView *view;
	GList       *files;
	GList       *scan;

	view = GTH_FILE_VIEW (file_list->priv->view);
	files = gth_file_store_get_visibles (GTH_FILE_STORE (gth_file_view_get_model (view)));

	pos++;
	if (pos >= 0)
		scan = g_list_nth (files, pos);
	else if (wrap)
		scan = g_list_first (files);
	else
		scan = NULL;

	for (/* void */; scan; scan = scan->next, pos++) {
		GthFileData *file_data = scan->data;

		if (skip_broken && file_data->error)
			continue;
		if (only_selected && ! gth_file_selection_is_selected (GTH_FILE_SELECTION (view), pos))
			continue;

		break;
	}

	_g_object_list_unref (files);

	return (scan != NULL) ? pos : -1;
}


int
gth_file_list_prev_file (GthFileList *file_list,
			 int          pos,
			 gboolean     skip_broken,
			 gboolean     only_selected,
			 gboolean     wrap)
{
	GthFileView *view;
	GList       *files;
	GList       *scan;

	view = GTH_FILE_VIEW (file_list->priv->view);
	files = gth_file_store_get_visibles (GTH_FILE_STORE (gth_file_view_get_model (view)));

	pos--;
	if (pos >= 0)
		scan = g_list_nth (files, pos);
	else if (wrap) {
		pos = g_list_length (files) - 1;
		scan = g_list_nth (files, pos);
	}
	else
		scan = NULL;

	for (/* void */; scan; scan = scan->prev, pos--) {
		GthFileData *file_data = scan->data;

		if (skip_broken && file_data->error)
			continue;
		if (only_selected && ! gth_file_selection_is_selected (GTH_FILE_SELECTION (view), pos))
			continue;

		break;
	}

	_g_object_list_unref (files);

	return (scan != NULL) ? pos : -1;
}
