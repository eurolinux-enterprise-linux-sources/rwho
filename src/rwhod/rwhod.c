/*
 * Copyright (c) 1983 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

char copyright[] =
  "@(#) Copyright (c) 1983 The Regents of the University of California.\n"
  "All rights reserved.\n";

/*
 * From: @(#)rwhod.c	5.20 (Berkeley) 3/2/91
 */
char rcsid[] = 
  "$Id: rwhod.c,v 1.20 2000/07/23 03:19:48 dholland Exp $";

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/file.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>

#ifndef __linux__
#include <nlist.h>
#endif
#include <errno.h>
#include <utmp.h>
#include <ctype.h>
#include <netdb.h>
#include <syslog.h>
#include <protocols/rwhod.h>
#include <stdio.h>
#include <stdlib.h>
#include <paths.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <grp.h>

#include "daemon.h"

#include "../version.h"

#define ENDIAN	LITTLE_ENDIAN

/*
 * Alarm interval. Don't forget to change the down time check in ruptime
 * if this is changed.
 */
#define AL_INTERVAL (3 * 60)

static struct sockaddr_in sine;

#ifndef __linux__
struct	nlist nl[] = {
#define	NL_BOOTTIME	0
	{ "_boottime" },
	0
};
#endif

static void	broadcaster(void);
static int	configure(int s);
static int	verify(const char *name);
static int	getloadavg(double ptr[3], int n);

/*
 * We communicate with each neighbor in
 * a list constructed at the time we're
 * started up.  Neighbors are currently
 * directly connected via a hardware interface.
 */
struct	neighbor {
	struct	neighbor *n_next;
	char	*n_name;		/* interface name */
	char	*n_addr;		/* who to send to */
	int	n_addrlen;		/* size of address */
	int	n_flags;		/* should forward?, interface flags */
};

static struct neighbor *neighbors;
static struct servent *sp;
static int sk;
static int use_pointopoint = 0;
static int use_broadcast = 0;
static int need_init = 1;
static int child_pid = 0;

#define WHDRSIZE	(((caddr_t) &((struct whod *) 0)->wd_we) \
			- ((caddr_t) 0))

static void huphandler(int);
static void termhandler(int);
static void sendpacket(struct whod *);
static void getboottime(struct whod *);

