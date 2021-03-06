/* 
 * tsh - A tiny shell program with job control
 * 
 * ardxwe@gmail.com
 */

#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */
#define DEBUG 0           /* debug or not debug */

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
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 1;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to maskocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};

struct job_t jobs[MAXJOBS]; /* The job list */

bool isredirect;
sigset_t mask, prev;        /* signal */
bool runbg;                 /* runbg or not runbg */
volatile pid_t prevpid;     /* prev job pid */
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
void showbgjobs(struct job_t* jobs, char** argv);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);


/* just for debug*/
void printsigset(const sigset_t*);

/* ioredirection */
void ioredirection(char**);


/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */
    isredirect = false;

    /* Redirect stderr to stdout (so that driver will get mask output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                usage();
        }
    }

    /* Instmask the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);


    /* Execute the shell's read/eval loop */
    while (1) {
        if (isredirect) {
            if (freopen("/dev/tty", "r", stdin) == NULL) {
                unix_error("io redirection error.");
            }
            if (freopen("/dev/tty", "w", stdout) == NULL) {
                unix_error("io redirection error.");
            }
            isredirect = false;
        }

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
void eval(char *cmdline) 
{
#if DEBUG
    printf("cmdline       :   %s", cmdline);
    fflush(stdout);
#endif

    pid_t pid;
    char* argv[MAXARGS];
    runbg = parseline(cmdline, argv);

    ioredirection(argv);
    sigemptyset(&mask);

    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask,SIGTSTP);

#if DEBUG
    printf("first mask    :   ");
    fflush(stdout);
    printsigset(&mask);
#endif

    sigprocmask(SIG_BLOCK, &mask, &prev);
#if DEBUG
    printf("prev          :   ");
    fflush(stdout);
    printsigset(&prev);
#endif

    if (builtin_cmd(argv)){
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return;
    }

    if ((pid = fork()) < 0){
        unix_error("fork error");
    }
    else if (pid == 0){
        /*child process */
        setpgid(0, 0);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        Signal(SIGINT, SIG_DFL);
        Signal(SIGTSTP, SIG_DFL);
        Signal(SIGCHLD, SIG_DFL);
        Signal(SIGQUIT, SIG_DFL);

#if DEBUG
        printf("\n");
        fflush(stdout);
        printf("child process\npid: (%d)", getpid());
        fflush(stdout);
        printf("cmdline: %s", cmdline);
        fflush(stdout);
#endif

        execv(argv[0], argv);
        /* error */
        printf("%s: Command not found\n", argv[0]);
        fflush(stdout);
        exit(0);
    }
    else {
        /* father process */
        prevpid = pid;

#if DEBUG
        printf("\n");
        fflush(stdout);
        printf("father process pid: (%d)\n", getpid());
        fflush(stdout);
        printf("childdddd%d\n", pid);
#endif

        sigprocmask(SIG_SETMASK, &prev, NULL);
        sigprocmask(SIG_BLOCK, &mask, &prev);
        addjob(jobs, pid, runbg ? BG : FG, cmdline);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        
        if(runbg){
            sigprocmask(SIG_BLOCK, &mask, &prev);
            struct job_t* currentjob =  getjobpid(jobs, pid);
            fprintf(stdout, "[%d] (%d) %s", currentjob->jid, pid, cmdline);
            fflush(stdout);
            sigprocmask(SIG_SETMASK, &prev, NULL);
        }
        else {
            sigset_t childset, emptyset;
            sigemptyset(&childset);
            sigfillset(&emptyset);

            sigaddset(&childset, SIGCHLD);
            sigaddset(&childset, SIGTSTP);

            sigdelset(&emptyset, SIGINT);
            sigdelset(&emptyset, SIGCHLD);
            sigdelset(&emptyset, SIGTSTP);

#if DEBUG

            fflush(stdout);
            printf("before suspend:   ");
            fflush(stdout);
            printsigset(&childset);
            fflush(stdout);
#endif

            sigprocmask(SIG_BLOCK, &childset, &prev);
            while(prevpid != 0) {
                sigsuspend(&emptyset);
            }
            sigprocmask(SIG_SETMASK, &prev, NULL);
        }
    }

    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) { /* ignore leading spaces */
	    buf++;
    }

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
	    delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) { /* ignore spaces */
            buf++;
        }

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    if (argc == 0) {  /* ignore blank line */
	    return 1;
    }

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	    argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    if (strcmp(argv[0], "quit") == 0){
        exit(0);
    }
    else if (strcmp(argv[0], "jobs") == 0){

#if DEBUG
        printf("start jobs\n");
        fflush(stdout);
#endif

        sigprocmask(SIG_BLOCK, &mask, NULL);
        showbgjobs(jobs, argv+1);
        sigprocmask(SIG_SETMASK, &prev, NULL);
    }

    else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0){
        sigprocmask(SIG_BLOCK, &mask, NULL);
        do_bgfg(argv);
        sigprocmask(SIG_SETMASK, &prev, NULL);
    }
    else {
        return 0;     /* not a builtin command */
    }
    return 1;
}

