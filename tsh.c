/*
 * tsh - A tiny shell program with job control
 *
 * <Aidan Chin 33803321 & Luke Rattanavijai 33714609>
 */

// test test
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE 1024   /* max line size */
#define MAXARGS 128    /* max args on a command line */
#define MAXJOBS 16     /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */
#define MAXPIPE 16     /* max pipes */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;   /* defined in libc */
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0;         /* if true, print additional output */
int nextjid = 1;         /* next job ID to allocate */
char sbuf[MAXLINE];      /* for composing sprintf messages */

struct job_t {           /* The job struct */
  pid_t pid;             /* job PID */
  int jid;               /* job ID [1, 2, ...] */
  int state;             /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE]; /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

void execute_pipe(char *cmds[MAXPIPE][MAXARGS], int n);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv, char *cmds[MAXPIPE][MAXARGS]); //modified to work with pipes
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

void redirect_input(const char *input_file);
void redirect_output(const char *output_file, int append);
void redirect_error(const char *error_file);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv) {
  char c;
  char cmdline[MAXLINE];
  int emit_prompt = 1; /* emit prompt (default) */

  /* Redirect stderr to stdout (so that driver will get all output
   * on the pipe connected to stdout) */
  dup2(1, 2);

  /* Parse the command line */
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
    case 'h': /* print help message */
      usage();
      break;
    case 'v': /* emit additional diagnostic info */
      verbose = 1;
      break;
    case 'p':          /* don't print a prompt */
      emit_prompt = 0; /* handy for automatic testing */
      break;
    default:
      usage();
    }
  }

  /* Install the signal handlers */

  /* These are the ones you will need to implement */
  Signal(SIGINT, sigint_handler);   /* ctrl-c */
  Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
  Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

  /* This one provides a clean way to kill the shell */
  Signal(SIGQUIT, sigquit_handler);

  /* Initialize the job list */
  initjobs(jobs);

  /* Execute the shell's read/eval loop */
  while (1) {

    /* Read command line */
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
      fflush(stdout);
      exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  }

  exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char *cmdline) {
    char *argv[MAXARGS]; // Argument list execve()
    char buf[MAXLINE]; // Holds modified command line
    char *cmds[MAXPIPE][MAXARGS]; // Commands for pipes
    int bg; // Should the job run in bg or fg?
    pid_t pid; // Process id
    int in_fd = -1, out_fd = -1, err_fd = -1; // File descriptors for redirection
    int append = 0; // Append flag for output redirection
    int pipefd[2]; // File descriptors for pipe
    int pipe_present = 0; // Flag to check if pipe is present

    strcpy(buf, cmdline);
    bg = parseline(buf, argv, cmds);

    int num_cmds = 0;
    while (cmds[num_cmds][0] != NULL)
        num_cmds++;

    if (num_cmds == 1) {
        // Check for redirection operators
        for (int i = 0; argv[i] != NULL; i++) {
            if (strcmp(argv[i], "<") == 0) {
                in_fd = open(argv[i + 1], O_RDONLY);
                if (in_fd < 0) {
                    perror("open");
                    return;
                }
                argv[i] = NULL;
            } else if (strcmp(argv[i], ">") == 0) {
                out_fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
                if (out_fd < 0) {
                    perror("open");
                    return;
                }
                argv[i] = NULL;
            } else if (strcmp(argv[i], ">>") == 0) {
                out_fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRWXG | S_IRWXO);
                if (out_fd < 0) {
                    perror("open");
                    return;
                }
                argv[i] = NULL;
            } else if (strcmp(argv[i], "2>") == 0) {
                err_fd = open(argv[i + 1], O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
                if (err_fd < 0) {
                    perror("open");
                    return;
                }
                argv[i] = NULL;
            }
        }

        if (!builtin_cmd(argv)) {
            if ((pid = fork()) == 0) { // Child process
                setpgid(0, 0);

                // Handle input redirection
                if (in_fd != -1) {
                    dup2(in_fd, STDIN_FILENO);
                    close(in_fd);
                }

                // Handle output redirection
                if (out_fd != -1) {
                    dup2(out_fd, STDOUT_FILENO);
                    close(out_fd);
                }

                // Handle error redirection
                if (err_fd != -1) {
                    dup2(err_fd, STDERR_FILENO);
                    close(err_fd);
                }

                if (execvp(argv[0], argv) < 0) {
                    perror(argv[0]);
                    exit(0);
                }
            }
            if (!bg) {
                waitfg(pid);
            } else {
                addjob(jobs, pid, BG, cmdline);
                printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
            }
        }
    } else {
        execute_pipe(cmds, num_cmds);
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */

int parseline(const char *cmdline, char **argv, char *cmds[MAXPIPE][MAXARGS]) {
  static char array[MAXLINE]; // copy of command line
  char *buf = array;          // ptr for command line
  char *delim;                // first space delimiter
  int argc = 0;               // number of args
  int bg;                     // background job?
  int pipe_count = 0;         // number of pipes

  strcpy(buf, cmdline);
  buf[strlen(buf) - 1] = ' ';   // replace trailing '\n' with space
  while (*buf && (*buf == ' ')) // ignore leading spaces
    buf++;

  while (pipe_count < MAXPIPE) { // make sure not too many pipes
    cmds[pipe_count][0] = NULL;
    char *cmd = strtok_r(buf, "|", &buf); // split into buf
    if (!cmd)
      break;

    /* Build the argv list */
    argc = 0;                     // reset argc for each command
    while (*cmd && (*cmd == ' ')) // ignore leading spaces
      cmd++;
    if (*cmd == '\'') {
      cmd++;
      delim = strchr(cmd, '\'');
    } else {
      delim = strchr(cmd, ' ');
    }

    while (delim) {
      argv[argc++] = cmd;
      *delim = '\0';
      cmd = delim + 1;
      while (*cmd && (*cmd == ' ')) // ignore spaces
        cmd++;
      if (*cmd == '\'') {
        cmd++;
        delim = strchr(cmd, '\'');
      } else {
        delim = strchr(cmd, ' ');
      }
    }
    argv[argc] = NULL;                                     // null termination
    memcpy(cmds[pipe_count], argv, argc * sizeof(char *)); // copy array to cmds
    pipe_count++;
  }

  if (argc == 0) /* ignore blank line */
    return 1;

  /* should the job run in the background? */
  if ((bg = (*argv[argc - 1] == '&')) != 0) {
    argv[--argc] = NULL;
  }
  return bg;
}

/* 
 * execute_pipe - Execute a series of piped commands
 * cmds: An array of commands and their arguments
 * n: The number of commands in the pipes
 */
void execute_pipe(char *cmds[MAXPIPE][MAXARGS], int n) {
  int i;
  int fds[MAXPIPE][2]; // array for file descriptors
  pid_t pid;

  for (i = 0; i < n - 1; i++) { // create the pipes with file descriptors
    pipe(fds[i]);
  }

  for (i = 0; i < n; i++) { // run through each command to fork and pipe
    if ((pid = fork()) == 0) {
      if (i > 0) { // If not the first command, redirect stdin to the previous
                   // pipe's read end
        dup2(fds[i - 1][0], 0);
        close(fds[i - 1][0]);
        close(fds[i - 1][1]);
      }
      if (i < n - 1) { // If not the last command, redirect stdin to the
                       // previous pipe's write end
        close(fds[i][0]);
        dup2(fds[i][1], 1);
        close(fds[i][1]);
      }
      for (int j = 0; j < n - 1; j++) { // close everything up
        close(fds[j][0]);
        close(fds[j][1]);
      }
      execvp(cmds[i][0], cmds[i]); // exec cmd
      app_error("execvp");
      exit(1);
    }
  }

  for (i = 0; i < n - 1; i++) { // close all pipes
    close(fds[i][0]);
    close(fds[i][1]);
  }

  for (i = 0; i < n; i++) { // wait for everything to finish
    wait(NULL);
  }
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  (jobs,  quit, bg, fg)
 */
int builtin_cmd(char **argv) {
  if (argv == NULL) {
    return 0;
  } // no command, return 0
  if (strcmp(argv[0], "jobs") == 0) { // lists running jobs
    listjobs(jobs);
    return 1; // success
  }
  if (strcmp(argv[0], "quit") == 0) {
    exit(0); // quit
    return 1;
  }
  if (strcmp(argv[0], "bg") == 0 ||
      strcmp(argv[0], "fg") == 0) { // changes job to background or foreground
    do_bgfg(argv);
    return 1;
  }
  if (!strcmp(argv[0], "&")) {
    return 1; // Ignore singleton &
  }
  return 0; // not a builtin command
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
  struct job_t *job;
  int jid;
  pid_t pid;

  if (argv[1] == NULL) { // Check if argument has jobid
    printf("%s Command needs a PID or %%jobid\n", argv[0]);
    return;
  }
  if (argv[1][0] == '%') {   // job id
    jid = atoi(&argv[1][1]); // extract job
    job = getjobjid(jobs, jid);
    if (job == NULL) { // check if job exists
      printf("%s: No such job\n", argv[1]);
      return;
    }
  } else {               // pid
    pid = atoi(argv[1]); // extract job
    job = getjobpid(jobs, pid);
    if (job == NULL) { // check if job exists
      printf("%s: No such job\n", argv[1]);
      return;
    }
  }

  pid = job->pid; // make pid for sure

  if (strcmp(argv[0], "bg")) { // change to background
    job->state = BG;
    kill(-pid, SIGCONT);
    printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
  } else { // change to foreground
    job->state = FG;
    kill(-pid, SIGCONT);
    waitfg(pid);
  }
  return;
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
  struct job_t *job = getjobpid(jobs, pid);

  while (job != NULL && job->state == FG) {
    usleep(1000);               // sleep to not take over the cpu
    job = getjobpid(jobs, pid); // check if job is still in FG state
  }
  return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig) {
  int newerrno = errno; /* Save errno to restore later */
  pid_t pid;
  int status;

  /* Reap all available zombie children
   * WNOHANG: Don't block if no child has exited
   * WUNTRACED: Also return if a child has stopped
   */
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
    struct job_t *job = getjobpid(jobs, pid);
    if (!job) { // code should not get here
      continue;
    }

    // child terminated normally
    if (WIFEXITED(status)) {
      deletejob(jobs, pid); // remove job and proccess id
    }
    // child terminated by signal
    else if (WIFSIGNALED(status)) {
      printf("Job [%d] (%d) terminated by signal %d\n", job->jid, pid,
             WTERMSIG(status));
      deletejob(jobs, pid); // remove job and proccess id
    }
    // child stopped
    else if (WIFSTOPPED(status)) {
      printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid,
             WSTOPSIG(status));
      job->state = ST; // update job state to stopped
    }
  }
  // no children to reap
  if (pid < 0 && errno != ECHILD) {
    unix_error("waitpid error");
  }

  errno = newerrno; /* Restore errno */
  return;
}
/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig) {
  int olderrno = errno;
  pid_t pid = fgpid(jobs);

  if (pid != 0) {
    // sending the SIGINT to the entire foreground process group
    kill(-pid, SIGINT);
  }
  errno = olderrno;
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig) {
  int olderrno = errno;
  pid_t pid = fgpid(jobs);

  if (pid != 0) {
    // send STGTSP to the entire foreground process group
    kill(-pid, SIGTSTP);
  }
  errno = olderrno;

  return;
}
/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
  job->pid = 0;
  job->jid = 0;
  job->state = UNDEF;
  job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) {
  int i, max = 0;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
      max = jobs[i].jid;
  return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
      jobs[i].pid = pid;
      jobs[i].state = state;
      jobs[i].jid = nextjid++;
      if (nextjid > MAXJOBS)
        nextjid = 1;
      strcpy(jobs[i].cmdline, cmdline);
      if (verbose) {
        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid,
               jobs[i].cmdline);
      }
      return 1;
    }
  }
  printf("Tried to create too many jobs\n");
  return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
      clearjob(&jobs[i]);
      nextjid = maxjid(jobs) + 1;
      return 1;
    }
  }
  return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
      return jobs[i].pid;
  return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) {
  int i;

  if (jid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
  int i;

  if (pid < 1)
    return 0;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
      return jobs[i].jid;
    }
  return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
      switch (jobs[i].state) {
      case BG:
        printf("Running ");
        break;
      case FG:
        printf("Foreground ");
        break;
      case ST:
        printf("Stopped ");
        break;
      default:
        printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
      }
      printf("%s", jobs[i].cmdline);
    }
  }
}
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) {
  printf("Usage: shell [-hvp]\n");
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
  fprintf(stdout, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
  fprintf(stdout, "%s\n", msg);
  exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
  struct sigaction action, old_action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); /* block sigs of type being handled */
  action.sa_flags = SA_RESTART; /* restart syscalls if possible */

  if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
  return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(1);
}

