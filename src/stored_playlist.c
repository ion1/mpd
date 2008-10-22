/* the Music Player Daemon (MPD)
 * Copyright (C) 2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "stored_playlist.h"
#include "playlist_save.h"
#include "song.h"
#include "mapper.h"
#include "path.h"
#include "utils.h"
#include "ls.h"
#include "database.h"
#include "idle.h"
#include "os_compat.h"

static struct stored_playlist_info *
load_playlist_info(const char *parent_path_fs, const char *name_fs)
{
	size_t name_length = strlen(name_fs);
	char buffer[MPD_PATH_MAX], *name, *name_utf8;
	int ret;
	struct stat st;
	struct stored_playlist_info *playlist;

	if (name_length < 1 + sizeof(PLAYLIST_FILE_SUFFIX) ||
	    strlen(parent_path_fs) + name_length >= sizeof(buffer) ||
	    memchr(name_fs, '\n', name_length) != NULL)
		return NULL;

	if (name_fs[name_length - sizeof(PLAYLIST_FILE_SUFFIX)] != '.' ||
	    memcmp(name_fs + name_length - sizeof(PLAYLIST_FILE_SUFFIX) + 1,
		   PLAYLIST_FILE_SUFFIX,
		   sizeof(PLAYLIST_FILE_SUFFIX) - 1) != 0)
		return NULL;

	pfx_dir(buffer, name_fs, name_length,
		parent_path_fs, strlen(parent_path_fs));

	ret = stat(buffer, &st);
	if (ret < 0 || !S_ISREG(st.st_mode))
		return NULL;

	name = g_strdup(name_fs);
	name[name_length - sizeof(PLAYLIST_FILE_SUFFIX)] = 0;
	name_utf8 = fs_charset_to_utf8(buffer, name);
	g_free(name);
	if (name_utf8 == NULL)
		return NULL;

	playlist = g_new(struct stored_playlist_info, 1);
	playlist->name = g_strdup(name_utf8);
	playlist->mtime = st.st_mtime;
	return playlist;
}

GPtrArray *
spl_list(void)
{
	char parent_path_fs[MPD_PATH_MAX];
	DIR *dir;
	struct dirent *ent;
	GPtrArray *list;
	struct stored_playlist_info *playlist;

	rpp2app_r(parent_path_fs, "");
	dir = opendir(parent_path_fs);
	if (dir == NULL)
		return NULL;

	list = g_ptr_array_new();

	while ((ent = readdir(dir)) != NULL) {
		playlist = load_playlist_info(parent_path_fs, ent->d_name);
		if (playlist != NULL)
			g_ptr_array_add(list, playlist);
	}

	closedir(dir);
	return list;
}

void
spl_list_free(GPtrArray *list)
{
	for (unsigned i = 0; i < list->len; ++i) {
		struct stored_playlist_info *playlist =
			g_ptr_array_index(list, i);
		g_free(playlist->name);
		g_free(playlist);
	}

	g_ptr_array_free(list, true);
}

static ListNode *
spl_get_index(List *list, int idx)
{
	int forward;
	ListNode *node;
	int i;

	if (idx >= list->numberOfNodes || idx < 0)
		return NULL;

	if (idx > (list->numberOfNodes/2)) {
		forward = 0;
		node = list->lastNode;
		i = list->numberOfNodes - 1;
	} else {
		forward = 1;
		node = list->firstNode;
		i = 0;
	}

	while (node != NULL) {
		if (i == idx)
			return node;

		if (forward) {
			i++;
			node = node->nextNode;
		} else {
			i--;
			node = node->prevNode;
		}
	}

	return NULL;
}

static enum playlist_result
spl_save(List *list, const char *utf8path)
{
	ListNode *node;
	FILE *file;
	char path_max_tmp[MPD_PATH_MAX];

	assert(utf8path != NULL);

	utf8_to_fs_playlist_path(path_max_tmp, utf8path);

	while (!(file = fopen(path_max_tmp, "w")) && errno == EINTR);
	if (file == NULL)
		return PLAYLIST_RESULT_ERRNO;

	node = list->firstNode;
	while (node != NULL) {
		playlist_print_uri(file, (const char *)node->data);
		node = node->nextNode;
	}

	while (fclose(file) != 0 && errno == EINTR);
	return PLAYLIST_RESULT_SUCCESS;
}

List *
spl_load(const char *utf8path)
{
	List *list;
	FILE *file;
	char buffer[MPD_PATH_MAX];
	char path_max_tmp[MPD_PATH_MAX];

	if (!is_valid_playlist_name(utf8path))
		return NULL;

	utf8_to_fs_playlist_path(path_max_tmp, utf8path);
	while (!(file = fopen(path_max_tmp, "r")) && errno == EINTR);
	if (file == NULL)
		return NULL;

	list = makeList(DEFAULT_FREE_DATA_FUNC, 0);

	while (myFgets(buffer, sizeof(buffer), file)) {
		char *s = buffer;
		const char *path_utf8;

		if (*s == PLAYLIST_COMMENT)
			continue;

		if (isValidRemoteUtf8Url(s))
			insertInListWithoutKey(list, xstrdup(s));
		else {
			struct song *song;

			path_utf8 = map_fs_to_utf8(s, path_max_tmp);
			if (path_utf8 == NULL)
				continue;

			song = db_get_song(path_utf8);
			if (song == NULL)
				continue;

			song_get_url(song, path_max_tmp);
			insertInListWithoutKey(list, xstrdup(path_max_tmp));
		}

		if (list->numberOfNodes >= playlist_max_length)
			break;
	}

	while (fclose(file) && errno == EINTR);
	return list;
}

static int
spl_move_index_internal(List *list, int src, int dest)
{
	ListNode *srcNode, *destNode;

	if (src >= list->numberOfNodes || dest >= list->numberOfNodes ||
	    src < 0 || dest < 0 || src == dest)
		return -1;

	srcNode = spl_get_index(list, src);
	if (!srcNode)
		return -1;

	destNode = spl_get_index(list, dest);

	/* remove src */
	if (srcNode->prevNode)
		srcNode->prevNode->nextNode = srcNode->nextNode;
	else
		list->firstNode = srcNode->nextNode;

	if (srcNode->nextNode)
		srcNode->nextNode->prevNode = srcNode->prevNode;
	else
		list->lastNode = srcNode->prevNode;

	/* this is all a bit complicated - but I tried to
	 * maintain the same order stuff is moved as in the
	 * real playlist */
	if (dest == 0) {
		list->firstNode->prevNode = srcNode;
		srcNode->nextNode = list->firstNode;
		srcNode->prevNode = NULL;
		list->firstNode = srcNode;
	} else if ((dest + 1) == list->numberOfNodes) {
		list->lastNode->nextNode = srcNode;
		srcNode->nextNode = NULL;
		srcNode->prevNode = list->lastNode;
		list->lastNode = srcNode;
	} else {
		if (destNode == NULL) {
			/* this shouldn't be happening. */
			return -1;
		}

		if (src > dest) {
			destNode->prevNode->nextNode = srcNode;
			srcNode->prevNode = destNode->prevNode;
			srcNode->nextNode = destNode;
			destNode->prevNode = srcNode;
		} else {
			destNode->nextNode->prevNode = srcNode;
			srcNode->prevNode = destNode;
			srcNode->nextNode = destNode->nextNode;
			destNode->nextNode = srcNode;
		}
	}

	idle_add(IDLE_STORED_PLAYLIST);
	return 0;
}

