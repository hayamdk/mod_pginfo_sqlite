#define IS_SHARED_MODULE
#include "core/tsdump_def.h"

#ifdef TSD_PLATFORM_MSVC

#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN64
#pragma comment(lib, "win/sqlite3_x64.lib")
#elif _WIN32
#pragma comment(lib, "win/sqlite3.lib")
#endif

#include <windows.h>
#include "win/sqlite3.h"

#else

#include <sqlite3.h>

#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include "utils/arib_proginfo.h"
#include "core/module_hooks.h"
#include "utils/tsdstr.h"
#include "utils/path.h"

#ifdef TSD_PLATFORM_MSVC
#define sqlite3_errmsg sqlite3_errmsg16
#define sqlite3_bind_text sqlite3_bind_text16
#define sqlite3_open sqlite3_open16
#define SQLITE3_OPEN_FUNCNAME "sqlite3_open16"
#define SQLITE3_BIND_TEXT_FUNCNAME "sqlite3_bind_text16"
#else
#define SQLITE3_OPEN_FUNCNAME "sqlite3_open"
#define SQLITE3_BIND_TEXT_FUNCNAME "sqlite3_bind_text"
#endif

static int set_db_fname = 0;
static TSDCHAR db_fname[MAX_PATH_LEN];

typedef struct {
	TSDCHAR fn_ts[MAX_PATH_LEN];
	int64_t actual_start;
	int64_t actual_end;
} mod_stat_t;

static int64_t timenum14_now()
{
	int64_t tn;
	struct tm lt;
	time_t t = time(NULL);

#ifdef TSD_PLATFORM_MSVC
	localtime_s(&lt, &t);
#else
	lt = *(localtime(&t));
#endif

	tn = lt.tm_year + 1900;
	tn *= 100;
	tn += (lt.tm_mon + 1);
	tn *= 100;
	tn += lt.tm_mday;
	tn *= 100;
	tn += lt.tm_hour;
	tn *= 100;
	tn += lt.tm_min;
	tn *= 100;
	tn += lt.tm_sec;
	return tn;
}

