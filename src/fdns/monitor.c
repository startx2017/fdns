/*
 * Copyright (C) 2014-2019 fdns Authors
 *
 * This file is part of fdns project
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "fdns.h"
#ifndef  _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>	// clone
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/un.h>

int encrypted[WORKERS_MAX];

typedef struct worker_t {
	pid_t pid;
	int keepalive;
	int fd[2];
#define STACK_SIZE (1024 * 1024)
	char child_stack[STACK_SIZE];		// space for child's stack
} Worker;
static Worker w[WORKERS_MAX];

static volatile sig_atomic_t got_SIGCHLD = 0;
static void child_sig_handler(int sig) {
	(void) sig;
	got_SIGCHLD = 1;
}

static void my_handler(int s) {
	logprintf("signal %d caught, shutting down all the workers\n", s);

	int i;
	for (i = 0; i < arg_workers; i++)
		kill(w[i].pid, SIGKILL);
	exit(0);
}

static int sandbox(void *sandbox_arg) {
	int id = *(int *) sandbox_arg;

	prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0); // kill this process in case the parent died

	// mount events are not forwarded between the host the sandbox
	if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) < 0)
		errExit("mount filesystem as slave");

	char *idstr;
	if (asprintf(&idstr, "--id=%d", id) == -1)
		errExit("asprintf");
	char *fdstr;
	if (asprintf(&fdstr, "--fd=%d", w[id].fd[1]) == -1)
		errExit("asprintf");


	// start a fdns worker process
	char *a[20];
	a[0] = PATH_FDNS;
	a[1] = idstr;
	a[2] = fdstr;
	int last = 3;
	if (arg_debug)
		a[last++] = "--debug";
	if (arg_nofilter)
		a[last++] = "--nofilter";
	if (arg_ipv6)
		a[last++]  = "--ipv6";
	if (arg_proxy_addr) {
		char *cmd;
		if (asprintf(&cmd, "--proxy-addr=%s", arg_proxy_addr) == -1)
			errExit("asprintf");
		a[last++] = cmd;
	}
	if (arg_certfile) {
		char *cmd;
		if (asprintf(&cmd, "--certfile=%s", arg_certfile) == -1)
			errExit("asprintf");
		a[last++] = cmd;
	}
	if (arg_proxy_addr_any)
		a[last++] = "--proxy-addr-any";
	if (arg_server) {
		char *cmd;
		if (asprintf(&cmd, "--server=%s", arg_server) == -1)
			errExit("asprintf");
		a[last++] = cmd;
	}
	if (arg_allow_all_queries)
		a[last++] = "--allow-all-queries";
	a[last] = NULL;
	assert(last < 20);

	// add a small 2 seconds sleep before restarting, just in case we are looping
	sleep(MONITOR_WAIT_TIMER);
	execv(a[0], a);
	exit(1);
}

static void start_sandbox(int id) {
	assert(id < WORKERS_MAX);
	encrypted[id] = 0;

	if (w[id].fd[0] == 0) {
		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, w[id].fd) < 0)
			errExit("socketpair");
		if (arg_debug)
			printf("workerid %d, sockpair %d, %d\n", id, w[id].fd[0], w[id].fd[1]);
	}

	int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
	w[id].pid = clone(sandbox,
		        w[id].child_stack + STACK_SIZE,
		        flags,
		        &id);
	w[id].keepalive = WORKER_KEEPALIVE_SHUTDOWN;
	if (w[id].pid == -1)
		errExit("clone");
}

static void install_signal_handler(void) {
	struct sigaction sga;

	// block SIGTERM/SIGHUP while handling SIGINT
	sigemptyset(&sga.sa_mask);
	sigaddset(&sga.sa_mask, SIGTERM);
	sigaddset(&sga.sa_mask, SIGHUP);
	sga.sa_handler = my_handler;
	sga.sa_flags = 0;
	sigaction(SIGINT, &sga, NULL);

	// block SIGINT/SIGHUP while handling SIGTERM
	sigemptyset(&sga.sa_mask);
	sigaddset(&sga.sa_mask, SIGINT);
	sigaddset(&sga.sa_mask, SIGHUP);
	sga.sa_handler = my_handler;
	sga.sa_flags = 0;
	sigaction(SIGTERM, &sga, NULL);

	// block SIGINT/SIGTERM while handling SIGHUP
	sigemptyset(&sga.sa_mask);
	sigaddset(&sga.sa_mask, SIGINT);
	sigaddset(&sga.sa_mask, SIGTERM);
	sga.sa_handler = my_handler;
	sga.sa_flags = 0;
	sigaction(SIGHUP, &sga, NULL);
}

void monitor(void) {
	assert(arg_id == -1);
	assert(arg_workers <= WORKERS_MAX && arg_workers >= WORKERS_MIN);
	net_local_unix_socket();
	install_signal_handler();

	// attempt to open UDP port 53
	int slocal = net_local_dns_socket(); // will exit if error
	close(slocal); // close the socket
	if (arg_proxy_addr_any)
		logprintf("listening on all available interfaces\n");
	else
		logprintf("listening on %s\n", (arg_proxy_addr)? arg_proxy_addr: DEFAULT_PROXY_ADDR);

	// init worker structures
	memset(w, 0, sizeof(w));

	// create a /run/fdns directory
	struct stat s;
	if (stat(PATH_RUN_FDNS, &s) ) {
		if (mkdir(PATH_RUN_FDNS, 0755) == -1) {
			fprintf(stderr, "Error: cannot create %s directory\n", PATH_RUN_FDNS);
			exit(1);
		}
	}

	// enable /dev/shm/fdns-stats - create the file if it doesn't exist
	shmem_open(1);

	// start workers
	int i;
	for (i = 0; i < arg_workers; i++)
		start_sandbox(i);

	// handle SIGCHLD in pselect loop
	sigset_t sigmask, empty_mask;
	struct sigaction sa;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1)
		errExit("sigprocmask");

	sa.sa_flags = 0;
	sa.sa_handler = child_sig_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		errExit("sigaction");

	sigemptyset(&empty_mask);

	struct timespec t = { 1, 0};	// one second timeout
	time_t timestamp = time(NULL);	// detect the computer going to sleep in order to reinitialize SSL connections
	while (1) {
		fd_set rset;
		FD_ZERO(&rset);
		int fdmax = 0;
		for (i = 0; i < arg_workers;i++) {
			FD_SET(w[i].fd[0], &rset);
			fdmax = (fdmax < w[i].fd[0])? w[i].fd[0]: fdmax;
		}
		fdmax++;

		int rv = pselect(fdmax, &rset, NULL, NULL, &t, &empty_mask);
		if (rv == -1) {
			if (errno == EINTR) {
				// select() man page reads:
				// "... the sets and  timeout become undefined, so
				// do not rely on their contents after an error. "
				t.tv_sec = 1;
				t.tv_nsec = 0;
				continue;
			}
			printf("***\n");
		}
		else if (rv == 0) {
			time_t ts = time(NULL);
			int i;

			// decrease keepalive wait when coming out of sleep/hibernation
			if (ts - timestamp > OUT_OF_SLEEP) {
				for (i = 0; i < arg_workers; i++) {
					if (w[i].keepalive > WORKER_KEEPALIVE_AFTER_SLEEP)
						w[i].keepalive = WORKER_KEEPALIVE_AFTER_SLEEP;
				}
			}

			// restart workers if the keepalive time expired
			for (i = 0; i < arg_workers; i++) {
				if (--w[i].keepalive <= 0) {
					logprintf("Restarting worker process %d\n", i);
					kill(w[i].pid, SIGKILL);
					int status;
					waitpid(w[i].pid, &status, 0);
					start_sandbox(i);
				}
			}
			t.tv_sec = 1;
			t.tv_nsec = 0;
			timestamp = time(NULL);
		}
		else if (got_SIGCHLD) {
			pid_t pid = -1;;
			int status;
			got_SIGCHLD = 0;

			// find a dead worker
			int i;
			for (i = 0; i < arg_workers; i++) {
				pid = waitpid(w[i].pid, &status, WNOHANG);
				if (pid == w[i].pid) {
					logprintf("Error: worker %d (pid %u) terminated, restarting it...\n", i, pid);
					kill(pid, SIGTERM); // just in case
					start_sandbox(i);
				}
			}
		}
		else {
			for (i = 0; i < arg_workers; i++) {
				if (FD_ISSET(w[i].fd[0], &rset)) {
					LogMsg msg;
					ssize_t len = read(w[i].fd[0], &msg, sizeof(LogMsg));
					if (len == -1) // todo: parse EINTR
						errExit("read");

					// check length
					if (len != msg.h.len) {
						logprintf("Error: log message with an invalid length\n");
						continue;
					}

					// parse the incoming message
					msg.buf[len - sizeof(LogMsgHeader)] = '\0';

					// parse incoming message
					if (strncmp(msg.buf, "Stats: ", 7) == 0) {
						Stats s;
						sscanf(msg.buf, "Stats: rx %u, dropped %u, fallback %u, cached %u",
						       &s.rx,
						       &s.drop,
						       &s.fallback,
						       &s.cached);

						// calculate global stats
						stats.rx += s.rx;
						stats.drop += s.drop;
						stats.fallback += s.fallback;
						stats.cached += s.cached;
						shmem_store_stats();
					}
					else if (strncmp(msg.buf, "Request: ", 9) == 0) {
						printf("%s", msg.buf + 9);
						shmem_store_log(msg.buf + 9);
					}
					else if (strncmp(msg.buf, "worker keepalive", 16) == 0)
						w[i].keepalive = WORKER_KEEPALIVE_SHUTDOWN;
					else {
						if (strncmp(msg.buf, "SSL connection opened", 21) == 0) {
							encrypted[i] = 1;
							shmem_store_stats();
						}
						else if (strncmp(msg.buf, "SSL connection closed", 21) == 0) {
							encrypted[i] = 0;;
							shmem_store_stats();
						}

						char *tmp;
						if (asprintf(&tmp, "(%d) %s", i, msg.buf) == -1)
							errExit("asprintf");
						logprintf("%s", tmp);
						shmem_store_log(tmp);
						free(tmp);
					}

					fflush(0);
				}
			}
		}
		fflush(0);
	}
}
