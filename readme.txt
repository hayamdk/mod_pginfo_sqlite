・説明
tsdump.exeに組み込んで番組情報をsqliteのDBに追加していくモジュール

・使い方
tsdump.exeと同じディレクトリにmodules.confを作って、その中にmod_pginfo_sqlite.dllという行を追加。
そのディレクトリにmod_pginfo_sqlite.dllとsqlite3.dll（SQLiteのウェブページから入手）を置く。
pginfo.sqlite3が雛形のDBファイル（initdb.sqlで同じものを作成できる）。
tsdump.exeの引数に「--pgdb DBのファイル名」を追加する。