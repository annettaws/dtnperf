/********************************************************
 **  Authors: Michele Rodolfi, michele.rodolfi@studio.unibo.it
 **           Anna d'Amico, anna.damico@studio.unibo.it
 **           Carlo Caini (DTNperf_3 project supervisor), carlo.caini@unibo.it
 **
 **
 **  Alma Mater Studiorum, University of Bologna
 ********************************************************/

/*
 * dtnperf_client.c
 */


#include "dtnperf_client.h"
#include "dtnperf_monitor.h"
#include "../includes.h"
#include "../definitions.h"
#include "../bundle_tools.h"
#include "../file_transfer_tools.h"
#include "../utils.h"
#include <semaphore.h>
#include <libgen.h>
#include <sys/stat.h>
#include <al_bp_api.h>


/* pthread_yield() is not standard,
   so use sched_yield if necessary */
#ifndef HAVE_PTHREAD_YIELD
#   ifdef HAVE_SCHED_YIELD
#       include <sched.h>
#       define pthread_yield    sched_yield
#   endif
#endif

// pthread variables
pthread_t sender;
pthread_t cong_ctrl;
pthread_t cong_expir_timer;
pthread_t wait_for_signal;
pthread_mutex_t mutexdata;
pthread_cond_t cond_ackreceiver;
sem_t window;			// semaphore for congestion control
int monitor_status;
int monitor_pid;

// variables of client threads
send_information_t * send_info;		// array info of sent bundles
long tot_bundles;					// for data mode
struct timeval start, end, now;			// time variables
struct timeval bundle_sent, ack_recvd;	// time variables
int sent_bundles = 0;					// sent bundles counter
unsigned int sent_data = 0;				// sent byte counter
int close_ack_receiver = 0;			// to signal the ack receiver to close
unsigned int data_written = 0;			// num of bytes written on the source file

boolean_t process_interrupted;
boolean_t expir_timer_cong_window;


FILE * log_file = NULL;
char source_file[256];					// complete name of source file: SOURCE_FILE_pid[_numBundle]
char * transfer_filename;			// basename of the file to transfer
u32_t transfer_filedim;				// size of the file to transfer
int transfer_fd;					// file descriptor

// flags to exit cleanly
boolean_t bp_handle_open;
boolean_t log_open;
boolean_t source_file_created;

boolean_t dedicated_monitor; // if client must start a dedicated monitor

// buffer settings
char* buffer = NULL;        	    // buffer containing data to be transmitted
size_t bufferLen;                   // lenght of buffer


// BP variables
al_bp_error_t error;
al_bp_handle_t handle;
al_bp_reg_id_t regid;
al_bp_reg_info_t reginfo;
al_bp_bundle_id_t * bundle_id;
al_bp_endpoint_id_t local_eid;
al_bp_endpoint_id_t dest_eid;
al_bp_endpoint_id_t mon_eid;
al_bp_bundle_object_t bundle;
char * file_bundle_names;
al_bp_bundle_object_t ack;

dtnperf_options_t * perf_opt;
dtnperf_connection_options_t * conn_opt;


/*  ----------------------------
 *          CLIENT CODE
 *  ---------------------------- */
