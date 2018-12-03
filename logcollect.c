/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#define NAME "logcollect"

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

/* program options */
static const char help_msg[] =
	NAME ": client for logcollectd\n"
	"usage:	" NAME " CMD [OPTIONS ...] [ARGS]\n"
	"\n"
	"Options\n"
	" -t, --tag=NAME	Tag using NAME\n"
	"\n"
	NAME " redirects stderr to a pipe and delivers\n"
	"the reading end to logcollectd\n"
	;
static const char optstring[] = "+?Vt:";

/* convenience wrapper for send with SCM_CREDENTIALS */
static int deliver_logcollect(int fd, const char *tag)
{
	int sock, ret;
	static const struct sockaddr_un peername = {
		.sun_family = AF_UNIX,
		.sun_path = "\0logcollectd",
	};
	union {
		struct cmsghdr hdr;
		char dat[CMSG_SPACE(sizeof(fd))];
	} cmsg = {
		.hdr.cmsg_len = CMSG_LEN(sizeof(fd)),
		.hdr.cmsg_level = SOL_SOCKET,
		.hdr.cmsg_type = SCM_RIGHTS,
	};
	struct iovec iov = {
		.iov_base = (void *)tag,
		.iov_len = strlen(tag),
	};
	const struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = &cmsg,
		.msg_controllen = cmsg.hdr.cmsg_len,
		.msg_name = (void *)&peername,
		.msg_namelen = sizeof(peername),
	};

	/* fill fd info */
	memcpy(CMSG_DATA(&cmsg.hdr), &fd, sizeof(fd));

	sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sock < 0)
		mylog(LOG_ERR, "socket: %s", ESTR(errno));
	ret = sendmsg(sock, &msg, 0);
	if (ret < 0)
		mylog(LOG_WARNING, "sendmsg: %s", ESTR(errno));
	close(sock);
	return ret;
}

__attribute__((unused))
static int ttytest(void)
{
	int fd;

	fd = open("/dev/tty", O_RDWR);
	close(fd);
	/* /dev/tty can only open when a controlling terminal is opened,
	 * /dev/console is not one of them
	 */
	return fd >= 0;
}

/* main process */
int main(int argc, char *argv[])
{
	int ret, opt;
	char *tag = NULL;

	/* parse program options */
	while ((opt = getopt(argc, argv, optstring)) != -1)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s: %s\n", NAME, VERSION);
		return 0;
	default:
		fprintf(stderr, "%s: option '%c' unrecognised\n", NAME, opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;

	case 't':
		tag = optarg;
		break;
	}

	openlog(NAME, LOG_PERROR, LOG_DAEMON);

	if (optind >= argc)
		mylog(LOG_ERR, "no command given");

	/* set argv to the start of the next program arguments */
	argv += optind;

	if (!tag)
		tag = getenv("NAME");
	if (!tag)
		tag = basename(*argv);

	if (1) {
		/* handover stderr */
		int pp[2];

		ret = pipe(pp);
		if (ret < 0)
			mylog(LOG_ERR, "pipe: %s", ESTR(errno));
		if (deliver_logcollect(pp[0], tag) >= 0) {
			if (dup2(pp[1], STDERR_FILENO) < 0)
				mylog(LOG_ERR, "dup2 %i %i: %s", pp[1], STDERR_FILENO, ESTR(errno));
			if (dup2(pp[1], STDOUT_FILENO) < 0)
				mylog(LOG_ERR, "dup2 %i %i: %s", pp[1], STDOUT_FILENO, ESTR(errno));
			mylog(LOG_INFO, "run '%s'", tag);
		} else {
			mylog(LOG_WARNING, "log pipe delivery failed, continue in straight mode");
		}
		close(pp[0]);
		close(pp[1]);
	}

	/* do something */
	execvp(*argv, argv);
	mylog(LOG_ERR, "execvp %s ...: %s", *argv, ESTR(errno));
	return 1;
}
