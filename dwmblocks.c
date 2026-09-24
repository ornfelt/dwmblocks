#include<errno.h>
#include<fcntl.h>
#include<poll.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<signal.h>
#include<sys/wait.h>
#include<time.h>
#ifndef NO_X
#include<X11/Xlib.h>
#endif
#ifdef __OpenBSD__
#define SIGPLUS			SIGUSR1+1
#define SIGMINUS		SIGUSR1-1
#else
#define SIGPLUS			SIGRTMIN
#define SIGMINUS		SIGRTMIN
#endif
#define LENGTH(X)               (sizeof(X) / sizeof (X[0]))
#define CMDLENGTH		100
#define MIN( a, b ) ( ( a < b) ? a : b )
#define STATUSLENGTH (LENGTH(blocks) * CMDLENGTH + 1)

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;
typedef struct {
	unsigned int signal;
	int button;
} SigEvent;
#ifndef __OpenBSD__
void dummysighandler(int num);
#endif
void sighandler(int signum, siginfo_t *si, void *ucontext);
void buttonhandler(const Block *block, int button);
void getcmds(int time);
void getsigcmds(unsigned int signal);
void setupsignals(void);
int getstatus(char *str, char *last);
void statusloop(void);
void termhandler(int signum);
void pstdout(void);
#ifndef NO_X
void setroot(void);
static void (*writestatus) (void) = setroot;
static int setupX(void);
static Display *dpy;
static int screen;
static Window root;
#else
static void (*writestatus) (void) = pstdout;
#endif


#include "blocks.h"

static char statusbar[LENGTH(blocks)][CMDLENGTH] = {0};
static char statusstr[2][STATUSLENGTH];
static volatile sig_atomic_t statusContinue = 1;
static int sigpipe[2];
static char *delimiter = delim;//delim from blocks.h, or the -d argument

//opens process *cmd and stores output in *output
void getcmd(const Block *block, char *output)
{
	//make sure status is same until output is ready
	char tempstatus[CMDLENGTH] = {0};
	int start = 0;
	//mark the block with its signal so dwm can tell which block was clicked
	if (block->signal)
		tempstatus[start++] = block->signal;
	strcpy(tempstatus+start, block->icon);
	FILE *cmdf = popen(block->command, "r");
	if (!cmdf)
		return;
	int i = strlen(tempstatus);
	fgets(tempstatus+i, CMDLENGTH-i-delimLen, cmdf);
	i = strlen(tempstatus);
	//only chop off newline if one is present at the end
	if (i != 0 && tempstatus[i-1] == '\n')
		tempstatus[--i] = '\0';
	//leave the block out if block and command output are both empty
	if (i == start)
		tempstatus[0] = '\0';
	else if (delimiter[0] != '\0')
		strncpy(tempstatus+i, delimiter, delimLen);
	strcpy(output, tempstatus);
	pclose(cmdf);
}

void getcmds(int time)
{
	const Block* current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if ((current->interval != 0 && time % current->interval == 0) || time == -1)
			getcmd(current,statusbar[i]);
	}
}

//runs the command of a clicked block in the background with BLOCK_BUTTON set,
//then signals dwmblocks to update the block from the command's normal output
void buttonhandler(const Block *block, int button)
{
	char shcmd[1024], btn[12];
	pid_t child;

	snprintf(btn, sizeof(btn), "%d", button);
	if (snprintf(shcmd, sizeof(shcmd), "%s\nkill -%d %d", block->command,
	             SIGMINUS+block->signal, (int)getpid()) >= (int)sizeof(shcmd))
		return;
	//fork twice so the command is reparented to init and never left as a zombie
	child = fork();
	if (child == 0) {
		if (fork() == 0) {
			int devnull = open("/dev/null", O_WRONLY);
			if (devnull != -1)
				dup2(devnull, STDOUT_FILENO);
			setenv("BLOCK_BUTTON", btn, 1);
			setsid();
			execl("/bin/sh", "sh", "-c", shcmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (child > 0)
		waitpid(child, NULL, 0);
}

void getsigcmds(unsigned int signal)
{
	const Block *current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if (current->signal == signal)
			getcmd(current,statusbar[i]);
	}
}

void setupsignals(void)
{
	//signals are queued on a pipe and handled in statusloop
	if (pipe(sigpipe) == -1) {
		perror("dwmblocks: pipe");
		exit(1);
	}
	for (int i = 0; i < 2; i++) {
		fcntl(sigpipe[i], F_SETFD, FD_CLOEXEC);
		fcntl(sigpipe[i], F_SETFL, O_NONBLOCK);
	}

#ifndef __OpenBSD__
	    /* initialize all real time signals with dummy handler */
    for (int i = SIGRTMIN; i <= SIGRTMAX; i++)
        signal(i, dummysighandler);
#endif

	struct sigaction sa = { .sa_sigaction = sighandler, .sa_flags = SA_SIGINFO | SA_RESTART };
	sigemptyset(&sa.sa_mask);
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].signal > 0)
			sigaction(SIGMINUS+blocks[i].signal, &sa, NULL);
	}

}

