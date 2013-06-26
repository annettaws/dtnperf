/********************************************************
 **  Authors: Michele Rodolfi, michele.rodolfi@studio.unibo.it
 **           Anna d'Amico, anna.damico@studio.unibo.it
 **           Carlo Caini (DTNperf_3 project supervisor), carlo.caini@unibo.it
 **
 **
 **  Copyright (c) 2013 Alma Mater Studiorum, University of Bologna
 ********************************************************/

/*
 * dtnperf_server.h
 */

#ifndef DTNPERF_SERVER_H_
#define DTNPERF_SERVER_H_

#include "../dtnperf_types.h"
#include <stddef.h>

void run_dtnperf_server(dtnperf_global_options_t * global_options );

// file expiration timer thread
void * file_expiration_timer(void * opt);

void print_server_usage(char* progname);
void parse_server_options(int argc, char ** argv, dtnperf_global_options_t * perf_g_opt);
al_bp_error_t prepare_server_ack_payload(dtnperf_server_ack_payload_t ack, char ** payload, size_t * payload_size);

// Ctrl+C handler
void server_handler(int signo);

void server_clean_exit(int status);

#endif /* DTNPERF_SERVER_H_ */
