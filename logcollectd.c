/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <grp.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "lib/libt.h"
#include "lib/libe.h"

#define NAME "logcollectd"

/* logging */
__attribute__((format(printf,2,3)))
static inline
void mylog(int level, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vsyslog(level, fmt, va);
	va_end(va);
	if (level <= LOG_ERR)
		exit(EXIT_FAILURE);
}
#define ESTR(x) strerror(x)

static int logsock = -1;
static const struct {
	short sun_family;
	char sun_path[9];
} logname = {
	.sun_family = AF_UNIX,
	.sun_path = "/dev/log",
};

static void connect_logsock(void)
{
	int ret, sock;

	if (logsock >= 0)
		return;

	ret = sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (ret < 0)
		mylog(LOG_ERR, "socket: %s", ESTR(errno));
	ret = connect(sock, (void *)&logname, sizeof(logname));
	if (ret < 0) {
		if (close(sock) < 0)
			mylog(LOG_ERR, "close %u: %s", sock, ESTR(errno));
	} else {
		mylog(LOG_NOTICE, "connected to %s", logname.sun_path);
		logsock = sock;
	}
}
static void disconnected_logsock(void)
{
	mylog(LOG_NOTICE, "disconnected from %s", logname.sun_path);
	logsock = -1;
}


static void on_data(int fd, void *dat)
{
	static char hdr[1024];
	static char buf[16*1024+1];
	static char timestr[128];
	const char *label = dat;
	char *tok;
	int ret;

	ret = read(fd, buf, sizeof(buf)-1);
	if (ret < 0)
		mylog(LOG_ERR, "read '%s' %i: %s", label, fd, ESTR(errno));
	if (!ret) {
		mylog(LOG_NOTICE, "eof logging '%s'", label);
		libe_remove_fd(fd);
		if (close(fd) < 0)
			mylog(LOG_ERR, "close '%s' %i: %s", label, fd, ESTR(errno));
		free(dat);
		return;
	}
	/* null terminate */
	buf[ret] = 0;

	/* produce time string */
	time_t now;
	time(&now);
	strftime(timestr, sizeof(timestr), "%b %e %T", gmtime(&now));

	/* produce header */
	snprintf(hdr, sizeof(hdr), "<%u>%s %s: ", LOG_NOTICE | LOG_LOCAL6,
			timestr, label);

	/* prepare static 1st item of iovec */
	struct iovec iov[2] = {
		{
			.iov_base = hdr,
			.iov_len = strlen(hdr),
		},
	};
	struct msghdr msg = {
		.msg_iov = iov,
		.msg_iovlen = 2,
	};

	connect_logsock();
	/* forward data, line-by-line */
	for (tok = strtok(buf, "\r\n"); tok && logsock >= 0; tok = strtok(NULL, "\r\n")) {
		if (!tok)
			continue;
		/* fill 2nd part of iovec */
		iov[1].iov_base = tok;
		iov[1].iov_len = strlen(tok);
		if (sendmsg(logsock, &msg, MSG_DONTWAIT) < 0) {
			if (errno == EAGAIN)
				break;
			mylog(LOG_WARNING, "sendmsg: %s", ESTR(errno));
			disconnected_logsock();
			break;
		}
	}
}

/* globals */
static void on_mysock(int fd, void *dat)
{
	static char text[128];
	static char cmsgdat[CMSG_SPACE(sizeof(struct ucred))];

	int ret, len;
	struct iovec iov = {
		.iov_base = text,
		.iov_len = sizeof(text)-1,
	};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = cmsgdat,
		.msg_controllen = sizeof(cmsgdat),
	};
	struct cmsghdr *cmsg;
	int peersock;

	/* recv */
	len = ret = recvmsg(fd, &msg, 0);
	if (ret < 0)
		mylog(LOG_ERR, "recv ctrldat: %s", ESTR(errno));

	/* null terminate */
	text[len] = 0;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
			cmsg->cmsg_type == SCM_RIGHTS) {
		memcpy(&peersock, CMSG_DATA(cmsg), sizeof(peersock));
	} else {
		mylog(LOG_WARNING, "received log request without file discriptor for '%s'", text);
		return;
	}
	mylog(LOG_NOTICE, "new log request '%s'", text);
	libe_add_fd(peersock, on_data, strdup(text));
}

static void on_signalfd(int fd, void *dat)
{
	int ret;
	struct signalfd_siginfo info;

	/* signals */
	ret = read(fd, &info, sizeof(info));
	if (ret < 0)
		mylog(LOG_ERR, "read signalfd: %s", ESTR(errno));
	switch (info.ssi_signo) {
	case SIGINT:
	case SIGTERM:
		mylog(LOG_WARNING, "terminated");
		exit(0);
		break;
	}
}

/* main process */
int main(int argc, char *argv[])
{
	int ret, fd;

	openlog(NAME, LOG_PERROR, LOG_DAEMON);
	/* setup signals */
	sigset_t set;
	sigfillset(&set);
	sigprocmask(SIG_BLOCK, &set, NULL);
	fd = ret = signalfd(-1, &set, 0);
	if (ret < 0)
		mylog(LOG_ERR, "signalfd failed: %s", ESTR(errno));
	libe_add_fd(fd, on_signalfd, NULL);

	/* open server socket */
	struct sockaddr_un myname = {
		.sun_family = AF_UNIX,
		.sun_path = "\0logcollectd",
	};
	fd = ret = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (ret < 0)
		mylog(LOG_ERR, "socket(unix, ...) failed: %s", ESTR(errno));
	ret = bind(fd, (void *)&myname, sizeof(myname));
	if (ret < 0)
		mylog(LOG_ERR, "bind(@%s) failed: %s", myname.sun_path+1, ESTR(errno));
	libe_add_fd(fd, on_mysock, NULL);

	/* happy logging ... */
	for (;;) {
		libt_flush();

		ret = libe_wait(libt_get_waittime());
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			/* no test for EINTR using signalfd ... */
			mylog(LOG_ERR, "libe_wait: %s", ESTR(errno));
		libe_flush();
	}
	/* not reachable */
	return EXIT_SUCCESS;
}
