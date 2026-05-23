//
// Created by mete on 23.04.2026.
//

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/input.h"

static sqlite3 *db = NULL;

void history_init(const char *db_path) {
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        return;
    }
    
    const char *sql = "CREATE TABLE IF NOT EXISTS history ("
                      "id   INTEGER PRIMARY KEY AUTOINCREMENT,"
                      "cmd  TEXT NOT NULL,"
                      "ts   INTEGER DEFAULT (strftime('%s','now'))"
                      ");";
    
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_exec(db,
    "CREATE TABLE IF NOT EXISTS cd_visits("
    "  path     TEXT PRIMARY KEY,"
    "  visits   INTEGER DEFAULT 1,"
    "  last_ts  INTEGER DEFAULT (strftime('%s','now'))"
    ");",
    NULL, NULL, NULL);
}

void history_add(const char *line) {
    if (!line || !db) return;

    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return;

    const char *check_sql = "SELECT cmd FROM history ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *check;
    if (sqlite3_prepare_v2(db, check_sql, -1, &check, NULL) == SQLITE_OK) {
        if (sqlite3_step(check) == SQLITE_ROW) {
            const char *last = (const char *)sqlite3_column_text(check, 0);
            if (last && strcmp(last, line) == 0) {
                sqlite3_finalize(check);
                return;
            }
        }
        sqlite3_finalize(check);
    }

    /* INSERT */
    const char *sql = "INSERT INTO history (cmd) VALUES (?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, line, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

char *history_get(int offset) {
    if (!db || offset < 1) return NULL;
    
    const char *sql = "SELECT cmd FROM history ORDER BY id DESC LIMIT 1 OFFSET ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    
    sqlite3_bind_int(stmt, 1, offset - 1);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cmd = (const char *)sqlite3_column_text(stmt, 0);
        char *result = strdup(cmd);
        sqlite3_finalize(stmt);
        return result;
    }
    
    sqlite3_finalize(stmt);
    return NULL;
}

char *history_search_prefix(const char *prefix) {
    if (!db || !prefix || !*prefix) return NULL;
    
    const char *sql = "SELECT cmd FROM history WHERE cmd LIKE ? || '%' ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cmd = (const char *)sqlite3_column_text(stmt, 0);
        char *result = strdup(cmd);
        sqlite3_finalize(stmt);
        return result;
    }
    
    sqlite3_finalize(stmt);
    return NULL;
}

char *history_search(const char *query, int skip) {
    if (!db || !query || !*query) return NULL;
    
    const char *sql = "SELECT cmd FROM history WHERE cmd LIKE '%' || ? || '%' ORDER BY id DESC LIMIT 1 OFFSET ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    
    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, skip);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cmd = (const char *)sqlite3_column_text(stmt, 0);
        char *result = strdup(cmd);
        sqlite3_finalize(stmt);
        return result;
    }
    
    sqlite3_finalize(stmt);
    return NULL;
}

char **history_search_multi(const char *query, int max_results, int *count_out, int **ids_out) {
    if (!db || !query || !*query) { *count_out = 0; *ids_out = NULL; return NULL; }

    const char *sql =
    "SELECT cmd, last_id FROM ("
    "  SELECT cmd, MAX(id) as last_id FROM history "
    "  WHERE cmd LIKE '%' || ? || '%' "
    "  GROUP BY cmd"
    ") ORDER BY last_id DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        *count_out = 0; *ids_out = NULL; return NULL;
    }
    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_results);

    char **results = malloc(max_results * sizeof(char *));
    int *ids = malloc(max_results * sizeof(int));
    if (!results || !ids) { 
        sqlite3_finalize(stmt); 
        if (results) free(results);
        if (ids) free(ids);
        *count_out = 0; 
        *ids_out = NULL;
        return NULL; 
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        const char *cmd = (const char *)sqlite3_column_text(stmt, 0);
        results[count] = strdup(cmd);
        ids[count] = sqlite3_column_int(stmt, 1);
        count++;
    }
    sqlite3_finalize(stmt);
    *count_out = count;
    *ids_out = ids;
    if (count == 0) { 
        free(results); 
        free(ids);
        *ids_out = NULL;
        return NULL; 
    }
    return results;
}

int history_total_count(void) {
    if (!db) return 0;
    
    const char *sql = "SELECT COUNT(*) FROM history;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count;
    }
    
    sqlite3_finalize(stmt);
    return 0;
}

int history_count(void) {
    if (!db) return 0;
    
    const char *sql = "SELECT COUNT(*) FROM history;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count;
    }
    
    sqlite3_finalize(stmt);
    return 0;
}
/* ------------------------------------------------------------------ */
/*  Smart cd — frecency database                                        */
/*                                                                      */
/*  Table: cd_visits(path TEXT, visits INT, last_ts INT)               */
/*  Score: visits / (1 + age_days)  — simple frecency                  */
/* ------------------------------------------------------------------ */

void cd_visit(const char *path) {
    if (!db || !path || !*path) return;

    const char *upsert =
        "INSERT INTO cd_visits(path, visits, last_ts) VALUES(?,1,strftime('%s','now'))"
        " ON CONFLICT(path) DO UPDATE SET"
        "   visits  = visits + 1,"
        "   last_ts = strftime('%s','now');";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, upsert, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/*
 * Returns up to `limit` paths whose basename or full path contains `query`,
 * ordered by frecency score descending.
 * Caller must free each string and the array itself.
 */
char **cd_frecency_list(const char *query, int limit, int *count_out) {
    *count_out = 0;
    if (!db || limit <= 0) return NULL;

    const char *sql =
        "SELECT path FROM cd_visits"
        " WHERE path LIKE '%' || ? || '%'"
        " ORDER BY CAST(visits AS REAL) /"
        "   (1.0 + (strftime('%s','now') - last_ts) / 86400.0) DESC"
        " LIMIT ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, query  ? query  : "", -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 2, limit);

    char **results = malloc(limit * sizeof(char *));
    if (!results) { sqlite3_finalize(stmt); return NULL; }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < limit) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        results[count++] = p ? strdup(p) : strdup("");
    }
    sqlite3_finalize(stmt);

    if (count == 0) { free(results); return NULL; }
    *count_out = count;
    return results;
}

/* Single best frecency match — returns malloc'd string or NULL. */
char *cd_frecency_top(const char *query) {
    int count;
    char **list = cd_frecency_list(query, 1, &count);
    if (!list) return NULL;
    char *result = list[0];   /* take ownership */
    free(list);               /* free the array, NOT list[0] */
    return result;
}
void history_close(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}
