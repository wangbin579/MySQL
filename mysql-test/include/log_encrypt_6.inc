# Test tries restart after redo log encryption is enabled
# - Error   :restart without keyring plugin
# - Pass    :restart without keyring plugin and --innodb_force_recovery=6
# - Pass    :remove redo files,restart without keyring plugin

--source include/no_valgrind_without_big.inc

--disable_query_log
call mtr.add_suppression("keyring_file initialization failure. Please check if the keyring_file_data points to readable keyring file or keyring file can be created in the specified location. The keyring_file will stay unusable until correct path to the keyring file gets provided");
call mtr.add_suppression("Error while loading keyring content. The keyring might be malformed");
call mtr.add_suppression("\\[Error\\] InnoDB: Encryption can't find masterkey, please check the keyring is loaded.");
call mtr.add_suppression("\\[ERROR\\] InnoDB: Failed to find tablespace for table `\.\.*`\.`\.\.*` in the cache.");
call mtr.add_suppression("\\[ERROR\\] InnoDB: If you are installing InnoDB, remember that you must create directories yourself, InnoDB does not create them.");
call mtr.add_suppression ("InnoDB: New log files created");
--enable_query_log

let $old_innodb_file_per_table = `SELECT @@innodb_file_per_table`;
let $old_innodb_redo_log_encrypt = `SELECT @@innodb_redo_log_encrypt`;
let $START_PAGE_SIZE= `select @@innodb_page_size`;
let $LOG_FILE_SIZE= `select @@innodb_log_file_size`;
let $MYSQLD_BASEDIR= `select @@basedir`;

--mkdir $MYSQL_TMP_DIR/log_encrypt_dir1_$redo_log_mode

let $MYSQLD_DATADIR1 = $MYSQL_TMP_DIR/log_encrypt_dir1_$redo_log_mode;

let BOOTSTRAP_SQL=$MYSQL_TMP_DIR/boot.sql;

--echo # create bootstrap file
write_file $BOOTSTRAP_SQL;
CREATE DATABASE test;
EOF
--echo # Stop the MTR default DB server
--source include/shutdown_mysqld.inc

let NEW_CMD = $MYSQLD --no-defaults --innodb_dedicated_server=OFF --initialize-insecure --innodb_log_file_size=$LOG_FILE_SIZE --innodb_page_size=$START_PAGE_SIZE --basedir=$MYSQLD_BASEDIR --datadir=$MYSQLD_DATADIR1 --init-file=$BOOTSTRAP_SQL --secure-file-priv="" --innodb_redo_log_encrypt=$redo_log_encrypt_mode $KEYRING_PLUGIN_EARLY_LOAD --keyring_file_data=$MYSQL_TMP_DIR/my_key_redo4 $KEYRING_PLUGIN_OPT </dev/null>>$MYSQL_TMP_DIR/bootstrap1.log 2>&1;

--echo # Run the bootstrap command of datadir1
--exec $NEW_CMD

--echo # Start the DB server with datadir1
--let $restart_parameters = restart: $KEYRING_PLUGIN_EARLY_LOAD $KEYRING_PLUGIN_OPT --keyring_file_data=$MYSQL_TMP_DIR/my_key_redo4 --innodb_redo_log_encrypt=$redo_log_encrypt_mode --datadir=$MYSQLD_DATADIR1 --innodb_page_size=$START_PAGE_SIZE --innodb_log_file_size=$LOG_FILE_SIZE
--source include/start_mysqld_no_echo.inc

CREATE DATABASE tde_db;
USE tde_db;

eval SET GLOBAL innodb_redo_log_encrypt = $redo_log_encrypt_mode;
SELECT @@global.innodb_redo_log_encrypt ;

SELECT @@global.innodb_redo_log_encrypt ;
CREATE TABLE tde_db.t1 (a BIGINT PRIMARY KEY, b LONGBLOB) ENGINE=InnoDB;
INSERT INTO t1 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t1;
CREATE TABLE tde_db.t2 (a BIGINT PRIMARY KEY, b LONGBLOB)
			ENCRYPTION='Y' ENGINE=InnoDB;
