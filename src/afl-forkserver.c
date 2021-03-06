/*
   american fuzzy lop++ - forkserver code
   --------------------------------------

   Originally written by Michal Zalewski

   Forkserver design by Jann Horn <jannhorn@googlemail.com>

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com> and
                        Dominik Maier <mail@dmnk.co>


   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2020 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   Shared code that implements a forkserver. This is used by the fuzzer
   as well the other components like afl-tmin.

 */

#include "config.h"
#include "types.h"
#include "debug.h"
#include "common.h"
#include "list.h"
#include "forkserver.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>

/**
 * The correct fds for reading and writing pipes
 */

/* Describe integer as memory size. */

list_t fsrv_list = {.element_prealloc_count = 0};

static void fsrv_exec_child(afl_forkserver_t *fsrv, char **argv) {

  execv(fsrv->target_path, argv);

}

/* Initializes the struct */

void afl_fsrv_init(afl_forkserver_t *fsrv) {

  // this structure needs default so we initialize it if this was not done
  // already

  fsrv->out_fd = -1;
  fsrv->out_dir_fd = -1;
  fsrv->dev_null_fd = -1;
#ifndef HAVE_ARC4RANDOM
  fsrv->dev_urandom_fd = -1;
#endif
  /* Settings */
  fsrv->use_stdin = 1;
  fsrv->no_unlink = 0;
  fsrv->exec_tmout = EXEC_TIMEOUT;
  fsrv->mem_limit = MEM_LIMIT;
  fsrv->out_file = NULL;

  /* exec related stuff */
  fsrv->child_pid = -1;
  fsrv->map_size = MAP_SIZE;
  fsrv->use_fauxsrv = 0;
  fsrv->last_run_timed_out = 0;

  fsrv->init_child_func = fsrv_exec_child;

  list_append(&fsrv_list, fsrv);

}

/* Initialize a new forkserver instance, duplicating "global" settings */
void afl_fsrv_init_dup(afl_forkserver_t *fsrv_to, afl_forkserver_t *from) {

  fsrv_to->use_stdin = from->use_stdin;
  fsrv_to->out_fd = from->out_fd;
  fsrv_to->dev_null_fd = from->dev_null_fd;
  fsrv_to->exec_tmout = from->exec_tmout;
  fsrv_to->mem_limit = from->mem_limit;
  fsrv_to->map_size = from->map_size;

#ifndef HAVE_ARC4RANDOM
  fsrv_to->dev_urandom_fd = from->dev_urandom_fd;
#endif

  // These are forkserver specific.
  fsrv_to->out_dir_fd = -1;
  fsrv_to->child_pid = -1;
  fsrv_to->use_fauxsrv = 0;
  fsrv_to->last_run_timed_out = 0;
  fsrv_to->out_file = NULL;

  fsrv_to->init_child_func = fsrv_exec_child;

  list_append(&fsrv_list, fsrv_to);

}

/* Internal forkserver for dumb_mode=1 and non-forkserver mode runs.
  It execvs for each fork, forwarding exit codes and child pids to afl. */

static void afl_fauxsrv_execv(afl_forkserver_t *fsrv, char **argv) {

  unsigned char tmp[4] = {0, 0, 0, 0};
  pid_t         child_pid = -1;

  if (!be_quiet) ACTF("Using Fauxserver:");

  /* Phone home and tell the parent that we're OK. If parent isn't there,
     assume we're not running in forkserver mode and just execute program. */

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) abort();  // TODO: Abort?

  void (*old_sigchld_handler)(int) = signal(SIGCHLD, SIG_DFL);

  while (1) {

    uint32_t was_killed;
    int      status;

    /* Wait for parent by reading from the pipe. Exit if read fails. */

    if (read(FORKSRV_FD, &was_killed, 4) != 4) exit(0);

    /* Create a clone of our process. */

    child_pid = fork();

    if (child_pid < 0) PFATAL("Fork failed");

    /* In child process: close fds, resume execution. */

    if (!child_pid) {  // New child

      signal(SIGCHLD, old_sigchld_handler);
      // FORKSRV_FD is for communication with AFL, we don't need it in the
      // child.
      close(FORKSRV_FD);
      close(FORKSRV_FD + 1);

      // TODO: exec...

      execv(fsrv->target_path, argv);

      /* Use a distinctive bitmap signature to tell the parent about execv()
        falling through. */

      *(u32 *)fsrv->trace_bits = EXEC_FAIL_SIG;

      PFATAL("Execv failed in fauxserver.");

    }

    /* In parent process: write PID to AFL. */

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) exit(0);

    /* after child exited, get and relay exit status to parent through waitpid.
     */

    if (waitpid(child_pid, &status, 0) < 0) {

      // Zombie Child could not be collected. Scary!
      PFATAL("Fauxserver could not determin child's exit code. ");

    }

    /* Relay wait status to AFL pipe, then loop back. */

    if (write(FORKSRV_FD + 1, &status, 4) != 4) exit(0);

  }

}

