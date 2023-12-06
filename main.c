/*
 * Copyright 2023 Tony Lechner
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the “Software”), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif /* #ifndef$ _XOPEN_SOURCE */

#include <errno.h>      // for errno itself
#include <signal.h>     // for kill(3), raise(3), signal(3)
#include <stdarg.h>     // for stdarg(3)
#include <stdlib.h>     // for exit(3)
#include <stdio.h>      // for perror(3), printf(3)
#include <string.h>     // for strerror(3)
#include <sys/wait.h>   // for waitpid(2)
#include <unistd.h>     // for _exit(3), fork(2), getpid(3), sysconf(3)

#ifndef N_CHILDREN
#define N_CHILDREN 3U
#endif /* #ifndef N_CHILDREN */

#if N_CHILDREN > 128U
#error Invalid N_CHILDREN
#endif /* #if N_CHILDREN > 128U */

static volatile
unsigned
_fork_id = N_CHILDREN;

static volatile
sig_atomic_t
_fatal_signum = 0;

static
void
_log (const char *fmt, ...);

static
void
_on_exit (void);

static
void
_on_signal (int signum);

int
main (void)
{
	// register a function to log on exit and re-raise any fatal signal we
	// received
	if (atexit(_on_exit) == -1) {
		_log("atexit registration failed");
		return EXIT_FAILURE;
	}

	// register a signal handler to log and record any fatal signal we receive
	// for later re-raising
	if (signal(SIGINT, _on_signal) == SIG_ERR) {
		perror("main: signal");
		return EXIT_FAILURE;
	}


	// creating a stack of N_CHILDREN processes
	// each waits on its child to finish. The last waits on a signal via
	// pause()
	_log("started");

	while (_fork_id > 1) {
		pid_t child_pid = fork();
		if (child_pid == -1) {
			perror("main: fork");
			return EXIT_FAILURE;
		}
		// we are the child process
		// keep iterating - either causing more children or breaking out
		if (child_pid == 0) {
			_fork_id--;
			_log("started");
			// keep iterating until there's no more children to make
			continue;
		}

		// we are the parent process
		// wait for our child to finish
		_log("waiting");
		pid_t wait_res;
		errno = 0;
		while ((wait_res = waitpid(child_pid, NULL, 0)) < 0) {
			// we expect to be interrupted
			if (errno != EINTR) {
				perror("main: wait_pid");
				return EXIT_FAILURE;
			}
		}
		exit(EXIT_SUCCESS);
	}

	// we are the last in the stack - wait for a signal
	_log("last child awaiting signal");
	pause();
	return EXIT_SUCCESS;
}

static
void
_log (const char *fmt, ...)
{
	if (fmt == NULL) return;
	char *buf = NULL;

	// log messages up to _SC_PAGE_SIZE for readability (so stderr
	// writes are more-or-less atomic)
	errno = 0;
	long sys_page_size = sysconf(_SC_PAGE_SIZE);
	if (sys_page_size < 0) {
		fprintf(
			stderr,
			"_log: sysconf: %s\n",
			(errno ? strerror(errno) : "indeterminant page size")
		);
		goto cleanup;
	}

	const size_t buf_siz = (size_t) sys_page_size;
	buf = malloc(sys_page_size * sizeof(*buf));
	if (buf == NULL) {
		perror("_log: malloc");
		goto cleanup;
	}
	size_t buf_available = buf_siz;
	int written = 0;

	// write preamble to buf
	written = snprintf(
		buf,
		buf_available,
		"fork #%3u (pid %llu):\t",
		_fork_id,
		(long long unsigned) getpid()
	);
	if (written < 0) {
		perror("_log: snprintf");
		goto cleanup;
	}

	// buf_siz should be large enough to fit our preamble in all cases
	// if it was truncated, consider it a programming error
	if ((unsigned) written > buf_available) {
		fprintf(stderr, "_log: buffer overflow\n");
		goto cleanup;
	}
	buf_available -= written;

	// write caller's msg to buf (possibly truncated)
	va_list vargs;
	va_start(vargs, fmt);
	written = vsnprintf(
		&buf[buf_siz-buf_available],
		buf_available,
		fmt,
		vargs
	);
	if (written < 0) {
		perror("_log: vsnprintf");
	}
	va_end(vargs);
	if (written < 0) {
		goto cleanup;
	}

	if ((unsigned) written <= buf_available) {
		buf_available -= written;
	}
	else {
		buf_available = 0;
	}

	// write linebreak, terminator to buf (always)
	if (buf_available < 2) buf_available = 2;
	buf[buf_siz-buf_available] = '\n';
	buf_available--;
	buf[buf_siz-buf_available] = '\0';
	buf_available--;

	// write buf to stderr
	if (fputs(buf, stderr) == EOF) {
		perror("_log: fputs");
		goto cleanup;
	}

cleanup:
	free(buf);
	return;
}

static
void
_on_exit (void)
{
	int signum = _fatal_signum;
	_log("exiting");

	if (!signum) return;
	if (signal(SIGINT, SIG_DFL) == SIG_ERR) {
		perror("_on_exit: signal");
		_exit(EXIT_FAILURE);
	}
	if (raise(signum)) {
		perror("_on_exit: kill");
	}
	_log("did not die after reraise! calling _exit(3)");
	_exit(EXIT_FAILURE);
}

static
void
_on_signal (int signum)
{
	_fatal_signum = signum;
	_log("caught signal");
}
