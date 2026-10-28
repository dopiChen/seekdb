# Deploy the upstream competition fixtures (basic.csv / basic.jsonl /
# invalid.txt / simple.parquet) into a dedicated directory and point
# secure_file_priv at it.
#
# Unlike the upstream include this copies files locally instead of using
# ssh/scp, so the suite also runs on single-host CI.  The directory is a
# fixed path because mysqltest echoes every statement into the expected
# .result file.
--disable_query_log
let $FILE_SQL_DATA_DIR = /tmp/seekdb_file_sql_official_data;
let $FILE_SQL_RM = rm -rf "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_RM;
let $FILE_SQL_MKDIR = mkdir -p "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_MKDIR;
let $FILE_SQL_COPY = cp mysql_test/test_suite/file_sql/data/official/* "$FILE_SQL_DATA_DIR"/;
exec $FILE_SQL_COPY;
SET GLOBAL secure_file_priv = '/tmp/seekdb_file_sql_official_data';
--enable_query_log
