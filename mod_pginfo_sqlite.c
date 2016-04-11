#define _CRT_SECURE_NO_WARNINGS

#pragma comment(lib, "tsdump.lib")
#pragma comment(lib, "sqlite3.lib")
#pragma comment(lib, "shlwapi.lib")

#include <windows.h>
#include <stdio.h>
#include <shlwapi.h>
#include <inttypes.h>

#include "sqlite3.h"

#define IN_SHARED_MODULE
#include "module_def.h"
#include "ts_proginfo.h"
#include "module_hooks.h"

static WCHAR *db_fname = NULL;

typedef struct {
	WCHAR fn_ts[MAX_PATH_LEN];
} mod_stat_t;

static uint64_t timenum14(const time_mjd_t *time_mjd)
{
	__int64 tn = 0;
	tn += time_mjd->year;
	tn *= 100;
	tn += time_mjd->mon;
	tn *= 100;
	tn += time_mjd->day;
	tn *= 100;
	tn += time_mjd->hour;
	tn *= 100;
	tn += time_mjd->min;
	tn *= 100;
	tn += time_mjd->sec;
	return tn;
}

static void *hook_pgoutput_create(const WCHAR *fname, const proginfo_t *pi, const ch_info_t *ch_info)
{
	mod_stat_t *pstat;
	if ( ! db_fname ) {
		return NULL;
	}
	pstat = (mod_stat_t*)malloc(sizeof(mod_stat_t));
	wcsncpy(pstat->fn_ts, fname, MAX_PATH_LEN - 1);
	return pstat;
}

/* 既存のレコードが存在するかどうか確認 */
static int db_search(sqlite3 *dbh, const proginfo_t *pi)
{
	sqlite3_stmt *stmt = NULL;
	time_mjd_t start_min, start_max;
	time_offset_t offset_day = {0};
	offset_day.sign = 1;
	offset_day.day = 1;

	int ret;
	const char str[] = "SELECT * FROM programs WHERE service_id = ? AND event_id = ? AND start > ? AND start < ?";
	ret = sqlite3_prepare_v2(dbh, str, sizeof(str)+1, &stmt, NULL);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_prepare_v2(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	ret = sqlite3_bind_int(stmt, 1, pi->service_id);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}
	ret = sqlite3_bind_int(stmt, 2, pi->event_id);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}

	/* 検索範囲はstartの前後1日 */
	time_add_offset(&start_max, &pi->start, &offset_day);
	offset_day.sign = -1;
	time_add_offset(&start_min, &pi->start, &offset_day);
	ret = sqlite3_bind_int64(stmt, 3, timenum14(&start_min));
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		sqlite3_finalize(stmt);
		return 0;
	}
	ret = sqlite3_bind_int64(stmt, 4, timenum14(&start_max));
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
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

static void str_concat(WCHAR *dst_base, int *dst_used, int dst_max, const WCHAR *str)
{
	wcscpy_s(&dst_base[*dst_used], dst_max-*dst_used-1, str);
	*dst_used += wcslen(&dst_base[*dst_used]);
}

static void bin_concat(uint8_t *dst_base, int *dst_used, int dst_max, const uint8_t * bin, int bin_len)
{
	int copysize = bin_len;
	if (*dst_used + copysize > dst_max) {
		copysize = dst_max - *dst_used;
	}
	if (copysize) {
		memcpy(&dst_base[*dst_used], bin, copysize);
	}
	*dst_used += copysize;
}

static uint16_t hton16(const uint16_t h)
{
	uint16_t n;
	((uint8_t*)&n)[0] = (h & 0xff00) >> 8;
	((uint8_t*)&n)[1] = h & 0xff;
	return n;
}

static uint16_t ntoh16(const uint16_t n)
{
	uint16_t h;
	h = (((uint8_t*)&n)[0] << 8) + ((uint8_t*)&n)[1];
	return h;
}