void run_dtnperf_client(dtnperf_global_options_t * perf_g_opt)
{
	/* ------------------------
	 * variables
	 * ------------------------ */
	char * client_demux_string;
	int pthread_status;

	char temp1[256]; // buffer for various purpose
	char temp2[256];
//	FILE * stream; // stream for preparing payolad
	al_bp_bundle_object_t bundle_stop;
	monitor_parameters_t mon_params;


	/* ------------------------
	 * initialize variables
	 * ------------------------ */
	perf_opt = perf_g_opt->perf_opt;
	conn_opt = perf_g_opt->conn_opt;
	boolean_t debug = perf_opt->debug;
	int debug_level =  perf_opt->debug_level;
	boolean_t verbose = perf_opt->verbose;
	boolean_t create_log = perf_opt->create_log;
	log_open = FALSE;
	bp_handle_open = FALSE;
	source_file_created = FALSE;
	tot_bundles = 0;
	process_interrupted = FALSE;
	perf_opt->log_filename = correct_dirname(perf_opt->log_filename);

	// Create a new log file
	if (create_log)
	{
		if ((log_file = fopen(perf_opt->log_filename, "w")) == NULL)
		{
			fprintf(stderr, "fatal error opening log file\n");
			client_clean_exit(1);
		}
		log_open = TRUE;
	}

	// Connect to BP Daemon
	if ((debug) && (debug_level > 0))
		printf("[debug] opening connection to local BP daemon...");

	if (perf_opt->use_ip)
		error = al_bp_open_with_ip(perf_opt->ip_addr,perf_opt->ip_port,&handle);
	else
		error = al_bp_open(&handle);

	if (error != BP_SUCCESS)
	{
		fprintf(stderr, "fatal error opening BP handle: %s\n", al_bp_strerror(error));
		if (create_log)
			fprintf(log_file, "fatal error opening BP handle: %s\n", al_bp_strerror(error));
		client_clean_exit(1);
	}
	else
	{
		bp_handle_open = TRUE;
	}

	if ((debug) && (debug_level > 0))
		printf("done\n");

	// Ctrl+C handler
	signal(SIGINT, &client_handler);

	/* -----------------------------------------------------
	 *   initialize and parse bundle src/dest/replyto EIDs
	 * ----------------------------------------------------- */

	// append process id to the client demux string
	client_demux_string = malloc (strlen(CLI_EP_STRING) + 10);
	sprintf(client_demux_string, "%s_%d", CLI_EP_STRING, getpid());

	// parse SERVER EID
	//  if the scheme is not "ipn" append server demux string to destination eid
	if(strncmp(perf_opt->dest_eid,"ipn",3) != 0)
		strcat(perf_opt->dest_eid, SERV_EP_STRING);

	if (verbose)
		fprintf(stdout, "%s (local)\n", perf_opt->dest_eid);
	// parse
	error = al_bp_parse_eid_string(&dest_eid, perf_opt->dest_eid);

	if (error != BP_SUCCESS)
	{
		fprintf(stderr, "fatal error parsing BP EID: invalid eid string '%s'\n", perf_opt->dest_eid);
		if (create_log)
			fprintf(log_file, "\nfatal error parsing BP EID: invalid eid string '%s'", perf_opt->dest_eid);
		client_clean_exit(1);
	}

	if (debug)
		printf("Destination: %s\n", dest_eid.uri);

	if (create_log)
		fprintf(log_file, "Destination: %s\n", dest_eid.uri);

	//build a local EID
			if(debug && debug_level > 0)
				printf("[debug] building a local eid...");
			al_bp_build_local_eid(handle, &local_eid,client_demux_string,"Client",dest_eid.uri);
			if(debug && debug_level > 0)
				printf("done\n");
			if (debug)
				printf("Source     : %s\n", local_eid.uri);
			if (create_log)
				fprintf(log_file, "\nSource     : %s\n", local_eid.uri);

	// parse REPLY-TO (if not specified, the same as the source)
	if (strlen(perf_opt->mon_eid) == 0)
	{
		//if the scheme is not "ipn" copy from local EID only the URI (not the demux string)
		if(strncmp(dest_eid.uri,"ipn",3) != 0 || perf_opt->bp_implementation == BP_DTN){
			perf_opt->eid_format_forced = 'D';
			char * ptr;
			ptr = strstr(local_eid.uri, CLI_EP_STRING);
			// copy from local EID only the uri (not the demux string)
			strncpy(perf_opt->mon_eid, local_eid.uri, ptr - local_eid.uri);
		}
		else
		{
			perf_opt->eid_format_forced = 'I';
			char * ptr, * temp;
			temp = (char *) malloc(sizeof(char)*AL_BP_MAX_ENDPOINT_ID);
			strcpy(temp,local_eid.uri);
			ptr = strtok(temp , ".");
			sprintf(temp,"%s.%s",ptr,MON_EP_NUM_SERVICE);
			strncpy(perf_opt->mon_eid, temp, strlen(temp));
			free(temp);
		}

	}
	// if the scheme is not "ipn" append monitor demux string to reply-to EID
	if(strncmp(perf_opt->mon_eid,"ipn",3) != 0)
		strcat(perf_opt->mon_eid, MON_EP_STRING);

	// parse
	error = al_bp_parse_eid_string(&mon_eid, perf_opt->mon_eid);
	if (error != BP_SUCCESS)
	{
		fprintf(stderr, "fatal error parsing BP EID: invalid eid string '%s'\n", perf_opt->dest_eid);
		if (create_log)
			fprintf(log_file, "\nfatal error parsing BP EID: invalid eid string '%s'", perf_opt->dest_eid);
		client_clean_exit(1);
	}

	// checking if there is a running monitor on this endpoint
	if(perf_g_opt->mode == DTNPERF_CLIENT_MONITOR)
	{
		if(debug && debug_level > 0)
			printf("[debug] checking for existing monitor on this endpoint...\n");
		error = al_bp_find_registration(handle, &mon_eid, &regid);
		if ( (error == BP_SUCCESS && perf_opt->bp_implementation == BP_DTN)
					|| (perf_opt->bp_implementation == BP_ION && (error == BP_EBUSY || error == BP_EPARSEEID)))
		{
			dedicated_monitor = FALSE;
			printf("there is already a monitor on this endpoint.\n");
			printf("regid 0x%x\n", (unsigned int) regid);
		}
		else
		{
			dedicated_monitor = TRUE;
			mon_params.client_id = getpid();
			mon_params.perf_g_opt = perf_g_opt;
			printf("there is not a monitor on this endpoint.\n");
			// if the scheme is not "ipn", append monitor demux string to reply-to EID
			if(strncmp(perf_opt->mon_eid,"ipn",3) != 0)
				sprintf(temp1, "%s_%d", mon_eid.uri, mon_params.client_id);
			else
				sprintf(temp1, "%s", mon_eid.uri);
			al_bp_parse_eid_string(&mon_eid, temp1);
			// start dedicated monitor
			if ((monitor_pid = fork()) == 0)
			{
				start_dedicated_monitor((void *) &mon_params);
				exit(0);
			}
			printf("dedicated (internal) monitor started\n");
		}
		if ((debug) && (debug_level > 0))
			printf(" done\n");
	}

	if(dedicated_monitor == TRUE)
		strcpy(mon_eid.uri, temp1);

	if (debug)
		printf("Reply-to   : %s\n\n", mon_eid.uri);

	if (create_log)
		fprintf(log_file, "Reply-to   : %s\n\n", mon_eid.uri);

	if(create_log)
		fflush(log_file);

	//create a new registration to the local router based on this eid
	if(debug && debug_level > 0)
		printf("[debug] registering to local BP daemon...");
	memset(&reginfo, 0, sizeof(reginfo));
	al_bp_copy_eid(&reginfo.endpoint, &local_eid);
	reginfo.flags = BP_REG_DEFER;
	reginfo.regid = BP_REGID_NONE;
	reginfo.expiration = 0;
	error = al_bp_register(&handle, &reginfo, &regid);
	if ( (error != BP_SUCCESS && perf_opt->bp_implementation == BP_DTN)
			|| (perf_opt->bp_implementation == BP_ION && (error == BP_EBUSY || error == BP_EPARSEEID)))
	{
		fflush(stdout);
		fprintf(stderr, "error creating registration: %d (%s)\n",
				error, al_bp_strerror(al_bp_errno(handle)));
		if (create_log)
			fprintf(log_file, "error creating registration: %d (%s)\n",
					error, al_bp_strerror(al_bp_errno(handle)));
		client_clean_exit(1);
	}
	if ((debug) && (debug_level > 0))
		printf(" done\n");
	if (debug)
		printf("regid 0x%x\n", (unsigned int) regid);
	if (create_log)
		fprintf(log_file, "regid 0x%x\n", (unsigned int) regid);

	// if bundle payload > MAX_MEM_PAYLOAD, then transfer a file
	if (!perf_opt->use_file && perf_opt->bundle_payload > MAX_MEM_PAYLOAD)
	{
		perf_opt->use_file = 1;
		perf_opt->bundle_payload = BP_PAYLOAD_FILE;
		if (verbose)
			printf("Payload %f > %d: using file instead of memory\n", perf_opt->bundle_payload, MAX_MEM_PAYLOAD);
		if (create_log)
			fprintf(log_file, "Payload %f > %d: using file instead of memory\n", perf_opt->bundle_payload, MAX_MEM_PAYLOAD);
	}

	/* ------------------------------------------------------------------------------
	 * select the operative-mode (between Time_mode, Data_mode and File_mode)
	 * ------------------------------------------------------------------------------ */

	if (perf_opt->op_mode == 'T')	// Time mode
	{

		if (verbose)
			printf("Working in Time_mode\n");

		if (create_log)
			fprintf(log_file, "Working in Time_mode\n");

		if (verbose)
			printf("requested Tx lenght %d (s) \n", perf_opt->transmission_time);

		if (create_log)
			fprintf(log_file, "requested Tx lenght %d (s)\n", perf_opt->transmission_time);
	}
	else if (perf_opt->op_mode == 'D') // Data mode
	{
		if (verbose)
			printf("Working in Data_mode\n");

		if (create_log)
			fprintf(log_file, "Working in Data_mode\n");

		if (verbose)
			printf("requested Transmission of %f bytes of data\n", perf_opt->data_qty);

		if (create_log)
			fprintf(log_file, "requested Transmission of %f bytes of data\n", perf_opt->data_qty);
	}
	else if (perf_opt->op_mode == 'F') // File mode
	{
		if (verbose)
			printf("Working in File_mode\n");

		if (create_log)
			fprintf(log_file, "Working in File_mode\n");

		if (verbose)
			printf("requested transmission of file %s\n", perf_opt->F_arg);

		if (create_log)
			fprintf(log_file, "requested transmission of file %s\n", perf_opt->F_arg);

	}

	if (verbose)
		printf(" transmitting data %s\n", perf_opt->use_file ? "using a file" : "using memory");
	if (create_log)
		fprintf(log_file, " transmitting data %s\n", perf_opt->use_file ? "using a file" : "using memory");

	if (verbose)
		printf("%s based congestion control:\n", perf_opt->congestion_ctrl == 'w' ? "window" : "rate");

	if (create_log)
		fprintf(log_file, "%s based congestion control:\n", perf_opt->congestion_ctrl == 'w' ? "window" : "rate");
	if(perf_opt->congestion_ctrl == 'w')
	{
		if (verbose)
			printf("\twindow is %d bundle\n", perf_opt->window);
		if (create_log)
			fprintf(log_file, "\twindow is %d bundle\n", perf_opt->window);
	}
	else
	{
		if (verbose)
			printf("\trate is %f %c\n", perf_opt->rate, perf_opt->rate_unit);
		if (create_log)
			fprintf(log_file, "\trate is %f %c\n", perf_opt->rate, perf_opt->rate_unit);
	}
	if (verbose)
		printf("payload is %f byte\n", perf_opt->bundle_payload);
	if (create_log)
		fprintf(log_file, "payload is %f byte\n", perf_opt->bundle_payload);


	sent_bundles = 0;

	if (perf_opt->op_mode == 'D' || perf_opt->op_mode == 'F') // Data or File mode
	{
		if ((debug) && (debug_level > 0))
			printf("[debug] calculating how many bundles are needed...");

		if (perf_opt->op_mode == 'F') // File mode
		{
			struct stat file;
			if (stat(perf_opt->F_arg, &file) < 0)
			{
				fprintf(stderr, "couldn't stat file %s : %s", perf_opt->F_arg, strerror(errno));
				if (create_log)
					fprintf(log_file, "couldn't stat file %s : %s", perf_opt->F_arg, strerror(errno));
				client_clean_exit(1);
			}

			// get transfer file basename
			strcpy(temp1, perf_opt->F_arg);
			strcpy(temp2, basename(temp1));
			transfer_filename = malloc(strlen(temp2) + 1);
			strcpy(transfer_filename, temp2);

			transfer_filedim = file.st_size;
			tot_bundles += bundles_needed(transfer_filedim, get_file_fragment_size(perf_opt->bundle_payload, strlen(transfer_filename), strlen(perf_opt->mon_eid)));

			file_bundle_names = (char *) malloc(sizeof(char) * tot_bundles);
		}
		else // Data mode
			tot_bundles += bundles_needed(perf_opt->data_qty, perf_opt->bundle_payload);

		if ((debug) && (debug_level > 0))
			printf(" n_bundles = %ld\n", tot_bundles);

	}

	// Create the bundle object
	if ((debug) && (debug_level > 0))
		printf("[debug] creating the bundle object...");
	error = al_bp_bundle_create(& bundle);
	if (error != BP_SUCCESS)
	{
		fprintf(stderr, "ERROR: couldn't create bundle object\n");

		if (create_log)
			fprintf(log_file, "ERROR: couldn't create bundle object\n");
		client_clean_exit(1);
	}
	if ((debug) && (debug_level > 0))
		printf(" done\n");

	// Create the array for the bundle send info (only for sliding window congestion control)
	if (perf_opt->congestion_ctrl == 'w') {
		if ((debug) && (debug_level > 0))
			printf("[debug] creating structure for sending information...");

		send_info = (send_information_t*) malloc(perf_opt->window * sizeof(send_information_t));
		init_info(send_info, perf_opt->window);

		if ((debug) && (debug_level > 0))
			printf(" done\n");
	}

	// Open File Transfered
	if(perf_opt->op_mode == 'F') // File mode
	{
		// open file to transfer in read mode
		if ((transfer_fd = open(perf_opt->F_arg, O_RDONLY)) < 0)
		{
			fprintf(stderr, "couldn't stat file %s : %s", perf_opt->F_arg, strerror(errno));
			if (create_log)
				fprintf(log_file, "couldn't stat file %s : %s", perf_opt->F_arg, strerror(errno));
			client_clean_exit(2);
		}
	}


	// Setting the bundle options
	al_bp_bundle_set_source(&bundle, local_eid);
	al_bp_bundle_set_dest(&bundle, dest_eid);
	al_bp_bundle_set_replyto(&bundle, mon_eid);
	set_bp_options(&bundle, conn_opt);

	// intialize stop bundle;
	al_bp_bundle_create(&bundle_stop);

	if ((debug) && (debug_level > 0))
		printf("[debug] entering in loop\n");

	// Run threads
	if (perf_opt->congestion_ctrl == 'w') // sliding window congestion control
		sem_init(&window, 0, perf_opt->window);
	else								// rate based congestion control
		sem_init(&window, 0, 0);


	sigset_t sigset;

	// blocking signals for the threads
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGUSR1);
	sigaddset(&sigset, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);


	pthread_cond_init(&cond_ackreceiver, NULL);
	pthread_mutex_init (&mutexdata, NULL);

	pthread_create(&sender, NULL, send_bundles, (void*)perf_g_opt);
	pthread_create(&cong_ctrl, NULL, congestion_control, (void*)perf_g_opt);
	pthread_create(&wait_for_signal, NULL, wait_for_sigint, (void*) client_demux_string);

	pthread_join(cong_ctrl, (void**)&pthread_status);
	pthread_join(sender, (void**)&pthread_status);

	pthread_mutex_destroy(&mutexdata);
	sem_destroy(&window);
	pthread_cond_destroy(&cond_ackreceiver);

	// if user sent Ctrl+C to the client,
	// let the wait_for_signal thread to terminate the execution
	if (process_interrupted)
		pause();

	if ((debug) && (debug_level > 0))
		printf("[debug] out from loop\n");

	// Get the TOTAL end time
	if ((debug) && (debug_level > 0))
		printf("[debug] getting total end-time...");

	gettimeofday(&end, NULL);

	if ((debug) && (debug_level > 0))
		printf(" end.tv_sec = %u s\n", (u_int)end.tv_sec);

	// Print final report
	print_final_report(NULL);
	if(perf_opt->create_log)
		print_final_report(log_file);

	// fill the stop bundle
	prepare_stop_bundle(&bundle_stop, mon_eid, conn_opt->expiration, conn_opt->priority, sent_bundles);
	al_bp_bundle_set_source(&bundle_stop, local_eid);

	// send stop bundle to monitor
	if (debug)
		printf("sending the stop bundle to the monitor...");
	if ((error = al_bp_bundle_send(handle, regid, &bundle_stop)) != 0)
	{
		fprintf(stderr, "error sending the stop bundle: %d (%s)\n", error, al_bp_strerror(error));
		if (create_log)
			fprintf(log_file, "error sending the stop bundle: %d (%s)\n", error, al_bp_strerror(error));
		client_clean_exit(1);
	}
	if (debug)
		printf("done.\n");

	// waiting monitor stops
	if (dedicated_monitor)
	{
		printf("\nWaiting for dedicated monitor to stop...\n");
		wait(&monitor_status);
	}

	// Close the BP handle --
	if ((debug) && (debug_level > 0))
		printf("[debug] closing DTN handle...");

	if (al_bp_close(handle) != BP_SUCCESS)
	{
		fprintf(stderr, "fatal error closing bp handle: %s\n", strerror(errno));
		if (create_log)
			fprintf(log_file, "fatal error closing bp handle: %s\n", strerror(errno));
		client_clean_exit(1);
	}
	else
	{
		bp_handle_open = FALSE;
	}

	// Unregister Local Eid only For ION
	if(perf_opt->bp_implementation == BP_ION)
	{
		if (al_bp_unregister(handle,regid,local_eid) != BP_SUCCESS)
		{
			fprintf(stderr, "fatal error unregister endpoint: %s\n", strerror(errno));
			if (create_log)
				fprintf(log_file, "fatal error unregister endpoint: %s\n", strerror(errno));
				client_clean_exit(1);
		}
		else
		{
			//bp_local_eid_register = FALSE;
			bp_handle_open = FALSE;
		}
	}
	if ((debug) && (debug_level > 0))
		printf(" done\n");

	if (create_log)
	{
		fclose(log_file);
		log_open = FALSE;
	}
	// deallocate memory
	if (perf_opt->op_mode == 'F')
	{
		close(transfer_fd);
	}
	if (perf_opt->use_file)
	{
	//	remove(source_file);
		source_file_created = FALSE;

		if (debug && debug > 1)
		{
			printf("[debug] removed file %s\n", source_file);
		}
	}

	// free resource
	free((void*)buffer);
	free(client_demux_string);
	free(transfer_filename);
	free(send_info);
	if(perf_opt->op_mode == 'F')
	{
		int i;
		al_bp_bundle_payload_t tmp_payload;
		tmp_payload.location = bundle.payload->location;
		// the last one is the last bundle sent
		for (i = 0; i< ( tot_bundles -1) ; i++ )
		{
			tmp_payload.filename.filename_len = strlen(&file_bundle_names[i]);
			tmp_payload.filename.filename_val = &file_bundle_names[i];
			al_bp_free_payload(&tmp_payload);
		}
		//free(file_bundle_names);
		printf("\nOK-OK\n");
	}
	//structure bundle is always free in every op mode
	al_bp_bundle_free(&bundle);
	printf("\nOK-OK-OK\n");
	al_bp_bundle_free(&bundle_stop);

	if (perf_opt->create_log)
		printf("\nClient log saved: %s\n", perf_opt->log_filename);

	printf("\n");

	exit(0);
}
// end client code