INSERT INTO t2 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t2;
SELECT @@global.innodb_redo_log_encrypt ;
CREATE TABLE tde_db.t3 (a BIGINT PRIMARY KEY, b LONGBLOB) ENGINE=InnoDB;
INSERT INTO t3 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t3;
CREATE TABLE tde_db.t4 (a BIGINT PRIMARY KEY, b LONGBLOB)
			ENCRYPTION='Y' ENGINE=InnoDB;
INSERT INTO t4 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t4;

SELECT @@global.innodb_redo_log_encrypt ;
CREATE TABLE tde_db.t5 (a BIGINT PRIMARY KEY, b LONGBLOB) ENGINE=InnoDB;
INSERT INTO t5 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t5;
CREATE TABLE tde_db.t6 (a BIGINT PRIMARY KEY, b LONGBLOB)
			ENCRYPTION='Y' ENGINE=InnoDB;
INSERT INTO t6 (a, b) VALUES (1, REPEAT('a', 6*512*512));
SELECT a,LEFT(b,10) FROM tde_db.t6;

UNINSTALL PLUGIN keyring_file;
DROP DATABASE tde_db;

# shutdown server
--let $_server_id= `SELECT @@server_id`
--source include/shutdown_mysqld.inc

--echo # Try starting without keyring : Error
let NEW_CMD = $MYSQLD --no-defaults --innodb_dedicated_server=OFF --innodb_page_size=$START_PAGE_SIZE --innodb_log_file_size=$LOG_FILE_SIZE --basedir=$MYSQLD_BASEDIR --datadir=$MYSQLD_DATADIR1  --secure-file-priv="" --console </dev/null>>$MYSQL_TMP_DIR/wl9290.err 2>&1;
--error 1
--exec $NEW_CMD

--echo # Restart without keyring plugin possible if redo files removed
--remove_file $MYSQLD_DATADIR1/ib_logfile0
--remove_file $MYSQLD_DATADIR1/ib_logfile1

--echo # Start the DB server with datadir1
--let $restart_parameters = restart: --datadir=$MYSQLD_DATADIR1 --innodb_page_size=$START_PAGE_SIZE --innodb_log_file_size=$LOG_FILE_SIZE
--source include/start_mysqld_no_echo.inc

--echo # Search for error message
let SEARCH_FILE= $MYSQL_TMP_DIR/wl9290.err;
let SEARCH_PATTERN=  Redo log was encrypted, but keyring is not loaded;
--source include/search_pattern.inc

# shutdown server
--let $_server_id= `SELECT @@server_id`
--source include/shutdown_mysqld.inc

--echo # Try starting without keyring and  --innodb_force_recovery=SRV_FORCE_NO_LOG_REDO.
--echo # Start the DB server with datadir1
--let $restart_parameters = restart: --datadir=$MYSQLD_DATADIR1 --innodb_page_size=$START_PAGE_SIZE --innodb_log_file_size=$LOG_FILE_SIZE --innodb_force_recovery=6 --no-console --log-error=$MYSQL_TMP_DIR/wl9290_2.err
--source include/start_mysqld_no_echo.inc
SELECT 1;

# shutdown server
--let $_server_id= `SELECT @@server_id`
--source include/shutdown_mysqld.inc

# restart the server with MTR default
--let $restart_parameters =
--source include/start_mysqld.inc

--remove_file $BOOTSTRAP_SQL
--remove_file $MYSQL_TMP_DIR/my_key_redo4
--remove_file $MYSQL_TMP_DIR/bootstrap1.log
--remove_file $MYSQL_TMP_DIR/wl9290.err
--remove_file $MYSQL_TMP_DIR/wl9290_2.err

# Remove residue files
--force-rmdir $MYSQLD_DATADIR1