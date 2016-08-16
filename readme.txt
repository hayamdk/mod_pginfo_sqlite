・説明
tsdumpに組み込んで番組情報をsqliteのDBに追加していくモジュール

・ビルド方法
 - Windows: mod_pginfo_sqlite.slnをVisualStudioで開きビルド
 - Linuxほか: ./build.sh を実行

・使い方
tsdumpと同じディレクトリにmodules.confを作って、その中にmod_pginfo_sqlite.dll(.so)という行を追加。
そのディレクトリにmod_pginfo_sqlite.dll(.so)とsqlite3.dll（SQLiteのウェブページから入手、Windowsのみ）を置く。
pginfo.sqlite3が雛形のDBファイル（initdb.sqlで同じものを作成できる）。
tsdumpの引数に「--pgdb DBのファイル名」を追加する。