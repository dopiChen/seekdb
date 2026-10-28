# Deploy the CSV/JSONL/Parquet test data used by the file_sql suite into a
# dedicated temporary directory and point secure_file_priv at it.
#
# The directory is a fixed path on purpose: mysqltest echoes every statement
# into the expected .result file, so a machine dependent path ($OBSERVER_DIR,
# $PWD, ...) would make the suite fail on any machine other than the one that
# recorded the result.
#
# The fixture list is explicit instead of a glob so that the directory contents is
# deterministic: data/ also carries the official sample files
# (basic.csv/basic.jsonl/invalid.txt/simple.parquet) that belong to other
# cases, and a wildcard copy would change the glob() row counts below.
#
# mysqltest (and obd) connect over TCP.  seekdb accepts secure_file_priv
# changes from connections that reach the server locally - unix socket or
# loopback TCP - so the SET below works in both harnesses.
--disable_query_log
let $FILE_SQL_DATA_DIR = /tmp/seekdb_file_sql_data;
let $FILE_SQL_RESET = rm -rf "$FILE_SQL_DATA_DIR" && mkdir -p "$FILE_SQL_DATA_DIR";
exec $FILE_SQL_RESET;
let $FILE_SQL_COPY = cp mysql_test/test_suite/file_sql/data/people.csv mysql_test/test_suite/file_sql/data/extra.csv mysql_test/test_suite/file_sql/data/nulls.csv mysql_test/test_suite/file_sql/data/types.csv mysql_test/test_suite/file_sql/data/people.jsonl mysql_test/test_suite/file_sql/data/people2.jsonl mysql_test/test_suite/file_sql/data/conflict.jsonl mysql_test/test_suite/file_sql/data/nested.jsonl mysql_test/test_suite/file_sql/data/bad.jsonl mysql_test/test_suite/file_sql/data/one.jsonl mysql_test/test_suite/file_sql/data/p_plain.parquet mysql_test/test_suite/file_sql/data/p_snappy.parquet mysql_test/test_suite/file_sql/data/p_dict.parquet "$FILE_SQL_DATA_DIR"/;
exec $FILE_SQL_COPY;
--enable_query_log

SET GLOBAL secure_file_priv = '/tmp/seekdb_file_sql_data';
SET GLOBAL enable_file_sql = true;
SET SESSION enable_file_sql = true;

# 用例内的真实表 COPY 需要一个默认库
CREATE DATABASE IF NOT EXISTS file_sql_test;
USE file_sql_test;