int getstatus(char *str, char *last)
{
	strcpy(last, str);
	str[0] = '\0';
	for (unsigned int i = 0; i < LENGTH(blocks); i++)
		strcat(str, statusbar[i]);
	if (strlen(str) >= strlen(delimiter))
		str[strlen(str)-strlen(delimiter)] = '\0';
	return strcmp(str, last);//0 if they are the same
}

#ifndef NO_X
void setroot(void)
{
	if (!getstatus(statusstr[0], statusstr[1]))//Only set root if text has changed.
		return;
	XStoreName(dpy, root, statusstr[0]);
	XFlush(dpy);
}

int setupX(void)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "dwmblocks: Failed to open display\n");
		return 0;
	}
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	return 1;
}
#endif

void pstdout(void)
{
	if (!getstatus(statusstr[0], statusstr[1]))//Only write out if text has changed.
		return;
	printf("%s\n",statusstr[0]);
	fflush(stdout);
}


void statusloop(void)
{
	struct pollfd pfd = { .fd = sigpipe[0], .events = POLLIN };
	struct timespec now, next;
	int i = 0, timeout;
	SigEvent ev;

	getcmds(-1);
	writestatus();
	clock_gettime(CLOCK_MONOTONIC, &next);
	next.tv_sec++;
	while (statusContinue) {
		//wait until the next second, handling signals as they come in
		clock_gettime(CLOCK_MONOTONIC, &now);
		timeout = (next.tv_sec - now.tv_sec) * 1000 + (next.tv_nsec - now.tv_nsec) / 1000000;
		if (timeout > 0 && poll(&pfd, 1, timeout) != 0) {
			while (read(sigpipe[0], &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.button) {
					for (unsigned int j = 0; j < LENGTH(blocks); j++)
						if (blocks[j].signal == ev.signal)
							buttonhandler(blocks + j, ev.button);
				} else
					getsigcmds(ev.signal);
			}
			writestatus();
			continue;
		}
		getcmds(++i);
		writestatus();
		clock_gettime(CLOCK_MONOTONIC, &next);
		next.tv_sec++;
	}
}

#ifndef __OpenBSD__
/* this signal handler should do nothing */
void dummysighandler(int signum)
{
    return;
}
#endif

void sighandler(int signum, siginfo_t *si, void *ucontext)
{
	//running the commands here isn't async-signal-safe, so just queue the signal.
	//dwm sends the clicked mouse button with sigqueue, a plain kill means update.
	int olderrno = errno;
	SigEvent ev = { signum-SIGPLUS, si->si_code == SI_QUEUE ? si->si_value.sival_int : 0 };
	write(sigpipe[1], &ev, sizeof(ev));
	errno = olderrno;
}

void termhandler(int signum)
{
	statusContinue = 0;
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {//Handle command line arguments
		if (!strcmp("-d",argv[i]) && i+1 < argc)
			delimiter = argv[++i];
		else if (!strcmp("-p",argv[i]))
			writestatus = pstdout;
	}
#ifndef NO_X
	if (!setupX())
		return 1;
#endif
	delimLen = MIN(delimLen, strlen(delimiter));
	delimiter[delimLen++] = '\0';
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	setupsignals();
	statusloop();
#ifndef NO_X
	XCloseDisplay(dpy);
#endif
	return 0;
}