/**
 * Create and Fill Buffer File Payload
 **/
void create_fill_payload_buf(boolean_t debug, int debug_level, boolean_t create_log,
						int num_bundle){
	FILE * stream;
	boolean_t eof_reached;

	if(perf_opt->op_mode == 'F')// File mode
		sprintf(source_file, "%s_%d_%d", SOURCE_FILE, getpid(),num_bundle);
	else 						// Time and Data mode
		sprintf(source_file, "%s_%d", SOURCE_FILE, getpid());

	// Create the file
	if (perf_opt->use_file)
	{
		// create the file
		if ((debug) && (debug_level > 0))
			printf("[debug] creating file %s...", source_file);

		stream = fopen(source_file,	"wb");

		if (stream == NULL)
		{
			fprintf(stderr, "ERROR: couldn't create file %s.\n \b Maybe you have not permissions\n", source_file);

			if (create_log)
				fprintf(log_file, "ERROR: couldn't create file %s.\n \b Maybe you have not permissions\n", source_file);

			client_clean_exit(2);
		}

		source_file_created = TRUE;

		fclose(stream);

		if ((debug) && (debug_level > 0))
			printf(" done\n");
	}

	// Fill the payload
	if ((debug) && (debug_level > 0))
		printf("[debug] filling payload...");

	if (perf_opt->use_file)
		error = al_bp_bundle_set_payload_file(&bundle, source_file, strlen(source_file));
	else
		error = al_bp_bundle_set_payload_mem(&bundle, buffer, bufferLen);
	if (error != BP_SUCCESS)
	{
		fprintf(stderr, "ERROR: couldn't set bundle payload\n");

		if (create_log)
			fprintf(log_file, "ERROR: couldn't set bundle payload\n");
		client_clean_exit(1);
	}
	if ((debug) && (debug_level > 0))
		printf(" done\n");

	// open payload stream in write mode
	if (open_payload_stream_write(bundle, &stream) < 0)
	{
		fprintf(stderr, "ERROR: couldn't open payload stream in write mode\n");

		if (create_log)
			fprintf(log_file, "ERROR: couldn't open payload stream in write mode\n");

		client_clean_exit(2);
	}

	// prepare the payload
	if(perf_opt->op_mode == 'F') // File mode
	{
		open_payload_stream_write(bundle, &stream);
		error = prepare_file_transfer_payload(perf_opt, stream, transfer_fd,
				transfer_filename, transfer_filedim, conn_opt->expiration , &eof_reached);
		if(error != BP_SUCCESS)
		{
			fprintf(stderr, "error preparing file transfer payload\n");
			if (create_log)
				fprintf(log_file, "error preparing file transfer payload");
			client_clean_exit(2);
		}
	}
	else // Time and Data mode
	{
		error = prepare_generic_payload(perf_opt, stream);
		if (error != BP_SUCCESS)
		{
			fprintf(stderr, "error preparing payload: %s\n", al_bp_strerror(error));
			if (create_log)
				fprintf(log_file, "error preparing payload: %s\n", al_bp_strerror(error));
			client_clean_exit(1);
		}
	}

	// close the stream
	close_payload_stream_write(&bundle, stream);

	if(debug)
		printf("[debug] payload prepared\n");

} // end create_fill_payload_buf