enum playlist_result
spl_move_index(const char *utf8path, int src, int dest)
{
	List *list;
	enum playlist_result result;

	if (!(list = spl_load(utf8path)))
		return PLAYLIST_RESULT_NO_SUCH_LIST;

	if (spl_move_index_internal(list, src, dest) != 0) {
		freeList(list);
		return PLAYLIST_RESULT_BAD_RANGE;
	}

	result = spl_save(list, utf8path);

	freeList(list);

	idle_add(IDLE_STORED_PLAYLIST);
	return result;
}

enum playlist_result
spl_clear(const char *utf8path)
{
	char filename[MPD_PATH_MAX];
	FILE *file;

	if (!is_valid_playlist_name(utf8path))
		return PLAYLIST_RESULT_BAD_NAME;

	utf8_to_fs_playlist_path(filename, utf8path);

	while (!(file = fopen(filename, "w")) && errno == EINTR);
	if (file == NULL)
		return PLAYLIST_RESULT_ERRNO;

	while (fclose(file) != 0 && errno == EINTR);

	idle_add(IDLE_STORED_PLAYLIST);
	return PLAYLIST_RESULT_SUCCESS;
}

static int
spl_remove_index_internal(List *list, int pos)
{
	ListNode *node = spl_get_index(list, pos);
	if (!node)
		return -1;

	deleteNodeFromList(list, node);

	return 0;
}

