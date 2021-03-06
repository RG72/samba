/* 
   simple tool to test persistent databases

   Copyright (C) Andrew Tridgell  2006-2007
   Copyright (c) Ronnie sahlberg  2007
   Copyright (C) Michael Adam     2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/filesys.h"
#include "system/network.h"

#include <popt.h>
#include <talloc.h>
#include <tevent.h>

#include "lib/util/debug.h"
#include "lib/util/time.h"

#include "ctdb_private.h"
#include "ctdb_client.h"

#include "common/cmdline.h"
#include "common/common.h"
#include "common/logging.h"

static struct timeval tp1,tp2;

static void start_timer(void)
{
	gettimeofday(&tp1,NULL);
}

static double end_timer(void)
{
	gettimeofday(&tp2,NULL);
	return (tp2.tv_sec + (tp2.tv_usec*1.0e-6)) - 
		(tp1.tv_sec + (tp1.tv_usec*1.0e-6));
}

static int timelimit = 10;
static int delay = 0;
static int verbose = 0;

static unsigned int pnn;

static TDB_DATA old_data;

static bool success = false;

static void print_counters(void)
{
	int i;
	uint32_t *old_counters;

	printf("[%4u] Counters: ", getpid());
	old_counters = (uint32_t *)old_data.dptr;
	for (i=0;i<old_data.dsize/sizeof(uint32_t); i++) {
		printf("%6u ", old_counters[i]);
	}
	printf("\n");
}

static void each_second(struct tevent_context *ev, struct tevent_timer *te,
			struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);

	print_counters();

	tevent_add_timer(ev, ctdb, timeval_current_ofs(1, 0), each_second, ctdb);
}

static void check_counters(struct ctdb_context *ctdb, TDB_DATA data)
{
	int i;
	uint32_t *counters, *old_counters;
	bool monotonous = true;

	counters     = (uint32_t *)data.dptr;
	old_counters = (uint32_t *)old_data.dptr;

	/* check that all the counters are monotonic increasing */
	for (i=0; i<old_data.dsize/sizeof(uint32_t); i++) {
		if (counters[i]<old_counters[i]) {
			printf("[%4u] ERROR: counters has decreased for node %u  From %u to %u\n", 
			       getpid(), i, old_counters[i], counters[i]);
			monotonous = false;
		}
	}

	if (old_data.dsize != data.dsize) {
		old_data.dsize = data.dsize;
		old_data.dptr = talloc_realloc_size(ctdb, old_data.dptr, old_data.dsize);
	}

	memcpy(old_data.dptr, data.dptr, data.dsize);
	if (verbose) print_counters();

	success = monotonous;
}


static void do_sleep(unsigned int sec)
{
	unsigned int i;
	for (i=0; i<sec; i++) {
		if (verbose) printf(".");
		sleep(1);
	}
	if (verbose) printf("\n");
}

static void test_store_records(struct ctdb_context *ctdb,
			       struct tevent_context *ev)
{
	TDB_DATA key;
	struct ctdb_db_context *ctdb_db;
	int ret;
	uint32_t *counters;
	ctdb_db = ctdb_db_handle(ctdb, "transaction.tdb");

	key.dptr = discard_const("testkey");
	key.dsize = strlen((const char *)key.dptr)+1;

	start_timer();
	while ((timelimit == 0) || (end_timer() < timelimit)) {
		TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
		TDB_DATA data;
		struct ctdb_transaction_handle *h;

		if (verbose) DEBUG(DEBUG_ERR, ("starting transaction\n"));
		h = ctdb_transaction_start(ctdb_db, tmp_ctx);
		if (h == NULL) {
			DEBUG(DEBUG_ERR, ("Failed to start transaction on node %d\n",
			       ctdb_get_pnn(ctdb)));
			talloc_free(tmp_ctx);
			return;
		}
		if (verbose) DEBUG(DEBUG_ERR, ("transaction started\n"));
		do_sleep(delay);

		if (verbose) DEBUG(DEBUG_ERR, ("calling transaction_fetch\n"));
		ret = ctdb_transaction_fetch(h, tmp_ctx, key, &data);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,("Failed to fetch record\n"));
			exit(1);
		}
		if (verbose) DEBUG(DEBUG_ERR, ("fetched data ok\n"));
		do_sleep(delay);

		if (data.dsize < sizeof(uint32_t) * (pnn+1)) {
			unsigned char *ptr = data.dptr;

			data.dptr = talloc_zero_size(tmp_ctx, sizeof(uint32_t) * (pnn+1));
			memcpy(data.dptr, ptr, data.dsize);
			talloc_free(ptr);

			data.dsize = sizeof(uint32_t) * (pnn+1);
		}

		if (data.dptr == NULL) {
			DEBUG(DEBUG_ERR, ("Failed to realloc array\n"));
			talloc_free(tmp_ctx);
			return;
		}

		counters = (uint32_t *)data.dptr;

		/* bump our counter */
		counters[pnn]++;

		if (verbose) DEBUG(DEBUG_ERR, ("calling transaction_store\n"));
		ret = ctdb_transaction_store(h, key, data);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,("Failed to store record\n"));
			exit(1);
		}
		if (verbose) DEBUG(DEBUG_ERR, ("stored data ok\n"));
		do_sleep(delay);

		if (verbose) DEBUG(DEBUG_ERR, ("calling transaction_commit\n"));
		ret = ctdb_transaction_commit(h);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,("Failed to commit transaction\n"));
			check_counters(ctdb, data);
			exit(1);
		}
		if (verbose) DEBUG(DEBUG_ERR, ("transaction committed\n"));

		/* store the counters and verify that they are sane */
		if (verbose || (pnn == 0)) {
			check_counters(ctdb, data);
		}

		do_sleep(delay);

		talloc_free(tmp_ctx);
	}

}