/**
 * Client Threads code
 **/
void * send_bundles(void * opt)
{
	dtnperf_options_t *perf_opt = ((dtnperf_global_options_t *)(opt))->perf_opt;
	boolean_t debug = perf_opt->debug;
	int debug_level = perf_opt->debug_level;
	boolean_t create_log = perf_opt->create_log;
	boolean_t condition;
	u32_t actual_payload;

	// Initialize timer
	if ((debug) && (debug_level > 0))
		printf("[debug send thread] initializing timer...");
	if (create_log)
		fprintf(log_file, " initializing timer...");

	gettimeofday(&start, NULL);

	if ((debug) && (debug_level > 0))
		printf(" start.tv_sec = %d s\n", (u_int)start.tv_sec);
	if (create_log)
		fprintf(log_file, " start.tv_sec = %d s\n", (u_int)start.tv_sec);

	if (perf_opt->op_mode == 'T')		// TIME MODE
	{									// Calculate end-time
		if ((debug) && (debug_level > 0))
			printf("[debug send thread] calculating end-time...");

		if (create_log)
			fprintf(log_file, " calculating end-time...");

		end = set (0);
		end.tv_sec = start.tv_sec + perf_opt->transmission_time;

		if ((debug) && (debug_level > 0))
			printf(" end.tv_sec = %d s\n", (u_int)end.tv_sec);

		if (create_log)
			fprintf(log_file, " end.tv_sec = %d s\n", (u_int)end.tv_sec);
	}

	if ((debug) && (debug_level > 0))
		printf("[debug send thread] entering loop...\n");
	if (create_log)
		fprintf(log_file, " entering loop...\n");

	if (perf_opt->op_mode == 'T') 	// TIME MODE
	{								// init variables for loop and setting condition
		now.tv_sec = start.tv_sec;
		condition = now.tv_sec <= end.tv_sec;
	}
	else							// DATA and FILE MODE
	{								// setting condition for loop
		condition = sent_bundles < tot_bundles;
	}

	//Only for DATA e TIME MODE is the payload is the same for all bundle
	if (perf_opt->op_mode == 'T' || perf_opt->op_mode == 'D')
	{
		create_fill_payload_buf(debug, debug_level, create_log, 0);
	}
	else //For FILE MODE created all the payload necessary
	{
		int i=0;
		for (i=0; i<tot_bundles; i++){
			create_fill_payload_buf(debug, debug_level, create_log, i);
		}
	}

	// send bundles loop
	while (condition)				//LOOP
	{
		// Set Payload FILE MODE
		// Add in bundles_file_array the bundle to send for free at the end every bundle
		if (perf_opt->op_mode == 'F')
		{
			sprintf(source_file, "%s_%d_%d", SOURCE_FILE, getpid(),sent_bundles);
			if (perf_opt->use_file)
				error = al_bp_bundle_set_payload_file(&bundle, source_file, strlen(source_file));
			else
				error = al_bp_bundle_set_payload_mem(&bundle, buffer, bufferLen);
			// memorized source_file
			file_bundle_names[sent_bundles] = (char *) malloc(sizeof(char) * bundle.payload->filename.filename_len);
			strcpy(&file_bundle_names[sent_bundles], bundle.payload->filename.filename_val);
		}
		// window debug
		if ((debug) && (debug_level > 1))
		{
			int cur;
			sem_getvalue(&window, &cur);
			printf("\t[debug send thread] window is %d\n", cur);
		}
		// wait for the semaphore
		sem_wait(&window);
		if (perf_opt->op_mode == 'T')	// TIME MODE
		{								// update time and condition
			gettimeofday(&now, NULL);
			condition = now.tv_sec <= end.tv_sec;
		}
		if(!condition)
			break;

		// Send the bundle
		if (debug)
			printf("sending the bundle...");

		if (perf_opt->congestion_ctrl == 'w')
			pthread_mutex_lock(&mutexdata);

		if ((error = al_bp_bundle_send(handle, regid, &bundle)) != 0)
		{
			fprintf(stderr, "error sending bundle: %d (%s)\n", error, al_bp_strerror(error));
			if (create_log)
				fprintf(log_file, "error sending bundle: %d (%s)\n", error, al_bp_strerror(error));
			client_clean_exit(1);
		}

		if ((error = al_bp_bundle_get_id(bundle, &bundle_id)) != 0)
		{
			fprintf(stderr, "error getting bundle id: %s\n", al_bp_strerror(error));
			if (create_log)
				fprintf(log_file, "error getting bundle id: %s\n", al_bp_strerror(error));
			client_clean_exit(1);
		}
		if (debug)
			printf(" bundle sent\n");
		if ((debug) && (debug_level > 0))
			printf("\t[debug send thread] ");

		printf("bundle sent timestamp: %llu.%llu\n", (unsigned long long) bundle_id->creation_ts.secs, (unsigned long long) bundle_id->creation_ts.seqno);
		if (create_log)
			fprintf(log_file, "\t bundle sent timestamp: %llu.%llu\n", (unsigned long long) bundle_id->creation_ts.secs, (unsigned long long) bundle_id->creation_ts.seqno);

		// put bundle id in send_info (only windowed congestion control)
		if (perf_opt->congestion_ctrl == 'w') {
			gettimeofday(&bundle_sent, NULL);
			add_info(send_info, *bundle_id, bundle_sent, perf_opt->window);
			if ((debug) && (debug_level > 0))
				printf("\t[debug send thread] added info for sent bundle\n");
			pthread_cond_signal(&cond_ackreceiver);
			pthread_mutex_unlock(&mutexdata);
		}

		// Increment sent_bundles
		++sent_bundles;
		if ((debug) && (debug_level > 0))
			printf("\t[debug send thread] now bundles_sent is %d\n", sent_bundles);
		if (create_log)
			fprintf(log_file, "\t now bundles_sent is %d\n", sent_bundles);
		// Increment data_qty
		al_bp_bundle_get_payload_size(bundle, &actual_payload);
		sent_data += actual_payload;
		if (perf_opt->op_mode == 'T')	// TIME MODE
		{								// update time and condition
			gettimeofday(&now, NULL);
			condition = now.tv_sec <= end.tv_sec;
		}
		else							// DATA MODE
		{								// update condition
			condition = sent_bundles < tot_bundles;
		}

	} // while
	if ((debug) && (debug_level > 0))
		printf("[debug send thread] ...out from loop\n");
	if (create_log)
		fprintf(log_file, " ...out from loop\n");

	pthread_mutex_lock(&mutexdata);
	close_ack_receiver = 1;
	if (perf_opt->congestion_ctrl == 'r')
	{
		// terminate congestion control thread
		pthread_cancel(cong_ctrl);
	}
	else
	{
		pthread_cond_signal(&cond_ackreceiver);
	}
	pthread_mutex_unlock(&mutexdata);
	// close thread
	pthread_exit(NULL);

} // end send_bundles