/* Spins up fork server (instrumented mode only). The idea is explained here:

   http://lcamtuf.blogspot.com/2014/10/fuzzing-binaries-without-execve.html

   In essence, the instrumentation allows us to skip execve(), and just keep
   cloning a stopped child. So, we just execute once, and then send commands
   through a pipe. The other part of this logic is in afl-as.h / llvm_mode */

void afl_fsrv_start(afl_forkserver_t *fsrv, char **argv,
                    volatile u8 *stop_soon_p, u8 debug_child_output) {

  int st_pipe[2], ctl_pipe[2];
  int status;
  s32 rlen;

  if (!be_quiet) ACTF("Spinning up the fork server...");

  if (fsrv->use_fauxsrv) {

    /* TODO: Come up with sone nice way to initalize this all */

    if (fsrv->init_child_func != fsrv_exec_child)
      FATAL("Different forkserver not compatible with fauxserver");

    fsrv->init_child_func = afl_fauxsrv_execv;

  }

  if (pipe(st_pipe) || pipe(ctl_pipe)) PFATAL("pipe() failed");

  fsrv->last_run_timed_out = 0;
  fsrv->fsrv_pid = fork();

  if (fsrv->fsrv_pid < 0) PFATAL("fork() failed");

  if (!fsrv->fsrv_pid) {

    /* CHILD PROCESS */

    struct rlimit r;

    /* Umpf. On OpenBSD, the default fd limit for root users is set to
       soft 128. Let's try to fix that... */

    if (!getrlimit(RLIMIT_NOFILE, &r) && r.rlim_cur < FORKSRV_FD + 2) {

      r.rlim_cur = FORKSRV_FD + 2;
      setrlimit(RLIMIT_NOFILE, &r);                        /* Ignore errors */

    }

    if (fsrv->mem_limit) {

      r.rlim_max = r.rlim_cur = ((rlim_t)fsrv->mem_limit) << 20;

#ifdef RLIMIT_AS
      setrlimit(RLIMIT_AS, &r);                            /* Ignore errors */
#else
      /* This takes care of OpenBSD, which doesn't have RLIMIT_AS, but
         according to reliable sources, RLIMIT_DATA covers anonymous
         maps - so we should be getting good protection against OOM bugs. */

      setrlimit(RLIMIT_DATA, &r);                          /* Ignore errors */
#endif                                                        /* ^RLIMIT_AS */

    }

    /* Dumping cores is slow and can lead to anomalies if SIGKILL is delivered
       before the dump is complete. */

    //    r.rlim_max = r.rlim_cur = 0;
    //    setrlimit(RLIMIT_CORE, &r);                      /* Ignore errors */

    /* Isolate the process and configure standard descriptors. If out_file is
       specified, stdin is /dev/null; otherwise, out_fd is cloned instead. */

    setsid();

    if (!(debug_child_output)) {

      dup2(fsrv->dev_null_fd, 1);
      dup2(fsrv->dev_null_fd, 2);

    }

    if (!fsrv->use_stdin) {

      dup2(fsrv->dev_null_fd, 0);

    } else {

      dup2(fsrv->out_fd, 0);
      close(fsrv->out_fd);

    }

    /* Set up control and status pipes, close the unneeded original fds. */

    if (dup2(ctl_pipe[0], FORKSRV_FD) < 0) PFATAL("dup2() failed");
    if (dup2(st_pipe[1], FORKSRV_FD + 1) < 0) PFATAL("dup2() failed");

    close(ctl_pipe[0]);
    close(ctl_pipe[1]);
    close(st_pipe[0]);
    close(st_pipe[1]);

    close(fsrv->out_dir_fd);
    close(fsrv->dev_null_fd);
#ifndef HAVE_ARC4RANDOM
    close(fsrv->dev_urandom_fd);
#endif
    if (fsrv->plot_file != NULL) fclose(fsrv->plot_file);

    /* This should improve performance a bit, since it stops the linker from
       doing extra work post-fork(). */

    if (!getenv("LD_BIND_LAZY")) setenv("LD_BIND_NOW", "1", 0);

    /* Set sane defaults for ASAN if nothing else specified. */

    setenv("ASAN_OPTIONS",
           "abort_on_error=1:"
           "detect_leaks=0:"
           "malloc_context_size=0:"
           "symbolize=0:"
           "allocator_may_return_null=1",
           0);

    /* MSAN is tricky, because it doesn't support abort_on_error=1 at this
       point. So, we do this in a very hacky way. */

    setenv("MSAN_OPTIONS",
           "exit_code=" STRINGIFY(MSAN_ERROR) ":"
           "symbolize=0:"
           "abort_on_error=1:"
           "malloc_context_size=0:"
           "allocator_may_return_null=1:"
           "msan_track_origins=0",
           0);

    fsrv->init_child_func(fsrv, argv);

    /* Use a distinctive bitmap signature to tell the parent about execv()
       falling through. */

    *(u32 *)fsrv->trace_bits = EXEC_FAIL_SIG;
    exit(0);

  }

  /* PARENT PROCESS */

  /* Close the unneeded endpoints. */

  close(ctl_pipe[0]);
  close(st_pipe[1]);

  fsrv->fsrv_ctl_fd = ctl_pipe[1];
  fsrv->fsrv_st_fd = st_pipe[0];

  /* Wait for the fork server to come up, but don't wait too long. */

  rlen = 0;
  if (fsrv->exec_tmout) {

    u32 time = read_timed(fsrv->fsrv_st_fd, &status, 4,
                          fsrv->exec_tmout * FORK_WAIT_MULT, stop_soon_p);

    if (!time) {

      kill(fsrv->fsrv_pid, SIGKILL);

    } else if (time > fsrv->exec_tmout * FORK_WAIT_MULT) {

      fsrv->last_run_timed_out = 1;
      kill(fsrv->fsrv_pid, SIGKILL);

    } else {

      rlen = 4;

    }

  } else {

    rlen = read(fsrv->fsrv_st_fd, &status, 4);

  }

  /* If we have a four-byte "hello" message from the server, we're all set.
     Otherwise, try to figure out what went wrong. */

  if (rlen == 4) {

    if (!be_quiet) OKF("All right - fork server is up.");

    if ((status & FS_OPT_ENABLED) == FS_OPT_ENABLED) {

      if (!be_quiet && getenv("AFL_DEBUG"))
        ACTF("Extended forkserver functions received (%08x).", status);

      if ((status & FS_OPT_SNAPSHOT) == FS_OPT_SNAPSHOT) {

        fsrv->snapshot = 1;
        if (!be_quiet) ACTF("Using SNAPSHOT feature.");

      }

      if ((status & FS_OPT_MAPSIZE) == FS_OPT_MAPSIZE) {

        u32 tmp_map_size = FS_OPT_GET_MAPSIZE(status);

        if (!fsrv->map_size) fsrv->map_size = MAP_SIZE;

        if (unlikely(tmp_map_size % 8)) {

          // should not happen
          WARNF("Target reported non-aligned map size of %ud", tmp_map_size);
          tmp_map_size = (((tmp_map_size + 8) >> 3) << 3);

        }

        if (!be_quiet) ACTF("Target map size: %u", tmp_map_size);
        if (tmp_map_size > fsrv->map_size)
          FATAL(
              "Target's coverage map size of %u is larger than the one this "
              "afl++ is set with (%u) (change MAP_SIZE_POW2 in config.h and "
              "recompile or set AFL_MAP_SIZE)\n",
              tmp_map_size, fsrv->map_size);
        fsrv->map_size = tmp_map_size;

      }

      if ((status & FS_OPT_AUTODICT) == FS_OPT_AUTODICT) {

        if (fsrv->function_ptr == NULL || fsrv->function_opt == NULL) {

          // this is not afl-fuzz - we deny and return
          status = (0xffffffff ^ (FS_OPT_ENABLED | FS_OPT_AUTODICT));
          if (write(fsrv->fsrv_ctl_fd, &status, 4) != 4)
            FATAL("Writing to forkserver failed.");
          return;

        }

        if (!be_quiet) ACTF("Using AUTODICT feature.");
        status = (FS_OPT_ENABLED | FS_OPT_AUTODICT);
        if (write(fsrv->fsrv_ctl_fd, &status, 4) != 4)
          FATAL("Writing to forkserver failed.");
        if (read(fsrv->fsrv_st_fd, &status, 4) != 4)
          FATAL("Reading from forkserver failed.");

        if (status < 2 || (u32)status > 0xffffff)
          FATAL("Dictionary has an illegal size: %d", status);

        u32 len = status, offset = 0, count = 0;
        u8 *dict = ck_alloc(len);
        if (dict == NULL)
          FATAL("Could not allocate %u bytes of autodictionary memory", len);

        while (len != 0) {

          rlen = read(fsrv->fsrv_st_fd, dict + offset, len);
          if (rlen > 0) {

            len -= rlen;
            offset += rlen;

          } else {

            FATAL(
                "Reading autodictionary fail at position %u with %u bytes "
                "left.",
                offset, len);

          }

        }

        offset = 0;
        while (offset < status && (u8)dict[offset] + offset < status) {

          fsrv->function_ptr(fsrv->function_opt, dict + offset + 1,
                             (u8)dict[offset]);
          offset += (1 + dict[offset]);
          count++;

        }

        if (!be_quiet) ACTF("Loaded %u autodictionary entries", count);
        ck_free(dict);

      }

    }

    return;

  }

  if (fsrv->last_run_timed_out)
    FATAL("Timeout while initializing fork server (adjusting -t may help)");

  if (waitpid(fsrv->fsrv_pid, &status, 0) <= 0) PFATAL("waitpid() failed");

  if (WIFSIGNALED(status)) {

    if (fsrv->mem_limit && fsrv->mem_limit < 500 && fsrv->uses_asan) {

      SAYF("\n" cLRD "[-] " cRST
           "Whoops, the target binary crashed suddenly, "
           "before receiving any input\n"
           "    from the fuzzer! Since it seems to be built with ASAN and you "
           "have a\n"
           "    restrictive memory limit configured, this is expected; please "
           "read\n"
           "    %s/notes_for_asan.md for help.\n",
           doc_path);

    } else if (!fsrv->mem_limit) {

      SAYF("\n" cLRD "[-] " cRST
           "Whoops, the target binary crashed suddenly, "
           "before receiving any input\n"
           "    from the fuzzer! There are several probable explanations:\n\n"

           "    - The binary is just buggy and explodes entirely on its own. "
           "If so, you\n"
           "      need to fix the underlying problem or find a better "
           "replacement.\n\n"

           MSG_FORK_ON_APPLE

           "    - Less likely, there is a horrible bug in the fuzzer. If other "
           "options\n"
           "      fail, poke <afl-users@googlegroups.com> for troubleshooting "
           "tips.\n");

    } else {

      u8 val_buf[STRINGIFY_VAL_SIZE_MAX];

      SAYF("\n" cLRD "[-] " cRST
           "Whoops, the target binary crashed suddenly, "
           "before receiving any input\n"
           "    from the fuzzer! There are several probable explanations:\n\n"

           "    - The current memory limit (%s) is too restrictive, causing "
           "the\n"
           "      target to hit an OOM condition in the dynamic linker. Try "
           "bumping up\n"
           "      the limit with the -m setting in the command line. A simple "
           "way confirm\n"
           "      this diagnosis would be:\n\n"

           MSG_ULIMIT_USAGE
           " /path/to/fuzzed_app )\n\n"

           "      Tip: you can use http://jwilk.net/software/recidivm to "
           "quickly\n"
           "      estimate the required amount of virtual memory for the "
           "binary.\n\n"

           "    - The binary is just buggy and explodes entirely on its own. "
           "If so, you\n"
           "      need to fix the underlying problem or find a better "
           "replacement.\n\n"

           MSG_FORK_ON_APPLE

           "    - Less likely, there is a horrible bug in the fuzzer. If other "
           "options\n"
           "      fail, poke <afl-users@googlegroups.com> for troubleshooting "
           "tips.\n",
           stringify_mem_size(val_buf, sizeof(val_buf), fsrv->mem_limit << 20),
           fsrv->mem_limit - 1);

    }

    FATAL("Fork server crashed with signal %d", WTERMSIG(status));

  }

  if (*(u32 *)fsrv->trace_bits == EXEC_FAIL_SIG)
    FATAL("Unable to execute target application ('%s')", argv[0]);

  if (fsrv->mem_limit && fsrv->mem_limit < 500 && fsrv->uses_asan) {

    SAYF("\n" cLRD "[-] " cRST
         "Hmm, looks like the target binary terminated "
         "before we could complete a\n"
         "    handshake with the injected code. Since it seems to be built "
         "with ASAN and\n"
         "    you have a restrictive memory limit configured, this is "
         "expected; please\n"
         "    read %s/notes_for_asan.md for help.\n",
         doc_path);

  } else if (!fsrv->mem_limit) {

    SAYF("\n" cLRD "[-] " cRST
         "Hmm, looks like the target binary terminated "
         "before we could complete a\n"
         "    handshake with the injected code. Perhaps there is a horrible "
         "bug in the\n"
         "    fuzzer. Poke <afl-users@googlegroups.com> for troubleshooting "
         "tips.\n");

  } else {

    u8 val_buf[STRINGIFY_VAL_SIZE_MAX];

    SAYF(
        "\n" cLRD "[-] " cRST
        "Hmm, looks like the target binary terminated "
        "before we could complete a\n"
        "    handshake with the injected code. There are %s probable "
        "explanations:\n\n"

        "%s"
        "    - The current memory limit (%s) is too restrictive, causing an "
        "OOM\n"
        "      fault in the dynamic linker. This can be fixed with the -m "
        "option. A\n"
        "      simple way to confirm the diagnosis may be:\n\n"

        MSG_ULIMIT_USAGE
        " /path/to/fuzzed_app )\n\n"

        "      Tip: you can use http://jwilk.net/software/recidivm to quickly\n"
        "      estimate the required amount of virtual memory for the "
        "binary.\n\n"

        "    - Less likely, there is a horrible bug in the fuzzer. If other "
        "options\n"
        "      fail, poke <afl-users@googlegroups.com> for troubleshooting "
        "tips.\n",
        getenv(DEFER_ENV_VAR) ? "three" : "two",
        getenv(DEFER_ENV_VAR)
            ? "    - You are using deferred forkserver, but __AFL_INIT() is "
              "never\n"
              "      reached before the program terminates.\n\n"
            : "",
        stringify_int(val_buf, sizeof(val_buf), fsrv->mem_limit << 20),
        fsrv->mem_limit - 1);

  }

  FATAL("Fork server handshake failed");

}