/*
  main program
*/
int main(int argc, const char *argv[])
{
	struct ctdb_context *ctdb;
	struct ctdb_db_context *ctdb_db;
	int unsafe_writes = 0;
	struct poptOption popt_options[] = {
		POPT_AUTOHELP
		POPT_CTDB_CMDLINE
		{ "timelimit", 't', POPT_ARG_INT, &timelimit, 0, "timelimit", "integer" },
		{ "delay", 'D', POPT_ARG_INT, &delay, 0, "delay (in seconds) between operations", "integer" },
		{ "verbose", 'v', POPT_ARG_NONE,  &verbose, 0, "switch on verbose mode", NULL },
		{ "unsafe-writes", 'u', POPT_ARG_NONE, &unsafe_writes, 0, "do not use tdb transactions when writing", NULL },
		POPT_TABLEEND
	};
	int opt;
	const char **extra_argv;
	int extra_argc = 0;
	poptContext pc;
	struct tevent_context *ev;

	if (verbose) {
		setbuf(stdout, (char *)NULL); /* don't buffer */
	} else {
		setlinebuf(stdout);
	}

	pc = poptGetContext(argv[0], argc, argv, popt_options, POPT_CONTEXT_KEEP_FIRST);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		default:
			fprintf(stderr, "Invalid option %s: %s\n", 
				poptBadOption(pc, 0), poptStrerror(opt));
			exit(1);
		}
	}

	/* setup the remaining options for the main program to use */
	extra_argv = poptGetArgs(pc);
	if (extra_argv) {
		extra_argv++;
		while (extra_argv[extra_argc]) extra_argc++;
	}

	ev = tevent_context_init(NULL);

	ctdb = ctdb_cmdline_client(ev, timeval_current_ofs(3, 0));
	if (ctdb == NULL) {
		DEBUG(DEBUG_ERR, ("Could not attach to daemon\n"));
		return 1;
	}

	/* attach to a specific database */
	if (unsafe_writes == 1) {
		ctdb_db = ctdb_attach(ctdb, timeval_current_ofs(2, 0),
				      "transaction.tdb", true, TDB_NOSYNC);
	} else {
		ctdb_db = ctdb_attach(ctdb, timeval_current_ofs(2, 0),
				      "transaction.tdb", true, 0);
	}

	if (!ctdb_db) {
		DEBUG(DEBUG_ERR, ("ctdb_attach failed - %s\n", ctdb_errstr(ctdb)));
		exit(1);
	}

	DEBUG(DEBUG_ERR, ("Waiting for cluster\n"));
	while (1) {
		uint32_t recmode=1;
		ctdb_ctrl_getrecmode(ctdb, ctdb, timeval_zero(), CTDB_CURRENT_NODE, &recmode);
		if (recmode == 0) break;
		tevent_loop_once(ev);
	}

	pnn = ctdb_get_pnn(ctdb);
	printf("Starting test on node %u. running for %u seconds. sleep delay: %u seconds.\n", pnn, timelimit, delay);

	if (!verbose && (pnn == 0)) {
		tevent_add_timer(ev, ctdb, timeval_current_ofs(1, 0),
				 each_second, ctdb);
	}

	test_store_records(ctdb, ev);

	if (verbose || (pnn == 0)) {
		if (success != true) {
			printf("The test FAILED\n");
			return 1;
		} else {
			printf("SUCCESS!\n");
		}
	}
	return 0;
}