void * congestion_control(void * opt)
{
	dtnperf_options_t *perf_opt = ((dtnperf_global_options_t *)(opt))->perf_opt;
	boolean_t debug = perf_opt->debug;
	int debug_level = perf_opt->debug_level;
	boolean_t create_log = perf_opt->create_log;

	al_bp_timestamp_t reported_timestamp;
	al_bp_endpoint_id_t ack_sender;
	HEADER_TYPE ack_header;
	al_bp_copy_eid(&ack_sender, &dest_eid);
	struct timeval temp;

	int position = -1;

	if (debug && debug_level > 0)
		printf("[debug cong ctrl] congestion control = %c\n", perf_opt->congestion_ctrl);

	pthread_sleep(0.5);

	if (perf_opt->congestion_ctrl == 'w') // window based congestion control
	{
		al_bp_bundle_create(&ack);
	//	gettimeofday(&temp, NULL);
		pthread_create(&cong_expir_timer, NULL, congestion_window_expiration_timer, NULL);
		while ((close_ack_receiver == 0) || (gettimeofday(&temp, NULL) == 0 && ack_recvd.tv_sec - temp.tv_sec <= perf_opt->wait_before_exit))
		{
			// if there are no bundles without ack, wait
			pthread_mutex_lock(&mutexdata);
			if (close_ack_receiver == 0 && count_info(send_info, perf_opt->window) == 0)
			{
				pthread_cond_wait(&cond_ackreceiver, &mutexdata);
				pthread_mutex_unlock(&mutexdata);
				// pthread_yield();
				sched_yield();
				continue;
			}
			// Wait for the reply
			if ((debug) && (debug_level > 0))
				printf("\t[debug cong ctrl] waiting for the reply...\n");

			if ((error = al_bp_bundle_receive(handle, ack, BP_PAYLOAD_MEM, count_info(send_info, perf_opt->window) == 0 ? perf_opt->wait_before_exit : -1)) != BP_SUCCESS)
			{
				if(count_info(send_info, perf_opt->window) == 0 && close_ack_receiver == 1)
					// send_bundles is terminated
					break;
				fprintf(stderr, "error getting server ack: %d (%s)\n", error, al_bp_strerror(al_bp_errno(handle)));
				if (create_log)
					fprintf(log_file, "error getting server ack: %d (%s)\n", error, al_bp_strerror(al_bp_errno(handle)));
				client_clean_exit(1);
			}
			// Check if is actually a server ack bundle
			get_bundle_header_and_options(&ack, &ack_header, NULL);
			if (ack_header != DSA_HEADER)
			{
				fprintf(stderr, "error getting server ack: wrong bundle header\n");
				if (create_log)
					fprintf(log_file, "error getting server ack: wrong bundle header\n");

				pthread_mutex_unlock(&mutexdata);
				//pthread_yield();
				sched_yield();
				continue;
			}

			gettimeofday(&ack_recvd, NULL);
			if ((debug) && (debug_level > 0))
				printf("\t[debug cong ctrl] ack received\n");

			// Get ack infos
			error = get_info_from_ack(&ack, NULL, &reported_timestamp);
			if (error != BP_SUCCESS)
			{
				fprintf(stderr, "error getting info from ack: %s\n", al_bp_strerror(error));
				if (create_log)
					fprintf(log_file, "error getting info from ack: %s\n", al_bp_strerror(error));
				client_clean_exit(1);
			}
			if ((debug) && (debug_level > 0))
				printf("\t[debug cong ctrl] ack received timestamp: %lu %lu\n", reported_timestamp.secs, reported_timestamp.seqno);
			position = is_in_info(send_info, reported_timestamp, perf_opt->window);
			if (position < 0)
			{
				fprintf(stderr, "error removing bundle info\n");
				if (create_log)
					fprintf(log_file, "error removing bundle info\n");
				client_clean_exit(1);
			}
			remove_from_info(send_info, position);
			if ((debug) && (debug_level > 0))
				printf("\t[debug cong ctrl] ack validated\n");
			sem_post(&window);
			if ((debug) && (debug_level > 1))
			{
				int cur;
				sem_getvalue(&window, &cur);
				printf("\t[debug cong ctrl] window is %d\n", cur);
			}

			pthread_mutex_unlock(&mutexdata);
			//pthread_yield();
			sched_yield();
		} // end while(n_bundles)

		al_bp_bundle_free(&ack);
	}
	else if (perf_opt->congestion_ctrl == 'r') // Rate based congestion control
	{
		double interval_secs;

		if (perf_opt->rate_unit == 'b') // rate is bundles per second
		{
			interval_secs = 1.0 / perf_opt->rate;
		}
		else 							// rate is bit or kbit per second
		{
			if (perf_opt->rate_unit == 'k') // Rate is kbit per second
			{
				perf_opt->rate = kilo2byte(perf_opt->rate);
			}
			else // rate is Mbit per second
			{
				perf_opt->rate = mega2byte(perf_opt->rate);
			}
			interval_secs = (double)perf_opt->bundle_payload * 8 / perf_opt->rate;
		}

		if (debug)
			printf("[debug cong ctrl] wait time for each bundle: %.4f s\n", interval_secs);

		pthread_mutex_lock(&mutexdata);
		while(close_ack_receiver == 0)
		{
			pthread_mutex_unlock(&mutexdata);
			sem_post(&window);
			//pthread_yield();
			sched_yield();
			if (debug && debug_level > 0)
				printf("[debug cong ctrl] increased window size\n");
			pthread_sleep(interval_secs);
			pthread_mutex_lock(&mutexdata);
		}
	}
	else // wrong char for congestion control
	{
		client_clean_exit(1);
	}

	pthread_exit(NULL);
	return NULL;
} // end congestion_control

void * congestion_window_expiration_timer(void * opt)
{
	struct timeval current_time;
	al_bp_timeval_t expiration = perf_opt->bundle_ack_options.ack_expiration + conn_opt->expiration;
	expir_timer_cong_window = FALSE;

	gettimeofday(&current_time, NULL);
	if(ack_recvd.tv_sec == 0)
		ack_recvd.tv_sec = current_time.tv_sec;

	while(1)
	{
		gettimeofday(&current_time, NULL);
		if( current_time.tv_sec - ack_recvd.tv_sec >=  expiration)
		{
			expir_timer_cong_window = TRUE;
			printf("\nExpiration timer congestion window\n");
			kill(getpid(), SIGINT);
		}
		sched_yield();
	}

	pthread_exit(NULL);
	return NULL;
} // end congestion_window_expiration_timer

void * start_dedicated_monitor(void * params)
{
	monitor_parameters_t * parameters = (monitor_parameters_t *) params;
	parameters->dedicated_monitor = TRUE;
	run_dtnperf_monitor(parameters);
	pthread_exit(NULL);
	return NULL;
} // end start_dedicated_monitor

void * wait_for_sigint(void * arg)
{
	sigset_t sigset;
	int signo;
	al_bp_handle_t force_stop_handle;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	pthread_sigmask(SIG_UNBLOCK, &sigset, NULL);
	sigwait(&sigset, &signo);

	if(expir_timer_cong_window)
	{
		printf("\nDTNperf client: Expired Timer to receive the Server's Ack\n");
		if (perf_opt->create_log)
			fprintf(log_file, "\nDTNperf client: Expired Timer to receive the Server's Ack\n");
	}
	else
	{
		printf("\nDTNperf client received SIGINT: Exiting\n");
		if (perf_opt->create_log)
			fprintf(log_file, "\nDTNperf client received SIGINT: Exiting\n");
	}

	// send a signal to the monitor to terminate it
	if (dedicated_monitor)
	{
		kill(monitor_pid, SIGUSR1);

		// wait for monitor to terminate
		wait(&monitor_status);
	}
	else
	{

		al_bp_bundle_object_t bundle_force_stop;

		// Open a new connection to BP Daemon
		if ((perf_opt->debug) && (perf_opt->debug_level > 0))
			printf("[debug] opening a new connection to local BP daemon...");

		if(perf_opt->bp_implementation == BP_DTN)
		{
			if (perf_opt->use_ip)
				error = al_bp_open_with_ip(perf_opt->ip_addr,perf_opt->ip_port,&force_stop_handle);
			else
				error = al_bp_open(&force_stop_handle);
		}

		if (error != BP_SUCCESS)
		{
			fprintf(stderr, "fatal error opening a new bp handle: %s\n", al_bp_strerror(error));
			if (perf_opt->create_log)
				fprintf(log_file, "fatal error opening a new bp handle: %s\n", al_bp_strerror(error));
			client_clean_exit(1);
		}
		if ((perf_opt->debug) && (perf_opt->debug_level > 0))
			printf("done\n");

		// create the bundle force stop
		al_bp_bundle_create(&bundle_force_stop);

		// fill the force stop bundle
		prepare_force_stop_bundle(&bundle_force_stop, mon_eid, conn_opt->expiration, conn_opt->priority);
		al_bp_bundle_set_source(&bundle_force_stop, local_eid);

		// send force_stop bundle to monitor
		printf("Sending the force stop bundle to the monitor...");
		if(perf_opt->bp_implementation == BP_DTN)
			error = al_bp_bundle_send(force_stop_handle, regid, &bundle_force_stop);
		else if(perf_opt->bp_implementation == BP_ION)
			error = al_bp_bundle_send(handle, regid, &bundle_force_stop);
		if ((error) != 0)
		{
			fprintf(stderr, "error sending the force stop bundle: %d (%s)\n", error, al_bp_strerror(error));
			if (perf_opt->create_log)
				fprintf(log_file, "error sending the force stop bundle: %d (%s)\n", error, al_bp_strerror(error));
			al_bp_close(force_stop_handle);
			exit(1);
		}
		printf("done.\n");


		al_bp_bundle_free(&bundle_force_stop);
	}

	process_interrupted = TRUE;

	// terminate all child threads
	pthread_cancel(sender);
	pthread_cancel(cong_ctrl);
	pthread_cancel(cong_expir_timer);

	client_clean_exit(0);


	return NULL;
} // end wait_for_sigint

