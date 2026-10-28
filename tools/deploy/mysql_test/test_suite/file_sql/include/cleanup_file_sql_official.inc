# Remove only the fixture directory created by init_file_sql_official.inc
--disable_query_log
let $FILE_SQL_DATA_DIR = /tmp/seekdb_file_sql_official_data;
let $FILE_SQL_RM = rm -rf "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_RM;
SET SESSION enable_file_sql = false;
SET GLOBAL enable_file_sql = false;
SET GLOBAL secure_file_priv = '';
--enable_query_log
