#define _CRT_SECURE_NO_WARNINGS

#pragma comment(lib, "tsdump.lib")
#pragma comment(lib, "sqlite3.lib")
#pragma comment(lib, "shlwapi.lib")

#include <windows.h>
#include <stdio.h>
#include <shlwapi.h>

#include "sqlite3.h"
#include "modules_def.h"

static WCHAR *db_fname = NULL;

typedef struct {
	ch_info_t ch_info;
	WCHAR *fn_ts;

	ProgInfo initial_pi;
} mod_stat_t;

static inline __int64 timenum14_start(const ProgInfo *pi)
{
	__int64 tn = 0;
	tn += pi->recyear;
	tn *= 100;
	tn += pi->recmonth;
	tn *= 100;
	tn += pi->recday;
	tn *= 100;
	tn += pi->rechour;
	tn *= 100;
	tn += pi->recmin;
	tn *= 100;
	tn += pi->recsec;
	return tn;
}

static inline __int64 timenum14_dur(const ProgInfo *pi)
{
	__int64 tn = 0;
	tn += pi->durhour;
	tn *= 100;
	tn += pi->durmin;
	tn *= 100;
	tn += pi->dursec;
	return tn;
}

static void *hook_pgoutput_create(const WCHAR *fname, const ProgInfo* pi, const ch_info_t *ch_info)
{
	mod_stat_t *pstat;
	if ( ! db_fname ) {
		return NULL;
	}
	pstat = (mod_stat_t*)malloc(sizeof(mod_stat_t));
	pstat->ch_info = *ch_info;
	pstat->fn_ts = _wcsdup(fname);

	pstat->initial_pi = *pi;

	return pstat;
}