void print_final_report(FILE * f)
{
	double goodput, sent = 0;
	struct timeval total;
	double total_secs;
	char * gput_unit, * sent_unit;
	if (f == NULL)
		f = stdout;
	timersub(&end, &start, &total);
	total_secs = (((double)total.tv_sec * 1000 *1000) + (double)total.tv_usec) / (1000 * 1000);

	if (sent_data / (1000 * 1000) >= 1)
	{
		sent = (double) sent_data / (1000 * 1000);
		sent_unit = "Mbyte";
	}
	else if (sent_data / 1000 >= 1)
	{
		sent = (double) sent_data / 1000;
		sent_unit = "Kbyte";
	}
	else
		sent_unit = "byte";

	goodput = sent_data * 8 / total_secs;
	if (goodput / (1000 * 1000) >= 1)
	{
		goodput /= 1000 * 1000;
		gput_unit = "Mbit/s";
	}
	else if (goodput / 1000 >= 1)
	{
		goodput /= 1000;
		gput_unit = "Kbit/s";
	}
	else
		gput_unit = "bit/s";

	fprintf(f, "\nBundles sent = %d , total data sent = %.3f %s\n", sent_bundles, sent, sent_unit);
	fprintf(f, "Total execution time = %.1f\n", total_secs);
	if(perf_opt->congestion_ctrl == 'w')
		fprintf(f, "Goodput = %.3f %s\n", goodput, gput_unit);
	else
		fprintf(f, "Throughput = %.3f %s\n", goodput, gput_unit);
} // end print_final_report

void print_client_usage(char* progname)
{
	fprintf(stderr, "\n");
	fprintf(stderr, "dtnperf client mode\n");
	fprintf(stderr, "SYNTAX: %s %s -d <dest_eid> <[-T <s> | -D <num> | -F <filename>]> [-W <size> | -R <rate>] [options]\n", progname, CLIENT_STRING);
	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n"
			" -d, --destination <eid>     Destination EID (i.e. server EID)(required).\n"
			" -T, --time <seconds>        Time-mode: seconds of transmission.\n"
			" -D, --data <num[B|k|M]>     Data-mode: amount data to transmit; B = Byte, k = kByte, M = MByte. Default 'M' (MB). Note: following the SI and the IEEE standards 1 MB=10^6 byte\n"
			" -F, --file <filename>       File-mode: file to transfer\n"
			" -W, --window <size>         Window-based congestion control: size of DTNperf transmission window, i.e. max number of bundles \"in flight\" (not still ACKed by a server ack); default: 1.\n"
			" -R, --rate <rate[k|M|b]>    Rate-based congestion control: Tx rate. k = kbit/s, M = Mbit/s, b = bundle/s. Default is kb/s\n"
			" -m, --monitor <eid>         External monitor EID (without this option an internal dedicated monitor is started).\n"
			" -C, --custody               Request of custody transfer (and of \"custody accepted\" status reports as well).\n"
			" -f, --forwarded             Request of \"forwarded\" status reports.\n"
			" -r, --received              Request of \"received\" status reports.\n"
			"     --del                   Request of \"deleted\" stautus reports.\n"
			" -N, --nofragment            Disable bundle fragmentation.\n"
			" -P, --payload <size[B|k|M]> Bundle payload size; B = Byte, k = kByte, M = MByte. Default= 'k' (kB). Note: following the SI and the IEEE standards 1 MB=10^6 bytes.\n"
			"                             Min payload size is %d bytes in TIME and DATA mode. In FILE mode it depends on filename length.\n"
			" -M, --memory                Store the bundle into memory instead of file (if payload < 50KB).\n"
			" -L, --log[=log_filename]    Create a log file. Default log filename is %s.\n"
			"     --log-dir <dir>         Directory where the client (and the internal monitor) saves the log file. Default is %s\n"
			"     --ip-addr <addr>        IP address of the BP daemon api. Default is 127.0.0.1 (DTN2 only).\n"
			"     --ip-port <port>        IP port of the BP daemon api. Default is 5010 (DTN2 only).\n"
			"     --debug[=level]         Debug messages [1-2]; if level is not indicated level = 2.\n"
			" -l, --lifetime <time>       Bundle lifetime (s). Default is 60 s.\n"
			" -p, --priority <val>        Bundle  priority [bulk|normal|expedited|reserved]. Default is normal.\n"
			"     --ack-to-mon            Force server to send ACKs to the monitor (indipendently of server settings. DTN2 only.\n"
			"     --no-ack-to-mon         Force server not to send  ACKs to the monitor (DTN2 only).\n"
			"     --ack-lifetime <time>   ACK lifetime (desired value asked to the server). Default is the same as bundle lifetime\n"
            "     --ack-priority <val>    ACK priority (desired value asked to the server) [bulk|normal|expedited|reserved]. Default is the same as bundle priority\n"
			"     --ordinal <num>         Ordinal number [0-254]. Default: 0 (ION Only).\n"
			"     --unreliable            Set unreliable value to True. Default: False (ION Only).\n"
			"     --critical              Set critical value to True. Default: False (ION Only).\n"
			"     --flow <num>      Flow label number. Default: 0 (ION Only).\n"
			" -v, --verbose               Print some information messages during execution.\n"
			" -h, --help                  This help.\n",
			(int) (HEADER_SIZE + BUNDLE_OPT_SIZE), LOG_FILENAME, LOGS_DIR_DEFAULT);
	fprintf(stderr, "\n");
	exit(1);
} // end print_client_usage

