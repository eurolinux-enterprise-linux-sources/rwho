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
 * From: @(#)rwho.c	5.5 (Berkeley) 6/1/90
 */
char rcsid[] = "$Id: rwho.c,v 1.7 1999/08/01 20:44:18 dholland Exp $";

#include <sys/param.h>
#include <sys/dir.h>
#include <sys/file.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <protocols/rwhod.h>

#include "../version.h"

static int utmpcmp(const void *, const void *);
#define	NUSERS	1000

struct myutmp {
	char	myhost[MAXHOSTNAMELEN];
	int	myidle;
	char	myline[PATH_MAX];
	char	myname[16];
	time_t	mytime;
};

#define	WHDRSIZE	((int)(sizeof (wd) - sizeof (wd.wd_we)))

/* 
 * this function should be shared with ruptime.
 */
static int down(time_t rcv, time_t now) {
	return (now - rcv > 11 * 60);
}

int 
main(int argc, char *argv[])
{
	static struct myutmp myutmp[NUSERS];
	int nusers = 0;

	int ch;
	struct direct *dp;
	int cc, width;
	struct whod wd;
	struct whod *w = &wd;
	struct whoent *we;
	struct myutmp *mp;
	int f, n, i;
	DIR *dirp;
	time_t now, myrecvtime, timecorrection;
	int aflg = 0;
	size_t size;

	while ((ch = getopt(argc, argv, "a")) != EOF) {
		switch((char)ch) {
		case 'a':
			aflg = 1;
			break;
		case '?':
		default:
			fprintf(stderr, "usage: rwho [-a]\n");
			exit(1);
		}
	}
	if (chdir(_PATH_RWHODIR) || (dirp = opendir(".")) == NULL) {
		perror(_PATH_RWHODIR);
		exit(1);
	}
	mp = myutmp;
	time(&now);
	while ((dp = readdir(dirp))!=NULL) {
		if (dp->d_ino == 0 || strncmp(dp->d_name, "whod.", 5)) {
			continue;
		}
		f = open(dp->d_name, O_RDONLY);
		if (f < 0) {
			continue;
		}
		cc = read(f, (char *)&wd, sizeof (struct whod));
		if (cc < WHDRSIZE) {
			(void) close(f);
			continue;
		}

		myrecvtime = w->wd_recvtime;
		/*
		 * After 2038 the rwho protocol, which sends int32s, will
		 * start to lose. However, by then, "now" will be an int64.
		 * So assume that if "now" is WAY bigger than myrecvtime, 
		 * the latter needs to be incremented by 2^32. 0x40000000 is
		 * 34 years, so it's _fairly_ safe to assume we won't have
		 * any packets that old in /var/spool/rwho.
		 * Increment the correction by values < 2^31 so it works 
		 * if time_t is still an int32. Save the correction for
		 * use on the login times.
		 */
		timecorrection = 0;
		while (now - myrecvtime + timecorrection > 0x40000000) {
			timecorrection += 0x70000000;
			timecorrection += 0x70000000;
			timecorrection += 0x20000000;
		}

		if (down(myrecvtime, now)) {
			(void) close(f);
			continue;
		}

		cc -= WHDRSIZE;
		we = w->wd_we;
		for (n = cc / sizeof (struct whoent); n > 0; n--) {
			if (aflg == 0 && we->we_idle >= 60*60) {
				we++;
				continue;
			}
			if (nusers >= NUSERS) {
				printf("too many users\n");
				exit(1);
			}

			/*
			 * Copy hostname. rwhod is supposed to make sure
			 * wd_hostname is null-terminated, but be defensive.
			 */
			size = sizeof(w->wd_hostname);
			if (size > sizeof(mp->myhost)) {
				size = sizeof(mp->myhost);
			}
			strncpy(mp->myhost, w->wd_hostname, size);
			mp->myhost[size-1] = 0;

			mp->myidle = we->we_idle;

			/*
			 * rwhod does not guarantee null-termination of
			 * the strings in we_utmp. 
			 */
			assert(sizeof(mp->myline) > 
			       sizeof(we->we_utmp.out_line));
			assert(sizeof(mp->myname) > 
			       sizeof(we->we_utmp.out_name));
			strncpy(mp->myline, we->we_utmp.out_line, 
				sizeof(we->we_utmp.out_line));
			mp->myline[sizeof(we->we_utmp.out_line)] = 0;
			strncpy(mp->myname, we->we_utmp.out_name,
				sizeof(we->we_utmp.out_name));
			mp->myname[sizeof(we->we_utmp.out_name)] = 0;
			mp->mytime = we->we_utmp.out_time + timecorrection;

			nusers++; 
			we++;
			mp++;
		}
		(void) close(f);
	}
	qsort(myutmp, nusers, sizeof(struct myutmp), utmpcmp);
	mp = myutmp;
	width = 0;
	for (i = 0; i < nusers; i++) {
		int j = strlen(mp->myhost) + 1 + strlen(mp->myline);
		if (j > width)
			width = j;
		mp++;
	}
	mp = myutmp;
	for (i = 0; i < nusers; i++) {
		char buf[BUFSIZ];
		snprintf(buf, sizeof(buf), "%s:%s", mp->myhost, mp->myline);
		printf("%-8.8s %-*s %.12s",
		       mp->myname,
		       width,
		       buf,
		       ctime(&mp->mytime)+4);
		mp->myidle /= 60;
		if (mp->myidle) {
			if (aflg) {
				if (mp->myidle >= 100*60)
					mp->myidle = 100*60 - 1;
				if (mp->myidle >= 60)
					printf(" %2d", mp->myidle / 60);
				else
					printf("   ");
			} else
				printf(" ");
			printf(":%02d", mp->myidle % 60);
		}
		printf("\n");
		mp++;
	}
	exit(0);
}

static int
utmpcmp(const void *v1, const void *v2)
{
	const struct myutmp *u1 = (const struct myutmp *)v1;
	const struct myutmp *u2 = (const struct myutmp *)v2;
	int rc;

	rc = strcmp(u1->myname, u2->myname);
	if (rc)
		return (rc);
	rc = strcmp(u1->myhost, u2->myhost);
	if (rc)
		return (rc);
	return (strcmp(u1->myline, u2->myline));
}
