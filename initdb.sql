PRAGMA journal_mode=WAL;
create table programs( id integer primary key, chname text, chnum integer, pgname text, start integer, duration integer, genre text, genre_raw blob, detail text, extended text );
create table bonparams( id integer, tsname text, bontuname text, bonspname text, bonspnum integer, bonchname text, bonchnum integer, serviceid integer );
create index programs_index on programs(chname, pgname, start);
create index bonparams_index on bonparams(tsname);