void parse_client_options(int argc, char ** argv, dtnperf_global_options_t * perf_g_opt)
{
	char c, done = 0;
	dtnperf_options_t * perf_opt = perf_g_opt->perf_opt;
	dtnperf_connection_options_t * conn_opt = perf_g_opt->conn_opt;
	boolean_t w = FALSE, r = FALSE;
	boolean_t set_ack_priority_as_bundle = TRUE;
	boolean_t set_ack_expiration_as_bundle = TRUE;

	while (!done)
	{
		static struct option long_options[] =
		{
				{"help", no_argument, 0, 'h'},
				{"verbose", no_argument, 0, 'v'},
				{"memory", no_argument, 0, 'M'},
				{"custody", no_argument, 0, 'C'},
				{"window", required_argument, 0, 'W'},
				{"destination", required_argument, 0, 'd'},
				{"monitor", required_argument, 0, 'm'},
				{"exitinterval", required_argument, 0, 'i'},			// interval before exit
				{"time", required_argument, 0, 'T'},			// time mode
				{"data", required_argument, 0, 'D'},			// data mode
				{"file", required_argument, 0, 'F'},			// file mode
				{"payload", required_argument, 0, 'P'},
				{"lifetime", required_argument, 0, 'l'},
				{"rate", required_argument, 0, 'R'},
				{"debug", optional_argument, 0, 33}, 				// 33 because D is for data mode
				{"priority", required_argument, 0, 'p'},
				{"nofragment", no_argument, 0, 'N'},
				{"received", no_argument, 0, 'r'},
				{"forwarded", no_argument, 0, 'f'},
				{"del", no_argument,0,48},					//request of bundle status deleted report
				{"log", optional_argument, 0, 'L'},				// create log file
				{"ldir", required_argument, 0, 40},
				{"ip-addr", required_argument, 0, 37},
				{"ip-port", required_argument, 0, 38},
				{"ack-to-mon", no_argument, 0, 44},			// force server to send acks to monitor
				{"no-ack-to-mon", no_argument, 0, 45},		// force server to NOT send acks to monitor
				{"ack-lifetime", required_argument, 0, 46}	,			// set server ack expiration equal to client bundles
				{"ack-priority", required_argument, 0, 47},	// set server ack priority as indicated or equal to client bundles
				{"ordinal", required_argument, 0, 52},
				{"unreliable", no_argument, 0, 53},
				{"critical", no_argument, 0, 54},
				{"flow", required_argument, 0, 55},
				{0,0,0,0}	// The last element of the array has to be filled with zeros.

		};

		int option_index = 0;
		c = getopt_long(argc, argv, "hvMCW:d:m:i:T:D:F:P:l:R:p:NrfL::", long_options, &option_index);

		switch (c)
		{
		case 'h':
			print_client_usage(argv[0]);
			exit(0);
			return ;

		case 'v':
			perf_opt->verbose = TRUE;
			break;

		case 'M':
			perf_opt->use_file = 0;
			perf_opt->payload_type = BP_PAYLOAD_MEM;
			break;

		case 'C':
			conn_opt->custody_transfer = 1;
			conn_opt->custody_receipts = 1;
			break;

		case 'W':
			perf_opt->congestion_ctrl = 'w';
			perf_opt->window = atoi(optarg);
			w = TRUE;
			break;

		case 'd':
			strncpy(perf_opt->dest_eid, optarg, AL_BP_MAX_ENDPOINT_ID);
			break;

		case 'm':
			strncpy(perf_opt->mon_eid, optarg, AL_BP_MAX_ENDPOINT_ID);
			break;

		case 'i':
			perf_opt->wait_before_exit = atoi(optarg)*1000;
			break;

		case 'T':
			perf_opt->op_mode = 'T';
			perf_opt->transmission_time = atoi(optarg);
			break;

		case 'D':
			perf_opt->op_mode = 'D';
			perf_opt->D_arg = strdup(optarg);
			perf_opt->data_unit = find_data_unit(perf_opt->D_arg);

			switch (perf_opt->data_unit)
			{
			case 'B':
				perf_opt->data_qty = atof(perf_opt->D_arg);
				break;
			case 'k':
				perf_opt->data_qty = kilo2byte(atof(perf_opt->D_arg));
				break;
			case 'M':
				perf_opt->data_qty = mega2byte(atof(perf_opt->D_arg));
				break;
			default:
				printf("\nERROR: (-D option) invalid data unit\n");
				exit(1);
//				printf("\nWARNING: (-D option) invalid data unit, assuming 'M' (MBytes)\n\n");
//				perf_opt->data_qty = mega2byte(atof(perf_opt->D_arg));
//				break;
			}
			break;

		case 'F':
			perf_opt->op_mode = 'F';
			perf_opt->F_arg = strdup(optarg);
			if(!file_exists(perf_opt->F_arg))
			{
				fprintf(stderr, "ERROR: Unable to open file %s: %s\n", perf_opt->F_arg, strerror(errno));
				exit(1);
			}
			break;

		case 'P':
			perf_opt->p_arg = optarg;
			perf_opt->data_unit = find_data_unit(perf_opt->p_arg);
			switch (perf_opt->data_unit)
			{
			case 'B':
				perf_opt->bundle_payload = atof(perf_opt->p_arg);
				break;
			case 'k':
				perf_opt->bundle_payload = kilo2byte(atof(perf_opt->p_arg));
				break;
			case 'M':
				perf_opt->bundle_payload = mega2byte(atof(perf_opt->p_arg));

				break;
			default:
				printf("\nERROR: (-P option) invalid data unit\n");
				exit(1);
//				printf("\nWARNING: (-p option) invalid data unit, assuming 'K' (KBytes)\n\n");
//				perf_opt->bundle_payload = kilo2byte(atof(perf_opt->p_arg));
//				break;
			}
			break;

		case 'l':
			conn_opt->expiration = atoi(optarg);
			break;

		case 'R':
			perf_opt->rate_arg = strdup(optarg);
			perf_opt->rate_unit = find_rate_unit(perf_opt->rate_arg);
			perf_opt->rate = atof(perf_opt->rate_arg);
			perf_opt->congestion_ctrl = 'r';
			r = TRUE;
			break;

		case 'p':
			if (!strcasecmp(optarg, "bulk"))   {
				conn_opt->priority.priority = BP_PRIORITY_BULK;
			} else if (!strcasecmp(optarg, "normal")) {
				conn_opt->priority.priority = BP_PRIORITY_NORMAL;
			} else if (!strcasecmp(optarg, "expedited")) {
				conn_opt->priority.priority = BP_PRIORITY_EXPEDITED;
			} else if (!strcasecmp(optarg, "reserved")) {
				conn_opt->priority.priority = BP_PRIORITY_RESERVED;
			} else {
				fprintf(stderr, "Invalid priority value %s\n", optarg);
				exit(1);
			}
			break;

		case 'N':
			conn_opt->disable_fragmentation = TRUE;
			break;

		case 'f':
			conn_opt->forwarding_receipts = TRUE;
			break;

		case 'r':
			conn_opt->receive_receipts = TRUE;
			break;

		case 'L':
			perf_opt->create_log = TRUE;
			if (optarg != NULL)
				perf_opt->log_filename = strdup(optarg);
			break;

		case 40:
			perf_opt->logs_dir = strdup(optarg);
			break;

		case 33: // debug
			perf_opt->debug = TRUE;
			if (optarg){
				int debug_level = atoi(optarg);
				if (debug_level >= 1 && debug_level <= 2)
					perf_opt->debug_level = atoi(optarg) - 1;
				else {
					fprintf(stderr, "wrong --debug argument\n");
					exit(1);
					return;
				}
			}
			else
				perf_opt->debug_level = 2;
			break;

		case 34: // incoming bundle destination directory
			perf_opt->dest_dir = strdup(optarg);
			break;

		case 37:
			if(perf_opt->bp_implementation != BP_DTN)
			{
				fprintf(stderr, "--ip-addr supported only in DTN2\n");
				exit(1);
				return;
			}
			perf_opt->ip_addr = strdup(optarg);
			perf_opt->use_ip = TRUE;
			break;

		case 38:
			if(perf_opt->bp_implementation != BP_DTN)
			{
				fprintf(stderr, "--ip-port supported only in DTN2\n");
				exit(1);
				return;
			}
			perf_opt->ip_port = atoi(optarg);
			perf_opt->use_ip = TRUE;
			break;


		case 44:
			if(perf_opt->bp_implementation != BP_DTN)
			{
				fprintf(stderr, "--ack-to-mon supported only in DTN2\n");
				exit(1);
				return;
			}
			perf_opt->bundle_ack_options.ack_to_mon = ATM_FORCE_YES;
			break;

		case 45:
			if(perf_opt->bp_implementation != BP_DTN)
			{
				fprintf(stderr, "--no-ack-to-mon supported only in DTN2\n");
				exit(1);
				return;
			}
			perf_opt->bundle_ack_options.ack_to_mon = ATM_FORCE_NO;
			break;

		case 46:
			set_ack_expiration_as_bundle = FALSE;
			perf_opt->bundle_ack_options.set_ack_expiration = TRUE;
			perf_opt->bundle_ack_options.ack_expiration = atoi(optarg);
			break;

		case 47:
			set_ack_priority_as_bundle = FALSE;
			perf_opt->bundle_ack_options.set_ack_priority = TRUE;
			perf_opt->bundle_ack_options.ack_priority.ordinal = 0;
			if (!strcasecmp(optarg, "bulk"))   {
				perf_opt->bundle_ack_options.ack_priority.priority = BP_PRIORITY_BULK;
			} else if (!strcasecmp(optarg, "normal")) {
				perf_opt->bundle_ack_options.ack_priority.priority = BP_PRIORITY_NORMAL;
			} else if (!strcasecmp(optarg, "expedited")) {
				perf_opt->bundle_ack_options.ack_priority.priority = BP_PRIORITY_EXPEDITED;
			} else if (!strcasecmp(optarg, "reserved")) {
				perf_opt->bundle_ack_options.ack_priority.priority = BP_PRIORITY_RESERVED;
			} else {
				fprintf(stderr, "Invalid ack priority value %s\n", optarg);
				exit(1);
			}
			printf("OK\n");
			break;

		case 48:
			conn_opt->deleted_receipts = TRUE;
			break;

		case 52:
			if( perf_opt->bp_implementation != BP_ION){
				fprintf(stderr, "--ordinal supported only in ION\n");
				exit(1);
				return;
			}
			conn_opt->priority.ordinal = atoi(optarg);
			if(conn_opt->priority.ordinal > 254)
			{
				fprintf(stderr, "Invalid ordinal number %lu\n", conn_opt->priority.ordinal);
				exit(1);
				return;
			}
			break;

		case 53:
			if( perf_opt->bp_implementation != BP_ION){
				fprintf(stderr, "--unreliable supported only in ION\n");
				exit(1);
				return;
			}
			conn_opt->unreliable = TRUE;
			break;

		case 54:
			if( perf_opt->bp_implementation != BP_ION){
				fprintf(stderr, "--critical supported only in ION\n");
				exit(1);
				return;
			}
			conn_opt->critical = TRUE;
			break;

		case 55:
			if( perf_opt->bp_implementation != BP_ION){
				fprintf(stderr, "--flow-label supported only in ION\n");
				exit(1);
				return;
			}
			conn_opt->flow_label = atoi(optarg);
			break;

		case '?':
			fprintf(stderr, "Unknown option: %c\n", optopt);
			exit(1);
			break;

		case (char)(-1):
			done = 1;																					done = 1;
			break;

		default:
			// getopt already prints an error message for unknown option characters
			print_client_usage(argv[0]);
			exit(1);
		} // --switch
	} // -- while

	// if mode is client and monitor eid is not specified, client node must be also monitor.
	if(perf_g_opt->mode == DTNPERF_CLIENT && perf_opt->mon_eid == NULL)
	{
		perf_g_opt->mode = DTNPERF_CLIENT_MONITOR;
	}

	// set ack-to-client request
	if (perf_opt->congestion_ctrl == 'w')
		perf_opt->bundle_ack_options.ack_to_client = TRUE;
	else
		perf_opt->bundle_ack_options.ack_to_client = FALSE;

	// set bundle ack priority as the same of bundle one
	if (set_ack_expiration_as_bundle)
		perf_opt->bundle_ack_options.ack_expiration = conn_opt->expiration;

	// set bundle ack priority as the same of bundle one
	if (set_ack_priority_as_bundle)
		perf_opt->bundle_ack_options.ack_priority = conn_opt->priority;
	// TEMP: ordinal = 0
	perf_opt->bundle_ack_options.ack_priority.ordinal = conn_opt->priority.ordinal;

#define CHECK_SET(_arg, _what)                                          	\
		if (_arg == 0) {                                                    	\
			fprintf(stderr, "\nSYNTAX ERROR: %s must be specified\n", _what);   \
			print_client_usage(argv[0]);                                               \
			exit(1);                                                        	\
		}

	CHECK_SET(perf_opt->dest_eid[0], "destination eid");
	CHECK_SET(perf_opt->op_mode, "-T or -D or -F");

	if (w && r)
	{
		fprintf(stderr, "\nSYNTAX ERROR: -w and -r options are mutually exclusive\n");   \
		print_client_usage(argv[0]);                                               \
		exit(1);
	}


	// check command line options
	check_options(perf_g_opt);

} // end parse_client_options