static void afl_fsrv_kill(afl_forkserver_t *fsrv) {

  if (fsrv->child_pid > 0) kill(fsrv->child_pid, SIGKILL);
  if (fsrv->fsrv_pid > 0) {

    kill(fsrv->fsrv_pid, SIGKILL);
    if (waitpid(fsrv->fsrv_pid, NULL, 0) <= 0) { WARNF("error waitpid\n"); }

  }

}

/* Delete the current testcase and write the buf to the testcase file */

void afl_fsrv_write_to_testcase(afl_forkserver_t *fsrv, u8 *buf, size_t len) {

  s32 fd = fsrv->out_fd;

  if (fsrv->out_file) {

    if (fsrv->no_unlink) {

      fd = open(fsrv->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    } else {

      unlink(fsrv->out_file);                             /* Ignore errors. */
      fd = open(fsrv->out_file, O_WRONLY | O_CREAT | O_EXCL, 0600);

    }

    if (fd < 0) PFATAL("Unable to create '%s'", fsrv->out_file);

  } else {

    lseek(fd, 0, SEEK_SET);

  }

  ck_write(fd, buf, len, fsrv->out_file);

  if (!fsrv->out_file) {

    if (ftruncate(fd, len)) PFATAL("ftruncate() failed");
    lseek(fd, 0, SEEK_SET);

  } else {

    close(fd);

  }

}

/* Execute target application, monitoring for timeouts. Return status
   information. The called program will update afl->fsrv->trace_bits. */

fsrv_run_result_t afl_fsrv_run_target(afl_forkserver_t *fsrv, u32 timeout,
                                      volatile u8 *stop_soon_p) {

  s32 res;
  u32 exec_ms;

  int status = 0;

  /* After this memset, fsrv->trace_bits[] are effectively volatile, so we
     must prevent any earlier operations from venturing into that
     territory. */

  memset(fsrv->trace_bits, 0, fsrv->map_size);

  MEM_BARRIER();

  /* we have the fork server (or faux server) up and running
  First, tell it if the previous run timed out. */

  if ((res = write(fsrv->fsrv_ctl_fd, &fsrv->last_run_timed_out, 4)) != 4) {

    if (*stop_soon_p) return 0;
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  fsrv->last_run_timed_out = 0;

  if ((res = read(fsrv->fsrv_st_fd, &fsrv->child_pid, 4)) != 4) {

    if (*stop_soon_p) return 0;
    RPFATAL(res, "Unable to request new process from fork server (OOM?)");

  }

  if (fsrv->child_pid <= 0) FATAL("Fork server is misbehaving (OOM?)");

  exec_ms = read_timed(fsrv->fsrv_st_fd, &status, 4, timeout, stop_soon_p);

  if (exec_ms > timeout) {

    /* If there was no response from forkserver after timeout seconds,
    we kill the child. The forkserver should inform us afterwards */

    kill(fsrv->child_pid, SIGKILL);
    fsrv->last_run_timed_out = 1;
    if (read(fsrv->fsrv_st_fd, &status, 4) < 4) exec_ms = 0;

  }

  if (!exec_ms) {

    if (*stop_soon_p) return 0;
    SAYF("\n" cLRD "[-] " cRST
         "Unable to communicate with fork server. Some possible reasons:\n\n"
         "    - You've run out of memory. Use -m to increase the the memory "
         "limit\n"
         "      to something higher than %lld.\n"
         "    - The binary or one of the libraries it uses manages to "
         "create\n"
         "      threads before the forkserver initializes.\n"
         "    - The binary, at least in some circumstances, exits in a way "
         "that\n"
         "      also kills the parent process - raise() could be the "
         "culprit.\n"
         "    - If using persistent mode with QEMU, "
         "AFL_QEMU_PERSISTENT_ADDR "
         "is\n"
         "      probably not valid (hint: add the base address in case of "
         "PIE)"
         "\n\n"
         "If all else fails you can disable the fork server via "
         "AFL_NO_FORKSRV=1.\n",
         fsrv->mem_limit);
    RPFATAL(res, "Unable to communicate with fork server");

  }

  if (!WIFSTOPPED(status)) fsrv->child_pid = 0;

  fsrv->total_execs++;

  /* Any subsequent operations on fsrv->trace_bits must not be moved by the
     compiler below this point. Past this location, fsrv->trace_bits[]
     behave very normally and do not have to be treated as volatile. */

  MEM_BARRIER();

  /* Report outcome to caller. */

  if (WIFSIGNALED(status) && !*stop_soon_p) {

    fsrv->last_kill_signal = WTERMSIG(status);

    if (fsrv->last_run_timed_out && fsrv->last_kill_signal == SIGKILL)
      return FSRV_RUN_TMOUT;

    return FSRV_RUN_CRASH;

  }

  /* A somewhat nasty hack for MSAN, which doesn't support abort_on_error and
     must use a special exit code. */

  if (fsrv->uses_asan && WEXITSTATUS(status) == MSAN_ERROR) {

    fsrv->last_kill_signal = 0;
    return FSRV_RUN_CRASH;

  }

  // Fauxserver should handle this now.
  // if (tb4 == EXEC_FAIL_SIG) return FSRV_RUN_ERROR;

  return FSRV_RUN_OK;

}

void afl_fsrv_killall() {

  LIST_FOREACH(&fsrv_list, afl_forkserver_t, {

    afl_fsrv_kill(el);

  });

}

void afl_fsrv_deinit(afl_forkserver_t *fsrv) {

  afl_fsrv_kill(fsrv);
  list_remove(&fsrv_list, fsrv);

}

