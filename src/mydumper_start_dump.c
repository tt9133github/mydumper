/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, SkySQL (andrew at skysql dot com)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <mysql.h>

#if defined MARIADB_CLIENT_VERSION_STR && !defined MYSQL_SERVER_VERSION
#define MYSQL_SERVER_VERSION MARIADB_CLIENT_VERSION_STR
#endif

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#ifdef ZWRAP_USE_ZSTD
#include "../zstd/zstd_zlibwrapper.h"
#else
#include <zlib.h>
#endif
#include <pcre.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <glib/gerror.h>
#include <gio/gio.h>
#include "config.h"
#include "server_detect.h"
#include "connection.h"
//#include "common_options.h"
#include "common.h"
#include <glib-unix.h>
#include <math.h>
#include "logging.h"
#include "set_verbose.h"
#include "locale.h"
#include <sys/statvfs.h>

#include "tables_skiplist.h"
#include "regex.h"

#include "mydumper_start_dump.h"
#include "mydumper_dump_into_file.h"
#include "mydumper_common.h"
#include "mydumper_stream.h"
#include "mydumper_database.h"
/* Some earlier versions of MySQL do not yet define MYSQL_TYPE_JSON */
#ifndef MYSQL_TYPE_JSON
#define MYSQL_TYPE_JSON 245
#endif

GMutex *init_mutex = NULL;
/* Program options */
extern GAsyncQueue *stream_queue;
extern int build_empty_files;
extern gchar *output_directory;
extern gchar *output_directory_param;
extern gchar *dump_directory;
extern guint statement_size;
extern guint rows_per_file;
extern guint chunk_filesize;
extern guint snapshot_count;
extern gboolean daemon_mode;
extern guint complete_insert;
extern gchar *fields_terminated_by;
extern gchar *fields_enclosed_by;
extern gchar *fields_escaped_by;
extern gchar *lines_starting_by;
extern gchar *lines_terminated_by;
extern gchar *statement_terminated_by;
extern gchar *fields_enclosed_by_ld;
extern gchar *fields_terminated_by_ld;
extern gchar *lines_starting_by_ld;
extern gchar *lines_terminated_by_ld;
extern gchar *disk_limits;
extern gboolean use_savepoints;
extern gboolean load_data;
extern gboolean stream;
extern int detected_server;
extern gboolean no_delete;
extern gboolean no_data;
extern char *defaults_file;
extern FILE * (*m_open)(const char *filename, const char *);
extern int (*m_close)(void *file);
extern int (*m_write)(FILE * file, const char * buff, int len);
extern gchar *compress_extension;
extern gchar *db;
extern GString *set_session;
extern guint num_threads;
extern char **tables;
extern gchar *tables_skiplist_file;

gint database_counter = 0;
gchar *ignore_engines = NULL;
char **ignore = NULL;
gchar *tidb_snapshot = NULL;
GList *no_updated_tables = NULL;
int longquery = 60;
int longquery_retries = 0;
int longquery_retry_interval = 60;
int skip_tz = 0;
int need_dummy_read = 0;
int need_dummy_toku_read = 0;
int compress_output = 0;
int killqueries = 0;
int lock_all_tables = 0;
int sync_wait = -1;
gboolean ignore_generated_fields = FALSE;
gboolean no_schemas = FALSE;
gboolean dump_checksums = FALSE;
gboolean no_locks = FALSE;
gboolean dump_events = FALSE;
gboolean dump_routines = FALSE;
gboolean no_dump_views = FALSE;
gboolean less_locking = FALSE;
gboolean success_on_1146 = FALSE;
gboolean no_backup_locks = FALSE;
gboolean insert_ignore = FALSE;
gboolean split_partitions = FALSE;
gboolean order_by_primary_key = FALSE;

GList *innodb_tables = NULL;
GMutex *innodb_tables_mutex = NULL;
GList *non_innodb_table = NULL;
GMutex *non_innodb_table_mutex = NULL;
GList *table_schemas = NULL;
GMutex *table_schemas_mutex = NULL;
GList *view_schemas = NULL;
GMutex *view_schemas_mutex = NULL;
GList *schema_post = NULL;
gint non_innodb_table_counter = 0;
gint non_innodb_done = 0;
guint less_locking_threads = 0;
guint updated_since = 0;
guint trx_consistency_only = 0;
gchar *set_names_str=NULL;
gchar *where_option=NULL;
guint64 max_rows=1000000;
GHashTable *all_anonymized_function=NULL;
guint pause_at=0;
guint resume_at=0;
gchar **db_items=NULL;


// For daemon mode
extern guint dump_number;
extern gboolean shutdown_triggered;
GAsyncQueue *start_scheduled_dump;
GMainLoop *m1;
GCond *ll_cond = NULL;
GMutex *ll_mutex = NULL;

guint errors;
struct tm tval;


static GOptionEntry start_dump_entries[] = {
    {"compress", 'c', 0, G_OPTION_ARG_NONE, &compress_output,
     "Compress output files", NULL},
    {"table-checksums", 'M', 0, G_OPTION_ARG_NONE, &dump_checksums,
     "Dump table checksums with the data", NULL},
    {"events", 'E', 0, G_OPTION_ARG_NONE, &dump_events, "Dump events. By default, it do not dump events", NULL},
    {"routines", 'R', 0, G_OPTION_ARG_NONE, &dump_routines,
     "Dump stored procedures and functions. By default, it do not dump stored procedures nor functions", NULL},
    {"no-views", 'W', 0, G_OPTION_ARG_NONE, &no_dump_views, "Do not dump VIEWs",
     NULL},
    {"long-query-retries", 0, 0, G_OPTION_ARG_INT, &longquery_retries,
     "Retry checking for long queries, default 0 (do not retry)", NULL},
    {"long-query-retry-interval", 0, 0, G_OPTION_ARG_INT, &longquery_retry_interval,
     "Time to wait before retrying the long query check in seconds, default 60", NULL},
    {"long-query-guard", 'l', 0, G_OPTION_ARG_INT, &longquery,
     "Set long query timer in seconds, default 60", NULL},    
    { "split-partitions", 0, 0, G_OPTION_ARG_NONE, &split_partitions,
      "Dump partitions into separate files. This options overrides the --rows option for partitioned tables.", NULL},
    {"tz-utc", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &skip_tz,
     "SET TIME_ZONE='+00:00' at top of dump to allow dumping of TIMESTAMP data "
     "when a server has data in different time zones or data is being moved "
     "between servers with different time zones, defaults to on use "
     "--skip-tz-utc to disable.",
     NULL},
    {"skip-tz-utc", 0, 0, G_OPTION_ARG_NONE, &skip_tz, "", NULL},
    {"tidb-snapshot", 'z', 0, G_OPTION_ARG_STRING, &tidb_snapshot,
     "Snapshot to use for TiDB", NULL},
    {"insert-ignore", 'N', 0, G_OPTION_ARG_NONE, &insert_ignore,
     "Dump rows with INSERT IGNORE", NULL},
    {"success-on-1146", 0, 0, G_OPTION_ARG_NONE, &success_on_1146,
     "Not increment error count and Warning instead of Critical in case of "
     "table doesn't exist",
     NULL},
    {"updated-since", 'U', 0, G_OPTION_ARG_INT, &updated_since,
     "Use Update_time to dump only tables updated in the last U days", NULL},
    { "no-check-generated-fields", 0, 0, G_OPTION_ARG_NONE, &ignore_generated_fields,
      "Queries related to generated fields are not going to be executed."
      "It will lead to restoration issues if you have generated columns", NULL },
    {"order-by-primary", 0, 0, G_OPTION_ARG_NONE, &order_by_primary_key,
     "Sort the data by Primary Key or Unique key if no primary key exists",
     NULL},    
    {"no-backup-locks", 0, 0, G_OPTION_ARG_NONE, &no_backup_locks,
     "Do not use Percona backup locks", NULL},
    {"lock-all-tables", 0, 0, G_OPTION_ARG_NONE, &lock_all_tables,
     "Use LOCK TABLE for all, instead of FTWRL", NULL},
    {"max-rows", 0, 0, G_OPTION_ARG_INT64, &max_rows,
     "Limit the number of rows per block after the table is estimated, default 1000000", NULL},
    { "where", 0, 0, G_OPTION_ARG_STRING, &where_option,
      "Dump only selected records.", NULL },
    {"no-schemas", 'm', 0, G_OPTION_ARG_NONE, &no_schemas,
      "Do not dump table schemas with the data and triggers", NULL},
    {"less-locking", 0, 0, G_OPTION_ARG_NONE, &less_locking,
     "Minimize locking time on InnoDB tables.", NULL},
    {"kill-long-queries", 'K', 0, G_OPTION_ARG_NONE, &killqueries,
     "Kill long running queries (instead of aborting)", NULL},
    {"no-locks", 'k', 0, G_OPTION_ARG_NONE, &no_locks,
     "Do not execute the temporary shared read lock.  WARNING: This will cause "
     "inconsistent backups",
     NULL},
    {"trx-consistency-only", 0, 0, G_OPTION_ARG_NONE, &trx_consistency_only,
     "Transactional consistency only", NULL},
    {"ignore-engines", 'i', 0, G_OPTION_ARG_STRING, &ignore_engines,
     "Comma delimited list of storage engines to ignore", NULL},
    { "set-names",0, 0, G_OPTION_ARG_STRING, &set_names_str,
      "Sets the names, use it at your own risk, default binary", NULL },
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};

void dump_table(MYSQL *conn, struct db_table *dbt,
                struct configuration *conf, gboolean is_innodb);
void create_jobs_for_non_innodb_table_list_in_less_locking_mode(MYSQL *, GList *, struct configuration *);
void restore_charset(GString *statement);
void set_charset(GString *statement, char *character_set,
                 char *collation_connection);
guint64 dump_table_data(MYSQL *conn, FILE *file, struct table_job *tj);
void dump_database_thread(MYSQL *, struct configuration*, struct database *);
void get_tables(MYSQL *conn, struct configuration *);
gchar *get_primary_key_string(MYSQL *conn, char *database, char *table);
void get_not_updated(MYSQL *conn, FILE *);
GList *get_chunks_for_table(MYSQL *, char *, char *,
                            struct configuration *conf);
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field,
                       char *from, char *to);
void dump_table_data_file(MYSQL *conn, struct table_job * tj);
void write_log_file(const gchar *log_domain, GLogLevelFlags log_level,
                    const gchar *message, gpointer user_data);
struct database * new_database(MYSQL *conn, char *database_name, gboolean already_dumped);
gboolean get_database(MYSQL *conn, char *database_name, struct database ** database);

void load_start_dump_entries(GOptionGroup *main_group){
  load_dump_into_file_entries(main_group);
  g_option_group_add_entries(main_group, start_dump_entries);
}


void initialize_start_dump(){
  initialize_common();
  non_innodb_table_mutex = g_mutex_new();
  innodb_tables_mutex = g_mutex_new();
  view_schemas_mutex = g_mutex_new();
  all_anonymized_function=g_hash_table_new ( g_str_hash, g_str_equal );
  table_schemas_mutex = g_mutex_new();
  init_mutex = g_mutex_new();
  ll_mutex = g_mutex_new();
  ll_cond = g_cond_new();

  initialize_dump_into_file();

  if (set_names_str){
    if (strlen(set_names_str)!=0){
      gchar *tmp_str=g_strdup_printf("/*!40101 SET NAMES %s*/",set_names_str);
      set_names_str=tmp_str;
    }else
      set_names_str=NULL;
  } else
    set_names_str=g_strdup("/*!40101 SET NAMES binary*/");  

  /* Give ourselves an array of engines to ignore */
  if (ignore_engines)
    ignore = g_strsplit(ignore_engines, ",", 0);

  // until we have an unique option on lock types we need to ensure this
  if (no_locks || trx_consistency_only)
    less_locking = 0;

  // clarify binlog coordinates with trx_consistency_only
  if (trx_consistency_only)
    g_warning("Using trx_consistency_only, binlog coordinates will not be "
              "accurate if you are writing to non transactional tables.");

  if (db){
    db_items=g_strsplit(db,",",0);
  }

  if (!compress_output) {
    m_open=&g_fopen;
    m_close=(void *) &fclose;
    m_write=(void *)&write_file;
    compress_extension=g_strdup("");
  } else {
    m_open=(void *) &gzopen;
    m_close=(void *) &gzclose;
    m_write=(void *)&gzwrite;
#ifdef ZWRAP_USE_ZSTD
    compress_extension = g_strdup(".zst");
#else
    compress_extension = g_strdup(".gz");
#endif
  }
}

