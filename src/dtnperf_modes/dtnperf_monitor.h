/*
 * dtnperf_monitor.h
 *
 *  Created on: 10/lug/2012
 *      Author: michele
 */

#ifndef DTNPERF_MONITOR_H_
#define DTNPERF_MONITOR_H_

#include "../dtnperf_types.h"
#include <stdio.h>

typedef struct monitor_parameters
{
	dtnperf_global_options_t * perf_g_opt;
	boolean_t dedicated_monitor;
	int client_id;
} monitor_parameters_t;

typedef enum
{
	NONE,
	STATUS_REPORT,
	SERVER_ACK,
	CLIENT_STOP,
	CLIENT_FORCE_STOP
} bundle_type_t;

typedef struct session
{
	al_bp_endpoint_id_t client_eid;
	char * full_filename;
	FILE * file;
	struct timeval * start;
	u32_t last_bundle_time; // secs of bp creation timestamp
	u32_t expiration;
	int delivered_count;
	int total_to_receive;
	struct timeval * stop_arrival_time;
	u32_t wait_after_stop;
	struct session * next;
	struct session * prev;
}session_t;

typedef struct session_list
{
	session_t * first;
	session_t * last;
	int count;
}session_list_t;

session_list_t * session_list_create();
void session_list_destroy(session_list_t * list);

session_t * session_create(al_bp_endpoint_id_t client_eid, char * full_filename, FILE * file, struct timeval start,
		u32_t bundle_timestamp_secs, u32_t bundle_expiration_time);
void session_destroy(session_t * session);

void session_put(session_list_t * list, session_t * session);

session_t * session_get(session_list_t * list, al_bp_endpoint_id_t client);

void session_del(session_list_t * list, session_t * session);
void session_close(session_list_t * list, session_t * session);
void run_dtnperf_monitor(monitor_parameters_t * parameters);

//session expiration timer thread
void * session_expiration_timer(void * opt);

void print_monitor_usage(char* progname);
void parse_monitor_options(int argc, char ** argv, dtnperf_global_options_t * perf_g_opt);

void monitor_clean_exit(int status);
void monitor_handler(int signo);

#endif /* DTNPERF_MONITOR_H_ */
