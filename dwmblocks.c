#include<errno.h>
#include<fcntl.h>
#include<poll.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<unistd.h>
#include<signal.h>
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
#ifndef __OpenBSD__
void dummysighandler(int num);
#endif
void sighandler(int num);
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

//opens process *cmd and stores output in *output
void getcmd(const Block *block, char *output)
{
	//make sure status is same until output is ready
	char tempstatus[CMDLENGTH] = {0};
	strcpy(tempstatus, block->icon);
	FILE *cmdf = popen(block->command, "r");
	if (!cmdf)
		return;
	int i = strlen(block->icon);
	fgets(tempstatus+i, CMDLENGTH-i-delimLen, cmdf);
	i = strlen(tempstatus);
	//only chop off newline if one is present at the end
	if (i != 0 && tempstatus[i-1] == '\n')
		tempstatus[--i] = '\0';
	//if block and command output are both not empty
	if (i != 0 && delim[0] != '\0')
		strncpy(tempstatus+i, delim, delimLen);
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

	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].signal > 0)
			signal(SIGMINUS+blocks[i].signal, sighandler);
	}

}

int getstatus(char *str, char *last)
{
	strcpy(last, str);
	str[0] = '\0';
	for (unsigned int i = 0; i < LENGTH(blocks); i++)
		strcat(str, statusbar[i]);
	if (strlen(str) >= strlen(delim))
		str[strlen(str)-strlen(delim)] = '\0';
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
	int i = 0, sig, timeout;

	getcmds(-1);
	writestatus();
	clock_gettime(CLOCK_MONOTONIC, &next);
	next.tv_sec++;
	while (statusContinue) {
		//wait until the next second, handling signals as they come in
		clock_gettime(CLOCK_MONOTONIC, &now);
		timeout = (next.tv_sec - now.tv_sec) * 1000 + (next.tv_nsec - now.tv_nsec) / 1000000;
		if (timeout > 0 && poll(&pfd, 1, timeout) != 0) {
			while (read(sigpipe[0], &sig, sizeof(sig)) == sizeof(sig))
				getsigcmds(sig);
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

void sighandler(int signum)
{
	//running the commands here isn't async-signal-safe, so just queue the signal
	int olderrno = errno, sig = signum-SIGPLUS;
	write(sigpipe[1], &sig, sizeof(sig));
	errno = olderrno;
}

void termhandler(int signum)
{
	statusContinue = 0;
}

int main(int argc, char** argv)
{
	for (int i = 0; i < argc; i++) {//Handle command line arguments
		if (!strcmp("-d",argv[i]))
			strncpy(delim, argv[++i], delimLen);
		else if (!strcmp("-p",argv[i]))
			writestatus = pstdout;
	}
#ifndef NO_X
	if (!setupX())
		return 1;
#endif
	delimLen = MIN(delimLen, strlen(delim));
	delim[delimLen++] = '\0';
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	setupsignals();
	statusloop();
#ifndef NO_X
	XCloseDisplay(dpy);
#endif
	return 0;
}