enum playlist_result
spl_remove_index(const char *utf8path, int pos)
{
	List *list;
	enum playlist_result result;

	if (!(list = spl_load(utf8path)))
		return PLAYLIST_RESULT_NO_SUCH_LIST;

	if (spl_remove_index_internal(list, pos) != 0) {
		freeList(list);
		return PLAYLIST_RESULT_BAD_RANGE;
	}

	result = spl_save(list, utf8path);

	freeList(list);

	idle_add(IDLE_STORED_PLAYLIST);
	return result;
}

enum playlist_result
spl_append_song(const char *utf8path, struct song *song)
{
	FILE *file;
	struct stat st;
	char path_max_tmp[MPD_PATH_MAX];

	if (!is_valid_playlist_name(utf8path))
		return PLAYLIST_RESULT_BAD_NAME;
	utf8_to_fs_playlist_path(path_max_tmp, utf8path);

	while (!(file = fopen(path_max_tmp, "a")) && errno == EINTR);
	if (file == NULL) {
		int save_errno = errno;
		while (fclose(file) != 0 && errno == EINTR);
		errno = save_errno;
		return PLAYLIST_RESULT_ERRNO;
	}

	if (fstat(fileno(file), &st) < 0) {
		int save_errno = errno;
		while (fclose(file) != 0 && errno == EINTR);
		errno = save_errno;
		return PLAYLIST_RESULT_ERRNO;
	}

	if (st.st_size >= ((MPD_PATH_MAX+1) * playlist_max_length)) {
		while (fclose(file) != 0 && errno == EINTR);
		return PLAYLIST_RESULT_TOO_LARGE;
	}

	playlist_print_song(file, song);

	while (fclose(file) != 0 && errno == EINTR);

	idle_add(IDLE_STORED_PLAYLIST);
	return PLAYLIST_RESULT_SUCCESS;
}

enum playlist_result
spl_append_uri(const char *url, const char *utf8file)
{
	struct song *song;

	song = db_get_song(url);
	if (song)
		return spl_append_song(utf8file, song);

	if (!isValidRemoteUtf8Url(url))
		return PLAYLIST_RESULT_NO_SUCH_SONG;

	song = song_remote_new(url);
	if (song) {
		int ret = spl_append_song(utf8file, song);
		song_free(song);
		return ret;
	}

	return PLAYLIST_RESULT_NO_SUCH_SONG;
}

enum playlist_result
spl_rename(const char *utf8from, const char *utf8to)
{
	struct stat st;
	char from[MPD_PATH_MAX];
	char to[MPD_PATH_MAX];

	if (!is_valid_playlist_name(utf8from) ||
	    !is_valid_playlist_name(utf8to))
		return PLAYLIST_RESULT_BAD_NAME;

	utf8_to_fs_playlist_path(from, utf8from);
	utf8_to_fs_playlist_path(to, utf8to);

	if (stat(from, &st) != 0)
		return PLAYLIST_RESULT_NO_SUCH_LIST;

	if (stat(to, &st) == 0)
		return PLAYLIST_RESULT_LIST_EXISTS;

	if (rename(from, to) < 0)
		return PLAYLIST_RESULT_ERRNO;

	idle_add(IDLE_STORED_PLAYLIST);
	return PLAYLIST_RESULT_SUCCESS;
}