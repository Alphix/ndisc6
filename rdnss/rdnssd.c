/**
 * rdnssd.c - daemon for DNS configuration from ICMPv6 RA
 */

/*************************************************************************
 *  Copyright © 2007 Pierre Ynard, Rémi Denis-Courmont.                  *
 *  This program is free software: you can redistribute and/or modify    *
 *  it under the terms of the GNU General Public License as published by *
 *  the Free Software Foundation, versions 2 or 3 of the license.        *
 *                                                                       *
 *  This program is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 *  GNU General Public License for more details.                         *
 *                                                                       *
 *  You should have received a copy of the GNU General Public License    *
 *  along with this program. If not, see <http://www.gnu.org/licenses/>. *
 *************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <locale.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <poll.h>
#include <errno.h>
#include <resolv.h>
#include <sys/wait.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>

#include "rdnssd.h"
#include "gettext.h"
#include "../src/gettime.h"

/* Internal defines */

static time_t now;

typedef struct
{
	struct in6_addr addr;
	unsigned int    ifindex;
	time_t          expiry;
} rdnss_t;

#define MAX_DOMAINLEN 256 // Maximum length according to resolv.conf(5)

typedef struct
{
	char          domain[MAX_DOMAINLEN + 1];
	time_t        expiry;
} dnssl_t;

#define MAX_RDNSS MAXNS

static struct
{
	size_t  count;
	rdnss_t list[MAX_RDNSS];
} servers = { .count = 0 };

static struct
{
	size_t  count;
	dnssl_t list[MAXDNSRCH]; // MAXDNSRCH defined in resolv.h
} domains = { .count = 0 };

/* The code */

static void write_resolv(const char *resolvpath)
{
	FILE *resolv;
	int rval;
	char tmpfile[strlen(resolvpath) + sizeof(".tmp")];

	sprintf(tmpfile, "%s.tmp", resolvpath);

	resolv = fopen(tmpfile, "w");

	if (! resolv) {
		syslog (LOG_ERR, _("Cannot write %s: %m"), tmpfile);
		return;
	}

	if (domains.count > 0) {
		fprintf(resolv, "search");

		for (size_t i = 0; i < domains.count; i++) {
			fprintf(resolv, " %s", domains.list[i].domain);
		}

		fprintf(resolv, "\n");
	}

	for (size_t i = 0; i < servers.count; i++) {
		char addr[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &servers.list[i].addr, addr, INET6_ADDRSTRLEN);

		if (IN6_IS_ADDR_LINKLOCAL(&servers.list[i].addr)) {
			char iface[IFNAMSIZ];
			if (if_indextoname(servers.list[i].ifindex, iface) != NULL)
				fprintf(resolv, "nameserver %s%%%s\n", addr, iface);
		} else
			fprintf(resolv, "nameserver %s\n", addr);
	}

	fclose(resolv);

	rval = rename(tmpfile, resolvpath);

	if (rval == -1)
		syslog(LOG_ERR, _("Cannot write %s: %m"), tmpfile);

}

static void trim_expired (void)
{
	while (domains.count > 0
	       && domains.list[domains.count - 1].expiry <= now)
		domains.count--;

	while (servers.count > 0
	       && servers.list[servers.count - 1].expiry <= now)
		servers.count--;
}

static int dnssl_older (const void *a, const void *b)
{
	time_t ta = ((const dnssl_t *)a)->expiry;
	time_t tb = ((const dnssl_t *)b)->expiry;

	if (ta < tb)
		return 1;
	if (ta > tb)
		return -1;
	return 0;
}

static int rdnss_older (const void *a, const void *b)
{
	time_t ta = ((const rdnss_t *)a)->expiry;
	time_t tb = ((const rdnss_t *)b)->expiry;

	if (ta < tb)
		return 1;
	if (ta > tb)
		return -1;
	return 0;
}

static void dnssl_update (const char *domain, time_t expiry)
{
	size_t i;

	/* Does this entry already exist? */
	for (i = 0; i < domains.count; i++)
	{
		if (strcmp (domain, domains.list[i].domain) == 0)
			break;
	}

	/* Add a new entry */
	if (i == domains.count)
	{
		if (expiry == now)
			return; /* Do not add already expired entry! */

		if (domains.count < MAXDNSRCH)
			i = domains.count++;
		else
		{
			/* No more room? replace the most obsolete entry */
			if ((expiry - domains.list[MAXDNSRCH - 1].expiry) >= 0)
				i = MAXDNSRCH - 1;
			else
				/* Do not write after end of the table */
				return;
		}
	}

	/* MAX_DOMAINLEN added only as "to be sure" check, such entry should not get here */
	strncpy (domains.list[i].domain, domain, MAX_DOMAINLEN);
	domains.list[i].expiry = expiry;

	qsort (domains.list, domains.count, sizeof (dnssl_t), dnssl_older);

#ifndef NDEBUG
	for (unsigned i = 0; i < domains.count; i++)
	{
		syslog (LOG_DEBUG, "%u: %s expires at %u\n", i, domains.list[i].domain,
		        (unsigned)domains.list[i].expiry);
	}
#endif
}

