/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sqlite3.h>

#include "common/debug.h"
#include "overlay.h"

void dt_overlay_record(const dt_imgid_t imgid, const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO overlay (imgid, overlay_id) "
                              "VALUES (?1, ?2)",
                              -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, overlay_id);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_overlay_remove(const dt_imgid_t imgid, const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM overlay"
                              " WHERE imgid = ?1 AND overlay_id = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, overlay_id);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_overlay_get_imgs(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT overlay_id"
                              " FROM overlay"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *res = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t overlay_id = sqlite3_column_int(stmt, 0);
    res = g_list_prepend(res, GINT_TO_POINTER(overlay_id));
  }
  sqlite3_finalize(stmt);
  return res;
}

GList *dt_overlay_get_used_in_imgs(const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid"
                              " FROM overlay"
                              " WHERE overlay_id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, overlay_id);

  GList *res = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
    res = g_list_prepend(res, GINT_TO_POINTER(imgid));
  }
  sqlite3_finalize(stmt);

  return res;
}
