#include <libguile.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

/* Global state for the FIFO file descriptor and process tracking.  */
static int fifo_fd = -1;
static pid_t getty_pid = -1;

/* Signal handler to catch shutdown requests and pass them safely.  */
static void
signal_handler (int sig)
{
  /* Forward signals or handle basic termination safely.  */
  sync ();
  if (sig == SIGINT || sig == SIGUSR1)
    {
      /* Clean up and exit, letting the kernel panic or handle the reset.  */
      _exit (0);
    }
}

/* Fallback shell loop if the Guile configuration fails to load.  */
static void
fallback_shell (void)
{
  pid_t pid;

  printf ("** /etc/sherpa.scm missing or failed! Starting emergency shell. **\n");
  pid = fork ();
  if (pid == 0)
    {
      execl ("/bin/sh", "sh", NULL);
      _exit (1);
    }
  else if (pid > 0)
    {
      int status;
      waitpid (pid, &status, 0);
    }
}

/* The core initialization and supervisor loop executing inside the Guile VM.  */
static void
inner_main (void *closure, int argc, char **argv)
{
  SCM fifo_path_scm;
  char *fifo_path = NULL;

  printf ("[SHERPK] Entering Guile VM...\n");

  if (access ("/etc/sherpa.scm", R_OK) == 0)
    {
      scm_c_primitive_load ("/etc/sherpa.scm");
    }
  else
    {
      fallback_shell ();
    }

  /* Look up the FIFO path variable defined within the Scheme script.  */
  fifo_path_scm = scm_lookup_closure_with_flags (scm_from_utf8_symbol ("%fifo-path"), 
                                                 SCM_BOOL_F);
  
  if (scm_is_true (fifo_path_scm))
    {
      SCM val = scm_variable_ref (fifo_path_scm);
      if (scm_is_string (val))
        fifo_path = scm_to_utf8_string (val);
    }

  /* Default path fallback if not specified by the Scheme environment.  */
  if (!fifo_path)
    fifo_path = strdup ("/var/run/sherpa.fifo");

  /* Initialize the named pipe based on the Guile-provided path.  */
  unlink (fifo_path);
  if (mkfifo (fifo_path, 0600) == 0)
    {
      fifo_fd = open (fifo_path, O_RDONLY | O_NONBLOCK);
    }

  /* Main SysVinit supervisor and sub-reaper cycle.  */
  while (1)
    {
      int status;
      pid_t reaped;

      /* Check the command channel if it was successfully opened.  */
      if (fifo_fd >= 0)
        {
          char buf[64];
          ssize_t cmd_len = read (fifo_fd, buf, sizeof (buf) - 1);
          if (cmd_len > 0)
            {
              buf[cmd_len] = '\0';
              if (strstr (buf, "reboot") || strstr (buf, "halt"))
                {
                  free (fifo_path);
                  sync ();
                  _exit (0);
                }
            }
        }

      /* Respawn manager for the primary system console.  */
      if (getty_pid <= 0)
        {
          getty_pid = fork ();
          if (getty_pid == 0)
            {
              setsid ();
              /* Cross-platform friendly shell execution.  */
              execl ("/bin/sh", "sh", "-i", NULL);
              _exit (1);
            }
        }

      /* Monitor and reap orphaned processes or dead children.  */
      while ((reaped = waitpid (-1, &status, WNOHANG)) > 0)
        {
          if (reaped == getty_pid)
            {
              printf ("** Primary console process terminated. Restarting... **\n");
              getty_pid = -1;
            }
        }

      /* Brief execution pause to mitigate high processor utilization.  */
      usleep (100000);
    }

  free (fifo_path);
}

/* System entry point verifying PID 1 state and booting the runtime.  */
int
main (int argc, char **argv)
{
  if (getpid () != 1)
    {
      fprintf (stderr, "** Error: Must be executed as PID 1. **\n");
      return 1;
    }

  /* Configure essential platform signals uniformly.  */
  signal (SIGUSR1, signal_handler);
  signal (SIGINT,  signal_handler);
  signal (SIGCHLD, SIG_DFL);

  printf ("\n** sherpa - a fine man's init **\n");

  /* Hand control over to the GNU Guile subsystem.  */
  scm_boot_guile (argc, argv, inner_main, NULL);

  return 0;
}