static void rdnss_update (const struct in6_addr *addr, unsigned int ifindex, time_t expiry)
{
	size_t i;

	/* Does this entry already exist? */
	for (i = 0; i < servers.count; i++)
	{
		if (memcmp (addr, &servers.list[i].addr, sizeof (*addr)) == 0
		    && (! IN6_IS_ADDR_LINKLOCAL(addr)
		        || ifindex == servers.list[i].ifindex))
			break;
	}

	/* Add a new entry */
	if (i == servers.count)
	{
		if (expiry == now)
			return; /* Do not add already expired entry! */

		if (servers.count < MAX_RDNSS)
			i = servers.count++;
		else
		{
			/* No more room? replace the most obsolete entry */
			if ((expiry - servers.list[MAX_RDNSS - 1].expiry) >= 0)
				i = MAX_RDNSS - 1;
			else
				/* Do not write after end of the table */
				return;
		}
	}

	memcpy (&servers.list[i].addr, addr, sizeof (*addr));
	servers.list[i].ifindex = ifindex;
	servers.list[i].expiry = expiry;

	qsort (servers.list, servers.count, sizeof (rdnss_t), rdnss_older);

#ifndef NDEBUG
	for (unsigned i = 0; i < servers.count; i++)
	{
		char buf[INET6_ADDRSTRLEN];
		inet_ntop (AF_INET6, &servers.list[i].addr, buf,
		           sizeof (buf));
		syslog (LOG_DEBUG, "%u: %48s expires at %u\n", i, buf,
		        (unsigned)servers.list[i].expiry);
	}
#endif
}

void parse_rdnss (const struct nd_opt_hdr *opt, unsigned int ifindex)
{
	struct nd_opt_rdnss *rdnss_opt;
	size_t nd_opt_len = opt->nd_opt_len;
	uint64_t lifetime;

	if (nd_opt_len < 3 /* too short per RFC */
			|| (nd_opt_len & 1) == 0) /* bad (even) length */
		return;

	rdnss_opt = (struct nd_opt_rdnss *) opt;

	{
		struct timespec ts;
		mono_gettime (&ts);
		now = ts.tv_sec;
	}

	lifetime = (uint64_t)now +
	           (uint64_t)ntohl(rdnss_opt->nd_opt_rdnss_lifetime);
	/* This should fit in a time_t */
	if (lifetime > INT32_MAX)
		lifetime = INT32_MAX;

	for (struct in6_addr *addr = (struct in6_addr *) (rdnss_opt + 1);
			 nd_opt_len >= 2; addr++, nd_opt_len -= 2)
		rdnss_update(addr, ifindex, lifetime);
}

void parse_dnssl (const struct nd_opt_hdr *opt)
{
	struct nd_opt_dnssl *dnssl_opt;
	size_t nd_opt_len = opt->nd_opt_len;
	uint64_t lifetime;
	char *dom_list;

	dnssl_opt = (struct nd_opt_dnssl *) opt;

	if (nd_opt_len < 2) /* too short per RFC */
		return;

	{
		struct timespec ts;
		mono_gettime (&ts);
		now = ts.tv_sec;
	}

	lifetime = (uint64_t)now +
		(uint64_t)ntohl(dnssl_opt->nd_opt_dnssl_lifetime);

	if (lifetime > INT32_MAX)
		lifetime = INT32_MAX;

	/* We need exact length of the option in bytes */
	nd_opt_len = (nd_opt_len - 1) << 3;
	/* We jump right after the header where the domain names are */
	dom_list = (char *) (dnssl_opt + 1);

	while (nd_opt_len > 0)
	{
		char domain[MAX_DOMAINLEN];
		size_t total_length = 0;
		while (*dom_list != '\0')
		{
			/* extract the length of next label */
			size_t len = * (uint8_t *) dom_list;
			dom_list++;
			nd_opt_len--;

			/* exit if longer than option */
			if(len > nd_opt_len)
				return;

			/* check the space in domain[] */
			if((total_length + len + 1) > MAX_DOMAINLEN)
				return;

			/* if this is not the first label */
			if (total_length > 0)
			{
				/* put dot before the label */
				domain[total_length] = '.';
				/* be sure to make room for a dot */
				total_length++;
				/* it will be put instead of the trailing zero byte
				 * character but for that we will reallocate now anyway */
			}

			/* copy next label */
			strncpy(domain + total_length, dom_list, len);

			/* move all the pointers and numbers */
			total_length += len;
			dom_list += len;
			nd_opt_len -= len;
		}

		/* if the length was zero */
		if (total_length == 0)
		{
			/* move to next byte */
			nd_opt_len--;
			dom_list++;
		}
		else
		{ /* otherwise we actually extracted something, so
			 * we put zero byte at the end of the domain name instead of dot */
			domain[total_length] = '\0';
			/* and update the table */
			dnssl_update(domain, lifetime);
		}

	}

}

