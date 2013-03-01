/*
 * Copyright (c) 2008-2009, 2013 Carnegie Mellon University.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/signal.h>
#include <arpa/inet.h>

#include "rekeysrv-locl.h"

char *target_acl_path = NULL;
int force_compat_enctype = 0;

void run_fg(int s, struct sockaddr *sa) {
  char addrstr[INET6_ADDRSTRLEN];
  if (sa->sa_family == AF_INET) {
    struct sockaddr_in *sin = (struct sockaddr_in *)sa;
    if (!inet_ntop(sin->sin_family, &sin->sin_addr, addrstr,
		   INET6_ADDRSTRLEN)) {
      syslog(LOG_ERR, "Cannot determine connection address: %m");
    } else {
      syslog(LOG_INFO, "Connection from %s", addrstr);
    }
  } else if (sa->sa_family == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
    if (!inet_ntop(sin6->sin6_family, &sin6->sin6_addr, addrstr,
		   INET6_ADDRSTRLEN)) {
      syslog(LOG_ERR, "Cannot determine connection address: %m");
    } else {
      syslog(LOG_INFO, "Connection from %s", addrstr);
    }
  } else {
    syslog(LOG_INFO, "Connection from unknown address type %d", sa->sa_family);
  }
  run_session(s);
  exit(0);
}
void run_one(int s, struct sockaddr *sa) {
  pid_t p;

#if 0
  p=0;
#else
  p = fork();
  if (p < 0) 
    syslog(LOG_ERR, "Cannot fork: %m");
#endif
  if (p == 0)
    run_fg(s, sa);
  close(s);
}

static char *pidfile=NULL;
static void sigdie(int sig) {
  if (pidfile)
    unlink(pidfile);
  _exit(255);
}
int main(int argc, char **argv) {
  int dofork=0;
  int inetd=0;
  int optch;
  while ((optch=getopt(argc, argv, "a:cdip:T:")) != -1) {
    switch (optch) {
    case 'a':
      admin_arg(optarg);
      break;
    case 'c':
      force_compat_enctype=1;
      break;
    case 'd':
      dofork=1;
      break;
    case 'i':
      inetd=1;
      break;
    case 'p':
      pidfile=optarg;
      break;
    case 'T':
      target_acl_path=optarg;
      break;
    case '?':
      optind=0;
      break;
    }
    if (optind == 0)
      break; /* fall through to usage */
  }
  
  if (argc > optind) {
    fprintf(stderr, "Usage: rekeysrv -i [-T targets]...\n");
    fprintf(stderr, "       rekeysrv [-d] [-p pidfile] [-T targets]\n");
    fprintf(stderr, "  -i       run under inetd\n");
    fprintf(stderr, "  -d       run as a background daemon\n");
    fprintf(stderr, "  -p file  PID file\n");
    fprintf(stderr, "  -T file  ACL file listing permitted targets\n");
    fprintf(stderr, "  -c       force old enctype compatibility\n");
    fprintf(stderr, "  -a       %s\n", admin_help_string);
    exit(1);
  }
  if (inetd && (dofork || pidfile)) {
    fprintf(stderr, "Can't fork or use pidfile when running under inetd\n");
    exit(1);
  }
  if (dofork) {
#ifdef HAVE_DAEMON
    if (daemon(0, 0)) {
      perror("Cannot fork");
      exit(1);
    }
#else
    pid_t pid=fork();
    if (pid > 0) _exit(0);
    if (pid > 0) {
      perror("Cannot fork");
      exit(1);
    }
#ifdef HAVE_SETSID
    setsid();
#else
#ifdef HAVE_SETPGRP
#ifdef SETPGRP_VOID
    setpgrp();
#else
    setpgrp(0,0);
#endif
#endif
#endif
#endif
  }
  if (pidfile) {
    FILE *pf;
    pf=fopen(pidfile, "w+");
    if (pf) {
      fprintf(pf, "%ld", (long)getpid());
      fclose(pf);
    } else {
      pidfile=NULL;
    }
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, sigdie);
    signal(SIGTERM, sigdie);
  }
  openlog("rekeysrv", LOG_PID, LOG_DAEMON);

  ssl_startup();
  if (inetd) {
    struct sockaddr_storage ss;
    struct sockaddr *sa = (struct sockaddr *)&ss;
    int r;
    socklen_t sl=sizeof(struct sockaddr_storage);
    r=getpeername(0, sa, &sl);
    if (r<0) {
      syslog(LOG_ERR, "Cannot getpeername: %m");
      exit (1);
    }
    run_fg(0, sa);
  } else {
    signal(SIGCHLD, SIG_IGN);
    net_startup();
    run_accept_loop(run_one);
  }
  exit(0);
}