GList * get_partitions_for_table(MYSQL *conn, char *database, char *table){
	MYSQL_RES *res=NULL;
	MYSQL_ROW row;

	GList *partition_list = NULL;

	gchar *query = g_strdup_printf("select PARTITION_NAME from information_schema.PARTITIONS where PARTITION_NAME is not null and TABLE_SCHEMA='%s' and TABLE_NAME='%s'", database, table);
	mysql_query(conn,query);
	g_free(query);

	res = mysql_store_result(conn);
	if (res == NULL)
		//partitioning is not supported
		return partition_list;
	while ((row = mysql_fetch_row(res))) {
		partition_list = g_list_append(partition_list, strdup(row[0]));
	}
	mysql_free_result(res);

	return partition_list;
}

// Global Var used:
// - dump_directory
// - compress_extension
gchar * build_filename(char *database, char *table, guint part, guint sub_part, const gchar *extension){
  GString *filename = g_string_sized_new(20);
  sub_part == 0 ?
    g_string_append_printf(filename, "%s.%s.%05d.%s%s", database, table, part, extension, compress_extension):
    g_string_append_printf(filename, "%s.%s.%05d.%05d.%s%s", database, table, part, sub_part, extension, compress_extension);
  gchar *r = g_build_filename(dump_directory, filename->str, NULL);
  g_string_free(filename,TRUE);
  return r;
}


gchar * build_data_filename(char *database, char *table, guint part, guint sub_part){
  return build_filename(database,table,part,sub_part,"sql");
}

void clear_dump_directory(gchar *directory) {
  GError *error = NULL;
  GDir *dir = g_dir_open(directory, 0, &error);

  if (error) {
    g_critical("cannot open directory %s, %s\n", directory,
               error->message);
    errors++;
    return;
  }

  const gchar *filename = NULL;

  while ((filename = g_dir_read_name(dir))) {
    gchar *path = g_build_filename(directory, filename, NULL);
    if (g_unlink(path) == -1) {
      g_critical("error removing file %s (%d)\n", path, errno);
      errors++;
      return;
    }
    g_free(path);
  }

  g_dir_close(dir);
}


/* Write some stuff we know about snapshot, before it changes */
void write_snapshot_info(MYSQL *conn, FILE *file) {
  MYSQL_RES *master = NULL, *slave = NULL, *mdb = NULL;
  MYSQL_FIELD *fields;
  MYSQL_ROW row;

  char *masterlog = NULL;
  char *masterpos = NULL;
  char *mastergtid = NULL;

  char *connname = NULL;
  char *slavehost = NULL;
  char *slavelog = NULL;
  char *slavepos = NULL;
  char *slavegtid = NULL;
  guint isms;
  guint i;

  mysql_query(conn, "SHOW MASTER STATUS");
  master = mysql_store_result(conn);
  if (master && (row = mysql_fetch_row(master))) {
    masterlog = row[0];
    masterpos = row[1];
    /* Oracle/Percona GTID */
    if (mysql_num_fields(master) == 5) {
      mastergtid = row[4];
    } else {
      /* Let's try with MariaDB 10.x */
      /* Use gtid_binlog_pos due to issue with gtid_current_pos with galera
       * cluster, gtid_binlog_pos works as well with normal mariadb server
       * https://jira.mariadb.org/browse/MDEV-10279 */
      mysql_query(conn, "SELECT @@gtid_binlog_pos");
      mdb = mysql_store_result(conn);
      if (mdb && (row = mysql_fetch_row(mdb))) {
        mastergtid = row[0];
      }
    }
  }

  if (masterlog) {
    fprintf(file, "SHOW MASTER STATUS:\n\tLog: %s\n\tPos: %s\n\tGTID:%s\n\n",
            masterlog, masterpos, mastergtid);
    g_message("Written master status");
  }

  isms = 0;
  mysql_query(conn, "SELECT @@default_master_connection");
  MYSQL_RES *rest = mysql_store_result(conn);
  if (rest != NULL && mysql_num_rows(rest)) {
    mysql_free_result(rest);
    g_message("Multisource slave detected.");
    isms = 1;
  }

  if (isms)
    mysql_query(conn, "SHOW ALL SLAVES STATUS");
  else
    mysql_query(conn, "SHOW SLAVE STATUS");

  guint slave_count=0;
  slave = mysql_store_result(conn);
  while (slave && (row = mysql_fetch_row(slave))) {
    fields = mysql_fetch_fields(slave);
    for (i = 0; i < mysql_num_fields(slave); i++) {
      if (isms && !strcasecmp("connection_name", fields[i].name))
        connname = row[i];
      if (!strcasecmp("exec_master_log_pos", fields[i].name)) {
        slavepos = row[i];
      } else if (!strcasecmp("relay_master_log_file", fields[i].name)) {
        slavelog = row[i];
      } else if (!strcasecmp("master_host", fields[i].name)) {
        slavehost = row[i];
      } else if (!strcasecmp("Executed_Gtid_Set", fields[i].name) ||
                 !strcasecmp("Gtid_Slave_Pos", fields[i].name)) {
        slavegtid = row[i];
      }
    }
    if (slavehost) {
      slave_count++;
      fprintf(file, "SHOW SLAVE STATUS:");
      if (isms)
        fprintf(file, "\n\tConnection name: %s", connname);
      fprintf(file, "\n\tHost: %s\n\tLog: %s\n\tPos: %s\n\tGTID:%s\n\n",
              slavehost, slavelog, slavepos, slavegtid);
      g_message("Written slave status");
    }
  }
  if (slave_count > 1)
    g_warning("Multisource replication found. Do not trust in the exec_master_log_pos as it might cause data inconsistencies. Search 'Replication and Transaction Inconsistencies' on MySQL Documentation");

  fflush(file);
  if (master)
    mysql_free_result(master);
  if (slave)
    mysql_free_result(slave);
  if (mdb)
    mysql_free_result(mdb);
}

// Free structures

void free_table_job(struct table_job *tj){
  if (tj->table)
    g_free(tj->table);
  if (tj->where)
    g_free(tj->where);
  if (tj->order_by)
    g_free(tj->order_by);
  if (tj->filename)
    g_free(tj->filename);
//  g_free(tj);
}

void message_dumping_data(struct thread_data *td, struct table_job *tj){
  g_message("Thread %d dumping data for `%s`.`%s`%s%s%s%s%s%s | Remaining jobs: %d",
                    td->thread_id, tj->database, tj->table, 
		    (tj->where || where_option ) ? " WHERE " : "", tj->where ? tj->where : "",
		    (tj->where && where_option ) ? " AND " : "", where_option ? where_option : "", 
                    tj->order_by ? " ORDER BY " : "", tj->order_by ? tj->order_by : "", g_async_queue_length(td->queue));
}

void thd_JOB_DUMP_DATABASE(struct configuration *conf, struct thread_data *td, struct job *job){
  struct dump_database_job * ddj = (struct dump_database_job *)job->job_data;
  g_message("Thread %d dumping db information for `%s`", td->thread_id,
            ddj->database->name);
  dump_database_thread(td->thrconn, conf, ddj->database);
  g_free(ddj);
  g_free(job);
  if (g_atomic_int_dec_and_test(&database_counter)) {
   g_async_queue_push(conf->ready_database_dump, GINT_TO_POINTER(1));
  }
}

void thd_JOB_DUMP(struct thread_data *td, struct job *job){
  struct table_job *tj = (struct table_job *)job->job_data;
  message_dumping_data(td,tj);
  if (use_savepoints && mysql_query(td->thrconn, "SAVEPOINT mydumper")) {
    g_critical("Savepoint failed: %s", mysql_error(td->thrconn));
  }
  dump_table_data_file(td->thrconn, tj);
  if (use_savepoints &&
      mysql_query(td->thrconn, "ROLLBACK TO SAVEPOINT mydumper")) {
    g_critical("Rollback to savepoint failed: %s", mysql_error(td->thrconn));
  }
  free_table_job(tj);
  g_free(job);
}

void thd_JOB_LOCK_DUMP_NON_INNODB(struct configuration *conf, struct thread_data *td, struct job *job, gboolean *first,GString *prev_database,GString *prev_table){
  GString *query=g_string_new(NULL);
  GList *glj;
  struct table_job *tj;
      struct tables_job *mj = (struct tables_job *)job->job_data;
      for (glj = mj->table_job_list; glj != NULL; glj = glj->next) {
        tj = (struct table_job *)glj->data;
        if (*first) {
          g_string_printf(query, "LOCK TABLES `%s`.`%s` READ LOCAL",
                          tj->database, tj->table);
          *first = 0;
        } else {
          if (g_ascii_strcasecmp(prev_database->str, tj->database) ||
              g_ascii_strcasecmp(prev_table->str, tj->table)) {
            g_string_append_printf(query, ", `%s`.`%s` READ LOCAL",
                                   tj->database, tj->table);
          }
        }
        g_string_printf(prev_table, "%s", tj->table);
        g_string_printf(prev_database, "%s", tj->database);
      }
      *first = 1;
      if (mysql_query(td->thrconn, query->str)) {
        g_critical("Non Innodb lock tables fail: %s", mysql_error(td->thrconn));
        exit(EXIT_FAILURE);
      }
      if (g_atomic_int_dec_and_test(&non_innodb_table_counter) &&
          g_atomic_int_get(&non_innodb_done)) {
        g_async_queue_push(conf->unlock_tables, GINT_TO_POINTER(1));
      }
      for (glj = mj->table_job_list; glj != NULL; glj = glj->next) {
        tj = (struct table_job *)glj->data;
        message_dumping_data(td,tj);
        dump_table_data_file(td->thrconn, tj);
        free_table_job(tj);
        g_free(tj);
      }
      mysql_query(td->thrconn, "UNLOCK TABLES /* Non Innodb */");
      g_list_free(mj->table_job_list);
      g_free(mj);
      g_free(job);
}

void initialize_thread(struct thread_data *td){
  m_connect(td->thrconn, "mydumper", NULL);
  g_message("Thread %d connected using MySQL connection ID %lu",
            td->thread_id, mysql_thread_id(td->thrconn));
}

void set_transaction_isolation_level_repeatable_read(MYSQL *conn){
  if (mysql_query(conn,
                  "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")) {
    g_critical("Failed to set isolation level: %s", mysql_error(conn));
    exit(EXIT_FAILURE);
  }
}


void initialize_consistent_snapshot(struct thread_data *td){
  if ( sync_wait != -1 && mysql_query(td->thrconn, g_strdup_printf("SET SESSION WSREP_SYNC_WAIT = %d",sync_wait))){
    g_critical("Failed to set wsrep_sync_wait for the thread: %s",
               mysql_error(td->thrconn));
    exit(EXIT_FAILURE);
  }
  set_transaction_isolation_level_repeatable_read(td->thrconn);
  if (mysql_query(td->thrconn,
                  "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */")) {
    g_critical("Failed to start consistent snapshot: %s", mysql_error(td->thrconn));
    exit(EXIT_FAILURE);
  }
}

