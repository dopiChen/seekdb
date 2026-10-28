# Remove exported COPY files and switch the feature back off.
--disable_query_log
let $FILE_SQL_RM = rm -f "$FILE_SQL_DATA_DIR"/copy_*.csv "$FILE_SQL_DATA_DIR"/copy_*.jsonl "$FILE_SQL_DATA_DIR"/select_into.csv;
exec $FILE_SQL_RM;
--enable_query_log

SET SESSION enable_file_sql = false;
SET GLOBAL enable_file_sql = false;
SET GLOBAL secure_file_priv = '';
DROP DATABASE IF EXISTS file_sql_test;
