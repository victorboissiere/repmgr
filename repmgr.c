/*
 * repmgr.c - Command interpreter for the repmgr
 * Copyright (C) 2ndQuadrant, 2010
 *
 * This module is a command-line utility to easily setup a cluster of
 * hot standby servers for an HA environment
 *
 * Commands implemented are.
 * MASTER REGISTER, STANDBY REGISTER, STANDBY CLONE, STANDBY FOLLOW,
 * STANDBY PROMOTE
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "repmgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "config.h"
#include "check_dir.h"

#define RECOVERY_FILE "recovery.conf"
#define RECOVERY_DONE_FILE "recovery.done"

#define NO_ACTION        0		/* Not a real action, just to initialize */
#define MASTER_REGISTER  1
#define STANDBY_REGISTER 2
#define STANDBY_CLONE 	 3
#define STANDBY_PROMOTE  4
#define STANDBY_FOLLOW 	 5

static void help(const char *progname);
static bool create_recovery_file(const char *data_dir);
static int  copy_remote_files(char *host, char *remote_user, char *remote_path, char *local_path, bool is_directory);
static bool check_parameters_for_action(const int action);

static void do_master_register(void);
static void do_standby_register(void);
static void do_standby_clone(void);
static void do_standby_promote(void);
static void do_standby_follow(void);
static void help(const char* progname);
static void usage(void);

/* Global variables */
static const char *progname;
static const char *keywords[6];
static const char *values[6];
char repmgr_schema[MAXLEN];
bool read_config_file = true;

/* Initialization of runtime options */
t_runtime_options runtime_options = { "", "", "", "", "", "", DEFAULT_WAL_KEEP_SEGMENTS, false, false, "" };
t_configuration_options options = { "", -1, "", "", "" };

static char		*server_mode = NULL;
static char		*server_cmd = NULL;

int
main(int argc, char **argv)
{
	static struct option long_options[] =
	{
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"dest-dir", required_argument, NULL, 'D'},
		{"config-file", required_argument, NULL, 'f'},
		{"remote-user", required_argument, NULL, 'R'},
		{"wal-keep-segments", required_argument, NULL, 'w'},
		{"force", no_argument, NULL, 'F'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};

	int			optindex;
	int			c;
	int			action = NO_ACTION;

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			printf("%s (PostgreSQL) " PG_VERSION "\n", progname);
			exit(0);
		}
	}


	while ((c = getopt_long(argc, argv, "d:h:p:U:D:f:R:w:F:v", long_options, &optindex)) != -1)
	{
		switch (c)
		{
		case 'd':
			strncpy(runtime_options.dbname, optarg, MAXLEN);
			break;
		case 'h':
			strncpy(runtime_options.host, optarg, MAXLEN);
			break;
		case 'p':
			if (atoi(optarg) > 0)
				strncpy(runtime_options.masterport, optarg, MAXLEN);
			break;
		case 'U':
			strncpy(runtime_options.username, optarg, MAXLEN);
			break;
		case 'D':
			strncpy(runtime_options.dest_dir, optarg, MAXFILENAME);
			break;
		case 'f':
			strncpy(runtime_options.config_file, optarg, MAXLEN);
			break;
		case 'R':
			strncpy(runtime_options.remote_user, optarg, MAXLEN);
			break;
		case 'w':
			if (atoi(optarg) > 0)
				strncpy(runtime_options.wal_keep_segments, optarg, MAXLEN);
			break;
		case 'F':
			runtime_options.force = true;
			break;
		case 'v':
			runtime_options.verbose = true;
			break;
		default:
			usage();
			exit(1);
		}
	}

	/*
	 * Now we need to obtain the action, this comes in one of these forms:
	 * MASTER REGISTER |
	 * STANDBY {REGISTER | CLONE [node] | PROMOTE | FOLLOW [node]}
	 *
	 * the node part is optional, if we receive it then we shouldn't
	 * have received a -h option
	 */
	if (optind < argc)
	{
		server_mode = argv[optind++];
		if (strcasecmp(server_mode, "STANDBY") != 0 && strcasecmp(server_mode, "MASTER") != 0)
		{
			usage();
			exit(1);
		}
	}

	if (optind < argc)
	{
		server_cmd = argv[optind++];
		if (strcasecmp(server_cmd, "REGISTER") == 0)
		{
			/*
			 * we don't use this info in any other place so i will
			 * just execute the compare again instead of having an
			 * additional variable to hold a value that we will use
			 * no more
			 */
			if (strcasecmp(server_mode, "MASTER") == 0)
				action = MASTER_REGISTER;
			else if (strcasecmp(server_mode, "STANDBY") == 0)
				action = STANDBY_REGISTER;
		}
		else if (strcasecmp(server_cmd, "CLONE") == 0)
			action = STANDBY_CLONE;
		else if (strcasecmp(server_cmd, "PROMOTE") == 0)
			action = STANDBY_PROMOTE;
		else if (strcasecmp(server_cmd, "FOLLOW") == 0)
			action = STANDBY_FOLLOW;
		else
		{
			usage();
			exit(1);
		}
	}

	/* For some actions we still can receive a last argument */
	if (action == STANDBY_CLONE)
	{
		if (optind < argc)
		{
			if (runtime_options.host[0])
			{
				log_err(_("Conflicting parameters you can't use -h while providing a node separately.\n"));
				usage();
				exit(1);
			}
			strncpy(runtime_options.host, argv[optind++], MAXLEN);
		}
	}

	switch (optind < argc)
	{
	case 0:
		break;
	default:
		log_err(_("%s: too many command-line arguments (first is \"%s\")\n"),
		        progname, argv[optind + 1]);
		usage();
		exit(1);
	}

	if (!check_parameters_for_action(action))
		exit(1);

	if (!runtime_options.dbname[0])
	{
		if (getenv("PGDATABASE"))
			strncpy(runtime_options.dbname, getenv("PGDATABASE"), MAXLEN);
		else if (getenv("PGUSER"))
			strncpy(runtime_options.dbname, getenv("PGUSER"), MAXLEN);
		else
			strncpy(runtime_options.dbname, DEFAULT_DBNAME, MAXLEN);
	}
	
	/*
	 * Read the configuration file: repmgr.conf
	 */
	parse_config(config_file, &config);
	if (config.node == -1)
	{
		fprintf(stderr, "Node information is missing. "
		        "Check the configuration file.\n");
		exit(1);
	}

	keywords[2] = "user";
	values[2] = runtime_options.username;
	keywords[3] = "dbname";
	values[3] = runtime_options.dbname;
	keywords[4] = "application_name";
	values[4] = (char *) progname;
	keywords[5] = NULL;
	values[5] = NULL;

	if (read_config_file) {

		if (!runtime_options.config_file[0])
			strncpy(runtime_options.config_file, DEFAULT_CONFIG_FILE, MAXLEN);

		/*
		 * Read the configuration file: repmgr.conf
		 */
		if (runtime_options.verbose)
			printf(_("Opening configuration file: %s\n"), runtime_options.config_file);

		parse_config(runtime_options.config_file, &options);
		if (options.node == -1)
		{
			log_err("Node information is missing. "
				"Check the configuration file.\n");
			exit(1);
		}

	}

	logger_init(progname, options.loglevel, options.logfacility);

	/* Prepare the repmgr schema variable */
	snprintf(repmgr_schema, MAXLEN, "%s%s", DEFAULT_REPMGR_SCHEMA_PREFIX, options.cluster_name);

	switch (action)
	{
	case MASTER_REGISTER:
		do_master_register();
		break;
	case STANDBY_REGISTER:
		do_standby_register();
		break;
	case STANDBY_CLONE:
		do_standby_clone();
		break;
	case STANDBY_PROMOTE:
		do_standby_promote();
		break;
	case STANDBY_FOLLOW:
		do_standby_follow();
		break;
	default:
		usage();
		exit(1);
	}
	logger_shutdown();

	return 0;
}


