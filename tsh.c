/*
 * tsh - A tiny shell program with job control
 *
 * <Put your name and login ID here>
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Misc manifest constants */
#define MAXLINE 1024   /* max line size */
#define MAXARGS 128    /* max args on a command line */
#define MAXJOBS 16     /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

#define CMDNUM 4

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
char *buildin_cmd[CMDNUM] = {"quit", "fg", "bg", "jobs"};

struct job_t {             /* The job struct */
    pid_t pid;             /* job PID */
    int jid;               /* job ID [1, 2, ...] */
    int state;             /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE]; /* command line */
};
volatile struct job_t jobs[MAXJOBS]; /* The job list */
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

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);

void sigquit_handler(int sig);

void clearjob(volatile struct job_t *job);

void initjobs(volatile struct job_t *job_list);

int maxjid(volatile struct job_t *job_list);

int addjob(volatile struct job_t *job_list, pid_t pid, int state, char *cmdline);

int deletejob(volatile struct job_t *job_list, pid_t pid);

pid_t fgpid(volatile struct job_t *job_list);

volatile struct job_t *getjobpid(volatile struct job_t *job_list, pid_t pid);

volatile struct job_t *getjobjid(volatile struct job_t *job_list, int jid);

int pid2jid(volatile struct job_t *job_list, pid_t pid);

void listjobs(volatile struct job_t *job_list);

void usage(void);

void unix_error(char *msg);

void app_error(char *msg);

typedef void handler_t(int);

handler_t *Signal(int signum, handler_t *handler);

void sio_write_string(char *string);

void sio_write_int(int num);

char *sio_itos(int num);

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
            case 'p':            /* don't print a prompt */
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
    char *argv[MAXARGS];
    int bg = parseline(cmdline, argv);

    if (!argv[0]) {
        return;
    }

    if (builtin_cmd(argv)) {
        // build command
        do_bgfg(argv);
    } else {
        // exec new program in child process

        // block SIGCHLD signal
        sigset_t sigchld, sig_old;
        sigemptyset(&sigchld);
        sigaddset(&sigchld, SIGCHLD);
        sigaddset(&sigchld, SIGINT);
        sigaddset(&sigchld, SIGTSTP);
        sigprocmask(SIG_BLOCK, &sigchld, &sig_old);

        int pid = fork();
        if (pid == 0) {
            // child process

            // unblock signal
            sigprocmask(SIG_SETMASK, &sig_old, NULL);

            // set child process group to new group
            setpgid(0, 0);

            execve(argv[0], argv, NULL);

            // load program failure
            printf("%s: Command not found\n", argv[0]);
            exit(0);
        } else {
            // parent process
            if (bg) {
                // background job
                addjob(jobs, pid, BG, cmdline);

                sigprocmask(SIG_SETMASK, &sig_old, NULL);

                // print info
                printf("[%d] (%d) %s", pid2jid(jobs, pid), pid, cmdline);
            } else {
                // foreground job
                addjob(jobs, pid, FG, cmdline);

                sigprocmask(SIG_SETMASK, &sig_old, NULL);

                waitfg(pid);
            }
        }
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested an FG job.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' ';   /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    } else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        } else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    if (argc == 0) /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc - 1] == '&')) != 0) {
        argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv) {
    for (int i = 0; i < CMDNUM; ++i) {
        if (strcmp(buildin_cmd[i], argv[0]) == 0) {
            return 1;
        }
    }
    return 0; /* not a builtin command */
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    if (strcmp(argv[0], buildin_cmd[0]) == 0) {
        // quit
        exit(0);
    } else if (strcmp(argv[0], buildin_cmd[3]) == 0) {
        // jobs
        listjobs(jobs);
    } else {
        printf("do build in cmd\n");
    }
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    // todo: 优化逻辑
    while (fgpid(jobs) == pid) {
        sleep(1);
    }
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
    int old_errno = errno;

    sigset_t mask, old;
    sigemptyset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old);

    for (int i = 0; i < MAXJOBS; ++i) {
        int status_p;
        int pid = waitpid(jobs[i].pid, &status_p, WNOHANG | WUNTRACED);

        if (pid > 0) {
            if (WIFSTOPPED(status_p)) {
                // job pause

                //print message like "Job [1] (22203) stopped by signal 20"
                sio_write_string("Job [");
                sio_write_int(pid2jid(jobs, pid));
                sio_write_string("] (");
                sio_write_int(pid);
                sio_write_string(") stopped by signal ");
                sio_write_int(WSTOPSIG(status_p));
                sio_write_string("\n");

                volatile struct job_t *job = getjobpid(jobs, pid);
                job->state = ST;
            } else if (WIFSIGNALED(status_p)) {
                // job stopped by signal

                // print message like "Job [1] (20408) terminated by signal 2"
                sio_write_string("Job [");
                sio_write_int(pid2jid(jobs, pid));
                sio_write_string("] (");
                sio_write_int(pid);
                sio_write_string(") terminated by signal ");
                sio_write_int(WTERMSIG(status_p));
                sio_write_string("\n");

                deletejob(jobs, pid);
            } else if (WIFEXITED(status_p)) {
                // job return
                deletejob(jobs, pid);
            }
        }
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = old_errno;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig) {
    int old_errno = errno;

    sigset_t mask, old;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old);

    for (int i = 0; i < MAXJOBS; ++i) {
        if (jobs[i].state == FG) {
            kill(-jobs[i].pid, SIGINT);
            break;
        }
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = old_errno;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig) {
    int old_errno = errno;

    sigset_t mask, old;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, &old);

    for (int i = 0; i < MAXJOBS; ++i) {
        if (jobs[i].state == FG) {
            kill(-jobs[i].pid, SIGTSTP);
            break;
        }
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
    errno = old_errno;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(volatile struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(volatile struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(volatile struct job_t *job_list) {
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(volatile struct job_t *job_list, pid_t pid, int state, char *cmdline) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy((char *) job_list[i].cmdline, cmdline);
            if (verbose) {
                printf("Added job [%d] %d %s\n", job_list[i].jid, job_list[i].pid, job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(volatile struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(volatile struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
            return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
volatile struct job_t *getjobpid(volatile struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
volatile struct job_t *getjobjid(volatile struct job_t *job_list, int jid) {
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(volatile struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(volatile struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid != 0) {
            printf("[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            switch (job_list[i].state) {
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
                    printf("listjobs: Internal error: job[%d].state=%d ",
                           i, job_list[i].state);
            }
            printf("%s", job_list[i].cmdline);
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

void sio_write_string(char *string) {
    write(STDOUT_FILENO, string, strlen(string));
}

void sio_write_int(int num) {
    char *s = sio_itos(num);
    sio_write_string(s);
}

char *sio_itos(int num) {
    if (num == 0) {
        return "0";
    }

    static char s[12];
    int i = 0;
    int neg = 0;

    if (num < 0) {
        num *= -1;
        s[i++] = '-';
        neg = 1;
    }

    while (num) {
        int d = num % 10;
        num /= 10;
        s[i] = (char) (d + 48);
        ++i;
    }
    s[i] = '\0';

    int l = neg ? 1 : 0;
    int r = i - 1;
    while (l < r) {
        char t = s[l];
        s[l] = s[r];
        s[r] = t;
        ++l, --r;
    }

    return s;
}