int parse_nd_opts (const struct nd_opt_hdr *opt, size_t opts_len, unsigned int ifindex)
{
	for (; opts_len >= sizeof(struct nd_opt_hdr);
	     opts_len -= opt->nd_opt_len << 3,
	     opt = (const struct nd_opt_hdr *)
		   ((const uint8_t *) opt + (opt->nd_opt_len << 3))) {

		if (opt->nd_opt_len == 0
		 || opts_len < ((size_t)opt->nd_opt_len << 3))
			return -1;

		switch (opt->nd_opt_type)
		{
		case ND_OPT_RDNSS:
			parse_rdnss(opt, ifindex);
			break;

		case ND_OPT_DNSSL:
			parse_dnssl(opt);
			break;

		default:
			continue;
		}

	}

	return 0;

}

static int drop_privileges (const char *username)
{
	if (username == NULL)
		return 0;

	struct passwd *pw = getpwnam (username);
	if (pw == NULL)
	{
		syslog (LOG_ERR, _("Cannot find user \"%s\""), username);
		return -1;
	}
	if (setgid (pw->pw_gid))
	{
		syslog (LOG_CRIT, _("Fatal error (%s): %m"), "setgid");
		return -1;
	}
	if (initgroups (username, pw->pw_gid))
	{
		syslog (LOG_CRIT, _("Fatal error (%s): %m"), "initgroups");
		return -1;
	}
	if (setuid (pw->pw_uid))
	{
		syslog (LOG_CRIT, _("Fatal error (%s): %m"), "setuid");
		return -1;
	}
	return 0;
}

static void prepare_fd (int fd)
{
	fcntl (fd, F_SETFD, FD_CLOEXEC);
	fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
}

static volatile int termsig = 0;

static void term_handler (int signum)
{
	termsig = signum;
}

static void ignore_handler (int signum)
{
	(void)signum;
}

static int worker (int pipe, const char *resolvpath, const char *username)
{
	sigset_t emptyset;
	int rval = 0, sock = -1;
	const rdnss_src_t *src;

#ifdef __linux__
	src = &rdnss_netlink;
	sock = src->setup ();
#endif

	if (sock == -1)
	{
		src = &rdnss_icmp;
		sock = src->setup ();
	}

	if (sock == -1)
		return -1;

	prepare_fd (sock);
	/* be defensive - we want to block on poll(), not recv() */

	if (drop_privileges(username) < 0) {
		close(sock);
		return -1;
	}

	sigemptyset (&emptyset);

	for (bool ready = false; termsig == 0;)
	{
		struct pollfd pfd =
			{ .fd = sock,  .events = POLLIN, .revents = 0 };
		struct timespec ts;
		char buf = 42;

		mono_gettime (&ts);
		now = ts.tv_sec;

		if (ready)
		{
			/* Flush out expired entries */
			trim_expired ();
			/* Update resolv.conf */
			write_resolv (resolvpath);
			/* Notify manager process */
			write (pipe, &buf, sizeof(buf));
		}

		if (servers.count)
		{
			/* Compute event deadline, min of servers and domains */
			time_t expiry = servers.list[servers.count - 1].expiry;
			if ((domains.count) &&
			    (domains.list[domains.count - 1].expiry < expiry))
				expiry = domains.list[domains.count - 1].expiry;
			if (ts.tv_sec < expiry)
				ts.tv_sec = expiry - ts.tv_sec;
			else
				ts.tv_sec = 0;
			ts.tv_nsec = 0;
		}

		if (ppoll (&pfd, 1, servers.count ? &ts : NULL, &emptyset) < 0)
		{
			if (errno == EINTR)
				continue;

			syslog (LOG_CRIT, _("Fatal error (%s): %m"), "ppoll");
			rval = -1;
			break;
		}

		if (pfd.revents)
		{
			/* Receive new server from kernel */
			src->process (sock);
			/* From now on, we can write resolv.conf
			 * TODO: send unsoliticited RS to avoid this hack */
			ready = true;
		}
	}

	close (sock);
	return rval;
}