static int db_search(sqlite3 *dbh, const ProgInfo *pi)
{
	sqlite3_stmt *stmt = NULL;
	int ret;
	const char str[] = "SELECT * FROM programs WHERE chname = ? AND pgname = ? AND start = ?";
	ret = sqlite3_prepare_v2(dbh, str, sizeof(str)+1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_prepare_v2(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	ret = sqlite3_bind_text16(stmt, 1, pi->chname, pi->chnamelen*sizeof(WCHAR), SQLITE_STATIC);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}
	ret = sqlite3_bind_text16(stmt, 2, pi->pname, pi->pnamelen*sizeof(WCHAR), SQLITE_STATIC);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}
	ret = sqlite3_bind_int64(stmt, 3, timenum14_start(pi));
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}
	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		sqlite3_finalize(stmt);
		return 2;
	} else if (ret == SQLITE_DONE) {
		sqlite3_finalize(stmt);
		return 1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

static int db_insert(sqlite3 *dbh, const WCHAR *ts_path, const ProgInfo *pi, const ch_info_t *ci)
{
	sqlite3_stmt *stmt = NULL;
	__int64 id;
	int retval = 0;
	const char str1[] = "INSERT INTO programs(chname, chnum, pgname, start, duration, genre, genre_raw, detail, extended) "
		"values(?, ?, ?, ?, ?, ?, ?, ?, ?)";
	const char str2[] = "INSERT INTO bonparams values(?, ?, ?, ?, ?, ?, ?, ?)";

	WCHAR genre_str[1024], ts_fname[MAX_PATH_LEN];
	putGenreStr(genre_str, 1024-1, pi->genretype, pi->genre);
	unsigned char genre[4];
	if (pi->genretype[2] == 0x01) {
		genre[0] = 3;
	} else if (pi->genretype[1] == 0x01) {
		genre[0] = 2;
	} else if (pi->genretype[0] == 0x01) {
		genre[0] = 1;
	} else {
		genre[0] = 0;
	}
	genre[1] = pi->genre[0];
	genre[2] = pi->genre[1];
	genre[3] = pi->genre[2];

	wcsncpy(ts_fname, ts_path, MAX_PATH_LEN);
	PathStripPath(ts_fname);

	if( sqlite3_prepare_v2(dbh, str1, sizeof(str1) + 1, &stmt, NULL) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_prepare_v2(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	if( sqlite3_bind_text16(stmt, 1, pi->chname, pi->chnamelen*sizeof(WCHAR), SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 2, pi->chnum) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 3, pi->pname, pi->pnamelen*sizeof(WCHAR), SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int64(stmt, 4, timenum14_start(pi)) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int64(stmt, 5, timenum14_dur(pi)) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 6, genre_str, 1024*sizeof(WCHAR), SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_blob(stmt, 7, genre, 4, SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 8, pi->pdetail, pi->pdetaillen*sizeof(WCHAR), SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 9, pi->pextend, pi->pextendlen*sizeof(WCHAR), SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}

	if( sqlite3_step(stmt) != SQLITE_DONE ) {
		fwprintf(stderr, L"[ERROR] sqlite3_step(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_reset(stmt) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_reset(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_finalize( stmt ) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_finalize(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	id = sqlite3_last_insert_rowid(dbh);

	if( sqlite3_prepare_v2(dbh, str2, sizeof(str2) + 1, &stmt, NULL) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_prepare_v2(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	if( sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 2, ts_fname, -1, SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 3, ci->tuner_name, -1, SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 4, ci->sp_str, -1, SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 5, ci->sp_num) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_text16(stmt, 6, ci->ch_str, -1, SQLITE_STATIC) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 7, ci->ch_num) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (ci->service_id >= 0) {
		if (sqlite3_bind_int(stmt, 8, ci->service_id) != SQLITE_OK) {
			fwprintf(stderr, L"[ERROR] sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
			goto END;
		}
	}

	if( sqlite3_step(stmt) != SQLITE_DONE ) {
		fwprintf(stderr, L"[ERROR] sqlite3_step(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}

	retval = 1;
END:
	if( sqlite3_finalize( stmt ) != SQLITE_OK ) {
		fwprintf(stderr, L"[ERROR] sqlite3_finalize(): %s\n", sqlite3_errmsg16(dbh));
		retval = 0;
	}
	return retval;
}

static void hook_pgoutput_close(void *ps, const ProgInfo* pi)
{
	mod_stat_t *pstat = (mod_stat_t*)ps;
	sqlite3 *dbh = NULL;
	int ret, commit_ok = 0;

	if (!pstat) {
		return;
	}

	if (!pi->isok) {
		goto END3;
	}

	ret = sqlite3_open16(db_fname, &dbh);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_open16(): %s\n", sqlite3_errmsg16(dbh));
		fprintf(stderr, "[ERROR] sqliteデータベースのオープンに失敗しました\n");
		goto END2;
	}
	sqlite3_busy_timeout(dbh, 10 * 1000); /* 10s */
	ret = sqlite3_exec(dbh, "PRAGMA synchronous=1;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		fprintf(stderr, "[ERROR] データベースを synchronous=1 に設定できませんでした\n");
		goto END2;
	}
	ret = sqlite3_exec(dbh, "BEGIN IMMEDIATE;", NULL, NULL, NULL); /* BEGIN DEFFERED; (デフォルト) ではだめ */
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		fprintf(stderr, "[ERROR] sqliteのトランザクション開始に失敗しました\n");
		goto END2;
	}
	ret = db_search(dbh, pi);
	if (ret == 0) {
		fprintf(stderr, "[ERROR] sqliteデータベースの検索に失敗しました\n");
		goto END1;
	} else if (ret == 1) {
		ret = db_insert(dbh, pstat->fn_ts, pi, &pstat->ch_info);
		if (!ret) {
			fprintf(stderr, "[ERROR] sqliteデータベースの追加に失敗しました\n");
			goto END1;
		}
	} else {
		fprintf(stderr, "[WARN] sqliteデータベースにすでに番組が登録済みです\n");
		goto END1;
	}
	ret = sqlite3_exec(dbh, "COMMIT;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		fprintf(stderr, "[ERROR] sqliteのコミットに失敗しました\n");
	} else {
		commit_ok = 1;
	}
END1:
	if (!commit_ok) {
		ret = sqlite3_exec(dbh, "ROLLBACK;", NULL, NULL, NULL);
		if (ret != SQLITE_OK) {
			fwprintf(stderr, L"[ERROR] sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
			fprintf(stderr, "[ERROR] sqliteのロールバックに失敗しました\n");
		} else {
			printf("[WARN] データベースをロールバックしました\n");
		}
	}
END2:
	/* openに失敗してもcloseする */
	ret = sqlite3_close(dbh);
	if (ret != SQLITE_OK) {
		fwprintf(stderr, L"[ERROR] sqlite3_close(): %s\n", sqlite3_errmsg16(dbh));
		fprintf(stderr, "[ERROR] sqliteデータベースのクローズに失敗しました\n");
	}
END3:
	free(pstat->fn_ts);
	free(pstat);
}

/*
static void hook_pgoutput(void *ps, const unsigned char*, const size_t)
{
	mod_stat_t *pstat = (mod_stat_t*)ps;
	sqlite3 *dbh = NULL;
	int ret, commit_ok = 0;

	if (!pstat) {
		return;
	}

	ProgInfo *pi = &pstat->initial_pi;
}
*/

static void hook_close_stream()
{
	if (db_fname) {
		free(db_fname);
	}
}

static void register_hooks()
{
	register_hook_pgoutput_create(hook_pgoutput_create);
	//register_hook_pgoutput(hook_pgoutput);
	register_hook_pgoutput_close(hook_pgoutput_close);
	register_hook_close_stream(hook_close_stream);
}

static const WCHAR *set_db(const WCHAR *param)
{
	db_fname = _wcsdup(param);
	return NULL;
}

static cmd_def_t cmds[] = {
	{ L"-pgdb", L"sqlite DBファイル", 1, set_db },
	NULL,
};

MODULE_EXPORT module_def_t mod_pginfo_sqlite = {
	TSDUMP_MODULE_V1,
	L"mod_pginfo_sqlite",
	register_hooks,
	cmds
};