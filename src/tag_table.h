/*
 * Copyright (C) 2003-2010 The Music Player Daemon Project
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

#ifndef MPD_TAG_TABLE_H
#define MPD_TAG_TABLE_H

#include "tag.h"

#include <glib.h>

/**
 * Looks up a string in a tag translation table (case insensitive).
 * Returns TAG_NUM_OF_ITEM_TYPES if the specified name was not found
 * in the table.
 */
static inline enum tag_type
tag_table_lookup(const char *const* table, const char *name)
{
	for (unsigned i = 0; i < TAG_NUM_OF_ITEM_TYPES; i++)
		if (table[i] != NULL &&
		    g_ascii_strcasecmp(name, table[i]) == 0)
			return (enum tag_type)i;

	return TAG_NUM_OF_ITEM_TYPES;
}

#endif
