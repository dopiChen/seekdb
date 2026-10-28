# Deploy the CSV/JSONL/Parquet test data used by the file_sql suite into a
# dedicated temporary directory and point secure_file_priv at it.
#
# The directory is a fixed path on purpose: mysqltest echoes every statement
# into the expected .result file, so a machine dependent path ($OBSERVER_DIR,
# $PWD, ...) would make the suite fail on any machine other than the one that
# recorded the result.
#
# mysqltest (and obd) connect over TCP.  seekdb accepts secure_file_priv
# changes from connections that reach the server locally - unix socket or
# loopback TCP - so the SET below works in both harnesses.
--disable_query_log
let $FILE_SQL_DATA_DIR = /tmp/seekdb_file_sql_data;
let $FILE_SQL_MKDIR = mkdir -p "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_MKDIR;
let $FILE_SQL_COPY = cp mysql_test/test_suite/file_sql/data/*.csv mysql_test/test_suite/file_sql/data/*.jsonl mysql_test/test_suite/file_sql/data/*.parquet "$FILE_SQL_DATA_DIR"/;
exec $FILE_SQL_COPY;
let $FILE_SQL_RM = rm -f "$FILE_SQL_DATA_DIR"/copy_*.csv "$FILE_SQL_DATA_DIR"/copy_*.jsonl "$FILE_SQL_DATA_DIR"/select_into.csv;
exec $FILE_SQL_RM;
--enable_query_log

SET GLOBAL secure_file_priv = '/tmp/seekdb_file_sql_data';
SET GLOBAL enable_file_sql = true;
SET SESSION enable_file_sql = true;

# 用例内的真实表 COPY 需要一个默认库
CREATE DATABASE IF NOT EXISTS file_sql_test;
USE file_sql_test;