static void merge_hook (const char *hookpath)
{
	pid_t pid = fork ();

	switch (pid)
	{
		case 0:
			execl (hookpath, hookpath, (char *)NULL);
			syslog (LOG_ERR, _("Cannot run \"%s\": %m"), hookpath);
			exit(1);

		case -1:
			syslog (LOG_ERR, _("Cannot run \"%s\": %m"), hookpath);
			break;

		default:
		{
			int status;
			while (waitpid (pid, &status, 0) != pid);
		}
	}

}

static int manager (int pipe, const char *hookpath)
{
	int rval = 0;
	sigset_t emptyset;

	sigemptyset(&emptyset);

	while (termsig == 0)
	{
		struct pollfd pfd = { .fd = pipe,  .events = POLLIN, .revents = 0 };
		char buf;

		if (ppoll(&pfd, 1, NULL, &emptyset) <= 0)
			continue;

		if (read(pipe, &buf, sizeof(buf)) > 0)
		{
			if (hookpath)
				merge_hook(hookpath);
		} else {
			syslog (LOG_ERR, _("Child process hung up unexpectedly, aborting"));
			rval = -1;
			break;
		}

	}

	return rval;
}

static int
rdnssd (const char *username, const char *resolvpath, const char *hookpath)
{
	int rval = 0;
	struct sigaction act;
	sigset_t handled;
	pid_t worker_pid;
	int pfd[2];

	rval = pipe(pfd);
	if (rval == -1) {
		syslog (LOG_CRIT, _("Fatal error (%s): %m"), "pipe");
		return -1;
	}

	sigemptyset (&handled);

	memset (&act, 0, sizeof (struct sigaction));
	act.sa_handler = term_handler;

	act.sa_handler = term_handler;
	sigaction (SIGTERM, &act, NULL);
	sigaddset (&handled, SIGTERM);

	act.sa_handler = term_handler;
	sigaction (SIGINT, &act, NULL);
	sigaddset (&handled, SIGINT);

	act.sa_handler = ignore_handler;
	sigaction (SIGPIPE, &act, NULL);
	sigaddset (&handled, SIGPIPE);

	/* TODO: HUP handling */

	sigprocmask (SIG_SETMASK, &handled, NULL);

	worker_pid = fork();

	switch (worker_pid)
	{
		case 0:
			close(pfd[0]);

			prepare_fd(pfd[1]);
			rval = worker(pfd[1], resolvpath, username);
			close(pfd[1]);

			exit(rval != 0);

		case -1:
			syslog (LOG_CRIT, _("Fatal error (%s): %m"), "fork");
			close(pfd[0]);
			close(pfd[1]);
			rval = -1;
			break;

		default:
			close(pfd[1]);

			prepare_fd(pfd[0]);
			rval = manager (pfd[0], hookpath);
			close(pfd[0]);
	}

	int status;

	kill (worker_pid, SIGTERM);
	while (waitpid (worker_pid, &status, 0) != worker_pid);

	return rval;
}


/** PID file handling */
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
static int
create_pidfile (const char *path)
{
	char buf[20]; // enough for > 2^64
	size_t len;
	int fd;

	(void)snprintf (buf, sizeof (buf), "%d", (int)getpid ());
	buf[sizeof (buf) - 1] = '\0';
	len = strlen (buf);

	fd = open (path, O_WRONLY|O_CREAT|O_NOFOLLOW, 0644);
	if (fd != -1)
	{
		struct stat s;

		errno = 0;
		if ((fstat (fd, &s) == 0)
		 && S_ISREG(s.st_mode)
		 && (lockf (fd, F_TLOCK, 0) == 0)
		 && (ftruncate (fd, 0) == 0)
		 && (write (fd, buf, len) == (ssize_t)len)
		 && (fdatasync (fd) == 0))
			return fd;

		if (errno == 0) /* !S_ISREG */
			errno = EACCES;

		(void)close (fd);
	}
	return -1;
}


static int
quick_usage (const char *path)
{
	fprintf (stderr, _("Try \"%s -h\" for more information.\n"),
	         path);
	return 2;
}


static int
usage (const char *path)
{
	printf (_(
"Usage: %s [OPTIONS]\n"
"Starts the IPv6 Recursive DNS Server discovery Daemon.\n"
"\n"
"  -f, --foreground  run in the foreground\n"
"  -H, --merge-hook  execute this hook whenever resolv.conf is updated\n"
"  -h, --help        display this help and exit\n"
"  -p, --pidfile     override the location of the PID file\n"
"  -r, --resolv-file set the path to the generated resolv.conf file\n"
"  -u, --user        override the user to set UID to\n"
"  -V, --version     display program version and exit\n"), path);
	return 0;
}