/* ----------------------------
 * check_options
 * ---------------------------- */
void check_options(dtnperf_global_options_t * global_options)
{

	dtnperf_options_t * perf_opt = global_options->perf_opt;

	// checks on values
	if ((perf_opt->op_mode == 'D') && (perf_opt->data_qty <= 0))
	{
		fprintf(stderr, "\nSYNTAX ERROR: (-D option) you should send a positive number of MBytes (%f)\n\n",
				perf_opt->data_qty);
		exit(2);
	}
	if ((perf_opt->op_mode == 'T') && (perf_opt->transmission_time <= 0))
	{
		fprintf(stderr, "\nSYNTAX ERROR: (-T option) you should specify a positive time\n\n");
		exit(2);
	}
	// checks on options combination
	if ((perf_opt->use_file) && (perf_opt->op_mode == 'T'))
	{
		if (perf_opt->bundle_payload <= ILLEGAL_PAYLOAD)
		{
			perf_opt->bundle_payload = DEFAULT_PAYLOAD;
			fprintf(stderr, "\nWARNING (a): bundle payload set to %f bytes\n", perf_opt->bundle_payload);
			fprintf(stderr, "(use_file && op_mode=='T' + payload <= %d)\n\n", ILLEGAL_PAYLOAD);
		}
	}
	if ((perf_opt->use_file) && (perf_opt->op_mode == 'D'))
	{
		if ((perf_opt->bundle_payload <= ILLEGAL_PAYLOAD)
				|| ((perf_opt->bundle_payload > perf_opt->data_qty)	&& (perf_opt->data_qty > 0)))
		{
			perf_opt->bundle_payload = perf_opt->data_qty;
			fprintf(stderr, "\nWARNING (b): bundle payload set to %f bytes\n", perf_opt->bundle_payload);
			fprintf(stderr, "(use_file && op_mode=='D' + payload <= %d or > %f)\n\n", ILLEGAL_PAYLOAD, perf_opt->data_qty);
		}
	}

	if (perf_opt->bundle_payload <= ILLEGAL_PAYLOAD)
	{
		perf_opt->bundle_payload = DEFAULT_PAYLOAD;
		fprintf(stderr, "\nWARNING: bundle payload set to %f bytes\n\n", perf_opt->bundle_payload);

	}
	if ((!perf_opt->use_file) && (perf_opt->bundle_payload > MAX_MEM_PAYLOAD))
	{
		perf_opt->bundle_payload = MAX_MEM_PAYLOAD;
		fprintf(stderr, "\nWARNING: MAX_MEM_PAYLOAD = %d\nbundle payload set to max: %f bytes\n", MAX_MEM_PAYLOAD, perf_opt->bundle_payload);
	}

	if ((!perf_opt->use_file) && (perf_opt->op_mode == 'D'))
	{
		if (perf_opt->data_qty <= MAX_MEM_PAYLOAD)
		{
			perf_opt->bundle_payload = perf_opt->data_qty;
			fprintf(stderr, "\nWARNING (c1): bundle payload set to %f bytes\n", perf_opt->bundle_payload);
			fprintf(stderr, "(!use_file + payload <= %d + data_qty <= %d + op_mode=='D')\n\n",
					ILLEGAL_PAYLOAD, MAX_MEM_PAYLOAD);
		}
		if (perf_opt->data_qty > MAX_MEM_PAYLOAD)
		{
			perf_opt->bundle_payload = MAX_MEM_PAYLOAD;
			fprintf(stderr, "(!use_file + payload <= %d + data_qty > %d + op_mode=='D')\n",
					ILLEGAL_PAYLOAD, MAX_MEM_PAYLOAD);
			fprintf(stderr, "\nWARNING (c2): bundle payload set to %f bytes\n\n", perf_opt->bundle_payload);
		}
	}

	if (perf_opt->window <= 0)
	{
		fprintf(stderr, "\nSYNTAX ERROR: (-W option) the window must be set to a posotive integer value \n\n");
		exit(2);
	}

} // end check_options

void client_handler(int sig)
{
	printf("\nDTNperf client received SIGINT: exiting\n");
	if (perf_opt->create_log)
		fprintf(log_file, "\nDTNperf client received SIGINT: exiting\n");

	client_clean_exit(0);
} // end client_handler

void client_clean_exit(int status)
{
	printf("Dtnperf client: exit\n");

	// terminate all child threads
	pthread_cancel(sender);
	pthread_cancel(cong_ctrl);

	if (perf_opt->create_log)
		printf("\nClient log saved: %s\n", perf_opt->log_filename);
	if (log_open)
		fclose(log_file);
	if(source_file_created)
	{
		remove(source_file);
		if (perf_opt->debug && perf_opt->debug > 1)
		{
			printf("[debug] removed file %s\n", source_file);
		}
	}

	if (bp_handle_open)
		al_bp_close(handle);
	al_bp_unregister(handle,regid,local_eid);
	exit(status);
} // end client_clean_exit