static int db_insert(sqlite3 *dbh, const WCHAR *ts_path, const proginfo_t *pi)
{
	sqlite3_stmt *stmt = NULL;
	int retval = 0;
	WCHAR ts_fname[MAX_PATH_LEN + 1];

	uint8_t genre_raw[15] = { 0 };
	WCHAR *genre1, *genre2, genre_str[2048];
	int genre_str_used = 0;

	uint8_t detail_raw[(sizeof(pi->items->aribdesc) + sizeof(pi->items->aribitem) + 4) * (sizeof(pi->items)/ sizeof(pi->items[0])) + 2];
	WCHAR detail_str[(sizeof(pi->items->desc) + sizeof(pi->items->item) + 6) * (sizeof(pi->items) / sizeof(pi->items[0]))];
	int detail_raw_used = 0, detail_str_used = 0;
	uint16_t nbo_len, nbo_len1, nbo_len2;

	time_mjd_t endtime;

	int i;

	const char str_sql[] = "INSERT INTO programs( nid, tsid, service_id, service_name, service_name_raw, event_id, program_name, program_name_raw,"
		" start, duration, genre, genre_raw, program_text, program_text_raw, program_detail, program_detail_raw, record_filename ) "
		"values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

	/* ジャンル情報 */
	genre_raw[0] = pi->genre_info.n_items;
	if (genre_raw[0] > 7) {
		genre_raw[0] = 7;
	}
	for (i = 0; i < genre_raw[0]; i++) {
		genre_raw[i*2+1] = (pi->genre_info.items[i].content_nibble_level_1 << 4) + pi->genre_info.items[i].content_nibble_level_2;
		genre_raw[i*2+2] = (pi->genre_info.items[i].user_nibble_1 << 4) + pi->genre_info.items[i].user_nibble_2;
		get_genre_str(&genre1, &genre2, pi->genre_info.items[i]);
		
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(WCHAR), genre1);
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(WCHAR), L" (");
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(WCHAR), genre2);
		if (i == genre_raw[0] - 1) {
			str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(WCHAR), L")");
		} else {
			str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(WCHAR), L")\n");
		}
	}

	/* 拡張形式イベント記述子 */
	if (pi->n_items < 0x10000) {
		nbo_len = hton16(pi->n_items);
	} else {
		nbo_len = hton16(0xffff);
	}
	bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len, sizeof(nbo_len));
	for (i = 0; i < ntoh16(nbo_len); i++) {
		nbo_len1 = hton16(pi->items[i].aribdesc_len);
		nbo_len2 = hton16(pi->items[i].aribitem_len);
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len1, sizeof(nbo_len1));
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), pi->items[i].aribdesc, pi->items[i].aribdesc_len);
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len2, sizeof(nbo_len2));
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), pi->items[i].aribitem, pi->items[i].aribitem_len);
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(WCHAR), pi->items[i].desc);
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(WCHAR), L"\n");
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(WCHAR), pi->items[i].item);
		if (i == ntoh16(nbo_len1) - 1) {
			str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(WCHAR), L"\n");
		} else {
			str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(WCHAR), L"\n\n");
		}
	}

	/* 録画ファイル名 */
	wcsncpy(ts_fname, ts_path, MAX_PATH_LEN);
	PathStripPath(ts_fname);

	if( sqlite3_prepare_v2(dbh, str_sql, sizeof(str_sql) + 1, &stmt, NULL) != SQLITE_OK ) {
		output_message(MSG_ERROR, L"sqlite3_prepare_v2(): %s\n", sqlite3_errmsg16(dbh));
		return 0;
	}
	if( sqlite3_bind_int(stmt, 1, pi->network_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, L"sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 2, pi->ts_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, L"sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 3, pi->service_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, L"sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 4, pi->service_name.str, sizeof(WCHAR)*pi->service_name.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 5, pi->service_name.aribstr, pi->service_name.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_int(stmt, 6, pi->event_id) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 7, pi->event_name.str, sizeof(WCHAR)*pi->event_name.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 8, pi->event_name.aribstr, pi->event_name.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_int64(stmt, 9, timenum14(&pi->start)) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	time_add_offset(&endtime, &pi->start, &pi->dur);
	if (sqlite3_bind_int64(stmt, 10, timenum14(&endtime)) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_int64(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 11, genre_str, sizeof(WCHAR)*genre_str_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 12, genre_raw, 15, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 13, pi->event_text.str, sizeof(WCHAR)*pi->event_text.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 14, pi->event_text.aribstr, pi->event_text.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 15, detail_str, sizeof(WCHAR)*detail_str_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 16, detail_raw, detail_raw_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_blob(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}
	if (sqlite3_bind_text16(stmt, 17, ts_fname, sizeof(WCHAR)*wcslen(ts_fname) , SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_bind_text16(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}

	if( sqlite3_step(stmt) != SQLITE_DONE ) {
		output_message(MSG_ERROR, L"sqlite3_step(): %s\n", sqlite3_errmsg16(dbh));
		goto END;
	}

	retval = 1;
END:
	if( sqlite3_finalize( stmt ) != SQLITE_OK ) {
		output_message(MSG_ERROR, L"sqlite3_finalize(): %s\n", sqlite3_errmsg16(dbh));
		retval = 0;
	}
	return retval;
}

static void hook_pgoutput_close(void *ps, const proginfo_t* pi)
{
	mod_stat_t *pstat = (mod_stat_t*)ps;
	sqlite3 *dbh = NULL;
	int ret, commit_ok = 0;

	if (!pstat) {
		return;
	}

	if (!PGINFO_READY(pi->status)) {
		goto END3;
	}

	ret = sqlite3_open16(db_fname, &dbh);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_open16(): %s\n", sqlite3_errmsg16(dbh));
		output_message(MSG_ERROR, L"sqliteデータベースのオープンに失敗しました\n");
		goto END2;
	}
	sqlite3_busy_timeout(dbh, 10 * 1000); /* 10s */
	ret = sqlite3_exec(dbh, "PRAGMA synchronous=1;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		output_message(MSG_ERROR, L"データベースを synchronous=1 に設定できませんでした\n");
		goto END2;
	}
	ret = sqlite3_exec(dbh, "BEGIN IMMEDIATE;", NULL, NULL, NULL); /* BEGIN DEFFERED; (デフォルト) ではだめ */
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		output_message(MSG_ERROR, L"sqliteのトランザクション開始に失敗しました\n");
		goto END2;
	}
	ret = db_search(dbh, pi);
	if (ret == 0) {
		output_message(MSG_ERROR, L"sqliteデータベースの検索に失敗しました\n");
		goto END1;
	} else if (ret == 1) {
		ret = db_insert(dbh, pstat->fn_ts, pi);
		if (!ret) {
			output_message(MSG_ERROR, L"sqliteデータベースの追加に失敗しました\n");
			goto END1;
		}
	} else {
		output_message(MSG_WARNING, L"sqliteデータベースにすでに番組が登録済みです\n");
		goto END1;
	}
	ret = sqlite3_exec(dbh, "COMMIT;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
		output_message(MSG_ERROR, L"sqliteのコミットに失敗しました\n");
	} else {
		commit_ok = 1;
	}
END1:
	if (!commit_ok) {
		ret = sqlite3_exec(dbh, "ROLLBACK;", NULL, NULL, NULL);
		if (ret != SQLITE_OK) {
			output_message(MSG_ERROR, L"sqlite3_exec(): %s\n", sqlite3_errmsg16(dbh));
			output_message(MSG_ERROR, L"sqliteのロールバックに失敗しました\n");
		} else {
			output_message(MSG_WARNING, L"データベースをロールバックしました\n");
		}
	}
END2:
	/* openに失敗してもcloseする */
	ret = sqlite3_close(dbh);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, L"sqlite3_close(): %s\n", sqlite3_errmsg16(dbh));
		output_message(MSG_ERROR, L"sqliteデータベースのクローズに失敗しました\n");
	}
END3:
	free(pstat);
}

static void hook_close_stream()
{
	if (db_fname) {
		free(db_fname);
	}
}

static void register_hooks()
{
	register_hook_pgoutput_create(hook_pgoutput_create);
	register_hook_pgoutput_close(hook_pgoutput_close);
	register_hook_close_stream(hook_close_stream);
}

static const WCHAR *set_db(const WCHAR *param)
{
	db_fname = _wcsdup(param);
	return NULL;
}

static cmd_def_t cmds[] = {
	{ L"--pgdb", L"sqlite DBファイル", 1, set_db },
	NULL,
};

MODULE_DEF module_def_t mod_pginfo_sqlite = {
	TSDUMP_MODULE_V3,
	L"mod_pginfo_sqlite",
	register_hooks,
	cmds
};
