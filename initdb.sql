PRAGMA journal_mode=WAL;
create table programs( id integer primary key, nid integer, tsid integer, service_id integer, service_name integer, service_name_raw blob, event_id integer, program_name text, program_name_raw blob, start integer, duration integer, genre text, genre_raw blob, program_text text, program_text_raw blob, program_detail text, program_detail_raw blob, record_filename text );
create index programs_index on programs( nid, tsid, service_id, event_id, service_name, program_name, start, duration, record_filename );