int
main(int argc, char *argv[])
{
	struct sockaddr_in from;
	struct passwd *pw = 0;
	struct stat st;
	char path[64];
	char *user = NULL;
	int on = 1;
	int opt;

	if (getuid()) {
		fprintf(stderr, "rwhod: not super user\n");
		exit(1);
	}

	while ((opt = getopt(argc, argv, "bpau:")) != EOF) {
	    switch (opt) {
	      case 'b':
		  use_broadcast = 1;
		  break;
	      case 'p':
		  use_pointopoint = 1;
		  break;
	      case 'a':
		  use_broadcast = 1;
		  use_pointopoint = 1;
		  break;
	      case 'u':
	      	  user = optarg;
		  break;
	      case '?':
	      default:
		  fprintf(stderr, "usage: rwhod [-bpa] [-u user]\n");
		  exit(1);
		  break;
	    }
	}
	if (optind<argc) {
	    fprintf(stderr, "usage: rwhod [-bpa] [-u user]\n");
	    exit(1);
	}
	if (!use_pointopoint && !use_broadcast) {
		/* use none is nonsensical; default to all */
		use_pointopoint = 1;
		use_broadcast = 1;
	}
	
	sp = getservbyname("who", "udp");
	if (sp == 0) {
		fprintf(stderr, "rwhod: udp/who: unknown service\n");
		exit(1);
	}
#ifndef DEBUG
	daemon(1, 0);
#endif
	if (chdir(_PATH_RWHODIR) < 0) {
		(void)fprintf(stderr, "rwhod: %s: %s\n",
		    _PATH_RWHODIR, strerror(errno));
		exit(1);
	}
	(void) signal(SIGHUP, huphandler);
	openlog("rwhod", LOG_PID, LOG_DAEMON);

	if ((sk = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		syslog(LOG_ERR, "socket: %m");
		exit(1);
	}
	if (setsockopt(sk, SOL_SOCKET, SO_BROADCAST, &on, sizeof (on)) < 0) {
		syslog(LOG_ERR, "setsockopt SO_BROADCAST: %m");
		exit(1);
	}
	sine.sin_family = AF_INET;
	sine.sin_port = sp->s_port;
	if (bind(sk, (struct sockaddr *)&sine, sizeof(sine)) < 0) {
		syslog(LOG_ERR, "bind: %m");
		exit(1);
	}

	(void) umask(022);

	signal(SIGTERM, termhandler);
	child_pid = fork();
	if (child_pid < 0) {
		syslog(LOG_ERR, "fork: %m");
		exit(1);
	}
	if (child_pid == 0) {
		broadcaster();
		exit(0);
	}

	/* We have to drop privs in two steps--first get the
	 * account info, then drop privs after chroot */
	if (user && (pw = getpwnam(user)) == NULL) {
		syslog(LOG_ERR, "unknown user: %s", user);
		exit(1);
	}

	/* Chroot to the spool directory
	 * (note this is already our $cwd) */
	if (chroot(_PATH_RWHODIR) < 0) {
		syslog(LOG_ERR, "chroot(%s): %m", _PATH_RWHODIR);
		kill(child_pid, SIGTERM);
		exit(1);
	}

	/* Now drop privs */
	if (pw) {
		if (setgroups(1, &pw->pw_gid) < 0
		 || setgid(pw->pw_gid) < 0
		 || setuid(pw->pw_uid) < 0) {
			syslog(LOG_ERR, "failed to drop privilege: %m");
			exit(1);
		}
	}

	for (;;) {
		struct whod wd;
		int cc, whod;
		size_t len = sizeof(from);

		memset(&wd, 0, sizeof(wd));
		cc = recvfrom(sk, (char *)&wd, sizeof(struct whod), 0,
			      (struct sockaddr *)&from, &len);
		if (cc <= 0) {
			if (cc < 0 && errno != EINTR)
				syslog(LOG_WARNING, "recv: %m");
			continue;
		}
		if (from.sin_port != sp->s_port) {
			syslog(LOG_WARNING, "%d: bad from port",
				ntohs(from.sin_port));
			continue;
		}
		if (wd.wd_vers != WHODVERSION)
			continue;
		if (wd.wd_type != WHODTYPE_STATUS)
			continue;
		/* 
		 * Ensure null termination of the name within the packet.
		 * Otherwise we might overflow or read past the end.
		 */
		wd.wd_hostname[sizeof(wd.wd_hostname)-1] = 0;
		if (!verify(wd.wd_hostname)) {
			syslog(LOG_WARNING, "malformed host name from %x",
				from.sin_addr);
			continue;
		}
		snprintf(path, sizeof(path), "whod.%s", wd.wd_hostname);
		/*
		 * Rather than truncating and growing the file each time,
		 * use ftruncate if size is less than previous size.
		 */
		whod = open(path, O_WRONLY | O_CREAT, 0644);
		if (whod < 0) {
			syslog(LOG_WARNING, "%s: %m", path);
			continue;
		}
#if ENDIAN != BIG_ENDIAN
		{
			int i, n = (cc - WHDRSIZE)/sizeof(struct whoent);
			struct whoent *we;

			/* undo header byte swapping before writing to file */
			wd.wd_sendtime = ntohl(wd.wd_sendtime);
			for (i = 0; i < 3; i++)
				wd.wd_loadav[i] = ntohl(wd.wd_loadav[i]);
			wd.wd_boottime = ntohl(wd.wd_boottime);
			we = wd.wd_we;
			for (i = 0; i < n; i++) {
				we->we_idle = ntohl(we->we_idle);
				we->we_utmp.out_time =
				    ntohl(we->we_utmp.out_time);
				we++;
			}
		}
#endif
		wd.wd_recvtime = time(NULL);
		write(whod, (char *)&wd, cc);
		if (fstat(whod, &st) < 0 || st.st_size > cc)
			ftruncate(whod, cc);
		(void) close(whod);
	}
}

/*
 * Terminate broadcaster process
 */
static void
termhandler(int dummy)
{
	(void) dummy;
	if (child_pid)
		kill(child_pid, SIGTERM);
	exit(0);
}

/*
 * Obtain boot time again
 */
static void
huphandler(int dummy)
{
	(void) dummy;
	need_init = 1;
}

/*
 * This is the part of rwhod that sends out packets
 */
static void
broadcaster()
{
	char		myname[MAXHOSTNAMELEN], *cp;
	size_t		mynamelen;
	struct whod	mywd;

	if (!configure(sk))
		exit(1);

	/*
	 * Establish host name as returned by system.
	 */
	if (gethostname(myname, sizeof (myname) - 1) < 0) {
		syslog(LOG_ERR, "gethostname: %m");
		exit(1);
	}
	if ((cp = index(myname, '.')) != NULL)
		*cp = '\0';
	mynamelen = strlen(myname);
	if (mynamelen > sizeof(mywd.wd_hostname)) 
		mynamelen = sizeof(mywd.wd_hostname);
	strncpy(mywd.wd_hostname, myname, mynamelen);
	mywd.wd_hostname[sizeof(mywd.wd_hostname)-1] = 0;

	getboottime(&mywd);

	while (1) {
		sendpacket(&mywd);
		(void) sleep(AL_INTERVAL);
	}
}

/*
 * Check out host name for unprintables
 * and other funnies before allowing a file
 * to be created.  Sorry, but blanks aren't allowed.
 */
static int
verify(const char *name)
{
	register int size = 0;

	while (*name) {
		if (*name=='/' || 
		    !isascii(*name) || !(isalnum(*name) || ispunct(*name)))
			return (0);
		name++, size++;
	}
	return size > 0;
}


static void
sendpacket(struct whod *wd)
{
	static int nutmps = 0;
	static time_t utmptime = 0;
	static off_t utmpsize = 0;
	static int alarmcount = 0;

	struct neighbor *np;
	struct whoent *we = wd->wd_we, *wlast;
	int i, cc;
	struct stat stb;
	struct utmp *uptr;
	double avenrun[3];
	time_t now = time(NULL);

	if (alarmcount % 10 == 0 || need_init) {
		getboottime(wd);
		need_init = 0;
	}
	alarmcount++;
	stat(_PATH_UTMP, &stb);
	if ((stb.st_mtime != utmptime) || (stb.st_size > utmpsize)) {
		utmptime = stb.st_mtime;
		if (stb.st_size > utmpsize) {
			utmpsize = stb.st_size + 10 * sizeof(struct utmp);
		}
		wlast = (struct whoent *) ((caddr_t) wd->wd_we)
						+ sizeof(wd->wd_we);
		wlast = &wd->wd_we[1024 / sizeof (struct whoent) - 1];
		setutent();
		while ((uptr = getutent()) && we < wlast) {
			if (uptr->ut_name[0]
			&& uptr->ut_type == USER_PROCESS) {
				bcopy(uptr->ut_line, we->we_utmp.out_line,
				   sizeof(uptr->ut_line));
				bcopy(uptr->ut_name, we->we_utmp.out_name,
				   sizeof(uptr->ut_name));
				we->we_utmp.out_time = htonl(uptr->ut_time);
				we++;
			}
		}
		nutmps = we - wd->wd_we;
		endutent();
	}

	/*
	 * The test on utmpent looks silly---after all, if no one is
	 * logged on, why worry about efficiency?---but is useful on
	 * (e.g.) compute servers.
	 */
	if (nutmps && chdir(_PATH_DEV)) {
		syslog(LOG_ERR, "chdir(%s): %m", _PATH_DEV);
		exit(1);
	}
	we = wd->wd_we;
	for (i = 0; i < nutmps; i++) {
		if (stat(we->we_utmp.out_line, &stb) >= 0)
			we->we_idle = htonl(now - stb.st_atime);
		we++;
	}
	getloadavg(avenrun, sizeof(avenrun)/sizeof(avenrun[0]));
	for (i = 0; i < 3; i++)
		wd->wd_loadav[i] = htonl((u_long)(avenrun[i] * 100));
	cc = (char *)we - (char *)wd;
	wd->wd_sendtime = htonl(time(0));
	wd->wd_vers = WHODVERSION;
	wd->wd_type = WHODTYPE_STATUS;
	for (np = neighbors; np != NULL; np = np->n_next) {
		if (sendto(sk, (char *)wd, cc, 0,
			   (struct sockaddr *) np->n_addr, np->n_addrlen) < 0) 
		  syslog(LOG_ERR, "sendto(%s): %m",
			 inet_ntoa(((struct sockaddr_in *)np->n_addr)->sin_addr));
	}

	if (nutmps && chdir(_PATH_RWHODIR)) {
		syslog(LOG_ERR, "chdir(%s): %m", _PATH_RWHODIR);
		exit(1);
	}
}

/*
 * Taken from:
 *
 * rwhod.c
 *
 * A simple rwhod server for Linux
 *
 * Version: 0.1
 *
 * Copyright (c) 1993 Peter Eriksson, Signum Support AB
 *
 * -----------------------------------------------------------------------
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * -----------------------------------------------------------------------
 *
 * Send comments/bug reports/fixes to: pen@signum.se or pen@lysator.liu.se
 */
int getloadavg(double ptr[3], int n)
{
	FILE *fp;

	if (n!=3) return -1;

	fp = fopen("/proc/loadavg", "r");
	if (!fp) return -1;

	if (fscanf(fp, "%lf %lf %lf", &ptr[0], &ptr[1], &ptr[2]) != 3) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}


void
getboottime(struct whod *wd)
{
#ifdef __linux__
	long uptime;
	time_t curtime;
	FILE *fp = fopen("/proc/uptime", "r");
	if (!fp) return /* -1 */;

	fscanf(fp, "%ld", &uptime);

	curtime = time(NULL);
	curtime -= uptime;
	wd->wd_boottime = htonl(curtime);

	fclose(fp);
	return /* 0 */;
#else
	static int kmemf = -1;
	static ino_t vmunixino;
	static time_t vmunixctime;
	struct stat sb;

	if (stat(_PATH_UNIX, &sb) < 0) {
		if (vmunixctime)
			return;
	} else {
		if (sb.st_ctime == vmunixctime && sb.st_ino == vmunixino)
			return;
		vmunixctime = sb.st_ctime;
		vmunixino= sb.st_ino;
	}
	if (kmemf >= 0)
		(void) close(kmemf);
loop:
	if (nlist(_PATH_UNIX, nl)) {
		syslog(LOG_WARNING, "%s: namelist botch", _PATH_UNIX);
		sleep(300);
		goto loop;
	}
	kmemf = open(_PATH_KMEM, O_RDONLY, 0);
	if (kmemf < 0) {
		syslog(LOG_ERR, "%s: %m", _PATH_KMEM);
		exit(1);
	}
	(void) lseek(kmemf, (long)nl[NL_BOOTTIME].n_value, L_SET);
	(void) read(kmemf, (char *)&wd->wd_boottime,
	    sizeof (wd->wd_boottime));
	wd->wd_boottime = htonl(wd->wd_boottime);
#endif
}

/*
 * Figure out device configuration and select
 * networks which deserve status information.
 */
static int
configure(int s)
{
	char buf[BUFSIZ], *cp, *cplim;
	struct ifconf ifc;
	struct ifreq ifreq, *ifr;
	struct sockaddr_in *sn;
	register struct neighbor *np;

	ifc.ifc_len = sizeof (buf);
	ifc.ifc_buf = buf;
	if (ioctl(s, SIOCGIFCONF, (char *)&ifc) < 0) {
		syslog(LOG_ERR, "ioctl (get interface configuration)");
		return (0);
	}
	ifr = ifc.ifc_req;
#ifdef AF_LINK
#define max(a, b) (a > b ? a : b)
#define size(p)	max((p).sa_len, sizeof(p))
#else
#define size(p) (sizeof (p))
#endif
	cplim = buf + ifc.ifc_len; /*skip over if's with big ifr_addr's */
	for (cp = buf; cp < cplim;
			cp += sizeof (ifr->ifr_name) + size(ifr->ifr_addr)) {
		ifr = (struct ifreq *)cp;
		for (np = neighbors; np != NULL; np = np->n_next)
			if (np->n_name &&
			    strcmp(ifr->ifr_name, np->n_name) == 0)
				break;
		if (np != NULL)
			continue;
		ifreq = *ifr;
		np = (struct neighbor *)malloc(sizeof (*np));
		if (np == NULL)
			continue;
		np->n_name = malloc(strlen(ifr->ifr_name) + 1);
		if (np->n_name == NULL) {
			free((char *)np);
			continue;
		}
		strcpy(np->n_name, ifr->ifr_name);
		np->n_addrlen = sizeof (ifr->ifr_addr);
		np->n_addr = malloc(np->n_addrlen);
		if (np->n_addr == NULL) {
			free(np->n_name);
			free((char *)np);
			continue;
		}
		bcopy((char *)&ifr->ifr_addr, np->n_addr, np->n_addrlen);
		if (ioctl(s, SIOCGIFFLAGS, (char *)&ifreq) < 0) {
			syslog(LOG_ERR, "ioctl (get interface flags)");
			free((char *)np);
			continue;
		}
		if ((ifreq.ifr_flags & IFF_UP) == 0 ||
		    (ifreq.ifr_flags & (IFF_BROADCAST|IFF_POINTOPOINT)) == 0) {
			free((char *)np);
			continue;
		}
		np->n_flags = ifreq.ifr_flags;
		if (np->n_flags & IFF_POINTOPOINT) {
			if (ioctl(s, SIOCGIFDSTADDR, (char *)&ifreq) < 0) {
				syslog(LOG_ERR, "ioctl (get dstaddr)");
				free(np);
				continue;
			}
			if (!use_pointopoint) {
				free(np);
				continue;
			}
			/* we assume addresses are all the same size */
			bcopy((char *)&ifreq.ifr_dstaddr,
			  np->n_addr, np->n_addrlen);
		}
		if (np->n_flags & IFF_BROADCAST) {
			if (ioctl(s, SIOCGIFBRDADDR, (char *)&ifreq) < 0) {
				syslog(LOG_ERR, "ioctl (get broadaddr)");
				free(np);
				continue;
			}
			if (!use_broadcast) {
				free(np);
				continue;
			}
			/* we assume addresses are all the same size */
			bcopy((char *)&ifreq.ifr_broadaddr,
			  np->n_addr, np->n_addrlen);
		}
		/* gag, wish we could get rid of Internet dependencies */
		sn = (struct sockaddr_in *)np->n_addr;
		sn->sin_port = sp->s_port;
		np->n_next = neighbors;
		neighbors = np;
	}
	return (1);
}

#ifdef DEBUG
sendto(s, buf, cc, flags, to, tolen)
	int s;
#ifdef	__linux__
	__const void *buf;
	int cc;
	unsigned int flags;
	__const struct sockaddr *to;
	int tolen;
#else
	char *buf;
	int cc, flags;
	char *to;
	int tolen;
#endif
{
	register struct whod *w = (struct whod *)buf;
	register struct whoent *we;
	struct sockaddr_in *sn = (struct sockaddr_in *)to;
	char *interval();

	printf("sendto %x.%d\n", ntohl(sn->sin_addr.s_addr), ntohs(sn->sin_port));
	printf("hostname %s %s\n", w->wd_hostname,
	   interval(ntohl(w->wd_sendtime) - ntohl(w->wd_boottime), "  up"));
	printf("load %4.2f, %4.2f, %4.2f\n",
	    ntohl(w->wd_loadav[0]) / 100.0, ntohl(w->wd_loadav[1]) / 100.0,
	    ntohl(w->wd_loadav[2]) / 100.0);
	cc -= WHDRSIZE;
	for (we = w->wd_we, cc /= sizeof (struct whoent); cc > 0; cc--, we++) {
		time_t t = ntohl(we->we_utmp.out_time);
		printf("%-8.8s %s:%s %.12s",
			we->we_utmp.out_name,
			w->wd_hostname, we->we_utmp.out_line,
			ctime(&t)+4);
		we->we_idle = ntohl(we->we_idle) / 60;
		if (we->we_idle) {
			if (we->we_idle >= 100*60)
				we->we_idle = 100*60 - 1;
			if (we->we_idle >= 60)
				printf(" %2d", we->we_idle / 60);
			else
				printf("   ");
			printf(":%02d", we->we_idle % 60);
		}
		printf("\n");
	}
}

char *
interval(time, updown)
	int time;
	char *updown;
{
	static char resbuf[32];
	int days, hours, minutes;

	if (time < 0 || time > 3*30*24*60*60) {
		(void) snprintf(resbuf, sizeof(resbuf), "   %s ??:??", updown);
		return (resbuf);
	}
	minutes = (time + 59) / 60;		/* round to minutes */
	hours = minutes / 60; minutes %= 60;
	days = hours / 24; hours %= 24;
	if (days)
		(void) snprintf(resbuf, sizeof(resbuf), "%s %2d+%02d:%02d",
		    updown, days, hours, minutes);
	else
		(void) snprintf(resbuf, sizeof(resbuf), "%s    %2d:%02d",
		    updown, hours, minutes);
	return (resbuf);
}
#endif