static void
do_master_register(void)
{
	PGconn 		*conn;
	PGresult	*res;
	char 		sqlquery[QUERY_STR_LEN];

	bool		schema_exists = false;
	char 		master_version[MAXVERSIONSTR];

	conn = establishDBConnection(options.conninfo, true);

	/* master should be v9 or better */
	pg_version(conn, master_version);
	if (strcmp(master_version, "") == 0)
	{
		PQfinish(conn);
		log_err( _("%s needs master to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* Check we are a master */
	if (is_standby(conn))
	{
		log_err(_("%s needs master to be PostgreSQL 9.0 or better\n"), progname);
		PQfinish(conn);
		return;
	}

	/* Check if there is a schema for this cluster */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT 1 FROM pg_namespace WHERE nspname = '%s'", repmgr_schema);
	log_debug("master register: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err(_("Can't get info about schemas: %s\n"), PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}

	if (PQntuples(res) > 0)			/* schema exists */
	{
		if (!runtime_options.force)	/* and we are not forcing so error */
		{
			log_notice(_("Schema %s already exists.\n"), repmgr_schema);
			PQclear(res);
			PQfinish(conn);
			return;
		}
		schema_exists = true;
	}
	PQclear(res);

	if (!schema_exists)
	{
		log_info("master register: creating database objects inside the %s schema", repmgr_schema);

		/* ok, create the schema */
		snprintf(sqlquery, QUERY_STR_LEN, "CREATE SCHEMA %s", repmgr_schema);
		log_debug("master register: %s\n", sqlquery);
		if (!PQexec(conn, sqlquery))
		{
			log_err(_("Cannot create the schema %s: %s\n"),
			        repmgr_schema, PQerrorMessage(conn));
			PQfinish(conn);
			return;
		}

		/* ... the tables */
		snprintf(sqlquery, QUERY_STR_LEN, "CREATE TABLE %s.repl_nodes (        "
		        "  id        integer primary key, "
		        "  cluster   text    not null,    "
		        "  conninfo  text    not null)", repmgr_schema);
		log_debug("master register: %s\n", sqlquery);
		if (!PQexec(conn, sqlquery))
		{
			log_err(_("Cannot create the table %s.repl_nodes: %s\n"),
			        repmgr_schema, PQerrorMessage(conn));
			PQfinish(conn);
			return;
		}

		snprintf(sqlquery, QUERY_STR_LEN, "CREATE TABLE %s.repl_monitor ( "
		        "  primary_node                   INTEGER NOT NULL, "
		        "  standby_node                   INTEGER NOT NULL, "
		        "  last_monitor_time              TIMESTAMP WITH TIME ZONE NOT NULL, "
		        "  last_wal_primary_location      TEXT NOT NULL,   "
		        "  last_wal_standby_location      TEXT NOT NULL,   "
		        "  replication_lag                BIGINT NOT NULL, "
		        "  apply_lag                      BIGINT NOT NULL) ", repmgr_schema);
		log_debug("master register: %s\n", sqlquery);
		if (!PQexec(conn, sqlquery))
		{
			log_err(_("Cannot create the table %s.repl_monitor: %s\n"),
			        repmgr_schema, PQerrorMessage(conn));
			PQfinish(conn);
			return;
		}

		/* and the view */
		snprintf(sqlquery, QUERY_STR_LEN, "CREATE VIEW %s.repl_status AS "
		        "  WITH monitor_info AS (SELECT *, ROW_NUMBER() OVER (PARTITION BY primary_node, standby_node "
		        " ORDER BY last_monitor_time desc) "
		        "  FROM %s.repl_monitor) "
		        "  SELECT primary_node, standby_node, last_monitor_time, last_wal_primary_location, "
		        "         last_wal_standby_location, pg_size_pretty(replication_lag) replication_lag, "
		        "         pg_size_pretty(apply_lag) apply_lag, age(now(), last_monitor_time) AS time_lag "
		        "    FROM monitor_info a "
		        "   WHERE row_number = 1", repmgr_schema, repmgr_schema);
		log_debug("master register: %s\n", sqlquery);
		if (!PQexec(conn, sqlquery))
		{
			log_err(_("Cannot create the view %s.repl_status: %s\n"),
			        repmgr_schema, PQerrorMessage(conn));
			PQfinish(conn);
			return;
		}
	}
	else
	{
		PGconn *master_conn;
		int 	id;

		/* Ensure there isn't any other master already registered */
		master_conn = getMasterConnection(conn, options.node, options.cluster_name, &id);
		if (master_conn != NULL)
		{
			PQfinish(master_conn);
			log_warning(_("There is a master already in cluster %s\n"), options.cluster_name);
			return;
		}
	}

	/* Now register the master */
	if (runtime_options.force)
	{
		snprintf(sqlquery, QUERY_STR_LEN, "DELETE FROM %s.repl_nodes "
		        " WHERE id = %d",
		        repmgr_schema, options.node);
		log_debug("master register: %s\n", sqlquery);

		if (!PQexec(conn, sqlquery))
		{
			log_warning(_("Cannot delete node details, %s\n"),
			        PQerrorMessage(conn));
			PQfinish(conn);
			return;
		}
	}

	snprintf(sqlquery, QUERY_STR_LEN, "INSERT INTO %s.repl_nodes "
	        "VALUES (%d, '%s', '%s')",
	        repmgr_schema, options.node, options.cluster_name, options.conninfo);
	log_debug("master register: %s\n", sqlquery);

	if (!PQexec(conn, sqlquery))
	{
		log_warning(_("Cannot insert node details, %s\n"),
		        PQerrorMessage(conn));
		PQfinish(conn);
		return;
	}

	PQfinish(conn);
	log_notice(_("Master node correctly registered for cluster %s with id %d (conninfo: %s)\n"),
		options.cluster_name, options.node, options.conninfo);
	return;
}


static void
do_standby_register(void)
{
	PGconn 		*conn;
	PGconn		*master_conn;
	int			master_id;

	PGresult	*res;
	char 		sqlquery[QUERY_STR_LEN];

	char master_version[MAXVERSIONSTR];
	char standby_version[MAXVERSIONSTR];

	conn = establishDBConnection(options.conninfo, true);

	/* should be v9 or better */
	pg_version(conn, standby_version);
	if (strcmp(standby_version, "") == 0)
	{
		PQfinish(conn);
		log_err(_("%s needs standby to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* Check we are a standby */
	if (!is_standby(conn))
	{
		log_err(_("repmgr: This node should be a standby (%s)\n"), options.conninfo);
		PQfinish(conn);
		return;
	}

	/* Check if there is a schema for this cluster */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT 1 FROM pg_namespace WHERE nspname = '%s'", repmgr_schema);
	log_debug("standby register: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about tablespaces: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}

	if (PQntuples(res) == 0)		/* schema doesn't exists */
	{
		log_err("Schema %s doesn't exists.\n", repmgr_schema);
		PQclear(res);
		PQfinish(conn);
		return;
	}
	PQclear(res);

	/* check if there is a master in this cluster */
	master_conn = getMasterConnection(conn, options.node, options.cluster_name, &master_id);
	if (!master_conn) {
		log_err(_("Cannot retrieve information about the connection to the master\n"));
		return;
	}

	/* master should be v9 or better */
	pg_version(master_conn, master_version);
	if (strcmp(master_version, "") == 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs master to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* master and standby version should match */
	if (strcmp(master_version, standby_version) != 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs versions of both master (%s) and standby (%s) to match.\n"),
		        progname, master_version, standby_version);
		return;
	}


	/* Now register the standby */
	if (runtime_options.force)
	{
		snprintf(sqlquery, QUERY_STR_LEN, "DELETE FROM %s.repl_nodes "
		        " WHERE id = %d",
		        repmgr_schema, options.node);
		log_debug("standby register: %s\n", sqlquery);

		if (!PQexec(master_conn, sqlquery))
		{
			log_err("Cannot delete node details, %s\n",
			        PQerrorMessage(master_conn));
			PQfinish(master_conn);
			PQfinish(conn);
			return;
		}
	}

	snprintf(sqlquery, QUERY_STR_LEN, "INSERT INTO %s.repl_nodes "
	        "VALUES (%d, '%s', '%s')",
	        repmgr_schema, options.node, options.cluster_name, options.conninfo);
	log_debug("standby register: %s\n", sqlquery);

	if (!PQexec(master_conn, sqlquery))
	{
		log_err("Cannot insert node details, %s\n",
		        PQerrorMessage(master_conn));
		PQfinish(master_conn);
		PQfinish(conn);
		return;
	}

	PQfinish(master_conn);
	PQfinish(conn);
	return;
}


static void
do_standby_clone(void)
{
	PGconn 		*conn;
	PGresult	*res;
	char 		sqlquery[QUERY_STR_LEN];

	int			r = 0;
	int			i;
	bool		pg_dir = false;
	char		master_data_directory[MAXFILENAME];
	char		master_config_file[MAXFILENAME];
	char		master_hba_file[MAXFILENAME];
	char		master_ident_file[MAXFILENAME];

	char		master_control_file[MAXFILENAME];
	char		local_control_file[MAXFILENAME];
	char		tblspc_dir[MAXFILENAME];

	const char	*first_wal_segment = NULL;
	const char	*last_wal_segment = NULL;

	char	master_version[MAXVERSIONSTR];

	/* if dest_dir hasn't been provided, initialize to current directory */
	if (!runtime_options.dest_dir[0])
	{
		strncpy(runtime_options.dest_dir, DEFAULT_DEST_DIR, MAXFILENAME);
	}

	/* Check this directory could be used as a PGDATA dir */
	switch (check_dir(runtime_options.dest_dir))
	{
	case 0:
		/* dest_dir not there, must create it */
		log_info(_("creating directory %s ...\n"), runtime_options.dest_dir);
		fflush(stdout);

		if (!create_directory(runtime_options.dest_dir))
		{
			log_err(_("%s: couldn't create directory %s ...\n"),
			        progname, runtime_options.dest_dir);
			return;
		}
		break;
	case 1:
		/* Present but empty, fix permissions and use it */
		log_info(_("fixing permissions on existing directory %s ...\n"),
		       runtime_options.dest_dir);
		fflush(stdout);

		if (!set_directory_permissions(runtime_options.dest_dir))
		{
			log_err(_("%s: could not change permissions of directory \"%s\": %s\n"),
			        progname, runtime_options.dest_dir, strerror(errno));
			return;
		}
		break;
	case 2:
		/* Present and not empty */
		log_warning( _("%s: directory \"%s\" exists but is not empty\n"),
		        progname, runtime_options.dest_dir);

		pg_dir = is_pg_dir(runtime_options.dest_dir);
		if (pg_dir && !runtime_options.force)
		{
			log_warning( _("\nThis looks like a PostgreSQL directory.\n"
			                  "If you are sure you want to clone here, "
			                  "please check there is no PostgreSQL server "
			                  "running and use the --force option\n"));
			return;
		}
		else if (pg_dir && runtime_options.force)
		{
			/* Let it continue */
			break;
		}
		else
			return;
	default:
		/* Trouble accessing directory */
		log_err( _("%s: could not access directory \"%s\": %s\n"),
		        progname, runtime_options.dest_dir, strerror(errno));
	}

	/* Connection parameters for master only */
	keywords[0] = "host";
	values[0] = runtime_options.host;
	keywords[1] = "port";
	values[1] = runtime_options.masterport;

	/* We need to connect to check configuration and start a backup */
	conn = PQconnectdbParams(keywords, values, true);
	if (!conn)
	{
		log_err(_("%s: could not connect to master\n"),
		        progname);
		return;
	}

	/* primary should be v9 or better */
	pg_version(conn, master_version);
	if (strcmp(master_version, "") == 0)
	{
		PQfinish(conn);
		log_err(_("%s needs master to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* Check we are cloning a primary node */
	if (is_standby(conn))
	{
		PQfinish(conn);
		log_err(_("\nThe command should clone a primary node\n"));
		return;
	}

	/* And check if it is well configured */
	if (!guc_setted(conn, "wal_level", "=", "hot_standby"))
	{
		PQfinish(conn);
		log_err(_("%s needs parameter 'wal_level' to be set to 'hot_standby'\n"), progname);
		return;
	}
	if (!guc_setted(conn, "wal_keep_segments", ">=", runtime_options.wal_keep_segments))
	{
		PQfinish(conn);
		log_err(_("%s needs parameter 'wal_keep_segments' to be set to %s or greater (see the '-w' option)\n"), progname, runtime_options.wal_keep_segments);
		return;
	}
	if (!guc_setted(conn, "archive_mode", "=", "on"))
	{
		PQfinish(conn);
		log_err(_("%s needs parameter 'archive_mode' to be set to 'on'\n"), progname);
		return;
	}

	log_info(_("Succesfully connected to primary. Current installation size is %s\n"), get_cluster_size(conn));

	/* Check if the tablespace locations exists and that we can write to them */
	snprintf(sqlquery, QUERY_STR_LEN, "select spclocation from pg_tablespace where spcname not in ('pg_default', 'pg_global')");
	log_debug("standby clone: %s\n", sqlquery);

	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about tablespaces: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	for (i = 0; i < PQntuples(res); i++)
	{
		strncpy(tblspc_dir, PQgetvalue(res, i, 0), MAXFILENAME);
		/* Check this directory could be used as a PGDATA dir */
		switch (check_dir(tblspc_dir))
		{
		case 0:
			/* tblspc_dir not there, must create it */
			if (runtime_options.verbose)
				printf(_("creating directory \"%s\"... "), tblspc_dir);
			fflush(stdout);

			if (!create_directory(tblspc_dir))
			{
				log_err(_("%s: couldn't create directory \"%s\"...\n"),
				        progname, tblspc_dir);
				PQclear(res);
				PQfinish(conn);
				return;
			}
			break;
		case 1:
			/* Present but empty, fix permissions and use it */
			if (runtime_options.verbose)
				printf(_("fixing permissions on existing directory \"%s\"... "),
				       tblspc_dir);
			fflush(stdout);

			if (!set_directory_permissions(tblspc_dir))
			{
				log_err(_("%s: could not change permissions of directory \"%s\": %s\n"),
				        progname, tblspc_dir, strerror(errno));
				PQclear(res);
				PQfinish(conn);
				return;
			}
			break;
		case 2:
			/* Present and not empty */
			if (!runtime_options.force)
			{
				fprintf(stderr,
				        _("%s: directory \"%s\" exists but is not empty\n"),
				        progname, tblspc_dir);
				PQclear(res);
				PQfinish(conn);
				return;
			}
		default:
			/* Trouble accessing directory */
			log_err(_("%s: could not access directory \"%s\": %s\n"),
			        progname, tblspc_dir, strerror(errno));
			PQclear(res);
			PQfinish(conn);
			return;
		}
	}

	log_notice("Starting backup...\n");

	/* Get the data directory full path and the configuration files location */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT name, setting "
	        "  FROM pg_settings "
	        " WHERE name IN ('data_directory', 'config_file', 'hba_file', 'ident_file')");
	log_debug("standby clone: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about data directory and configuration files: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	for (i = 0; i < PQntuples(res); i++)
	{
		if (strcmp(PQgetvalue(res, i, 0), "data_directory") == 0)
			strncpy(master_data_directory, PQgetvalue(res, i, 1), MAXFILENAME);
		else if (strcmp(PQgetvalue(res, i, 0), "config_file") == 0)
			strncpy(master_config_file, PQgetvalue(res, i, 1), MAXFILENAME);
		else if (strcmp(PQgetvalue(res, i, 0), "hba_file") == 0)
			strncpy(master_hba_file, PQgetvalue(res, i, 1), MAXFILENAME);
		else if (strcmp(PQgetvalue(res, i, 0), "ident_file") == 0)
			strncpy(master_ident_file, PQgetvalue(res, i, 1), MAXFILENAME);
		else
			log_warning(_("unknown parameter: %s\n"), PQgetvalue(res, i, 0));
	}
	PQclear(res);

	/*
	 * inform the master we will start a backup and get the first XLog filename
	 * so we can say to the user we need those files
	 */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT pg_xlogfile_name(pg_start_backup('repmgr_standby_clone_%ld'))", time(NULL));
	log_debug("standby clone: %s\n", sqlquery);
	
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't start backup: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	first_wal_segment = PQgetvalue(res, 0, 0);
	PQclear(res);

	/*
	 * 1) first move global/pg_control
	 *
	 * 2) then move data_directory ommiting the files we have already moved and pg_xlog
	 *    content
	 *
	 * 3) finally We need to backup configuration files (that could be on other directories, debian
	 * like systems likes to do that), so look at config_file, hba_file and ident_file but we
	 * can omit external_pid_file ;)
	 *
	 * On error we need to return but before that execute pg_stop_backup()
	 */

	/* need to create the global sub directory */
	snprintf(master_control_file, MAXFILENAME, "%s/global/pg_control", master_data_directory);
	snprintf(local_control_file, MAXFILENAME, "%s/global", runtime_options.dest_dir);
	if (!create_directory(local_control_file))
	{
		log_err(_("%s: couldn't create directory %s ...\n"),
		        progname, runtime_options.dest_dir);
		goto stop_backup;
	}

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user, master_control_file, local_control_file, false);
	if (r != 0)
		goto stop_backup;

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user, master_data_directory, runtime_options.dest_dir, true);
	if (r != 0)
		goto stop_backup;

	/*
	 * Copy tablespace locations, i'm doing this separately because i couldn't find and appropiate
	 * rsync option but besides we could someday make all these rsync happen concurrently
	 */
	snprintf(sqlquery, QUERY_STR_LEN, "select spclocation from pg_tablespace where spcname not in ('pg_default', 'pg_global')");
	log_debug("standby clone: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about tablespaces: %s\n", PQerrorMessage(conn));
		PQclear(res);
		goto stop_backup;
	}
	for (i = 0; i < PQntuples(res); i++)
	{
		r = copy_remote_files(runtime_options.host, runtime_options.remote_user, PQgetvalue(res, i, 0), PQgetvalue(res, i, 0), true);
		if (r != 0)
			goto stop_backup;
	}

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user, master_config_file, runtime_options.dest_dir, false);
	if (r != 0)
		goto stop_backup;

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user, master_hba_file, runtime_options.dest_dir, false);
	if (r != 0)
		goto stop_backup;

	r = copy_remote_files(runtime_options.host, runtime_options.remote_user, master_ident_file, runtime_options.dest_dir, false);
	if (r != 0)
		goto stop_backup;

stop_backup:
	/* inform the master that we have finished the backup */
	conn = PQconnectdbParams(keywords, values, true);
	if (!conn)
	{
		log_err(_("%s: could not connect to master\n"),
		        progname);
		return;
	}

	log_notice("Finishing backup...\n");

	snprintf(sqlquery, QUERY_STR_LEN, "SELECT pg_xlogfile_name(pg_stop_backup())");
	log_debug("standby clone: %s\n", sqlquery);
	
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't stop backup: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	last_wal_segment = PQgetvalue(res, 0, 0);

	/* Now, if the rsync failed then exit */
	if (r == 0)
	{

	if (runtime_options.verbose)
		printf(_("%s requires primary to keep WAL files %s until at least %s\n"),
		       progname, first_wal_segment, last_wal_segment);

		/* we need to create the pg_xlog sub directory too, i'm reusing a variable here */
		snprintf(local_control_file, MAXFILENAME, "%s/pg_xlog", runtime_options.dest_dir);
		if (!create_directory(local_control_file))
		{
			log_err(_("%s: couldn't create directory %s, you will need to do it manually...\n"),
				progname, runtime_options.dest_dir);
		}

		/* Finally, write the recovery.conf file */
		create_recovery_file(runtime_options.dest_dir);

	}

	PQclear(res);
	PQfinish(conn);

	/* We don't start the service because we still may want to move the directory */
	return;
}


static void
do_standby_promote(void)
{
	PGconn 		*conn;
	PGresult	*res;
	char 		sqlquery[QUERY_STR_LEN];
	char 		script[QUERY_STR_LEN];

	PGconn		*old_master_conn;
	int			old_master_id;

	int			r;
	char		data_dir[MAXLEN];
	char		recovery_file_path[MAXFILENAME];
	char		recovery_done_path[MAXFILENAME];

	char	standby_version[MAXVERSIONSTR];

	/* We need to connect to check configuration */
	conn = establishDBConnection(options.conninfo, true);

	/* we need v9 or better */
	pg_version(conn, standby_version);
	if (strcmp(standby_version, "") == 0)
	{
		PQfinish(conn);
		log_err(_("%s needs standby to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* Check we are in a standby node */
	if (!is_standby(conn))
	{
		log_err("repmgr: The command should be executed on a standby node\n");
		return;
	}

	/* we also need to check if there isn't any master already */
	old_master_conn = getMasterConnection(conn, options.node, options.cluster_name, &old_master_id);
	if (old_master_conn != NULL)
	{
		PQfinish(old_master_conn);
		log_err("There is a master already in this cluster\n");
		return;
	}

	if (runtime_options.verbose)
		printf(_("\n%s: Promoting standby...\n"), progname);

	/* Get the data directory full path and the last subdirectory */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT setting "
	        " FROM pg_settings WHERE name = 'data_directory'");
	log_debug("standby promote: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about data directory: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	strcpy(data_dir, PQgetvalue(res, 0, 0));
	PQclear(res);
	PQfinish(conn);

	snprintf(recovery_file_path, MAXFILENAME, "%s/%s", data_dir, RECOVERY_FILE);
	snprintf(recovery_done_path, MAXFILENAME, "%s/%s", data_dir, RECOVERY_DONE_FILE);
	rename(recovery_file_path, recovery_done_path);

	/* We assume the pg_ctl script is in the PATH */
	snprintf(script, QUERY_STR_LEN, "pg_ctl -D %s -m fast restart", data_dir);
	r = system(script);
	if (r != 0)
	{
		log_err("Can't restart service\n");
		return;
	}

	/* reconnect to check we got promoted */
	/*
	 * XXX i'm removing this because it gives an annoying message saying couldn't connect
	 * but is just the server starting up
	*    conn = establishDBConnection(options.conninfo, true);
	*    if (is_standby(conn))
	*    	log_err("\n%s: STANDBY PROMOTE failed, this is still a standby node.\n", progname);
	*    else
	*    	log_err("\n%s: you should REINDEX any hash indexes you have.\n", progname);
	*    PQfinish(conn);
	*/

	return;
}


static void
do_standby_follow(void)
{
	PGconn 		*conn;
	PGresult	*res;
	char 		sqlquery[QUERY_STR_LEN];
	char 		script[QUERY_STR_LEN];

	PGconn		*master_conn;
	int			master_id;

	int			r;
	char		data_dir[MAXLEN];

	char	master_version[MAXVERSIONSTR];
	char	standby_version[MAXVERSIONSTR];

	/* We need to connect to check configuration */
	conn = establishDBConnection(options.conninfo, true);

	/* Check we are in a standby node */
	if (!is_standby(conn))
	{
		log_err("\n%s: The command should be executed in a standby node\n", progname);
		return;
	}

	/* should be v9 or better */
	pg_version(conn, standby_version);
	if (strcmp(standby_version, "") == 0)
	{
		PQfinish(conn);
		log_err(_("\n%s needs standby to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* we also need to check if there is any master in the cluster */
	master_conn = getMasterConnection(conn, options.node, options.cluster_name, &master_id);
	if (master_conn == NULL)
	{
		PQfinish(conn);
		log_err("There isn't a master to follow in this cluster\n");
		return;
	}

	/* Check we are going to point to a master */
	if (is_standby(master_conn))
	{
		PQfinish(conn);
		log_err("%s: The node to follow should be a master\n", progname);
		return;
	}

	/* should be v9 or better */
	pg_version(master_conn, master_version);
	if (strcmp(master_version, "") == 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs master to be PostgreSQL 9.0 or better\n"), progname);
		return;
	}

	/* master and standby version should match */
	if (strcmp(master_version, standby_version) != 0)
	{
		PQfinish(conn);
		PQfinish(master_conn);
		log_err(_("%s needs versions of both master (%s) and standby (%s) to match.\n"),
		        progname, master_version, standby_version);
		return;
	}

	/*
	 * set the host and masterport variables with the master ones
	 * before closing the connection because we will need them to
	 * recreate the recovery.conf file
	 */
	strncpy(runtime_options.host, PQhost(master_conn), MAXLEN);
	strncpy(runtime_options.masterport, PQport(master_conn), MAXLEN);
	PQfinish(master_conn);

	if (runtime_options.verbose)
		printf(_("\n%s: Changing standby's master...\n"), progname);

	/* Get the data directory full path */
	snprintf(sqlquery, QUERY_STR_LEN, "SELECT setting "
	        " FROM pg_settings WHERE name = 'data_directory'");
	log_debug("standby follow: %s\n", sqlquery);
	res = PQexec(conn, sqlquery);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_err("Can't get info about data directory: %s\n", PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		return;
	}
	strcpy(data_dir, PQgetvalue(res, 0, 0));
	PQclear(res);
	PQfinish(conn);

	/* write the recovery.conf file */
	if (!create_recovery_file(data_dir))
		return;

	/* Finally, restart the service */
	/* We assume the pg_ctl script is in the PATH */
	snprintf(script, QUERY_STR_LEN, "pg_ctl -D %s -m fast restart", data_dir);
	r = system(script);
	if (r != 0)
	{
		log_err("Can't restart service\n");
		return;
	}

	return;
}


void usage(void)
{
	log_err(_("\n\n%s: Replicator manager \n"), progname);
	log_err(_("Try \"%s --help\" for more information.\n"), progname);
}

void help(const char *progname)
{
	printf(_("\n%s: Replicator manager \n"), progname);
	printf(_("Usage:\n"));
	printf(_(" %s [OPTIONS] master  {register}\n"), progname);
	printf(_(" %s [OPTIONS] standby {register|clone|promote|follow}\n"), progname);
	printf(_("\nGeneral options:\n"));
	printf(_("  --help                     show this help, then exit\n"));
	printf(_("  --version                  output version information, then exit\n"));
	printf(_("  --verbose                  output verbose activity information\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME        database to connect to\n"));
	printf(_("  -h, --host=HOSTNAME        database server host or socket directory\n"));
	printf(_("  -p, --port=PORT            database server port\n"));
	printf(_("  -U, --username=USERNAME    database user name to connect as\n"));
	printf(_("\nConfiguration options:\n"));
	printf(_("  -D, --data-dir=DIR         local directory where the files will be copied to\n"));
	printf(_("  -f, --config_file=PATH     path to the configuration file\n"));
	printf(_("  -R, --remote-user=USERNAME database server username for rsync\n"));
	printf(_("  -w, --wal-keep-segments=VALUE  minimum value for the GUC wal_keep_segments (default: 5000)\n"));
	printf(_("  -F, --force                force potentially dangerous operations to happen\n"));

	printf(_("\n%s performs some tasks like clone a node, promote it "), progname);
	printf(_("or making follow another node and then exits.\n"));
	printf(_("COMMANDS:\n"));
	printf(_(" master register       - registers the master in a cluster\n"));
	printf(_(" standby register      - registers a standby in a cluster\n"));
	printf(_(" standby clone [node]  - allows creation of a new standby\n"));
	printf(_(" standby promote       - allows manual promotion of a specific standby into a "));
	printf(_("new master in the event of a failover\n"));
	printf(_(" standby follow        - allows the standby to re-point itself to a new master\n"));
}


static bool
create_recovery_file(const char *data_dir)
{
	FILE		*recovery_file;
	char		recovery_file_path[MAXFILENAME];
	char		line[MAXLEN];

	snprintf(recovery_file_path, MAXFILENAME, "%s/%s", data_dir, RECOVERY_FILE);

	recovery_file = fopen(recovery_file_path, "w");
	if (recovery_file == NULL)
	{
		log_err("could not create recovery.conf file, it could be necessary to create it manually\n");
		return false;
	}

	snprintf(line, MAXLEN, "standby_mode = 'on'\n");
	if (fputs(line, recovery_file) == EOF)
	{
		log_err("recovery file could not be written, it could be necessary to create it manually\n");
		fclose(recovery_file);
		return false;
	}

	snprintf(line, MAXLEN, "primary_conninfo = 'host=%s port=%s'\n", runtime_options.host, runtime_options.masterport);
	if (fputs(line, recovery_file) == EOF)
	{
		log_err("recovery file could not be written, it could be necessary to create it manually\n");
		fclose(recovery_file);
		return false;
	}

	/*FreeFile(recovery_file);*/
	fclose(recovery_file);

	return true;
}


static int
copy_remote_files(char *host, char *remote_user, char *remote_path, char *local_path, bool is_directory)
{
	char script[QUERY_STR_LEN];
	char options[QUERY_STR_LEN];
	char host_string[QUERY_STR_LEN];
	int  r;

	if (strnlen(runtime_options.rsync_options, QUERY_STR_LEN) == 0)
	    snprintf(options, QUERY_STR_LEN, "--archive --checksum --compress --progress --rsh=ssh");
	else
	    strncpy(options, runtime_options.rsync_options, QUERY_STR_LEN);
	
	if (runtime_options.force)
		strcat(options, " --delete");

	if (remote_user == NULL)
	{
		snprintf(host_string, QUERY_STR_LEN, "%s",host);
	}
	else
	{
		snprintf(host_string, QUERY_STR_LEN, "%s@%s",remote_user,host);
	}

	if (is_directory)
	{
		strcat(options, " --exclude=pg_xlog* --exclude=pg_control --exclude=*.pid");
		snprintf(script, QUERY_STR_LEN, "rsync %s %s:%s/* %s",
		        options, host_string, remote_path, local_path);
	}
	else
	{
		snprintf(script, QUERY_STR_LEN, "rsync %s %s:%s %s/.",
		        options, host_string, remote_path, local_path);
	}

	if (runtime_options.verbose)
		printf("rsync command line:  '%s'\n",script);

	r = system(script);

	if (r != 0)
		log_err(_("Can't rsync from remote file or directory (%s:%s)\n"),
		        host_string, remote_path);

	return r;
}


/*
 * Tries to avoid useless or conflicting parameters
 */
static bool
check_parameters_for_action(const int action)
{
	bool ok = true;

	switch (action)
	{
	case MASTER_REGISTER:
		/*
		 * To register a master we only need the repmgr.conf
		 * all other parameters are at least useless and could be
		 * confusing so reject them
		 */
		if (runtime_options.host[0] || runtime_options.masterport[0] || runtime_options.username[0] || 
		        runtime_options.dbname[0])
		{
			log_err("You can't use connection parameters to the master when issuing a MASTER REGISTER command.\n");
			usage();
			ok = false;
		}
		if (runtime_options.dest_dir[0])
		{
			log_err("You don't need a destination directory for MASTER REGISTER command\n");
			usage();
			ok = false;
		}
		break;
	case STANDBY_REGISTER:
		/*
		 * To register a standby we only need the repmgr.conf
		 * we don't need connection parameters to the master
		 * because we can detect the master in repl_nodes
		 */
		if (runtime_options.host[0] || runtime_options.masterport[0] || runtime_options.username[0] || 
		        runtime_options.dbname[0])
		{
			log_err("You can't use connection parameters to the master when issuing a STANDBY REGISTER command.\n");
			usage();
			ok = false;
		}
		if (runtime_options.dest_dir[0])
		{
			log_err("You don't need a destination directory for STANDBY REGISTER command\n");
			usage();
			ok = false;
		}
		break;
	case STANDBY_PROMOTE:
		/*
		 * To promote a standby we only need the repmgr.conf
		 * we don't want connection parameters to the master
		 * because we will try to detect the master in repl_nodes
		 * if we can't find it then the promote action will be cancelled
		 */
		if (runtime_options.host[0] || runtime_options.masterport[0] || runtime_options.username[0] || 
		        runtime_options.dbname[0])
		{
			log_err("You can't use connection parameters to the master when issuing a STANDBY PROMOTE command.\n");
			usage();
			ok = false;
		}
		if (runtime_options.dest_dir[0])
		{
			log_err("You don't need a destination directory for STANDBY PROMOTE command\n");
			usage();
			ok = false;
		}
		break;
	case STANDBY_FOLLOW:
		/*
		 * To make a standby follow a master we only need the repmgr.conf
		 * we don't want connection parameters to the new master
		 * because we will try to detect the master in repl_nodes
		 * if we can't find it then the follow action will be cancelled
		 */
		if (runtime_options.host[0] || runtime_options.masterport[0] || runtime_options.username[0] || 
		        runtime_options.dbname[0])
		{
			log_err("You can't use connection parameters to the master when issuing a STANDBY FOLLOW command.\n");
			usage();
			ok = false;
		}
		if (runtime_options.dest_dir[0])
		{
			log_err("You don't need a destination directory for STANDBY FOLLOW command\n");
			usage();
			ok = false;
		}
		break;
	case STANDBY_CLONE:
		/*
		 * To clone a master into a standby we need connection parameters
		 * repmgr.conf is useless because we don't have a server running
		 * in the standby
		 */
		if (runtime_options.config_file[0])
		{
			log_err("You need to use connection parameters to the master when issuing a STANDBY CLONE command.\n");
			usage();
			ok = false;
		}
		read_config_file = false;
		break;
	}

	return ok;
}
