/*
 * Copyright (C) 1995, 1997-1999 Jeffrey A. Uphoff
 *
 * NSM for Linux.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <arpa/inet.h>

#include "statd.h"
#include "notlist.h"

extern void my_svc_exit (void);


/*
 * Services SM_SIMU_CRASH requests.
 */
void *
sm_simu_crash_1_svc (void *argp, struct svc_req *rqstp)
{
  static char *result = NULL;

#ifdef RESTRICTED_STATD
	struct in_addr	caller;

	/* 1.	Reject anyone not calling from 127.0.0.1.
	 *	Ignore the my_name specified by the caller, and
	 *	use "127.0.0.1" instead.
	 */
	caller = svc_getcaller(rqstp->rq_xprt)->sin_addr;
	if (caller.s_addr != htonl(INADDR_LOOPBACK)) {
		note(N_WARNING,
			"Call to statd from non-local host %s",
			inet_ntoa(caller));
		goto failure;
	}
	if (ntohs(svc_getcaller(rqstp->rq_xprt)->sin_port) >= 1024) {
		note(N_WARNING,
		     "Call to statd-simu-crash from unprivileged port\n");
		goto failure;
	}
#endif
  note (N_WARNING, "*** SIMULATING CRASH! ***");
  my_svc_exit ();

  if (rtnl)
    nlist_kill (&rtnl);

#ifdef RESTRICTED_STATD
 failure:
#endif
  return ((void *)&result);
}