static uint64_t timenum14(const time_mjd_t *time_mjd)
{
	int64_t tn = 0;
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

static void *hook_pgoutput_create(const TSDCHAR *fname, const proginfo_t *pi, const ch_info_t *ch_info, const int actually_start)
{
	UNREF_ARG(ch_info);
	UNREF_ARG(pi);
	mod_stat_t *pstat;

	if ( !set_db_fname ) {
		return NULL;
	}
	pstat = (mod_stat_t*)malloc(sizeof(mod_stat_t));
	tsd_strlcpy(pstat->fn_ts, fname, MAX_PATH_LEN - 1);
	if (actually_start) {
		pstat->actual_start = timenum14_now();
	} else {
		pstat->actual_start = -1;
	}
	pstat->actual_end = -1;
	return pstat;
}

/* 既存のレコードが存在するかどうか確認 */
static int db_search(sqlite3 *dbh, const proginfo_t *pi, int64_t *rec_id, int64_t *actual_start)
{
	sqlite3_stmt *stmt = NULL;
	time_mjd_t start_min, start_max;
	time_offset_t offset_day = {0};
	offset_day.sign = 1;
	offset_day.day = 1;

	int ret;
	int retval = 0;
	const char str[] = "SELECT * FROM programs WHERE service_id = ? AND event_id = ? AND start > ? AND start < ?";

	if (sqlite3_prepare_v2(dbh, str, sizeof(str) + 1, &stmt, NULL) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_prepare_v2(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_int(stmt, 1, pi->service_id) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
		sqlite3_finalize(stmt);
		goto END;
	}
	if (sqlite3_bind_int(stmt, 2, pi->event_id) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		sqlite3_finalize(stmt);
		goto END;
	}

	/* 検索範囲はstartの前後1日 */
	time_add_offset(&start_max, &pi->start, &offset_day);
	offset_day.sign = -1;
	time_add_offset(&start_min, &pi->start, &offset_day);

	if (sqlite3_bind_int64(stmt, 3, timenum14(&start_min)) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
		sqlite3_finalize(stmt);
		goto END;
	}
	if (sqlite3_bind_int64(stmt, 4, timenum14(&start_max)) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
		sqlite3_finalize(stmt);
		goto END;
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_ROW) {
		*rec_id = sqlite3_column_int64(stmt, 0);
		if (sqlite3_column_type(stmt, 11) == SQLITE_INTEGER) {
			/* 開始時間がセット済みのレコードの場合 */
			*actual_start = sqlite3_column_int64(stmt, 11);
		} else {
			*actual_start = -1;
		}
		retval = 2;
	} else if (ret == SQLITE_DONE) {
		retval = 1;
	}
END:
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_finalize(): %s\n"), sqlite3_errmsg(dbh));
		retval = 0;
	}
	return retval;
}

static void str_concat(TSDCHAR *dst_base, int *dst_used, int dst_max, const TSDCHAR *str)
{
	tsd_strlcpy(&dst_base[*dst_used], str, dst_max - *dst_used - 1);
	*dst_used += tsd_strlen(&dst_base[*dst_used]);
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

static int db_insert(sqlite3 *dbh, const TSDCHAR *ts_path, const proginfo_t *pi,
	int64_t actual_start, int64_t actual_end, int64_t logtime, int replace, int logmode, int64_t id)
{
	sqlite3_stmt *stmt = NULL;
	int retval = 0;
	//TSDCHAR ts_fname[MAX_PATH_LEN + 1];
	const TSDCHAR *ts_fname;
	const char *str_sql;

	uint8_t genre_raw[15] = { 0 };
	const TSDCHAR *genre1, *genre2;
	TSDCHAR genre_str[2048];
	int genre_str_used = 0;

	uint8_t detail_raw[(sizeof(pi->items->desc.aribstr_len) + sizeof(pi->items->item.aribstr_len) + 4) * (sizeof(pi->items)/ sizeof(pi->items[0])) + 2];
	TSDCHAR detail_str[(sizeof(pi->items->desc) + sizeof(pi->items->item) + 6) * (sizeof(pi->items) / sizeof(pi->items[0]))];
	int detail_raw_used = 0, detail_str_used = 0;
	uint16_t nbo_len, nbo_len1, nbo_len2;

	time_mjd_t endtime;
	int i;

	if (replace) {
		str_sql = "REPLACE INTO programs( nid, tsid, service_id, service_name, service_name_raw, event_id, program_name, program_name_raw,"
			" start, end, genre, genre_raw, program_text, program_text_raw, program_detail, program_detail_raw, record_filename, actual_start, actual_end, future, id ) "
			"values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	} else if(!logmode) {
		str_sql = "INSERT INTO programs( nid, tsid, service_id, service_name, service_name_raw, event_id, program_name, program_name_raw,"
			" start, end, genre, genre_raw, program_text, program_text_raw, program_detail, program_detail_raw, record_filename, actual_start, actual_end, future ) "
			"values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	} else {
		str_sql = "INSERT INTO changelog( nid, tsid, service_id, service_name, service_name_raw, event_id, program_name, program_name_raw,"
			" start, end, genre, genre_raw, program_text, program_text_raw, program_detail, program_detail_raw, record_filename, time, actual_time ) "
			"values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
	}

	/* ジャンル情報 */
	genre_raw[0] = (uint8_t)pi->genre_info.n_items;
	if (genre_raw[0] > 7) {
		genre_raw[0] = 7;
	}
	for (i = 0; i < genre_raw[0]; i++) {
		genre_raw[i*2+1] = (uint8_t)((pi->genre_info.items[i].content_nibble_level_1 << 4) + pi->genre_info.items[i].content_nibble_level_2);
		genre_raw[i*2+2] = (uint8_t)((pi->genre_info.items[i].user_nibble_1 << 4) + pi->genre_info.items[i].user_nibble_2);
		get_genre_str(&genre1, &genre2, pi->genre_info.items[i]);
		
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(TSDCHAR), genre1);
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(TSDCHAR), TSD_TEXT(" ("));
		str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(TSDCHAR), genre2);
		if (i == genre_raw[0] - 1) {
			str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(TSDCHAR), TSD_TEXT(")"));
		} else {
			str_concat(genre_str, &genre_str_used, sizeof(genre_str) / sizeof(TSDCHAR), TSD_TEXT(")\n"));
		}
	}

	/* 拡張形式イベント記述子 */
	if (!(pi->status & PGINFO_GET_EXTEND_TEXT)) {
		nbo_len = hton16(0);
	} else if (pi->n_items < 0x10000) {
		nbo_len = hton16((uint16_t)pi->n_items);
	} else {
		nbo_len = hton16(0xffff);
	}
	bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len, sizeof(nbo_len));
	for (i = 0; i < ntoh16(nbo_len); i++) {
		nbo_len1 = hton16((uint16_t)pi->items[i].desc.aribstr_len);
		nbo_len2 = hton16((uint16_t)pi->items[i].item.aribstr_len);
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len1, sizeof(nbo_len1));
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), pi->items[i].desc.aribstr, pi->items[i].desc.aribstr_len);
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), (uint8_t*)&nbo_len2, sizeof(nbo_len2));
		bin_concat(detail_raw, &detail_raw_used, sizeof(detail_raw), pi->items[i].item.aribstr, pi->items[i].item.aribstr_len);
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(TSDCHAR), pi->items[i].desc.str);
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(TSDCHAR), TSD_TEXT("\n"));
		str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(TSDCHAR), pi->items[i].item.str);
		if (i == ntoh16(nbo_len1) - 1) {
			str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(TSDCHAR), TSD_TEXT("\n"));
		} else {
			str_concat(detail_str, &detail_str_used, sizeof(detail_str) / sizeof(TSDCHAR), TSD_TEXT("\n\n"));
		}
	}

	/* 録画ファイル名 */
	//wcsncpy(ts_fname, ts_path, MAX_PATH_LEN);
	//PathStripPath(ts_fname);
	ts_fname = path_getfile(ts_path);

	if( sqlite3_prepare_v2(dbh, str_sql, strlen(str_sql), &stmt, NULL) != SQLITE_OK ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_prepare_v2(): %s\n"), sqlite3_errmsg(dbh));
		return 0;
	}
	if( sqlite3_bind_int(stmt, 1, pi->network_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 2, pi->ts_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if( sqlite3_bind_int(stmt, 3, pi->service_id) != SQLITE_OK ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_text(stmt, 4, pi->service_name.str, sizeof(TSDCHAR)*pi->service_name.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 5, pi->service_name.aribstr, pi->service_name.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_blob(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_int(stmt, 6, pi->event_id) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_text(stmt, 7, pi->event_name.str, sizeof(TSDCHAR)*pi->event_name.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 8, pi->event_name.aribstr, pi->event_name.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_blob(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}

	if(pi->status & PGINFO_UNKNOWN_STARTTIME) {
		if (sqlite3_bind_null(stmt, 9) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
		if (sqlite3_bind_null(stmt, 10) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
	} else {
		if (sqlite3_bind_int64(stmt, 9, timenum14(&pi->start)) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
		if (pi->status & PGINFO_UNKNOWN_DURATION) {
			if (sqlite3_bind_null(stmt, 10) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		} else {
			time_add_offset(&endtime, &pi->start, &pi->dur);
			if (sqlite3_bind_int64(stmt, 10, timenum14(&endtime)) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		}
	}

	if (sqlite3_bind_text(stmt, 11, genre_str, sizeof(TSDCHAR)*genre_str_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 12, genre_raw, 15, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_blob(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_text(stmt, 13, pi->event_text.str, sizeof(TSDCHAR)*pi->event_text.str_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 14, pi->event_text.aribstr, pi->event_text.aribstr_len, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_blob(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_text(stmt, 15, detail_str, sizeof(TSDCHAR)*detail_str_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_blob(stmt, 16, detail_raw, detail_raw_used, SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_blob(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}
	if (sqlite3_bind_text(stmt, 17, ts_fname, sizeof(TSDCHAR)*tsd_strlen(ts_fname) , SQLITE_STATIC) != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_BIND_TEXT_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}

	if (!logmode) {

		if (actual_start > 0) {
			if (sqlite3_bind_int64(stmt, 18, actual_start) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		} else {
			if (sqlite3_bind_null(stmt, 18) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		}

		if (actual_end > 0) {
			if (sqlite3_bind_int64(stmt, 19, actual_end) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		} else {
			if (sqlite3_bind_null(stmt, 19) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		}

		if (sqlite3_bind_int(stmt, 20, 0) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
	} else {
		if (logtime > 0) {
			if (sqlite3_bind_int64(stmt, 18, logtime) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		} else {
			if (sqlite3_bind_null(stmt, 18) != SQLITE_OK) {
				output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_null(): %s\n"), sqlite3_errmsg(dbh));
				goto END;
			}
		}
		if (sqlite3_bind_int64(stmt, 19, timenum14_now()) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
	}

	if (replace && !logmode) {
		if (sqlite3_bind_int64(stmt, 21, id) != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_bind_int64(): %s\n"), sqlite3_errmsg(dbh));
			goto END;
		}
	}

	if( sqlite3_step(stmt) != SQLITE_DONE ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_step(): %s\n"), sqlite3_errmsg(dbh));
		goto END;
	}

	retval = 1;
END:
	if( sqlite3_finalize( stmt ) != SQLITE_OK ) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_finalize(): %s\n"), sqlite3_errmsg(dbh));
		retval = 0;
	}
	return retval;
}

static void register_to_db(mod_stat_t *pstat, const proginfo_t* pi, int logmode)
{
	sqlite3 *dbh = NULL;
	int ret, commit_ok = 0;
	int64_t record_id, actual_start, logtime14;
	time_mjd_t logtime;

	if (!pstat) {
		return;
	}

	if (!PGINFO_READY(pi->status)) {
		return;
	}

	ret = sqlite3_open(db_fname, &dbh);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT(""SQLITE3_OPEN_FUNCNAME"(): %s\n"), sqlite3_errmsg(dbh));
		output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースのオープンに失敗しました\n"));
		goto END2;
	}
	sqlite3_busy_timeout(dbh, 10 * 1000); /* 10s */
	ret = sqlite3_exec(dbh, "PRAGMA synchronous=1;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_exec(): %s\n"), sqlite3_errmsg(dbh));
		output_message(MSG_ERROR, TSD_TEXT("データベースを synchronous=1 に設定できませんでした\n"));
		goto END2;
	}
	ret = sqlite3_exec(dbh, "BEGIN IMMEDIATE;", NULL, NULL, NULL); /* BEGIN DEFFERED; (デフォルト) ではだめ */
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_exec(): %s\n"), sqlite3_errmsg(dbh));
		output_message(MSG_ERROR, TSD_TEXT("sqliteのトランザクション開始に失敗しました\n"));
		goto END2;
	}

	if(!logmode) {
		ret = db_search(dbh, pi, &record_id, &actual_start);
		if (ret == 0) {
			output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースの検索に失敗しました\n"));
			goto END1;
		} else if (ret == 1) {
			/* 同じ番組の情報がまだデータベースに登録されていない */
			ret = db_insert(dbh, pstat->fn_ts, pi, pstat->actual_start, pstat->actual_end, 0, 0, 0, 0);
			if (!ret) {
				output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースの追加に失敗しました\n"));
				goto END1;
			}
		} else {
			/* 同じ番組の情報がデータベースに登録済み */
			output_message(MSG_WARNING, TSD_TEXT("sqliteデータベースにすでに番組が登録済みなので上書きします\n"));
			ret = db_insert(dbh, pstat->fn_ts, pi, actual_start, pstat->actual_end, 0, 1, 0, record_id);
			if (!ret) {
				output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースの追加に失敗しました\n"));
				goto END1;
			}
		}
	} else {
		if( get_stream_timestamp(pi, &logtime) ) {
			logtime14 = timenum14(&logtime);
		} else {
			logtime14 = -1;
		}
		ret = db_insert(dbh, pstat->fn_ts, pi, 0, 0, logtime14, 0, 1, 0);
		if (!ret) {
			output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースの追加に失敗しました\n"));
			goto END1;
		}
	}

	ret = sqlite3_exec(dbh, "COMMIT;", NULL, NULL, NULL);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_exec(): %s\n"), sqlite3_errmsg(dbh));
		output_message(MSG_ERROR, TSD_TEXT("sqliteのコミットに失敗しました\n"));
	} else {
		commit_ok = 1;
	}
END1:
	if (!commit_ok) {
		ret = sqlite3_exec(dbh, "ROLLBACK;", NULL, NULL, NULL);
		if (ret != SQLITE_OK) {
			output_message(MSG_ERROR, TSD_TEXT("sqlite3_exec(): %s\n"), sqlite3_errmsg(dbh));
			output_message(MSG_ERROR, TSD_TEXT("sqliteのロールバックに失敗しました\n"));
		} else {
			output_message(MSG_WARNING, TSD_TEXT("データベースをロールバックしました\n"));
		}
	}
END2:
	/* openに失敗してもcloseする */
	ret = sqlite3_close(dbh);
	if (ret != SQLITE_OK) {
		output_message(MSG_ERROR, TSD_TEXT("sqlite3_close(): %s\n"), sqlite3_errmsg(dbh));
		output_message(MSG_ERROR, TSD_TEXT("sqliteデータベースのクローズに失敗しました\n"));
	}
}

static void hook_pgoutput_changed(void *ps, const proginfo_t* pi_old, const proginfo_t *pi_new)
{
	mod_stat_t *pstat = (mod_stat_t*)ps;
	register_to_db(pstat, pi_old, 1);
}

static void hook_pgoutput_end(void *ps, const proginfo_t* pi)
{
	UNREF_ARG(pi);
	mod_stat_t *pstat = (mod_stat_t*)ps;
	pstat->actual_end = timenum14_now();
}

static void hook_pgoutput_close(void *ps, const proginfo_t* pi)
{
	mod_stat_t *pstat = (mod_stat_t*)ps;
	register_to_db(pstat, pi, 0);
	if (pstat) {
		free(pstat);
	}
}

static void register_hooks()
{
	register_hook_pgoutput_create(hook_pgoutput_create);
	register_hook_pgoutput_changed(hook_pgoutput_changed);
	register_hook_pgoutput_end(hook_pgoutput_end);
	register_hook_pgoutput_close(hook_pgoutput_close);
}

static const TSDCHAR *set_db(const TSDCHAR *param)
{
	set_db_fname = 1;
	tsd_strlcpy(db_fname, param, MAX_PATH_LEN);
	return NULL;
}

static cmd_def_t cmds[] = {
	{ TSD_TEXT("--pgdb"), TSD_TEXT("sqlite DBファイル"), 1, set_db },
	{ NULL },
};

TSD_MODULE_DEF(
	mod_pginfo_sqlite,
	register_hooks,
	cmds,
	NULL
);
