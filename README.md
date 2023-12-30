# signal\_process\_stack\_example

This is a POSIX C program built to demonstrate the differences in how signals
are handled between various permutations of docker options.

It recursively makes 3 child process - which form a stack. The top of the stack
waits for any signal, while the rest await their child's exit.

Press ^C to see how the stack unwinds in various situations.

To demonstrate some additional complexities, processes will catch SIGINT, continue
on as normal, and re-raise it once their normal logic has completed. If
re-raising didn't term the process, it logs as such and calls `_exit(3)`.

## What happens when...

### I start a child process

When you call `fork(2)` to make a process, the kernel creates the process
and records the new process's ID, its parent process's ID, and the [process
group](https://en.wikipedia.org/wiki/Process_group) ID - among other things. It
writes these in a spot in kernel memory called the Process Table. It then hands
the child process's ID to the process that called `fork(2)`.

POSIX exposes a way to both send signals to and await the termination of
individual processes and groups of processes.

#### How processes are organized

Processes are grouped into "process groups."

For example, if I run the shell command `echo foo | cat`, both `echo(1)` and
`cat(1)` are started with the same process group ID.

The shell waits on the whole process group to terminate - not any individual
member of the group.

#### What is expected of me when I start a child process?

_You are expected to call one of the `wait(3)` series of functions._ When the
child ends, this will remove the child's entry from the process table - and the
wait function will return.

Process table entries live until someone retrieves it through the `wait(3)`
series of functions. Imporantly: this table entry will live on even after the
child process ends (this handles the case where the child process ended between
when the parent forked and called wait).

#### What happens if you don't wait for children?

Sometimes things happen and the programmer fails to call a wait function for
a child process - either through a bug or because their program was ended early.

Init systems - the very first process started on a POSIX system - periodically
empty the table of proceses that can never have their table entries received.

However, this only happens when the parent also ends.

If your program keeps running, keeps creating processes, and and never waits,
the system process table will eventually be full. This is called PID starvation.

### ...I run things on my machine?

In a POSIX system, signals typically go to entire [process
groups](https://en.wikipedia.org/wiki/Process_group).

Some examples:
- Pressing ^C in a terminal emulator sends SIGINT to the entire foreground
  process group.
- Closing a terminal window sends SIGHUP to all process groups controlled by
  it
- Logging out sends SIGHUP to all process groups you started.
- Your init system will end your daemon by sending SIGTERM to your daemon and
  all of its children (by default).

Thus, your process's children will also shut down - there is no need to forward
signals. It is your responsibity, to `wait(3)` on the process to avoid leaving
around its process table entry. It is not, however, your responsibility to
forward signals you receive to your children.

```
$ make clean all
[snip]
$ ./signal_process_stack_example
fork #  3 (pid 38528):  started
fork #  3 (pid 38528):  waiting
fork #  2 (pid 38529):  started
fork #  2 (pid 38529):  waiting
fork #  1 (pid 38530):  started
fork #  1 (pid 38530):  last child awaiting signal
^Cfork #  2 (pid 38529):        caught signal
fork #  3 (pid 38528):  caught signal
fork #  1 (pid 38530):  caught signal
fork #  1 (pid 38530):  exiting
fork #  2 (pid 38529):  exiting
fork #  3 (pid 38528):  exiting
```

Observe that:

1. All processes individually caught SIGINT after `^C`
1. They caught SIGINT in no particular order
1. Because they have a SIGINT handler installed, they were able to respond to
   the signal and cleanly exit


### ...I run things via `docker run`?

The way Docker containers work change up this paradigm.

Docker starts your process and waits for it to exit. Any processes still
running when that first process has exited will be immediately SIGKILLED.

```
$ docker run --rm -it -v "$PWD:/app" --user `id -u`:`id -g` --workdir /app gcc make clean all
[snip]
$ docker run --rm -v "$PWD:/app" --user `id -u`:`id -g` --workdir /app gcc ./signal_process_stack_example
fork #  3 (pid 1):      started
fork #  3 (pid 1):      waiting
fork #  2 (pid 7):      started
fork #  2 (pid 7):      waiting
fork #  1 (pid 8):      started
fork #  1 (pid 8):      last child awaiting signal
^Cfork #  3 (pid 1):    caught signal
```

_(You will need to use `docker stop -s KILL container_id_here` to stop the container.)_

Observe that although our process caught SIGINT, reset the handler, and reraised it - no
processes stopped.

So what happened? Two things:

First, Docker started your process in a new control group - part of the magic that
makes containers isolated. As the first process in the control group, you are PID 1.
PID 1 is special in Linux, because it is intended for an [init
system](https://en.wikipedia.org/wiki/Init).

PID 1 has a big tweaks to the normal rules: **signals that normally terminate
a process in their default state instead do nothing.**

Secondly, docker sends the configured stop signal for a container (SIGTERM by
default) to PID 1 - **and only PID 1**.

### ...I run things via `docker run` with the `--intearctive` and `--tty` flags?

If you invoke `docker run` with the `--interactive` and `--tty` flags, the tty being
attached causes signals to be forwarded to the entire process group. This is
a little more like normal.

```
$ docker run --rm -it -v "$PWD:/app" --user `id -u`:`id -g` --workdir /app gcc ./signal_process_stack_example
fork #  3 (pid 1):      started
fork #  3 (pid 1):      waiting
fork #  2 (pid 7):      started
fork #  2 (pid 7):      waiting
fork #  1 (pid 8):      started
fork #  1 (pid 8):      last child awaiting signal
^Cfork #  2 (pid 7):    caught signal
fork #  1 (pid 8):      caught signal
fork #  3 (pid 1):      caught signal
fork #  1 (pid 8):      exiting
fork #  2 (pid 7):      exiting
fork #  3 (pid 1):      exiting
fork #  3 (pid 1):      did not die after reraise! calling _exit(3)
```

Observe, however, that our PID 1 is still not dying when signaled.

Additionally, these flags can only be used when it's actually being called in
a terminal (i.e. by you - nothing automated).  This isn't terribly useful when
trying to run a server  - where we can't just add a terminal to the mix.

We thus have two choices:

1. Override the default signals in our code to explicitly exit.
1. Find a program that is good at acting as an init system.

Roleplaying an init system might be fun to implement, but that's an awful lot of
complexity for the average server process to implement.

Fortunately, docker now ships with an opt-in init system to inject into
containers. It's called [tini](https://github.com/krallin/tini).

### ...I run things via `docker run` with the `--init` flag?

If you invoke `docker run` with the `--init` flag, docker will wrap your
container's command with a simple init system.

```
docker run --rm --init -v "$PWD:/app" --user `id -u`:`id -g` --workdir /app gcc ./signal_process_stack_example
fork #  3 (pid 7):      started
fork #  3 (pid 7):      waiting
fork #  2 (pid 8):      started
fork #  2 (pid 8):      waiting
fork #  1 (pid 9):      started
fork #  1 (pid 9):      last child awaiting signal
^Cfork #  3 (pid 7):    caught signal

```

_(You'll need to `docker stop` again)_

This is nice in that we didn't have to reimplement the default signal handlers in
our applications. But observe that we didn't actually stop again. This is
because our programs won't exit until it's done waiting for its children - and
only the first process in our stack was forwarded the signal by tini.

By default, tini follow's docker's behavior: signals get forwarded only to the
process it started, and then it waits for us to finish.

### ...I run things via `docker run` with the `--init` flag and set `TINI_KILL_PROCESS_GROUP`?

Fortunately, tini exposes a configuration point that tells it to signal
the entire process group instead of just the main process it started.

This is an environment variable read by `tini`, `TINI_KILL_PROCESS_GROUP`.

```
$ docker run --rm --init -e TINI_KILL_PROCESS_GROUP=1 -v "$PWD:/app" --user `id -u`:`id -g` --workdir /app gcc ./signal_process_stack_example
fork #  3 (pid 7):      started
fork #  3 (pid 7):      waiting
fork #  2 (pid 8):      started
fork #  2 (pid 8):      waiting
fork #  1 (pid 9):      started
fork #  1 (pid 9):      last child awaiting signal
^Cfork #  1 (pid 9):    caught signal
fork #  2 (pid 8):      caught signal
fork #  3 (pid 7):      caught signal
fork #  1 (pid 9):      exiting
fork #  2 (pid 8):      exiting
fork #  3 (pid 7):      exiting
```

And at long last, we have roughly the same behavior between docker and
non-docker worlds - without having to modify our application code.

## Additional Gotchyas

### Shell Behavior

Shells do not forward signals - they assume whole process groups receive
signals.

Additionally, shells do not respond to signals while a foreground command is
being executed. Their signal handling logic is executed _between_ commands.
This includes trapped signals.

```sh
echo foo
# any signal caught could be handled here
echo bar
# any signal caught could be handled here
```

However, the one exception to this rule is the `wait` builtin - which will
immediately return with an exit status of `128 + signal number`.

Thus, if a shell script is executing a particularly long-running command, the
shell will not react to a signal until that long-running command is done
executing.

In a normal POSIX environment where the shell and command will both be signaled,
this isn't a big deal. The child will also receive the signal and terminate.

But in most of the docker configuration permutations above - where the shell's
child process will _not_ be signaled - this leads to a situation where the
long-running command is not interruptible without directly signaling it.

To work around this, you can start long-running commands in the background,
forwarding the shell's terminating signal to the shell's entire process group.

```sh
#!/bin/sh

forward_signal_to_pg()
{
	signame="${1:-TERM}"
	trap - "$signame"
	kill "-$signame" 0
    wait
}
trap 'forward_signal_to_pg INT' INT
trap 'forward_signal_to_pg TERM' TERM

./long_running_command.sh &
wait $!
```

## Copying

[MIT-0](https://opensource.org/license/mit-0/)

Copyright 2023 Tony Lechner

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the “Software”), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