void showbgjobs(struct job_t *jobs, char** argv){
    int i;
    FILE* fp = NULL;
    for (i = 0; i < MAXJOBS; i++) {
        if(jobs[i].pid != 0){
            if (argv[0] != NULL && strcmp(argv[0], ">")) {
                fp = fopen(argv[1], "r");
                fprintf(fp, "[%d] (%d) ", jobs[i].jid, jobs[i].pid);
                fprintf(fp, "Running ");
                fprintf(fp, "%s", jobs[i].cmdline);
            }
            else {
                printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
                switch(jobs[i].state) {
                    case BG:
                    case FG:
                        printf("Running ");
                        fflush(stdout);
                        break;
                    case ST:
                        printf("Stopped ");
                        fflush(stdout);
                        break;
                }
                printf("%s", jobs[i].cmdline);
                fflush(stdout);
            }
        }
    }
    return;
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    struct job_t* currentjob;
    if (argv[1] == NULL){
        printf("%s command requeires PID or %%jobid argument\n", argv[0]);
        fflush(stdout);
        return;
    }
    else if ((argv[1][0] != '%') && ((argv[1][0] < '0') || (argv[1][0] > '9'))) {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        fflush(stdout);
        return;
    }

    else if (argv[1][0] == '%') {
        currentjob = getjobjid(jobs, (pid_t)atoi(argv[1] + 1));
        if (currentjob  == NULL) {
            printf("No such job\n");
            fflush(stdout);
            return;
        }
    }

    else {
        currentjob = getjobpid(jobs, atoi(argv[1]));
        if (currentjob == NULL) {
            printf("No such process\n");
            fflush(stdout);
            return;
        }
    }
    if (strcmp(argv[0], "bg") == 0){
        currentjob->state = BG;
        printf("[%d] (%d) %s", currentjob->jid, currentjob->pid, currentjob->cmdline);
        fflush(stdout);
        kill(-(currentjob->pid), SIGCONT);
    }
    else {
        currentjob->state = FG;
        waitfg(currentjob->pid);
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    sigset_t childset, emptyset;
    sigemptyset(&childset);
    sigfillset(&emptyset);

    sigaddset(&childset, SIGCHLD);
    sigaddset(&childset, SIGTSTP);
    sigdelset(&emptyset, SIGCHLD);
    sigdelset(&emptyset, SIGTSTP);

    sigprocmask(SIG_BLOCK, &childset, NULL);
    kill(-(pid), SIGCONT);
    /* wait for sigcont */
    sigsuspend(&emptyset);
    /* wait for sigchld */
    sigsuspend(&emptyset);
    sigprocmask(SIG_SETMASK, &prev, NULL);
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps mask
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{

#if DEBUG
    printf("sigchld handle call\n");
    fflush(stdout);
#endif

    int olderrno = errno, status;
    pid_t pid;
    sigprocmask(SIG_BLOCK, &mask, NULL);
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0){
        if(pid == prevpid) {
            prevpid = 0;
        }
        if (WIFSTOPPED(status)) {

            struct job_t* job = getjobpid(jobs, pid);
            if (job->state == ST);
            else {
                job->state = ST;
                printf("Job [%d] (%d) stoped by signal %d\n", pid2jid(pid), pid, SIGTSTP);
                fflush(stdout);
            } 
        }
        else if(WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGINT) {
                if (getjobpid(jobs, pid) != NULL) {
                    printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, SIGINT);
                    fflush(stdout);
                }
            }
            deletejob(jobs, pid);
        }
        else if(WIFEXITED(status)){

#if DEBUG
            printf("退出值为 %d\n", WEXITSTATUS(status));
            fflush(stdout);
#endif

            deletejob(jobs, pid);
        }
        else if (WIFCONTINUED(status)) {
            printf("continued\n");
            fflush(stdout);
        }

#if DEBUG
        printf("childhandle pid: %d\n", pid);
        fflush(stdout);
#endif

    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    errno = olderrno;
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{

#if DEBUG
    printf("sigint handle call\n");
    fflush(stdout);
#endif

    sigprocmask(SIG_BLOCK, &mask, NULL);
    pid_t pid = fgpid(jobs);
    struct job_t* job = getjobpid(jobs, pid);
    if (job != NULL) {
        printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, SIGINT);
        fflush(stdout);
        deletejob(jobs, pid);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);

    if (pid) {
        kill(-pid, SIGINT);
    }
    else {
        exit(0);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{

#if DEBUG
    printf("sig: %d\n", sig);
    fflush(stdout);
    printf("sigstophandle\n");
    fflush(stdout);
#endif

    sigprocmask(SIG_BLOCK, &mask, NULL);
    pid_t pid = fgpid(jobs);
    struct job_t* job = getjobpid(jobs, pid);
    if (job) {
        job->state = ST;
        fflush(stdout);
        kill(-pid, SIGTSTP);
        printf("Job [%d] (%d) stoped by signal %d\n", pid2jid(pid), pid, SIGTSTP);
        fflush(stdout);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    fflush(stdout);
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
    for (int i = 0; i < MAXJOBS; i++) {
	    clearjob(&jobs[i]);
    }
}

/* maxjid - Returns largest maskocated job ID */
int maxjid(struct job_t *jobs) 
{
    int max = 0;

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid > max) {
            max = jobs[i].jid;
        }
    }
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    if (pid < 1) {
	    return 0;
    }
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS) {
                nextjid = 1;
            }
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                /* printf("[%d] %d %s", jobs[i].jid, jobs[i].pid, jobs[i].cmdline); */
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    if (pid < 1) {
	    return 0;
    }
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].state == FG) {
            return jobs[i].pid;
        }
    }
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    if (pid < 1) {
	    return NULL;
    }
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            return &jobs[i];
        }
    }
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    if (jid < 1) {
	    return NULL;
    }
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid == jid)
            return &jobs[i];
    }
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    if (pid < 1) {
	    return 0;
    }
    for (int i = 0; i < MAXJOBS; i++) {
	    if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            fflush(stdout);
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
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                    i, jobs[i].state);
            }
            fflush(stdout);
            printf("%s", jobs[i].cmdline);
            fflush(stdout);
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
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscmasks if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

/* do ioredirection */
void ioredirection(char** argv) {
    int endcmd = 0x7FFFFFFF;
    for (int i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "<") == 0) {
            isredirect = true;
            if(freopen(argv[i+1], "r", stdin) == NULL) {
                return unix_error("io redirection error.\n");
            }
            if(endcmd > i) {
                endcmd = i;
            }
        }
        else if (strcmp(argv[i], ">") == 0) {
            isredirect = true;
            if (freopen(argv[i+1],"a",stdout) == NULL) {
                return unix_error("io redirection error.\n");
            }
            if (endcmd > i) {
                endcmd = i;
            }
        }
    }
    if(endcmd != 0x7FFFFFFF) {
        argv[endcmd] = NULL;
    }
}

/* print sig set */
void printsigset(const sigset_t *pset)
{
    for (int i = 0; i < 64; i++)
    {
        if (sigismember(pset, i + 1))
            putchar('1');
        else
            putchar('0');
        fflush(stdout);
    }
    printf("\n");
    fflush(stdout);
}