void check_connection_status(struct thread_data *td){
  if (detected_server == SERVER_TYPE_TIDB) {
    // Worker threads must set their tidb_snapshot in order to be safe
    // Because no locking has been used.
    gchar *query =
        g_strdup_printf("SET SESSION tidb_snapshot = '%s'", tidb_snapshot);
    if (mysql_query(td->thrconn, query)) {
      g_critical("Failed to set tidb_snapshot: %s", mysql_error(td->thrconn));
      exit(EXIT_FAILURE);
    }
    g_free(query);
    g_message("Thread %d set to tidb_snapshot '%s'", td->thread_id,
              tidb_snapshot);
  }

  /* Unfortunately version before 4.1.8 did not support consistent snapshot
   * transaction starts, so we cheat */
  if (need_dummy_read) {
    mysql_query(td->thrconn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.mydumperdummy");
    MYSQL_RES *res = mysql_store_result(td->thrconn);
    if (res)
      mysql_free_result(res);
  }
  if (need_dummy_toku_read) {
    mysql_query(td->thrconn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.tokudbdummy");
    MYSQL_RES *res = mysql_store_result(td->thrconn);
    if (res)
      mysql_free_result(res);
  }
}

void *working_thread(struct thread_data *td) {
  struct configuration *conf = td->conf;
  // mysql_init is not thread safe, especially in Connector/C
  g_mutex_lock(init_mutex);
  td->thrconn = mysql_init(NULL);
  g_mutex_unlock(init_mutex);

  initialize_thread(td);
  execute_gstring(td->thrconn, set_session);

  // Initialize connection 
  if (!skip_tz && mysql_query(td->thrconn, "/*!40103 SET TIME_ZONE='+00:00' */")) {
    g_critical("Failed to set time zone: %s", mysql_error(td->thrconn));
  }
  if (!td->less_locking_stage){
    if (use_savepoints && mysql_query(td->thrconn, "SET SQL_LOG_BIN = 0")) {
      g_critical("Failed to disable binlog for the thread: %s",
                 mysql_error(td->thrconn));
      exit(EXIT_FAILURE);
    }
    initialize_consistent_snapshot(td);
    check_connection_status(td);
  }
  if (set_names_str)
    mysql_query(td->thrconn, set_names_str);

  g_async_queue_push(td->ready, GINT_TO_POINTER(1));
  // Thread Ready to process jobs
 
  struct job *job = NULL;
  int first = 1;
  GString *query = g_string_new(NULL);
  GString *prev_table = g_string_new(NULL);
  GString *prev_database = g_string_new(NULL);
  /* if less locking we need to wait until that threads finish
      progressively waking up these threads */
  if (!td->less_locking_stage && less_locking) {
    g_mutex_lock(ll_mutex);

    while (less_locking_threads >= td->thread_id) {
      g_cond_wait(ll_cond, ll_mutex);
    }

    g_mutex_unlock(ll_mutex);
  }

  GMutex *resume_mutex=NULL;

  for (;;) {
    if (conf->pause_resume){
      resume_mutex = (GMutex *)g_async_queue_try_pop(conf->pause_resume);
      if (resume_mutex != NULL){
        g_mutex_lock(resume_mutex);
        g_mutex_unlock(resume_mutex);
        resume_mutex=NULL;
        continue;
      }
    }

    job = (struct job *)g_async_queue_pop(td->queue);
    if (shutdown_triggered && (job->type != JOB_SHUTDOWN)) {
      continue;
    }

    switch (job->type) {
    case JOB_LOCK_DUMP_NON_INNODB:
      thd_JOB_LOCK_DUMP_NON_INNODB(conf, td, job, &first, prev_database, prev_table);
      break;
    case JOB_DUMP:
      thd_JOB_DUMP(td, job);
      break;
    case JOB_DUMP_NON_INNODB:
      thd_JOB_DUMP(td, job);
      if (g_atomic_int_dec_and_test(&non_innodb_table_counter) &&
          g_atomic_int_get(&non_innodb_done)) {
        g_async_queue_push(conf->unlock_tables, GINT_TO_POINTER(1));
      }
      break;
    case JOB_CHECKSUM:
      do_JOB_CHECKSUM(td,job);
      break;
    case JOB_DUMP_DATABASE:
      thd_JOB_DUMP_DATABASE(conf,td,job);
      break;
    case JOB_CREATE_DATABASE:
      do_JOB_CREATE_DATABASE(td,job);
      break;
    case JOB_SCHEMA:
      do_JOB_SCHEMA(td,job);
      break;
    case JOB_VIEW:
      do_JOB_VIEW(td,job);
      break;
    case JOB_TRIGGERS:
      do_JOB_TRIGGERS(td,job);
      break;
    case JOB_SCHEMA_POST:
      do_JOB_SCHEMA_POST(td,job);
      break;
    case JOB_SHUTDOWN:
      g_message("Thread %d shutting down", td->thread_id);
      if (td->less_locking_stage){
        g_mutex_lock(ll_mutex);
        less_locking_threads--;
        g_cond_broadcast(ll_cond);
        g_mutex_unlock(ll_mutex);
        g_string_free(query, TRUE);
        g_string_free(prev_table, TRUE);
        g_string_free(prev_database, TRUE);
      }
      if (td->thrconn)
        mysql_close(td->thrconn);
      g_free(job);
      mysql_thread_end();
      return NULL;
      break;
    default:
      g_critical("Something very bad happened!");
      exit(EXIT_FAILURE);
    }
  }
  if (td->thrconn)
    mysql_close(td->thrconn);
  mysql_thread_end();
  return NULL;
}

void set_disk_limits(guint p_at, guint r_at){
  pause_at=p_at;
  resume_at=r_at;
}

gboolean is_disk_space_ok(guint val){
  struct statvfs buffer;
  int ret = statvfs(output_directory, &buffer);
  if (!ret) {
    const double available = (double)(buffer.f_bfree * buffer.f_frsize) / 1024 / 1024;
    return available > val;
  }else{
    g_warning("Disk space check failed");
  }
  return TRUE;
}

void *monitor_disk_space_thread (void *queue){
  (void)queue;
  guint i=0;
  // This should be done with mutex not queues! what was I thinking?
  GMutex **pause_mutex_per_thread=g_new(GMutex * , num_threads) ;
  for(i=0;i<num_threads;i++){
    pause_mutex_per_thread[i]=g_mutex_new();
  }

  gboolean previous_state = TRUE, current_state = TRUE;

  while (disk_limits != NULL){
    current_state = previous_state ? is_disk_space_ok(pause_at) : is_disk_space_ok(resume_at);
    if (previous_state != current_state){
      if (!current_state){
        g_warning("Pausing backup disk space lower than %dMB. You need to free up to %dMB to resume",pause_at,resume_at);
        for(i=0;i<num_threads;i++){
          g_mutex_lock(pause_mutex_per_thread[i]);
          g_async_queue_push(queue,pause_mutex_per_thread[i]);
        }
      }else{
        g_warning("Resuming backup");
        for(i=0;i<num_threads;i++){
          g_mutex_unlock(pause_mutex_per_thread[i]);
        }
      }
      previous_state = current_state;

    }
    sleep(10);
  }
  return NULL;
}

GMutex **pause_mutex_per_thread=NULL;

gboolean sig_triggered(void * user_data, int signal) {
  if (signal == SIGTERM){
    shutdown_triggered = TRUE;
  }else{

    guint i=0;
    if (pause_mutex_per_thread == NULL){
      pause_mutex_per_thread=g_new(GMutex * , num_threads) ;
      for(i=0;i<num_threads;i++){
        pause_mutex_per_thread[i]=g_mutex_new();
      }
    }
    if (((struct configuration *)user_data)->pause_resume == NULL)
      ((struct configuration *)user_data)->pause_resume = g_async_queue_new();
    GAsyncQueue *queue = ((struct configuration *)user_data)->pause_resume;
    if (!daemon_mode){
      g_critical("Ctrl+c detected! Are you sure you want to cancel(Y/N)?");
      for(i=0;i<num_threads;i++){
        g_mutex_lock(pause_mutex_per_thread[i]);
        g_async_queue_push(queue,pause_mutex_per_thread[i]);
      }
      int c=0;
      while (1){
        do{
          c=fgetc(stdin);
        }while (c=='\n');
        if ( c == 'N' || c == 'n'){
          for(i=0;i<num_threads;i++)
            g_mutex_unlock(pause_mutex_per_thread[i]);
          return TRUE;
        }
        if ( c == 'Y' || c == 'y'){
          shutdown_triggered = TRUE;
          for(i=0;i<num_threads;i++)
            g_mutex_unlock(pause_mutex_per_thread[i]);
          goto finish;
        }
      }
    }
  }
finish:
  g_message("Shutting down gracefully");
  return FALSE;
}

gboolean sig_triggered_int(void * user_data) {
  return sig_triggered(user_data,SIGINT);
}
gboolean sig_triggered_term(void * user_data) {
  return sig_triggered(user_data,SIGTERM);
}

void *signal_thread(void *data) {
  GMainLoop * loop=NULL;
  g_unix_signal_add(SIGINT, sig_triggered_int, data);
  g_unix_signal_add(SIGTERM, sig_triggered_term, data);
  loop = g_main_loop_new (NULL, TRUE);
  g_main_loop_run (loop);
  g_message("Ending signal thread");
  return NULL;
}


MYSQL *create_main_connection() {
  MYSQL *conn;
  conn = mysql_init(NULL);

  m_connect(conn, "mydumper",db_items!=NULL?db_items[0]:db);

  set_session = g_string_new(NULL);
  detected_server = detect_server(conn);
  GHashTable * set_session_hash = initialize_hash_of_session_variables();
  if (defaults_file){
    load_hash_from_key_file(set_session_hash, all_anonymized_function, defaults_file, "mydumper_variables");
  }
  refresh_set_session_from_hash(set_session,set_session_hash);
  execute_gstring(conn, set_session);

  switch (detected_server) {
  case SERVER_TYPE_MYSQL:
    g_message("Connected to a MySQL server");
    set_transaction_isolation_level_repeatable_read(conn);
    break;
  case SERVER_TYPE_DRIZZLE:
    g_message("Connected to a Drizzle server");
    break;
  case SERVER_TYPE_TIDB:
    g_message("Connected to a TiDB server");
    break;
  default:
    g_critical("Cannot detect server type");
    exit(EXIT_FAILURE);
    break;
  }

  return conn;
}

void *exec_thread(void *data) {
  (void)data;

  while (1) {
    g_async_queue_pop(start_scheduled_dump);
    MYSQL *conn = create_main_connection();
    char *dump_number_str=g_strdup_printf("%d",dump_number);
    dump_directory = g_build_path("/", output_directory, dump_number_str, NULL);
    g_free(dump_number_str);
    clear_dump_directory(dump_directory);
    start_dump(conn);
    // start_dump already closes mysql
    // mysql_close(conn);
    mysql_thread_end();

    // Don't switch the symlink on shutdown because the dump is probably
    // incomplete.
    if (!shutdown_triggered) {
      char *dump_symlink_source= g_strdup_printf("%d", dump_number);
      char *dump_symlink_dest =
          g_strdup_printf("%s/last_dump", output_directory);

      // We don't care if this fails
      g_unlink(dump_symlink_dest);

      if (symlink(dump_symlink_source, dump_symlink_dest) == -1) {
        g_critical("error setting last good dump symlink %s, %d",
                   dump_symlink_dest, errno);
      }
      g_free(dump_symlink_dest);

      if (dump_number >= snapshot_count-1) dump_number = 0;
      else dump_number++;
    }
  }
  return NULL;
}

void start_dump(MYSQL *conn) {
  struct configuration conf = {1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0};
  char *p;
  char *p2;
  char *p3;
  char *u;

  guint64 nits[num_threads];
  GList *nitl[num_threads];
  int tn = 0;
  guint64 min = 0;
  struct db_table *dbt=NULL;
  struct schema_post *sp;
  guint n;
  FILE *nufile = NULL;
  guint have_backup_locks = 0;
  GThread *disk_check_thread = NULL;
  GString *db_quoted_list=NULL;
  if (db){
    guint i=0;
    db_quoted_list=g_string_sized_new(strlen(db));
    g_string_append_printf(db_quoted_list,"'%s'",db_items[i]);
    i++;
    while (i<g_strv_length(db_items)){

      g_string_append_printf(db_quoted_list,",'%s'",db_items[i]);
      i++;

    } 
    
  }
  if (disk_limits!=NULL){
    conf.pause_resume = g_async_queue_new();
    disk_check_thread = g_thread_create(monitor_disk_space_thread, conf.pause_resume, FALSE, NULL);
  }

  if (!daemon_mode){
    GError *serror;
    GThread *sthread =
        g_thread_create(signal_thread, &conf, FALSE, &serror);
    if (sthread == NULL) {
      g_critical("Could not create signal thread: %s", serror->message);
      g_error_free(serror);
      exit(EXIT_FAILURE);
    }
  }

  for (n = 0; n < num_threads; n++) {
    nits[n] = 0;
    nitl[n] = NULL;
  }
  if (ignore_generated_fields)
    g_warning("Queries related to generated fields are not going to be executed. It will lead to restoration issues if you have generated columns");

  p = g_strdup_printf("%s/metadata.partial", dump_directory);
  p2 = g_strndup(p, (unsigned)strlen(p) - 8);

  FILE *mdfile = g_fopen(p, "w");
  if (!mdfile) {
    g_critical("Couldn't write metadata file %s (%d)", p, errno);
    exit(EXIT_FAILURE);
  }

  if (updated_since > 0) {
    u = g_strdup_printf("%s/not_updated_tables", dump_directory);
    nufile = g_fopen(u, "w");
    if (!nufile) {
      g_critical("Couldn't write not_updated_tables file (%d)", errno);
      exit(EXIT_FAILURE);
    }
    get_not_updated(conn, nufile);
  }

  /* We check SHOW PROCESSLIST, and if there're queries
     larger than preset value, we terminate the process.

     This avoids stalling whole server with flush */

  if (!no_locks) {

    while (TRUE) {
      int longquery_count = 0;
      if (mysql_query(conn, "SHOW PROCESSLIST")) {
        g_warning("Could not check PROCESSLIST, no long query guard enabled: %s",
                  mysql_error(conn));
        break;
      } else {
       MYSQL_RES *res = mysql_store_result(conn);
        MYSQL_ROW row;

        /* Just in case PROCESSLIST output column order changes */
        MYSQL_FIELD *fields = mysql_fetch_fields(res);
        guint i;
        int tcol = -1, ccol = -1, icol = -1, ucol = -1;
        for (i = 0; i < mysql_num_fields(res); i++) {
        if (!strcasecmp(fields[i].name, "Command"))
            ccol = i;
          else if (!strcasecmp(fields[i].name, "Time"))
            tcol = i;
          else if (!strcasecmp(fields[i].name, "Id"))
            icol = i;
          else if (!strcasecmp(fields[i].name, "User"))
            ucol = i;
        }
        if ((tcol < 0) || (ccol < 0) || (icol < 0)) {
          g_critical("Error obtaining information from processlist");
          exit(EXIT_FAILURE);
        }
        while ((row = mysql_fetch_row(res))) {
          if (row[ccol] && strcmp(row[ccol], "Query"))
            continue;
          if (row[ucol] && !strcmp(row[ucol], "system user"))
            continue;
          if (row[tcol] && atoi(row[tcol]) > longquery) {
            if (killqueries) {
              if (mysql_query(conn,
                              p3 = g_strdup_printf("KILL %lu", atol(row[icol])))) {
                g_warning("Could not KILL slow query: %s", mysql_error(conn));
                longquery_count++;
              } else {
                g_warning("Killed a query that was running for %ss", row[tcol]);
              }
              g_free(p3);
            } else {
              longquery_count++;
            }
          }
        }
        mysql_free_result(res);
        if (longquery_count == 0)
          break;
        else {
          if (longquery_retries == 0) {
            g_critical("There are queries in PROCESSLIST running longer than "
                       "%us, aborting dump,\n\t"
                       "use --long-query-guard to change the guard value, kill "
                       "queries (--kill-long-queries) or use \n\tdifferent "
                       "server for dump",
                       longquery);
            exit(EXIT_FAILURE);
          }
          longquery_retries--;
          g_warning("There are queries in PROCESSLIST running longer than "
                         "%us, retrying in %u seconds (%u left).",
                         longquery, longquery_retry_interval, longquery_retries);
          sleep(longquery_retry_interval);
        }
      }
    }
  }

  if (!no_locks && (detected_server != SERVER_TYPE_TIDB)) {
    // Percona Server 8 removed LOCK BINLOG so backup locks is useless for
    // mydumper now and we need to fail back to FTWRL
    mysql_query(conn, "SELECT @@version_comment, @@version");
    MYSQL_RES *res2 = mysql_store_result(conn);
    MYSQL_ROW ver;
    while ((ver = mysql_fetch_row(res2))) {
      if (g_str_has_prefix(ver[0], "Percona") &&
          g_str_has_prefix(ver[1], "8.")) {
        g_message("Disabling Percona Backup Locks for Percona Server 8");
        no_backup_locks = 1;
      }
    }
    mysql_free_result(res2);

    // Percona Backup Locks
    if (!no_backup_locks) {
      mysql_query(conn, "SELECT @@have_backup_locks");
      MYSQL_RES *rest = mysql_store_result(conn);
      if (rest != NULL && mysql_num_rows(rest)) {
        mysql_free_result(rest);
        g_message("Using Percona Backup Locks");
        have_backup_locks = 1;
      }
    }

    if (have_backup_locks) {
      if (mysql_query(conn, "LOCK TABLES FOR BACKUP")) {
        g_critical("Couldn't acquire LOCK TABLES FOR BACKUP, snapshots will "
                   "not be consistent: %s",
                   mysql_error(conn));
        errors++;
      }

      if (mysql_query(conn, "LOCK BINLOG FOR BACKUP")) {
        g_critical("Couldn't acquire LOCK BINLOG FOR BACKUP, snapshots will "
                   "not be consistent: %s",
                   mysql_error(conn));
        errors++;
      }
    } else if (lock_all_tables) {
      // LOCK ALL TABLES
      GString *query = g_string_sized_new(16777216);
      gchar *dbtb = NULL;
      gchar **dt = NULL;
      GList *tables_lock = NULL;
      GList *iter = NULL;
      guint success = 0;
      guint retry = 0;
      guint lock = 1;
      int i = 0;

      if (db) {
        g_string_printf(
            query,
            "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA in (%s) AND TABLE_TYPE ='BASE TABLE' AND NOT "
            "(TABLE_SCHEMA = 'mysql' AND (TABLE_NAME = 'slow_log' OR "
            "TABLE_NAME = 'general_log'))",
            db_quoted_list->str);
      } else if (tables) {
        for (i = 0; tables[i] != NULL; i++) {
          dt = g_strsplit(tables[i], ".", 0);
          dbtb = g_strdup_printf("`%s`.`%s`", dt[0], dt[1]);
          tables_lock = g_list_prepend(tables_lock, dbtb);
        }
        tables_lock = g_list_reverse(tables_lock);
      } else {
        g_string_printf(
            query,
            "SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES "
            "WHERE TABLE_TYPE ='BASE TABLE' AND TABLE_SCHEMA NOT IN "
            "('information_schema', 'performance_schema', 'data_dictionary') "
            "AND NOT (TABLE_SCHEMA = 'mysql' AND (TABLE_NAME = 'slow_log' OR "
            "TABLE_NAME = 'general_log'))");
      }

      if (tables_lock == NULL) {
        if (mysql_query(conn, query->str)) {
          g_critical("Couldn't get table list for lock all tables: %s",
                     mysql_error(conn));
          errors++;
        } else {
          MYSQL_RES *res = mysql_store_result(conn);
          MYSQL_ROW row;

          while ((row = mysql_fetch_row(res))) {
            lock = 1;
            if (tables) {
              int table_found = 0;
              for (i = 0; tables[i] != NULL; i++)
                if (g_ascii_strcasecmp(tables[i], row[1]) == 0)
                  table_found = 1;
              if (!table_found)
                lock = 0;
            }
            if (lock && tables_skiplist_file && check_skiplist(row[0], row[1]))
              continue;
            if (lock && !eval_regex(row[0], row[1]))
              continue;

            if (lock) {
              dbtb = g_strdup_printf("`%s`.`%s`", row[0], row[1]);
              tables_lock = g_list_prepend(tables_lock, dbtb);
            }
          }
          tables_lock = g_list_reverse(tables_lock);
        }
      }

      // Try three times to get the lock, this is in case of tmp tables
      // disappearing
      while (!success && retry < 4) {
        n = 0;
        for (iter = tables_lock; iter != NULL; iter = iter->next) {
          if (n == 0) {
            g_string_printf(query, "LOCK TABLE %s READ", (char *)iter->data);
            n = 1;
          } else {
            g_string_append_printf(query, ", %s READ", (char *)iter->data);
          }
        }
        if (mysql_query(conn, query->str)) {
          gchar *failed_table = NULL;
          gchar **tmp_fail;

          tmp_fail = g_strsplit(mysql_error(conn), "'", 0);
          tmp_fail = g_strsplit(tmp_fail[1], ".", 0);
          failed_table = g_strdup_printf("`%s`.`%s`", tmp_fail[0], tmp_fail[1]);
          for (iter = tables_lock; iter != NULL; iter = iter->next) {
            if (strcmp(iter->data, failed_table) == 0) {
              tables_lock = g_list_remove(tables_lock, iter->data);
            }
          }
          g_free(tmp_fail);
          g_free(failed_table);
        } else {
          success = 1;
        }
        retry += 1;
      }
      if (!success) {
        g_critical("Lock all tables fail: %s", mysql_error(conn));
        exit(EXIT_FAILURE);
      }
      g_free(query->str);
      g_list_free(tables_lock);
    } else {
      if (mysql_query(conn, "FLUSH TABLES WITH READ LOCK")) {
        g_critical("Couldn't acquire global lock, snapshots will not be "
                   "consistent: %s",
                   mysql_error(conn));
        errors++;
      }
    }
  } else if (detected_server == SERVER_TYPE_TIDB) {
    g_message("Skipping locks because of TiDB");
    if (!tidb_snapshot) {

      // Generate a @@tidb_snapshot to use for the worker threads since
      // the tidb-snapshot argument was not specified when starting mydumper

      if (mysql_query(conn, "SHOW MASTER STATUS")) {
        g_critical("Couldn't generate @@tidb_snapshot: %s", mysql_error(conn));
        exit(EXIT_FAILURE);
      } else {

        MYSQL_RES *result = mysql_store_result(conn);
        MYSQL_ROW row = mysql_fetch_row(
            result); /* There should never be more than one row */
        tidb_snapshot = g_strdup(row[1]);
        mysql_free_result(result);
      }
    }

    // Need to set the @@tidb_snapshot for the master thread
    gchar *query =
        g_strdup_printf("SET SESSION tidb_snapshot = '%s'", tidb_snapshot);

    g_message("Set to tidb_snapshot '%s'", tidb_snapshot);

    if (mysql_query(conn, query)) {
      g_critical("Failed to set tidb_snapshot: %s", mysql_error(conn));
      exit(EXIT_FAILURE);
    }
    g_free(query);

  } else {
    g_warning("Executing in no-locks mode, snapshot will not be consistent");
  }
  if (mysql_get_server_version(conn) < 40108) {
    mysql_query(
        conn,
        "CREATE TABLE IF NOT EXISTS mysql.mydumperdummy (a INT) ENGINE=INNODB");
    need_dummy_read = 1;
  }

  // tokudb do not support consistent snapshot
  mysql_query(conn, "SELECT @@tokudb_version");
  MYSQL_RES *rest = mysql_store_result(conn);
  if (rest != NULL && mysql_num_rows(rest)) {
    mysql_free_result(rest);
    g_message("TokuDB detected, creating dummy table for CS");
    mysql_query(
        conn,
        "CREATE TABLE IF NOT EXISTS mysql.tokudbdummy (a INT) ENGINE=TokuDB");
    need_dummy_toku_read = 1;
  }

  // Do not start a transaction when lock all tables instead of FTWRL,
  // since it can implicitly release read locks we hold
  if (!lock_all_tables) {
    mysql_query(conn, "START TRANSACTION /*!40108 WITH CONSISTENT SNAPSHOT */");
  }

  if (need_dummy_read) {
    mysql_query(conn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.mydumperdummy");
    MYSQL_RES *res = mysql_store_result(conn);
    if (res)
      mysql_free_result(res);
  }
  if (need_dummy_toku_read) {
    mysql_query(conn,
                "SELECT /*!40001 SQL_NO_CACHE */ * FROM mysql.tokudbdummy");
    MYSQL_RES *res = mysql_store_result(conn);
    if (res)
      mysql_free_result(res);
  }
  GDateTime *datetime = g_date_time_new_now_local();
  char *datetimestr=g_date_time_format(datetime,"\%Y-\%m-\%d \%H:\%M:\%S");
  fprintf(mdfile, "Started dump at: %s\n", datetimestr);

  g_message("Started dump at: %s", datetimestr);
  g_free(datetimestr);

  if (detected_server == SERVER_TYPE_MYSQL) {
    if (set_names_str)
  		mysql_query(conn, set_names_str);

    write_snapshot_info(conn, mdfile);
  }
  GThread *stream_thread = NULL;
  if (stream){
    stream_queue = g_async_queue_new();
    stream_thread = g_thread_create((GThreadFunc)process_stream, stream_queue, TRUE, NULL);
  }
  GThread **threads = g_new(GThread *, num_threads * (less_locking + 1));
  struct thread_data *td =
      g_new(struct thread_data, num_threads * (less_locking + 1));

  if (less_locking) {
    conf.queue_less_locking = g_async_queue_new();
    conf.ready_less_locking = g_async_queue_new();
    less_locking_threads = num_threads;
    for (n = num_threads; n < num_threads * 2; n++) {
      td[n].conf = &conf;
      td[n].thread_id = n + 1;
      td[n].queue = conf.queue_less_locking;
      td[n].ready = conf.ready_less_locking;
      td[n].less_locking_stage = TRUE;
      threads[n] = g_thread_create((GThreadFunc)working_thread,
                                   &td[n], TRUE, NULL);
      g_async_queue_pop(conf.ready_less_locking);
    }
    g_async_queue_unref(conf.ready_less_locking);
  }

  conf.queue = g_async_queue_new();
  conf.ready = g_async_queue_new();
  conf.unlock_tables = g_async_queue_new();
  conf.ready_database_dump = g_async_queue_new();

  for (n = 0; n < num_threads; n++) {
    td[n].conf = &conf;
    td[n].thread_id = n + 1;
    td[n].queue = conf.queue;
    td[n].ready = conf.ready;
    td[n].less_locking_stage = FALSE;
    threads[n] =
        g_thread_create((GThreadFunc)working_thread, &td[n], TRUE, NULL);
    g_async_queue_pop(conf.ready);
  }

  g_async_queue_unref(conf.ready);

  if (trx_consistency_only) {
    g_message("Transactions started, unlocking tables");
    mysql_query(conn, "UNLOCK TABLES /* trx-only */");
    if (have_backup_locks)
      mysql_query(conn, "UNLOCK BINLOG");
  }

  if (db) {
    guint i=0;
    for (i=0;i<g_strv_length(db_items);i++){
      create_job_to_dump_database(new_database(conn,db_items[i],TRUE), &conf, less_locking);
      if (!no_schemas)
        create_job_to_dump_schema(db_items[i], &conf);
    }
  } else if (tables) {
    get_tables(conn, &conf);
  } else {
    MYSQL_RES *databases;
    MYSQL_ROW row;
    if (mysql_query(conn, "SHOW DATABASES") ||
        !(databases = mysql_store_result(conn))) {
      g_critical("Unable to list databases: %s", mysql_error(conn));
      exit(EXIT_FAILURE);
    }

    while ((row = mysql_fetch_row(databases))) {
      if (!strcasecmp(row[0], "information_schema") ||
          !strcasecmp(row[0], "performance_schema") ||
          (!strcasecmp(row[0], "data_dictionary")))
        continue;
      struct database * db_tmp=NULL;
      if (get_database(conn,row[0],&db_tmp) && !no_schemas && (!eval_regex(row[0], NULL))){
        g_mutex_lock(db_tmp->ad_mutex);
        if (!db_tmp->already_dumped){
          create_job_to_dump_schema(db_tmp->name, &conf);
          db_tmp->already_dumped=TRUE;
        }
        g_mutex_unlock(db_tmp->ad_mutex);
      }
      create_job_to_dump_database(db_tmp, &conf, less_locking);
      /* Checks PCRE expressions on 'database' string */
//      if (!no_schemas && (regexstring == NULL || check_regex(row[0], NULL))){
//        dump_create_database(row[0], &conf);
//      }
    }
    mysql_free_result(databases);
  }
  g_async_queue_pop(conf.ready_database_dump);
  g_async_queue_unref(conf.ready_database_dump);
  g_list_free(no_updated_tables);

  if (!non_innodb_table) {
    g_async_queue_push(conf.unlock_tables, GINT_TO_POINTER(1));
  }

  GList *iter;
  table_schemas = g_list_reverse(table_schemas);
  for (iter = table_schemas; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    create_job_to_dump_table(conn, dbt, &conf);
  }

  non_innodb_table = g_list_reverse(non_innodb_table);
  if (less_locking) {

    for (iter = non_innodb_table; iter != NULL; iter = iter->next) {
      dbt = (struct db_table *)iter->data;
      tn = 0;
      min = nits[0];
      for (n = 1; n < num_threads; n++) {
        if (nits[n] < min) {
          min = nits[n];
          tn = n;
        }
      }
      nitl[tn] = g_list_prepend(nitl[tn], dbt);
      nits[tn] += dbt->datalength;
    }
    nitl[tn] = g_list_reverse(nitl[tn]);

    for (n = 0; n < num_threads; n++) {
      if (nits[n] > 0) {
        g_atomic_int_inc(&non_innodb_table_counter);
        create_jobs_for_non_innodb_table_list_in_less_locking_mode(conn, nitl[n], &conf);
        g_list_free(nitl[n]);
      }
    }
    g_list_free(non_innodb_table);

    if (g_atomic_int_get(&non_innodb_table_counter))
      g_atomic_int_inc(&non_innodb_done);
    else
      g_async_queue_push(conf.unlock_tables, GINT_TO_POINTER(1));

    for (n = 0; n < num_threads; n++) {
      struct job *j = g_new0(struct job, 1);
      j->type = JOB_SHUTDOWN;
      g_async_queue_push(conf.queue_less_locking, j);
    }
  } else {
    for (iter = non_innodb_table; iter != NULL; iter = iter->next) {
      dbt = (struct db_table *)iter->data;
      if (dump_checksums) {
        create_job_to_dump_checksum(dbt, &conf);
      }
      dump_table(conn, dbt, &conf, FALSE);
      g_atomic_int_inc(&non_innodb_table_counter);
    }
    g_list_free(non_innodb_table);
    g_atomic_int_inc(&non_innodb_done);
  }

  innodb_tables = g_list_reverse(innodb_tables);
  for (iter = innodb_tables; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    if (dump_checksums) {
      create_job_to_dump_checksum(dbt, &conf);
    }
    dump_table(conn, dbt, &conf, TRUE);
  }
  g_list_free(innodb_tables);
  innodb_tables=NULL;

/*  table_schemas = g_list_reverse(table_schemas);
  for (iter = table_schemas; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    dump_schema(conn, dbt, &conf);
  }*/

  view_schemas = g_list_reverse(view_schemas);
  for (iter = view_schemas; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    create_job_to_dump_view(dbt, &conf);
    g_free(dbt->table);
    g_free(dbt);
  }
  g_list_free(view_schemas);
  view_schemas=NULL;

  schema_post = g_list_reverse(schema_post);
  for (iter = schema_post; iter != NULL; iter = iter->next) {
    sp = (struct schema_post *)iter->data;
    create_job_to_dump_post(sp->database, &conf);
    g_free(sp);
  }
  g_list_free(schema_post);
  schema_post=NULL;

  if (!no_locks && !trx_consistency_only) {
    g_async_queue_pop(conf.unlock_tables);
    g_message("Non-InnoDB dump complete, unlocking tables");
    mysql_query(conn, "UNLOCK TABLES /* FTWRL */");
    if (have_backup_locks)
      mysql_query(conn, "UNLOCK BINLOG");
  }
  // close main connection
  mysql_close(conn);

  if (less_locking) {
    for (n = num_threads; n < num_threads * 2; n++) {
      g_thread_join(threads[n]);
    }
    g_async_queue_unref(conf.queue_less_locking);
  }

  for (n = 0; n < num_threads; n++) {
    struct job *j = g_new0(struct job, 1);
    j->type = JOB_SHUTDOWN;
    g_async_queue_push(conf.queue, j);
  }

  for (n = 0; n < num_threads; n++) {
    g_thread_join(threads[n]);
  }

  // TODO: We need to create jobs for metadata.
  table_schemas = g_list_reverse(table_schemas);
  for (iter = table_schemas; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;
    write_table_metadata_into_file(dbt);
  }
  g_list_free(table_schemas);
  table_schemas=NULL;

  g_async_queue_unref(conf.queue);
  g_async_queue_unref(conf.unlock_tables);

  datetime = g_date_time_new_now_local();
  datetimestr=g_date_time_format(datetime,"\%Y-\%m-\%d \%H:\%M:\%S");
  fprintf(mdfile, "Finished dump at: %s\n", datetimestr);
  fclose(mdfile);
  if (updated_since > 0)
    fclose(nufile);
  g_rename(p, p2);
  if (stream) {
    g_async_queue_push(stream_queue, g_strdup(p2));
  }
  g_free(p);
  g_free(p2);
  g_message("Finished dump at: %s",datetimestr);
  g_free(datetimestr);

  if (stream) {
    g_async_queue_push(stream_queue, g_strdup(""));
    g_thread_join(stream_thread);
    if (no_delete == FALSE && output_directory_param == NULL)
      if (g_rmdir(output_directory) != 0)
        g_critical("Backup directory not removed: %s", output_directory);
  }
  g_free(td);
  g_free(threads);
  if (disk_check_thread!=NULL){
    disk_limits=NULL;
  }

}

void get_not_updated(MYSQL *conn, FILE *file) {
  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  gchar *query =
      g_strdup_printf("SELECT CONCAT(TABLE_SCHEMA,'.',TABLE_NAME) FROM "
                      "information_schema.TABLES WHERE TABLE_TYPE = 'BASE "
                      "TABLE' AND UPDATE_TIME < NOW() - INTERVAL %d DAY",
                      updated_since);
  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  while ((row = mysql_fetch_row(res))) {
    no_updated_tables = g_list_prepend(no_updated_tables, row[0]);
    fprintf(file, "%s\n", row[0]);
  }
  no_updated_tables = g_list_reverse(no_updated_tables);
  fflush(file);
}

gboolean detect_generated_fields(MYSQL *conn, struct db_table *dbt) {
  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  gboolean result = FALSE;
  if (ignore_generated_fields)
    return FALSE;

  gchar *query = g_strdup_printf(
      "select COLUMN_NAME from information_schema.COLUMNS where "
      "TABLE_SCHEMA='%s' and TABLE_NAME='%s' and extra like '%%GENERATED%%' and extra not like '%%DEFAULT_GENERATED%%'",
      dbt->database->escaped, dbt->escaped_table);

  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  if (res == NULL){
  	return FALSE;
  }

  if ((row = mysql_fetch_row(res))) {
    result = TRUE;
  }
  mysql_free_result(res);

  return result;
}

GString *get_insertable_fields(MYSQL *conn, char *database, char *table) {
  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  GString *field_list = g_string_new("");

  gchar *query =
      g_strdup_printf("select COLUMN_NAME from information_schema.COLUMNS "
                      "where TABLE_SCHEMA='%s' and TABLE_NAME='%s' and extra "
                      "not like '%%VIRTUAL GENERATED%%' and extra not like '%%STORED GENERATED%%'",
                      database, table);
  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  gboolean first = TRUE;
  while ((row = mysql_fetch_row(res))) {
    if (first) {
      first = FALSE;
    } else {
      g_string_append(field_list, ",");
    }

    gchar *tb = g_strdup_printf("`%s`", row[0]);
    g_string_append(field_list, tb);
    g_free(tb);
  }
  mysql_free_result(res);

  return field_list;
}

gchar *get_primary_key_string(MYSQL *conn, char *database, char *table) {
  if (!order_by_primary_key) return NULL;

  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  GString *field_list = g_string_new("");

  gchar *query =
          g_strdup_printf("SELECT k.COLUMN_NAME, ORDINAL_POSITION "
                          "FROM information_schema.table_constraints t "
                          "LEFT JOIN information_schema.key_column_usage k "
                          "USING(constraint_name,table_schema,table_name) "
                          "WHERE t.constraint_type IN ('PRIMARY KEY', 'UNIQUE') "
                          "AND t.table_schema='%s' "
                          "AND t.table_name='%s' "
                          "ORDER BY t.constraint_type, ORDINAL_POSITION; ",
                          database, table);
  mysql_query(conn, query);
  g_free(query);

  res = mysql_store_result(conn);
  gboolean first = TRUE;
  while ((row = mysql_fetch_row(res))) {
    if (first) {
      first = FALSE;
    } else if (atoi(row[1]) > 1) {
      g_string_append(field_list, ",");
    } else {
      break;
    }

    gchar *tb = g_strdup_printf("`%s`", row[0]);
    g_string_append(field_list, tb);
    g_free(tb);
  }
  mysql_free_result(res);
  // Return NULL if we never found a PRIMARY or UNIQUE key
  if (first) {
    g_string_free(field_list, TRUE);
    return NULL;
  } else {
    return g_string_free(field_list, FALSE);
  }
}

/* Heuristic chunks building - based on estimates, produces list of ranges for
   datadumping WORK IN PROGRESS
*/
GList *get_chunks_for_table(MYSQL *conn, char *database, char *table,
                            struct configuration *conf) {

  GList *chunks = NULL;
  MYSQL_RES *indexes = NULL, *minmax = NULL, *total = NULL;
  MYSQL_ROW row;
  char *field = NULL;
  int showed_nulls = 0;

  /* first have to pick index, in future should be able to preset in
   * configuration too */
  gchar *query = g_strdup_printf("SHOW INDEX FROM `%s`.`%s`", database, table);
  mysql_query(conn, query);
  g_free(query);
  indexes = mysql_store_result(conn);

  if (indexes){
    while ((row = mysql_fetch_row(indexes))) {
      if (!strcmp(row[2], "PRIMARY") && (!strcmp(row[3], "1"))) {
        /* Pick first column in PK, cardinality doesn't matter */
        field = row[4];
        break;
      }
    }

    /* If no PK found, try using first UNIQUE index */
    if (!field) {
      mysql_data_seek(indexes, 0);
      while ((row = mysql_fetch_row(indexes))) {
        if (!strcmp(row[1], "0") && (!strcmp(row[3], "1"))) {
          /* Again, first column of any unique index */
          field = row[4];
          break;
        }
      }
    }
    /* Still unlucky? Pick any high-cardinality index */
    if (!field && conf->use_any_index) {
      guint64 max_cardinality = 0;
      guint64 cardinality = 0;

      mysql_data_seek(indexes, 0);
      while ((row = mysql_fetch_row(indexes))) {
        if (!strcmp(row[3], "1")) {
          if (row[6])
            cardinality = strtoul(row[6], NULL, 10);
          if (cardinality > max_cardinality) {
            field = row[4];
            max_cardinality = cardinality;
          }
        }
      }
    }
  }
  /* Oh well, no chunks today - no suitable index */
  if (!field)
    goto cleanup;

  /* Get minimum/maximum */
  mysql_query(conn, query = g_strdup_printf(
                        "SELECT %s MIN(`%s`),MAX(`%s`) FROM `%s`.`%s`",
                        (detected_server == SERVER_TYPE_MYSQL)
                            ? "/*!40001 SQL_NO_CACHE */"
                            : "",
                        field, field, database, table));
  g_free(query);
  minmax = mysql_store_result(conn);

  if (!minmax)
    goto cleanup;

  row = mysql_fetch_row(minmax);
  MYSQL_FIELD *fields = mysql_fetch_fields(minmax);

  /* Check if all values are NULL */
  if (row[0] == NULL)
    goto cleanup;

  char *min = row[0];
  char *max = row[1];

  guint64 estimated_chunks, estimated_step, nmin, nmax, cutoff, rows;

  /* Support just bigger INTs for now, very dumb, no verify approach */
  switch (fields[0].type) {
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_SHORT:
    /* Got total number of rows, skip chunk logic if estimates are low */
    rows = estimate_count(conn, database, table, field, min, max);
    if (rows <= rows_per_file)
      goto cleanup;

    /* This is estimate, not to use as guarantee! Every chunk would have eventual
     * adjustments */
    estimated_chunks = rows / rows_per_file;
    /* static stepping */
    nmin = strtoul(min, NULL, 10);
    nmax = strtoul(max, NULL, 10);
    estimated_step = (nmax - nmin) / estimated_chunks + 1;
    if (estimated_step > max_rows)
      estimated_step = max_rows;
    cutoff = nmin;
    while (cutoff <= nmax) {
      chunks = g_list_prepend(
          chunks,
          g_strdup_printf("%s%s%s%s(`%s` >= %llu AND `%s` < %llu)",
                          !showed_nulls ? "`" : "",
                          !showed_nulls ? field : "",
                          !showed_nulls ? "`" : "",
                          !showed_nulls ? " IS NULL OR " : "", field,
                          (unsigned long long)cutoff, field,
                          (unsigned long long)(cutoff + estimated_step)));
      cutoff += estimated_step;
      showed_nulls = 1;
    }
    chunks = g_list_reverse(chunks);
// TODO: We need to add more chunk options for different types
  default:
    goto cleanup;
  }

cleanup:
  if (indexes)
    mysql_free_result(indexes);
  if (minmax)
    mysql_free_result(minmax);
  if (total)
    mysql_free_result(total);
  return chunks;
}

/* Try to get EXPLAIN'ed estimates of row in resultset */
guint64 estimate_count(MYSQL *conn, char *database, char *table, char *field,
                       char *from, char *to) {
  char *querybase, *query;
  int ret;

  g_assert(conn && database && table);

  querybase = g_strdup_printf("EXPLAIN SELECT `%s` FROM `%s`.`%s`",
                              (field ? field : "*"), database, table);
  if (from || to) {
    g_assert(field != NULL);
    char *fromclause = NULL, *toclause = NULL;
    char *escaped;
    if (from) {
      escaped = g_new(char, strlen(from) * 2 + 1);
      mysql_real_escape_string(conn, escaped, from, strlen(from));
      fromclause = g_strdup_printf(" `%s` >= %s ", field, escaped);
      g_free(escaped);
    }
    if (to) {
      escaped = g_new(char, strlen(to) * 2 + 1);
      mysql_real_escape_string(conn, escaped, to, strlen(to));
      toclause = g_strdup_printf(" `%s` <= %s", field, escaped);
      g_free(escaped);
    }
    query = g_strdup_printf("%s WHERE %s %s %s", querybase,
                            (from ? fromclause : ""),
                            ((from && to) ? "AND" : ""), (to ? toclause : ""));

    if (toclause)
      g_free(toclause);
    if (fromclause)
      g_free(fromclause);
    ret = mysql_query(conn, query);
    g_free(querybase);
    g_free(query);
  } else {
    ret = mysql_query(conn, querybase);
    g_free(querybase);
  }

  if (ret) {
    g_warning("Unable to get estimates for %s.%s: %s", database, table,
              mysql_error(conn));
  }

  MYSQL_RES *result = mysql_store_result(conn);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);

  guint i;
  for (i = 0; i < mysql_num_fields(result); i++) {
    if (!strcmp(fields[i].name, "rows"))
      break;
  }

  MYSQL_ROW row = NULL;

  guint64 count = 0;

  if (result)
    row = mysql_fetch_row(result);

  if (row && row[i])
    count = strtoul(row[i], NULL, 10);

  if (result)
    mysql_free_result(result);

  return (count);
}
/*
char * escape_string(MYSQL *conn, char *str){
  char * r=g_new(char, strlen(str) * 2 + 1);
  mysql_real_escape_string(conn, r, str, strlen(str));
  return r;
}

struct database * new_database(MYSQL *conn, char *database_name, gboolean already_dumped){
  struct database * d=g_new(struct database,1);
  d->name = g_strdup(database_name);
  d->filename = get_ref_table(d->name);
  d->escaped = escape_string(conn,d->name);
  d->already_dumped = already_dumped;
  d->ad_mutex=g_mutex_new();
  g_hash_table_insert(database_hash, d->name,d);
  return d;
}

gboolean get_database(MYSQL *conn, char *database_name, struct database ** database){
  *database=g_hash_table_lookup(database_hash,database_name);
  if (*database == NULL){
    *database=new_database(conn,database_name,FALSE);
    return TRUE;
  }
  return FALSE;
}
*/
typedef gchar * (*fun_ptr2)(gchar **);

GList *get_anonymized_function_for(MYSQL *conn, gchar *database, gchar *table){
  // TODO #364: this is the place where we need to link the column between file loaded and dbt.
  // Currently, we are using identity_function, which return the same data.
  // Key: `database`.`table`.`column`

  MYSQL_RES *res = NULL;
  MYSQL_ROW row;

  gchar *query =
      g_strdup_printf("select COLUMN_NAME from information_schema.COLUMNS "
                      "where TABLE_SCHEMA='%s' and TABLE_NAME='%s' ORDER BY ORDINAL_POSITION;",
                      database, table);
  mysql_query(conn, query);
  g_free(query);

  GList *anonymized_function_list=NULL;
  res = mysql_store_result(conn);
  gchar * k = g_strdup_printf("`%s`.`%s`",database,table);
  GHashTable *ht = g_hash_table_lookup(all_anonymized_function,k);
  fun_ptr2 f;
  if (ht){
    while ((row = mysql_fetch_row(res))) {
      f=(fun_ptr2)g_hash_table_lookup(ht,row[0]);
      if (f  != NULL){
        anonymized_function_list=g_list_append(anonymized_function_list,f);
      }else{
        anonymized_function_list=g_list_append(anonymized_function_list,&identity_function);
      }
    }
  }else{
    g_message("No anonymized func for that");
  }
  mysql_free_result(res);
  g_free(k);
  return anonymized_function_list;
}

struct db_table *new_db_table( MYSQL *conn, struct database *database, char *table, char *datalength){
  struct db_table *dbt = g_new(struct db_table, 1);
  dbt->database = database;
  dbt->table = g_strdup(table);
  dbt->table_filename = get_ref_table(dbt->table);
  dbt->rows_lock= g_mutex_new();
  dbt->escaped_table = escape_string(conn,dbt->table);
  dbt->anonymized_function=get_anonymized_function_for(conn, database->name, table);
  dbt->rows=0;
  if (!datalength)
    dbt->datalength = 0;
  else
    dbt->datalength = g_ascii_strtoull(datalength, NULL, 10);
  return dbt; 
}

void green_light(MYSQL *conn, struct configuration *conf, gboolean is_view, struct database * database, MYSQL_ROW *row, gchar *ecol){
    /* Green light! */
 g_mutex_lock(database->ad_mutex);
 if (!database->already_dumped){
   create_job_to_dump_schema(database->name, conf);
   database->already_dumped=TRUE;
 }
 g_mutex_unlock(database->ad_mutex);

    struct db_table *dbt = new_db_table( conn, database, (*row)[0], (*row)[6]);

    // if is a view we care only about schema
    if (!is_view) {
      // with trx_consistency_only we dump all as innodb_tables
      if (!no_data) {
        if (ecol != NULL && g_ascii_strcasecmp("MRG_MYISAM",ecol)) {
          if (trx_consistency_only ||
              (ecol != NULL && !g_ascii_strcasecmp("InnoDB", ecol))) {
            g_mutex_lock(innodb_tables_mutex);
            innodb_tables = g_list_prepend(innodb_tables, dbt);
            g_mutex_unlock(innodb_tables_mutex);
          } else if (ecol != NULL &&
                     !g_ascii_strcasecmp("TokuDB", ecol)) {
            g_mutex_lock(innodb_tables_mutex);
            innodb_tables = g_list_prepend(innodb_tables, dbt);
            g_mutex_unlock(innodb_tables_mutex);
          } else {
            g_mutex_lock(non_innodb_table_mutex);
            non_innodb_table = g_list_prepend(non_innodb_table, dbt);
            g_mutex_unlock(non_innodb_table_mutex);
          }
        }
      }
      if (!no_schemas) {
        g_mutex_lock(table_schemas_mutex);
        table_schemas = g_list_prepend(table_schemas, dbt);
        g_mutex_unlock(table_schemas_mutex);
      }
    } else {
      if (!no_schemas) {
        g_mutex_lock(view_schemas_mutex);
        view_schemas = g_list_prepend(view_schemas, dbt);
        g_mutex_unlock(view_schemas_mutex);
      }
    }

}

gboolean determine_if_schema_is_elected_to_dump_post(MYSQL *conn, struct database *database){
  char *query;
  MYSQL_RES *result = mysql_store_result(conn);
  MYSQL_ROW row;
  // Store Procedures and Events
  // As these are not attached to tables we need to define when we need to dump
  // or not Having regex filter make this hard because we dont now if a full
  // schema is filtered or not Also I cant decide this based on tables from a
  // schema being dumped So I will use only regex to dump or not SP and EVENTS I
  // only need one match to dump all

  gboolean post_dump = FALSE;

  if (dump_routines) {
    // SP
    query = g_strdup_printf("SHOW PROCEDURE STATUS WHERE CAST(Db AS BINARY) = '%s'", database->escaped);
    if (mysql_query(conn, (query))) {
      g_critical("Error: DB: %s - Could not execute query: %s", database->name,
                 mysql_error(conn));
      errors++;
      g_free(query);
      return FALSE;
    }
    result = mysql_store_result(conn);
    while ((row = mysql_fetch_row(result)) && !post_dump) {
      /* Checks skip list on 'database.sp' string */
      if (tables_skiplist_file && check_skiplist(database->name, row[1]))
        continue;

      /* Checks PCRE expressions on 'database.sp' string */
      if (!eval_regex(database->name, row[1]))
        continue;

      post_dump = TRUE;
    }

    if (!post_dump) {
      // FUNCTIONS
      query = g_strdup_printf("SHOW FUNCTION STATUS WHERE CAST(Db AS BINARY) = '%s'", database->escaped);
      if (mysql_query(conn, (query))) {
        g_critical("Error: DB: %s - Could not execute query: %s", database->name,
                   mysql_error(conn));
        errors++;
        g_free(query);
        return FALSE;
      }
      result = mysql_store_result(conn);
      while ((row = mysql_fetch_row(result)) && !post_dump) {
        /* Checks skip list on 'database.sp' string */
        if (tables_skiplist_file && check_skiplist(database->name, row[1]))
          continue;
        /* Checks PCRE expressions on 'database.sp' string */
        if ( !eval_regex(database->name, row[1]))
          continue;

        post_dump = TRUE;
      }
    }
    mysql_free_result(result);
  }

  if (dump_events && !post_dump) {
    // EVENTS
    query = g_strdup_printf("SHOW EVENTS FROM `%s`", database->name);
    if (mysql_query(conn, (query))) {
      g_critical("Error: DB: %s - Could not execute query: %s", database->name,
                 mysql_error(conn));
      errors++;
      g_free(query);
      return FALSE;
    }
    result = mysql_store_result(conn);
    while ((row = mysql_fetch_row(result)) && !post_dump) {
      /* Checks skip list on 'database.sp' string */
      if (tables_skiplist_file && check_skiplist(database->name, row[1]))
        continue;
      /* Checks PCRE expressions on 'database.sp' string */
      if ( !eval_regex(database->name, row[1]))
        continue;

      post_dump = TRUE;
    }
    mysql_free_result(result);
  }
  return post_dump;
}

void dump_database_thread(MYSQL *conn, struct configuration *conf, struct database *database) {

  char *query;
  mysql_select_db(conn, database->name);
  if (detected_server == SERVER_TYPE_MYSQL ||
      detected_server == SERVER_TYPE_TIDB)
    query = g_strdup("SHOW TABLE STATUS");
  else
    query =
        g_strdup_printf("SELECT TABLE_NAME, ENGINE, TABLE_TYPE as COMMENT FROM "
                        "DATA_DICTIONARY.TABLES WHERE TABLE_SCHEMA='%s'",
                        database->escaped);

  if (mysql_query(conn, (query))) {
      g_critical("Error: DB: %s - Could not execute query: %s", database->name,
               mysql_error(conn));
    errors++;
    g_free(query);
    return;
  }

  MYSQL_RES *result = mysql_store_result(conn);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);
  guint i;
  int ecol = -1, ccol = -1;
  for (i = 0; i < mysql_num_fields(result); i++) {
    if (!strcasecmp(fields[i].name, "Engine"))
      ecol = i;
    else if (!strcasecmp(fields[i].name, "Comment"))
      ccol = i;
  }

  if (!result) {
    g_critical("Could not list tables for %s: %s", database->name, mysql_error(conn));
    errors++;
    return;
  }

  MYSQL_ROW row;
  while ((row = mysql_fetch_row(result))) {

    int dump = 1;
    int is_view = 0;

    /* We now do care about views!
            num_fields>1 kicks in only in case of 5.0 SHOW FULL TABLES or SHOW
       TABLE STATUS row[1] == NULL if it is a view in 5.0 'SHOW TABLE STATUS'
            row[1] == "VIEW" if it is a view in 5.0 'SHOW FULL TABLES'
    */
    if ((detected_server == SERVER_TYPE_MYSQL) &&
        (row[ccol] == NULL || !strcmp(row[ccol], "VIEW")))
      is_view = 1;

    /* Check for broken tables, i.e. mrg with missing source tbl */
    if (!is_view && row[ecol] == NULL) {
      g_warning("Broken table detected, please review: %s.%s", database->name,
                row[0]);
      dump = 0;
    }

    /* Skip ignored engines, handy for avoiding Merge, Federated or Blackhole
     * :-) dumps */
    if (dump && ignore && !is_view) {
      for (i = 0; ignore[i] != NULL; i++) {
        if (g_ascii_strcasecmp(ignore[i], row[ecol]) == 0) {
          dump = 0;
          break;
        }
      }
    }

    /* Skip views */
    if (is_view && no_dump_views)
      dump = 0;

    if (!dump)
      continue;

    /* In case of table-list option is enabled, check if table is part of the
     * list */
    if (tables) {
/*      int table_found = 0;
      for (i = 0; tables[i] != NULL; i++)
        if (g_ascii_strcasecmp(tables[i], row[0]) == 0)
          table_found = 1;
*/
      if (!is_table_in_list(row[0], tables))
        dump = 0;
    }
    if (!dump)
      continue;

    /* Special tables */
    if (g_ascii_strcasecmp(database->name, "mysql") == 0 &&
        (g_ascii_strcasecmp(row[0], "general_log") == 0 ||
         g_ascii_strcasecmp(row[0], "slow_log") == 0 ||
         g_ascii_strcasecmp(row[0], "innodb_index_stats") == 0 ||
         g_ascii_strcasecmp(row[0], "innodb_table_stats") == 0)) {
      dump = 0;
      continue;
    }

    /* Checks skip list on 'database.table' string */
    if (tables_skiplist_file && check_skiplist(database->name, row[0]))
      continue;

    /* Checks PCRE expressions on 'database.table' string */
    if (!eval_regex(database->name, row[0]))
      continue;

    /* Check if the table was recently updated */
    if (no_updated_tables && !is_view) {
      GList *iter;
      for (iter = no_updated_tables; iter != NULL; iter = iter->next) {
        if (g_ascii_strcasecmp(
                iter->data, g_strdup_printf("%s.%s", database->name, row[0])) == 0) {
          g_message("NO UPDATED TABLE: %s.%s", database->name, row[0]);
          dump = 0;
        }
      }
    }

    if (!dump)
      continue;

    green_light(conn,conf, is_view,database,&row,row[ecol]);

  }

  mysql_free_result(result);

  if (determine_if_schema_is_elected_to_dump_post(conn,database)) {
    struct schema_post *sp = g_new(struct schema_post, 1);
    sp->database = database;
    schema_post = g_list_prepend(schema_post, sp);
  }

  g_free(query);

  return;
}

void get_tables(MYSQL *conn, struct configuration *conf) {

  gchar **dt = NULL;
  char *query = NULL;
  guint i, x;

  for (x = 0; tables[x] != NULL; x++) {
    dt = g_strsplit(tables[x], ".", 0);

    query =
        g_strdup_printf("SHOW TABLE STATUS FROM %s LIKE '%s'", dt[0], dt[1]);

    if (mysql_query(conn, (query))) {
      g_critical("Error: DB: %s - Could not execute query: %s", dt[0],
                 mysql_error(conn));
      errors++;
      return;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    guint ecol = -1;
    guint ccol = -1;
    for (i = 0; i < mysql_num_fields(result); i++) {
      if (!strcasecmp(fields[i].name, "Engine"))
        ecol = i;
      else if (!strcasecmp(fields[i].name, "Comment"))
        ccol = i;
    }

    if (!result) {
      g_warning("Could not list table for %s.%s: %s", dt[0], dt[1],
                mysql_error(conn));
      errors++;
      return;
    }
    struct database * database=NULL;
    if (get_database(conn, dt[0],&database)){
      create_job_to_dump_schema(database->name, conf);
      g_async_queue_push(conf->ready_database_dump, GINT_TO_POINTER(1));
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {

      int is_view = 0;

      if ((detected_server == SERVER_TYPE_MYSQL) &&
          (row[ccol] == NULL || !strcmp(row[ccol], "VIEW")))
        is_view = 1;
      green_light(conn, conf, is_view, database, &row,row[ecol]);
    }
  }

  g_free(query);
}

void dump_table_data_file(MYSQL *conn, struct table_job *tj) {
  void *outfile = NULL;

  outfile = m_open(tj->filename,"w");

  if (!outfile) {
    g_critical("Error: DB: %s TABLE: %s Could not create output file %s (%d)",
               tj->database, tj->table, tj->filename, errno);
    errors++;
    return;
  }
  guint64 rows_count =
      dump_table_data(conn, (FILE *)outfile, tj);

  if (!rows_count)
    g_message("Empty table %s.%s", tj->database, tj->table);
  
}

struct table_job * new_table_job(struct db_table *dbt, char *partition, char *where, guint nchunk, gboolean has_generated_fields, char *order_by){
  struct table_job *tj = g_new0(struct table_job, 1);
// begin Refactoring: We should review this, as dbt->database should not be free, so it might be no need to g_strdup.
  // from the ref table?? TODO
  tj->database=dbt->database->name;
  tj->table=g_strdup(dbt->table);
// end 
  tj->partition=partition;
  tj->where=where;
  tj->order_by=order_by;
  tj->nchunk=nchunk; 
  tj->filename = build_data_filename(dbt->database->filename, dbt->table_filename, tj->nchunk, 0);
  tj->has_generated_fields=has_generated_fields;
  tj->dbt=dbt;
  return tj;
}

void m_async_queue_push_conservative(GAsyncQueue *queue, struct job *element){
  // Each job weights 500 bytes aprox.
  // if we reach to 200k of jobs, which is 100MB of RAM, we are going to wait 5 seconds 
  // which is not too much considering that it will impossible to proccess 200k of jobs 
  // in 5 seconds. 
  // I don't think that we need to this values as parameters, unless that a user needs to 
  // set hundreds of threads
  while (g_async_queue_length(queue)>200000){
    g_warning("Too many jobs in the queue. We are pausing the jobs creation for 5 seconds.");
    sleep(5);
  }
  g_async_queue_push(queue, element);
}

void dump_table(MYSQL *conn, struct db_table *dbt,
                struct configuration *conf, gboolean is_innodb) {
//  char *database = dbt->database;
//  char *table = dbt->table;
	GList * partitions = NULL;
	if (split_partitions)
		partitions = get_partitions_for_table(conn, dbt->database->name, dbt->table);

  GList *chunks = NULL;
  if (rows_per_file)
    chunks = get_chunks_for_table(conn, dbt->database->name, dbt->table, conf);

  gboolean has_generated_fields =
    detect_generated_fields(conn, dbt);

  if (partitions){
    int npartition=0;
		for (partitions = g_list_first(partitions); partitions; partitions=g_list_next(partitions)) {
			struct job *j = g_new0(struct job,1);
			struct table_job *tj = NULL;
			j->job_data=(void*) tj;
			j->conf=conf;
			j->type= is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
      tj = new_table_job(dbt, (char *) g_strdup_printf(" PARTITION (%s) ", (char *)partitions->data), NULL, npartition, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
      j->job_data = (void *)tj;
			if (!is_innodb && npartition)
			  g_atomic_int_inc(&non_innodb_table_counter);
			g_async_queue_push(conf->queue,j);
			npartition++;
		}
		g_list_free_full(g_list_first(partitions), (GDestroyNotify)g_free);    

  } else if (chunks) {
    int nchunk = 0;
    GList *iter;
    for (iter = chunks; iter != NULL; iter = iter->next) {
      struct job *j = g_new0(struct job, 1);
      struct table_job *tj = NULL;
      j->conf = conf;
      j->type = is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
      tj = new_table_job(dbt, NULL, (char *)iter->data, nchunk, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
      j->job_data = (void *)tj;
      if (!is_innodb && nchunk)
        g_atomic_int_inc(&non_innodb_table_counter);
      m_async_queue_push_conservative(conf->queue, j);
      nchunk++;
    }
    g_list_free(chunks);
  } else {
    struct job *j = g_new0(struct job, 1);
    struct table_job *tj = NULL;
    j->conf = conf;
    j->type = is_innodb ? JOB_DUMP : JOB_DUMP_NON_INNODB;
    tj = new_table_job(dbt, NULL, NULL, 0, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
    j->job_data = (void *)tj;
    g_async_queue_push(conf->queue, j);
  }
}

void create_jobs_for_non_innodb_table_list_in_less_locking_mode(MYSQL *conn, GList *noninnodb_tables_list,
                 struct configuration *conf) {
  struct db_table *dbt=NULL;
  GList *chunks = NULL;
  GList * partitions = NULL;

  struct job *j = g_new0(struct job, 1);
  struct tables_job *tjs = g_new0(struct tables_job, 1);
  j->conf = conf;
  j->type = JOB_LOCK_DUMP_NON_INNODB;
  j->job_data = (void *)tjs;

  GList *iter;

  for (iter = noninnodb_tables_list; iter != NULL; iter = iter->next) {
    dbt = (struct db_table *)iter->data;

    if (rows_per_file)
      chunks = get_chunks_for_table(conn, dbt->database->name, dbt->table, conf);
    gboolean has_generated_fields =
      detect_generated_fields(conn, dbt);

    if (split_partitions)
      partitions = get_partitions_for_table(conn, dbt->database->name, dbt->table);

    if (partitions){
      int npartition=0;
      for (partitions = g_list_first(partitions); partitions; partitions=g_list_next(partitions)) {
        struct table_job *tj = NULL;
        tj = new_table_job(dbt, (char *) g_strdup_printf(" PARTITION (%s) ", (char *)partitions->data), NULL, npartition, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
        tjs->table_job_list = g_list_prepend(tjs->table_job_list, tj);
        npartition++;
      }
      g_list_free_full(g_list_first(partitions), (GDestroyNotify)g_free);

    } else if (chunks) {
      int nchunk = 0;
      GList *citer;
      for (citer = chunks; citer != NULL; citer = citer->next) {
        struct table_job *tj = new_table_job(dbt, NULL, (char *)iter->data, nchunk, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
        tjs->table_job_list = g_list_prepend(tjs->table_job_list, tj);
        nchunk++;
      }
      g_list_free(chunks);
    } else {
      struct table_job *tj = NULL;
      tj = new_table_job(dbt, NULL, NULL, 0, has_generated_fields, get_primary_key_string(conn, dbt->database->name, dbt->table));
      tjs->table_job_list = g_list_prepend(tjs->table_job_list, tj);
    }
  }
  tjs->table_job_list = g_list_reverse(tjs->table_job_list);
  g_async_queue_push(conf->queue_less_locking, j);
}

void append_columns (GString *statement, MYSQL_FIELD *fields, guint num_fields){
  guint i = 0;
  for (i = 0; i < num_fields; ++i) {
    if (i > 0) {
      g_string_append_c(statement, ',');
    }
    g_string_append_printf(statement, "`%s`", fields[i].name);
  }
}

void append_insert (gboolean condition, GString *statement, char *table, MYSQL_FIELD *fields, guint num_fields){
  if (condition) {
    if (insert_ignore) {
      g_string_printf(statement, "INSERT IGNORE INTO `%s` (", table);
    } else {
      g_string_printf(statement, "INSERT INTO `%s` (", table);
    }
    append_columns(statement,fields,num_fields);
    g_string_append(statement, ") VALUES");
  } else {
    if (insert_ignore) {
      g_string_printf(statement, "INSERT IGNORE INTO `%s` VALUES", table);
    } else {
      g_string_printf(statement, "INSERT INTO `%s` VALUES", table);
    }
  }
}

/* Do actual data chunk reading/writing magic */
guint64 dump_table_data(MYSQL *conn, FILE *file, struct table_job * tj){
  // There are 2 possible options to chunk the files:
  // - no chunk: this means that will be just 1 data file
  // - chunk_filesize: this function will be spliting the per filesize, this means that multiple files will be created
  // Split by row is before this step
  // It could write multiple INSERT statments in a data file if statement_size is reached
  guint i;
  guint fn = 0;
  guint sub_part=0;
  guint st_in_file = 0;
  guint num_fields = 0;
  guint64 num_rows = 0;
  guint64 num_rows_st = 0;
  MYSQL_RES *result = NULL;
  char *query = NULL;
  gchar *fcfile = NULL;
  gchar *load_data_fn=NULL;
//  gchar *filename_prefix = NULL;
  struct db_table * dbt = tj->dbt;
  /* Buffer for escaping field values */
  GString *escaped = g_string_sized_new(3000);
  FILE *main_file=file;
//  if (chunk_filesize) {
//    fcfile = build_data_filename(dbt->database->filename, dbt->table_filename, fn, sub_part);
//  }else{
    fcfile = g_strdup(tj->filename);
//  }

  gboolean has_generated_fields = tj->has_generated_fields;

  /* Ghm, not sure if this should be statement_size - but default isn't too big
   * for now */
  GString *statement = g_string_sized_new(statement_size);
  GString *statement_row = g_string_sized_new(0);

  GString *select_fields;

  if (has_generated_fields) {
    select_fields = get_insertable_fields(conn, tj->database, tj->table);
  } else {
    select_fields = g_string_new("*");
  }

  /* Poor man's database code */
  query = g_strdup_printf(
      "SELECT %s %s FROM `%s`.`%s` %s %s %s %s %s %s %s",
      (detected_server == SERVER_TYPE_MYSQL) ? "/*!40001 SQL_NO_CACHE */" : "",
      select_fields->str, tj->database, tj->table, tj->partition?tj->partition:"", (tj->where || where_option ) ? "WHERE" : "",
      tj->where ? tj->where : "",  (tj->where && where_option ) ? "AND" : "", where_option ? where_option : "", tj->order_by ? "ORDER BY" : "",
      tj->order_by ? tj->order_by : "");
  g_string_free(select_fields, TRUE);
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    // ERROR 1146
    if (success_on_1146 && mysql_errno(conn) == 1146) {
      g_warning("Error dumping table (%s.%s) data: %s ", tj->database, tj->table,
                mysql_error(conn));
    } else {
      g_critical("Error dumping table (%s.%s) data: %s ", tj->database, tj->table,
                 mysql_error(conn));
      errors++;
    }
    goto cleanup;
  }

  num_fields = mysql_num_fields(result);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);

  MYSQL_ROW row;

  g_string_set_size(statement, 0);

  gboolean first_time=TRUE;
  /* Poor man's data dump code */
  while ((row = mysql_fetch_row(result))) {
    gulong *lengths = mysql_fetch_lengths(result);
    num_rows++;

    if (!statement->len) {
	    
      // A file can be chunked by amount of rows or file size. 
      //
      if (!st_in_file) { 
        // File Header
        if (detected_server == SERVER_TYPE_MYSQL) {
          if (set_names_str)
            g_string_printf(statement,"%s;\n",set_names_str);
          g_string_append(statement, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/;\n");
          if (!skip_tz) {
            g_string_append(statement, "/*!40103 SET TIME_ZONE='+00:00' */;\n");
          }
        } else if (detected_server == SERVER_TYPE_TIDB) {
          if (!skip_tz) {
            g_string_printf(statement, "/*!40103 SET TIME_ZONE='+00:00' */;\n");
          }
        } else {
          g_string_printf(statement, "SET FOREIGN_KEY_CHECKS=0;\n");
        }

        if (!write_data(file, statement)) {
          g_critical("Could not write out data for %s.%s", tj->database, tj->table);
          goto cleanup;
        }
      }
      if ( load_data ){
        if (first_time){
          load_data_fn=build_filename(dbt->database->filename, dbt->table_filename, fn, sub_part, "dat");
          char * basename=g_path_get_basename(load_data_fn);
  	      g_string_printf(statement, "LOAD DATA LOCAL INFILE '%s' REPLACE INTO TABLE `%s` ",basename,tj->table);
          g_free(basename);
	        if (fields_terminated_by_ld)
	        	g_string_append_printf(statement, "FIELDS TERMINATED BY '%s' ",fields_terminated_by_ld);
	        if (fields_enclosed_by_ld)
  	        g_string_append_printf(statement, "ENCLOSED BY '%s' ",fields_enclosed_by_ld);
	        if (fields_escaped_by)
            g_string_append_printf(statement, "ESCAPED BY '%s' ",fields_escaped_by);
	        g_string_append(statement, "LINES ");
	        if (lines_starting_by_ld)
  	        g_string_append_printf(statement, "STARTING BY '%s' ",lines_starting_by_ld);
	        g_string_append_printf(statement, "TERMINATED BY '%s' (", lines_terminated_by_ld);

          append_columns(statement,fields,num_fields);
          g_string_append(statement,");\n");
	        if (!write_data(main_file, statement)) {
		        g_critical("Could not write out data for %s.%s", tj->database, tj->table);
		        goto cleanup;
  	      }else{
            g_string_set_size(statement, 0);
            g_free(fcfile);
            fcfile=load_data_fn;
  
            if (!compress_output) {
              fclose((FILE *)file);
              file = g_fopen(fcfile, "a");
            } else {
              gzclose((gzFile)file);
              file = (void *)gzopen(fcfile, "a");
            }
	        }
          first_time=FALSE;
        }
      }else{
        append_insert ((complete_insert || has_generated_fields), statement, tj->table, fields, num_fields);
      }
      num_rows_st = 0;
    }

    if (statement_row->len) {
      g_string_append(statement, statement_row->str);
      g_string_set_size(statement_row, 0);
      num_rows_st++;
    }

    g_string_append(statement_row, lines_starting_by);
    GList *f = dbt->anonymized_function;
    gchar * (*fun_ptr)(gchar **) = &identity_function;
    for (i = 0; i < num_fields; i++) {
      if (f){
      fun_ptr=f->data;
      f=f->next;
      }
      if (load_data){
        if (!row[i]) {
//          g_string_append(statement_row,fields_enclosed_by);
          g_string_append(statement_row, "\\N");
//          g_string_append(statement_row,fields_enclosed_by);
        }else if (fields[i].type != MYSQL_TYPE_LONG && fields[i].type != MYSQL_TYPE_LONGLONG  && fields[i].type != MYSQL_TYPE_INT24  && fields[i].type != MYSQL_TYPE_SHORT ){
          g_string_append(statement_row,fields_enclosed_by);
          g_string_set_size(escaped, lengths[i] * 2 + 1);
          mysql_real_escape_string(conn, escaped->str, fun_ptr(&(row[i])), lengths[i]);
          g_string_append(statement_row,escaped->str);
          g_string_append(statement_row,fields_enclosed_by);
        }else
          g_string_append(statement_row,fun_ptr(&(row[i])));
      }else{
        /* Don't escape safe formats, saves some time */
        if (!row[i]) {
          g_string_append(statement_row, "NULL");
        } else if (fields[i].flags & NUM_FLAG) {
          g_string_append(statement_row, fun_ptr(&(row[i])));
        } else {
          /* We reuse buffers for string escaping, growing is expensive just at
           * the beginning */
          g_string_set_size(escaped, lengths[i] * 2 + 1);
          mysql_real_escape_string(conn, escaped->str, fun_ptr(&(row[i])), lengths[i]);
          if (fields[i].type == MYSQL_TYPE_JSON)
            g_string_append(statement_row, "CONVERT(");
          g_string_append_c(statement_row, '\"');
          g_string_append(statement_row, escaped->str);
          g_string_append_c(statement_row, '\"');
          if (fields[i].type == MYSQL_TYPE_JSON)
            g_string_append(statement_row, " USING UTF8MB4)");
        }
      }
      if (i < num_fields - 1) {
        g_string_append(statement_row, fields_terminated_by);
      } else {
        g_string_append_printf(statement_row,"%s", lines_terminated_by);

        /* INSERT statement is closed before over limit */
        if (statement->len + statement_row->len + 1 > statement_size) {
          if (num_rows_st == 0) {
            g_string_append(statement, statement_row->str);
            g_string_set_size(statement_row, 0);
            g_warning("Row bigger than statement_size for %s.%s", tj->database,
                      tj->table);
          }
          g_string_append(statement, statement_terminated_by);

          if (!write_data(file, statement)) {
            g_critical("Could not write out data for %s.%s", tj->database, tj->table);
            goto cleanup;
          } else {
            st_in_file++;
            if (chunk_filesize &&
                st_in_file * (guint)ceil((float)statement_size / 1024 / 1024) >
                    chunk_filesize) {
              if (tj->where == NULL){
                fn++;
              }else{
                sub_part++;
              }
              m_close(file);
              if (stream) g_async_queue_push(stream_queue, g_strdup(fcfile));
              g_free(fcfile);
              fcfile = build_data_filename(dbt->database->filename, dbt->table_filename, fn, sub_part);
              file = m_open(fcfile,"w");
              st_in_file = 0;
            }
          }
          g_string_set_size(statement, 0);
        } else {
          if (num_rows_st && ! load_data)
            g_string_append_c(statement, ',');
          g_string_append(statement, statement_row->str);
          num_rows_st++;
          g_string_set_size(statement_row, 0);
        }
      }
    }
  }
  if (mysql_errno(conn)) {
    g_critical("Could not read data from %s.%s: %s", tj->database, tj->table,
               mysql_error(conn));
    errors++;
  }

  if (statement_row->len > 0) {
    /* this last row has not been written out */
    if (statement->len > 0) {
      /* strange, should not happen */
      g_string_append(statement, statement_row->str);
    } else {
      append_insert (complete_insert, statement, tj->table, fields, num_fields); 
      g_string_append(statement, statement_row->str);
    }
  }

  if (statement->len > 0) {
    g_string_append(statement, statement_terminated_by);
    if (!write_data(file, statement)) {
      g_critical(
          "Could not write out closing newline for %s.%s, now this is sad!",
          tj->database, tj->table);
      goto cleanup;
    }
    st_in_file++;
  }

cleanup:
  g_free(query);

  g_string_free(escaped, TRUE);
  g_string_free(statement, TRUE);
  g_string_free(statement_row, TRUE);

  if (result) {
    mysql_free_result(result);
  }

  if (file) {
    m_close(file);
  }

  if (!st_in_file && !build_empty_files) {
    // dropping the useless file
    if (remove(fcfile)) {
      g_warning("Failed to remove empty file : %s\n", fcfile);
    }
  } else if (chunk_filesize) {
    if (stream) g_async_queue_push(stream_queue, g_strdup(fcfile));
  }else{
    if (stream) g_async_queue_push(stream_queue, g_strdup(tj->filename));
  }

  g_mutex_lock(dbt->rows_lock);
  dbt->rows+=num_rows;
  g_mutex_unlock(dbt->rows_lock);

  g_free(fcfile);

  return num_rows;
}


gboolean write_data(FILE *file, GString *data) {
  size_t written = 0;
  ssize_t r = 0;
  while (written < data->len) {
    r=m_write(file, data->str + written, data->len);
    if (r < 0) {
      g_critical("Couldn't write data to a file: %s", strerror(errno));
      errors++;
      return FALSE;
    }
    written += r;
  }

  return TRUE;
}