static int
version (void)
{
	printf (_("rdnssd: IPv6 Recursive DNS Server discovery Daemon %s (%s)\n"),
	        VERSION, "$Rev$");
	printf (_(" built %s on %s\n"), __DATE__, PACKAGE_BUILD_HOSTNAME);
	printf (_("Configured with: %s\n"), PACKAGE_CONFIGURE_INVOCATION);
	puts (_("Written by Pierre Ynard and Remi Denis-Courmont\n"));

	printf (_("Copyright (C) %u-%u Pierre Ynard, Remi Denis-Courmont\n"),
	        2007, 2007);
	puts (_(
"This is free software; see the source for copying conditions.\n"
"There is NO warranty; not even for MERCHANTABILITY or\n"
"FITNESS FOR A PARTICULAR PURPOSE.\n"));
        return 0;
}


int main (int argc, char *argv[])
{
	const char *username = "nobody", *hookpath = NULL,
	           *pidpath = LOCALSTATEDIR "/run/rdnssd.pid",
	           *resolvpath = LOCALSTATEDIR "/run/rdnssd/resolv.conf";
	int pidfd, val, pipefd = -1;
	bool fg = false;

	static const struct option opts[] =
	{
		{ "foreground",		no_argument,		NULL, 'f' },
		{ "hook",			required_argument,	NULL, 'H' },
		{ "merge-hook",		required_argument,	NULL, 'H' },
		{ "help",			no_argument,		NULL, 'h' },
		{ "pidfile",		required_argument,	NULL, 'p' },
		{ "resolv-file",	required_argument,	NULL, 'r' },
		{ "user",			required_argument,	NULL, 'u' },
		{ "username",		required_argument,	NULL, 'u' },
		{ "version",		no_argument,		NULL, 'V' },
		{ NULL,				no_argument,		NULL, '\0'}
	};
	static const char optstring[] = "fH:hp:r:u:V";

	setlocale (LC_ALL, "");
	bindtextdomain (PACKAGE, LOCALEDIR);
	textdomain (PACKAGE);

	while ((val = getopt_long (argc, argv, optstring, opts, NULL)) != -1)
	{
		switch (val)
		{
			case 'f':
				fg = true;
				break;

			case 'H':
				hookpath = optarg;
				break;

			case 'h':
				return usage (argv[0]);

			case 'p':
				pidpath = optarg;
				break;

			case 'r':
				resolvpath = optarg;
				break;

			case 'u':
				username = optarg;
				break;

			case 'V':
				return version ();

			case '?':
				return quick_usage (argv[0]);
		}
	}

	/* Fork, and wait until the process is ready */
	if (!fg)
	{
		int pfd[2];
		pid_t pid;

		if (pipe (pfd))
			pfd[0] = pfd[1] = -1;
		pid = fork ();

		switch (pid)
		{
			case -1:
				perror ("fork");
				return 1;

			case 0:
				close (pfd[0]);
				setsid (); /* cannot fail */
				pipefd = pfd[1];
				break;

			default: /* parent process */
				close (pfd[1]);
				if (read (pfd[0], &val, sizeof (val)) != sizeof (val))
					val = 1;
				close (pfd[0]);

				if (val != 0)
				{
					/* failure! */
					while (waitpid (pid, &val, 0) != pid);
					return WIFEXITED (val) ? WEXITSTATUS (val) : 1;
				}
				return 0;
		}
	}

	openlog ("rdnssd", LOG_PERROR | LOG_PID, LOG_DAEMON);

	/* main process */
	pidfd = create_pidfile (pidpath);
	if (pidfd == -1)
	{
		syslog (LOG_ERR, _("Cannot create %s (%m) - already running?"),
		        pidpath);
		closelog ();
		return 1;
	}

	/* working fine: notify parent */
	if (!fg)
	{
		val = 0;
		write (pipefd, &val, sizeof (val));
		close (pipefd);
		freopen ("/dev/null", "r", stdin);
		freopen ("/dev/null", "w", stdout);
		freopen ("/dev/null", "w", stderr);
	}

	val = rdnssd (username, resolvpath, hookpath);

	closelog ();
	/* Unlink *before* close, i.e. unlink while still holding the
	 * advisory lock. Otherwise there would be a race condition whereby
	 * we could remove another instance's PID file. */
	unlink (pidpath);
	close (pidfd);

	return val != 0;
}
