# Deploy the CSV/JSONL test data used by the file_sql suite into the observer
# data directory and point secure_file_priv at it.
#
# The suite needs the same layout the competition test harness uses:
#   $OBSERVER_DIR/file_sql_data/{people.csv,extra.csv,people.jsonl}
--disable_query_log
let $FILE_SQL_DATA_DIR = $OBSERVER_DIR/file_sql_data;
let $FILE_SQL_MKDIR = mkdir -p "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_MKDIR;
let $FILE_SQL_COPY = cp $PWD/mysql_test/test_suite/file_sql/data/*.csv $PWD/mysql_test/test_suite/file_sql/data/*.jsonl $PWD/mysql_test/test_suite/file_sql/data/*.parquet "$FILE_SQL_DATA_DIR"/;
exec $FILE_SQL_COPY;
let $FILE_SQL_RM = rm -f "$FILE_SQL_DATA_DIR"/copy_*.csv "$FILE_SQL_DATA_DIR"/copy_*.jsonl "$FILE_SQL_DATA_DIR"/select_into.csv;
exec $FILE_SQL_RM;
--enable_query_log

--eval SET GLOBAL secure_file_priv = '$FILE_SQL_DATA_DIR';
SET GLOBAL enable_file_sql = true;
SET SESSION enable_file_sql = true;

# 用例内的真实表 COPY 需要一个默认库
CREATE DATABASE IF NOT EXISTS file_sql_test;
USE file_sql_test;
