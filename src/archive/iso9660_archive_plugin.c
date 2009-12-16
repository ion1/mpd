/*
 * Copyright (C) 2003-2009 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
  * iso archive handling (requires cdio, and iso9660)
  */

#include "config.h"
#include "archive/iso9660_archive_plugin.h"
#include "archive_api.h"
#include "input_plugin.h"

#include <cdio/cdio.h>
#include <cdio/iso9660.h>

#include <glib.h>
#include <string.h>

#define CEILING(x, y) ((x+(y-1))/y)

struct iso9660_archive_file {
	struct archive_file base;

	iso9660_t *iso;
	iso9660_stat_t *statbuf;
	size_t cur_ofs;
	size_t max_blocks;
	GSList	*list;
	GSList	*iter;
};

static const struct input_plugin iso9660_input_plugin;

static inline GQuark
iso9660_quark(void)
{
	return g_quark_from_static_string("iso9660");
}

/* archive open && listing routine */

static void
listdir_recur(const char *psz_path, struct iso9660_archive_file *context)
{
	iso9660_t *iso = context->iso;
	CdioList_t *entlist;
	CdioListNode_t *entnode;
	iso9660_stat_t *statbuf;
	char pathname[4096];

	entlist = iso9660_ifs_readdir (iso, psz_path);
	if (!entlist) {
		return;
	}
	/* Iterate over the list of nodes that iso9660_ifs_readdir gives  */
	_CDIO_LIST_FOREACH (entnode, entlist) {
		statbuf = (iso9660_stat_t *) _cdio_list_node_data (entnode);

		strcpy(pathname, psz_path);
		strcat(pathname, statbuf->filename);

		if (_STAT_DIR == statbuf->type ) {
			if (strcmp(statbuf->filename, ".") && strcmp(statbuf->filename, "..")) {
				strcat(pathname, "/");
				listdir_recur(pathname, context);
			}
		} else {
			//remove leading /
			context->list = g_slist_prepend( context->list,
				g_strdup(pathname + 1));
		}
	}
	_cdio_list_free (entlist, true);
}

static struct archive_file *
iso9660_archive_open(const char *pathname)
{
	struct iso9660_archive_file *context =
		g_new(struct iso9660_archive_file, 1);

	archive_file_init(&context->base, &iso9660_archive_plugin);

	context->list = NULL;

	/* open archive */
	context->iso = iso9660_open (pathname);
	if (context->iso   == NULL) {
		g_warning("iso %s open failed\n", pathname);
		return NULL;
	}

	listdir_recur("/", context);

	return &context->base;
}

static void
iso9660_archive_scan_reset(struct archive_file *file)
{
	struct iso9660_archive_file *context =
		(struct iso9660_archive_file *)file;

	//reset iterator
	context->iter = context->list;
}

static char *
iso9660_archive_scan_next(struct archive_file *file)
{
	struct iso9660_archive_file *context =
		(struct iso9660_archive_file *)file;

	char *data = NULL;
	if (context->iter != NULL) {
		///fetch data and goto next
		data = context->iter->data;
		context->iter = g_slist_next(context->iter);
	}
	return data;
}

static void
iso9660_archive_close(struct archive_file *file)
{
	struct iso9660_archive_file *context =
		(struct iso9660_archive_file *)file;

	GSList *tmp;
	if (context->list) {
		//free list
		for (tmp = context->list; tmp != NULL; tmp = g_slist_next(tmp))
			g_free(tmp->data);
		g_slist_free(context->list);
	}
	//close archive
	iso9660_close(context->iso);

	g_free(context);
}

/* single archive handling */

static bool
iso9660_archive_open_stream(struct archive_file *file, struct input_stream *is,
		const char *pathname, GError **error_r)
{
	struct iso9660_archive_file *context =
		(struct iso9660_archive_file *)file;

	//setup file ops
	is->plugin = &iso9660_input_plugin;
	//insert back reference
	is->data = context;
	//we are not seekable
	is->seekable = false;

	context->statbuf = iso9660_ifs_stat_translate (context->iso, pathname);

	if (context->statbuf == NULL) {
		g_set_error(error_r, iso9660_quark(), 0,
			    "not found in the ISO file: %s", pathname);
		return false;
	}
	context->cur_ofs = 0;
	context->max_blocks = CEILING(context->statbuf->size, ISO_BLOCKSIZE);
	return true;
}

static void
iso9660_input_close(struct input_stream *is)
{
	struct iso9660_archive_file *context = is->data;
	g_free(context->statbuf);

	iso9660_archive_close((struct archive_file *)context);
}


static size_t
iso9660_input_read(struct input_stream *is, void *ptr, size_t size, GError **error_r)
{
	struct iso9660_archive_file *context = (struct iso9660_archive_file *) is->data;
	int toread, readed = 0;
	int no_blocks, cur_block;
	size_t left_bytes = context->statbuf->size - context->cur_ofs;

	size = (size * ISO_BLOCKSIZE) / ISO_BLOCKSIZE;

	if (left_bytes < size) {
		toread = left_bytes;
		no_blocks = CEILING(left_bytes,ISO_BLOCKSIZE);
	} else {
		toread = size;
		no_blocks = toread / ISO_BLOCKSIZE;
	}
	if (no_blocks > 0) {

		cur_block = context->cur_ofs / ISO_BLOCKSIZE;

		readed = iso9660_iso_seek_read (context->iso, ptr,
			context->statbuf->lsn + cur_block, no_blocks);

		if (readed != no_blocks * ISO_BLOCKSIZE) {
			g_set_error(error_r, iso9660_quark(), 0,
				    "error reading ISO file at lsn %lu",
				    (long unsigned int) cur_block);
			return 0;
		}
		if (left_bytes < size) {
			readed = left_bytes;
		}
		context->cur_ofs += readed;
	}
	return readed;
}

static bool
iso9660_input_eof(struct input_stream *is)
{
	struct iso9660_archive_file *context = is->data;

	return (context->cur_ofs == context->statbuf->size);
}

/* exported structures */

static const char *const iso9660_archive_extensions[] = {
	"iso",
	NULL
};

static const struct input_plugin iso9660_input_plugin = {
	.close = iso9660_input_close,
	.read = iso9660_input_read,
	.eof = iso9660_input_eof,
};

const struct archive_plugin iso9660_archive_plugin = {
	.name = "iso",
	.open = iso9660_archive_open,
	.scan_reset = iso9660_archive_scan_reset,
	.scan_next = iso9660_archive_scan_next,
	.open_stream = iso9660_archive_open_stream,
	.close = iso9660_archive_close,
	.suffixes = iso9660_archive_extensions
};