void redirect_input(const char *input_file) {
    int fd = open(input_file, O_RDONLY); // opens file, read only
    if (fd < 0) {
        perror("open"); // program exits with error status
        exit(EXIT_FAILURE);
    }
    if (dup2(fd, STDIN_FILENO) < 0) {
        perror("dup2"); // program exits with error status
        exit(EXIT_FAILURE);
    }
    close(fd);
}

void redirect_output(const char *output_file, int append) {
    int fd;
    if (append) {
        // open file on write only, create if it doesn't exist, append to the end
        fd = open(output_file, O_WRONLY | O_CREAT | O_APPEND, S_IRWXU | S_IRWXG | S_IRWXO);
    } else {
        // open file on write only, create if it doesn't exist, truncate to 0 length
        fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
    }
    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE); // exits with failure
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
        perror("dup2"); // program exits with error status
        exit(EXIT_FAILURE);
    }
    close(fd);
}

void redirect_error(const char *error_file) {
  // open file on write only, create if it doesn't exist, truncate to 0 length
    int fd = open(error_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRWXU | S_IRWXG | S_IRWXO);
    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE); // exits with error
    }
    if (dup2(fd, STDERR_FILENO) < 0) {
        perror("dup2");
        exit(EXIT_FAILURE); // exits with error
    }
    close(fd);
}
