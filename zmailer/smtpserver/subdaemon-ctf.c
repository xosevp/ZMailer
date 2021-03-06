/*
 *    Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *      This will be free software, but only when it is finished.
 */
/*
 *    Several extensive changes by Matti Aarnio <mea@nic.funet.fi>
 *      Copyright 1991-2006.
 */

/*  SMTPSERVER  CONTENT-FILTER MULTIPLEXER-SERVER  SUBDAEMON    */

/*
 * Protocol from client to server is of TEXT LINES that end with '\n'
 * and are without '\r'... (that are meaningless inside the system.)
 *
 * The fd-passing system sends one byte of random junk, and a bunch
 * of ancilliary data (the fd.)
 */

/*
 *  contentpolicy.c -- module for ZMailer's smtpserver
 *  By Matti Aarnio <mea@nic.funet.fi> 1998, 2000, 2002-2003
 *
 *  This is the ugly one, we run SENSOR program on each
 *  accepted message -- if the sensor decrees the program
 *  unacceptable, we return policy-analysis result per
 *  analyzed file to the caller of this program.
 *
 *  The protocol in between the smtpserver, and the content
 *  policy analysis program is a simple one:
 *   0) Caller starts the policy program
 *   1) When the policy program is ready to answer to questions,
 *      it writes "#hungry\n" to its STDOUT.
 *   2) This caller wrapper detects the state, and feeds it
 *      a job-spec:   relfilepath \n
 *   3) The policy-program will then respond with a line
 *      matching format:
 *           %i %i.%i.%i comment text \n
 *   4) the interface collects that, and presents onwards.
 *  Loop restart from phase 1), UNLESS the policy program
 *  has yielded e.g. EOF, in which case the loop terminates.
 *
 *  If no answer is received (merely two consequtive #hungry
 *  states, or non-conformant answers), an "ok" is returned,
 *  and the situation is logged.
 *
 */

#include "smtpserver.h"

char *contentfilter;

static const char *Hungry = "#hungry\n";

static int subdaemon_handler_ctf_init  __((struct subdaemon_state **));
static int subdaemon_handler_ctf_input __((struct subdaemon_state *, struct peerdata*));
static int subdaemon_handler_ctf_prepoll  __((struct subdaemon_state *, struct zmpollfd **, int *));
static int subdaemon_handler_ctf_postpoll __((struct subdaemon_state *, struct zmpollfd *, int));
static int subdaemon_handler_ctf_shutdown   __((struct subdaemon_state *));
static int subdaemon_handler_ctf_killpeer __((struct subdaemon_state *, struct peerdata*));

struct subdaemon_handler subdaemon_handler_contentfilter = {
	subdaemon_handler_ctf_init,
	subdaemon_handler_ctf_input,
	subdaemon_handler_ctf_prepoll,
	subdaemon_handler_ctf_postpoll,
	subdaemon_handler_ctf_shutdown,
	subdaemon_handler_ctf_killpeer
};

#define MAXCTFS 20

static int MaxCtfs = 2;
int contentfilter_maxctfs;

struct ctf_peerstate {
	time_t last_cmd_time;
	struct peerdata *replypeer;
	int   contentfilterpid;
	FILE *tofp;
	int   fromfd;
	struct zmpollfd *pollfd;
	char *buf;
	int   inlen;
	int   bufsize;
	int   sawhungry;
	struct fdgets_fdbuf fdb;
};

typedef struct state_ctf {
	long proc_ino;
	time_t proc_mtime, proc_ctime;
	struct ctf_peerstate peers[MAXCTFS];
} Ctfstate;


static void subdaemon_killctf __(( struct ctf_peerstate * CTFp ));

static void
subdaemon_killctf(CTFp)
     struct ctf_peerstate *CTFp;
{
	if (CTFp->tofp != NULL)
	  fclose(CTFp->tofp);
	CTFp->tofp = NULL;

	if (CTFp->fromfd >= 0)
	  close(CTFp->fromfd);
	CTFp->fromfd = -1;

	if (CTFp->contentfilterpid > 1)
	  kill(CTFp->contentfilterpid, SIGKILL);
	CTFp->contentfilterpid = 0;

	CTFp->pollfd = NULL;
}


/* ============================================================ */
/* Eugene Crosser reported that this code is broken..
   .. changed things to do socket calls from client side,  */

static int subdaemon_ctf_sock __((struct ctf_peerstate * CTFp));
static int
subdaemon_ctf_sock(CTFp)
     struct ctf_peerstate *CTFp;
{
	int msgsock;
	struct sockaddr_un server;

	memset((char*)&server,0,sizeof(server));
	server.sun_family = AF_UNIX;
	strncpy(server.sun_path,contentfilter,sizeof(server.sun_path)-1);
	server.sun_path[sizeof(server.sun_path)-1]='\0';

	msgsock = socket(AF_UNIX,SOCK_STREAM,0);

	if (msgsock < 0) {
	  type(NULL,0,NULL, "contentfilter socket(%s) error %d (%s)",
	       contentfilter,errno,strerror(errno));
	  return(0);
	}
	if (connect(msgsock,(struct sockaddr *)&server,
		    sizeof(server) - sizeof (server.sun_path)
		    + strlen(server.sun_path)+1) < 0) {
	  type(NULL,0,NULL, "contentfilter connect(%s) error %d (%s)",
	       contentfilter,errno,strerror(errno));
	  return 0;
	}

	CTFp->tofp   = fdopen(msgsock, "w");
	CTFp->fromfd = msgsock;

	CTFp->contentfilterpid = 0;

	return 3;
}


#ifndef HAVE_PUTENV
static const char *newenviron[] =
  { "SMTPSERVER=y", NULL };
#endif

static int subdaemon_ctf_proc __((struct ctf_peerstate * CTFp));
static int subdaemon_ctf_proc (CTFp)
     struct ctf_peerstate *CTFp;
{
	int rpid = 0, to[2], from[2], rc;

	if (contentfilter == NULL) {
	  return -1;
	}

	to[0] = to[1] = from[0] = from[1] = -1;
	if (pipe(to) < 0 || pipe(from) < 0) {
	  /* Roll-back by closing possibly successfully
	     created pipes.. */
	  if (to[0] >= 0) close(to[0]);
	  if (to[1] >= 0) close(to[1]);
	  if (from[0] >= 0) close(from[0]);
	  if (from[1] >= 0) close(from[1]);
	  return -1;
	}

	fcntl(to[1],   F_SETFD, FD_CLOEXEC);
	fcntl(from[0], F_SETFD, FD_CLOEXEC);

	rpid = fork();
	if (rpid == 0) {	/* child */
	  const char *zconf = getzenv("ZCONFIG"); /* this is pretty much guaranteed to exist.. */

	  rpid = getpid();
	  if (to[0] != 0)
	    dup2(to[0], 0);
	  if (from[1] != 1)
	    dup2(from[1], 1);
	  dup2(1, 2);
	  if (to[0] > 2)   close(to[0]);
	  if (from[1] > 2) close(from[1]);

	  runasrootuser();	/* XXX: security alert! */
#ifdef HAVE_PUTENV
	  putenv("SMTPSERVER=y");
#else
	  environ = (char **) newenviron;
#endif
	  execl(contentfilter, contentfilter, "-Z", zconf, NULL);

#define	BADEXEC	"#BADEXEC\n\n"
	  write(1, BADEXEC, sizeof(BADEXEC)-1);
	  _exit(1);

	} else if (rpid < 0) {
	  /* Roll-back by closing possibly successfully
	     created pipes.. */
	  if (to[0] >= 0) close(to[0]);
	  if (to[1] >= 0) close(to[1]);
	  if (from[0] >= 0) close(from[0]);
	  if (from[1] >= 0) close(from[1]);
	  return -1;
	}

	CTFp->contentfilterpid = rpid;

	close(to[0]);
	close(from[1]);

	CTFp->tofp   = fdopen(to[1], "w");
	fd_blockingmode(to[1]);
	if (! CTFp->tofp ) {
	  close(to[1]);
	  close(from[0]);
	  return -1; /* BAD BAD! */
	}

	CTFp->fromfd = from[0];
	fd_blockingmode(CTFp->fromfd);
	
	for (;;) {
	  rc = fdgets( & CTFp->buf, 0, & CTFp->bufsize, &CTFp->fdb, CTFp->fromfd, 10);
	  if ( rc < 1 || ! CTFp->buf ) {
	    /* FIXME: ERROR PROCESSING ! */
	    if (rc == 0 || rc == -1) {
	      /* EOF! */
	      subdaemon_killctf(CTFp);
	    }
	    close(to[1]);
	    close(from[0]);
	    return -1;
	  }
	  if (strncmp( CTFp->buf, BADEXEC, sizeof(BADEXEC) - 3) == 0) {
	    subdaemon_killctf(CTFp);
	    close(to[1]);
	    close(from[0]);
	    return -1;
	  }

	  if (strcmp( CTFp->buf, Hungry ) == 0) {
	    CTFp->sawhungry = 1;
	    break;
	  }
	}
	fd_nonblockingmode(CTFp->fromfd);
	
	return rpid;
}

static int subdaemon_ctf_start __((Ctfstate * CTF, int idx));
static int
subdaemon_ctf_start __((CTF, idx))
    Ctfstate * CTF;
    int idx;
{
	struct stat stbuf;

	if (!contentfilter) return -1; /* D'uh!  Not configured! */

	if (stat(contentfilter, &stbuf)) {
	  type(NULL,0,NULL, "contentfilter stat(%s) error %d",
	       contentfilter, errno);
	  return 0;
	}

	CTF->proc_ino   = stbuf.st_ino;
	CTF->proc_ctime = stbuf.st_ctime;
	CTF->proc_mtime = stbuf.st_mtime;

	if (S_ISREG(stbuf.st_mode))
	  return subdaemon_ctf_proc( & CTF->peers[idx] );
	else
	  return subdaemon_ctf_sock( & CTF->peers[idx] );
}



/* ------------------------------------------------------------ */


static int
subdaemon_handler_ctf_init (statep)
     struct subdaemon_state **statep;
{
	struct state_ctf *state = calloc(1, sizeof(struct state_ctf));
	*statep = (struct subdaemon_state*) state;

	MaxCtfs = contentfilter_maxctfs;
	if (MaxCtfs > MAXCTFS) MaxCtfs = MAXCTFS;
	if (MaxCtfs < 1) MaxCtfs = 1;

	if (state) {
	  int idx;
	  for (idx = 0; idx < MaxCtfs; ++idx) {
	    /* state->contentfilterpid[idx] = 0; */
	    state->peers[idx].fromfd = -1;
	    state->peers[idx].pollfd = NULL;
	  }
	}

#if 0
	{
	  extern int logstyle;
	  extern char *logfile;
	  extern void openlogfp __((SmtpState * SS, int insecure));

	  logstyle = 0;
	  if (logfp) fclose(logfp); logfp = NULL;
	  logfile = "smtpserver-ctf-subdaemons.log";
	  openlogfp(NULL, 1);
	  setlinebuf(logfp);
	}
#endif
	return 0;
}

/*
 * subdaemon_handler_xx_input()
 *   ret > 0:  XOFF... busy right now!
 *   ret == 0: XON... give me more work!
 */
static int
subdaemon_handler_ctf_input (state, peerdata)
     struct subdaemon_state *state;
     struct peerdata *peerdata;
{
	int rc = 0;
	int idx;
	Ctfstate *CTF = (Ctfstate*)state;

	/* FIXME:FIXME: don't start more than necessary! */

	for (idx = 0; idx < MaxCtfs; ++idx) {
	  struct ctf_peerstate *CTFp = & CTF->peers[idx];

	  if (CTFp->replypeer)
	    continue;

	  if (CTFp->fromfd < 0) {
	    rc = subdaemon_ctf_start(CTF, idx);
	    if (rc < 2) {
	      /* FIXME: error processing! */
	      struct timeval tv;
	      tv.tv_sec = 0;
	      tv.tv_usec = 500000;
	      select(0, NULL, NULL, NULL, &tv); /* Sleep about 0.5 sec.. */
	      return 1;
	    }

	    /* Now   CTFp->fromfd   is in NON-BLOCKING MODE!
	       However  CTFp->tofp  is definitely in blocking! */
	  }

	  if (CTFp->tofp == NULL)
	    continue; /* Next! (recently killed, fromfd not yet EOFed ?) */

	  if (!CTFp->sawhungry)
	    continue; /* Next! (no liveness reply yet) */

	  CTFp->replypeer = peerdata;

	  fwrite(peerdata->inpbuf, peerdata->inlen, 1, CTFp->tofp);
	  fflush(CTFp->tofp);

	  time( &CTFp->last_cmd_time );

	  CTFp->bufsize    = 0;
	  CTFp->sawhungry  = 0;
	  peerdata->inlen = 0;

	  return 0;
	}
	return 1;
}

static int
subdaemon_handler_ctf_killpeer (state, peerdata)
     struct subdaemon_state *state;
     struct peerdata *peerdata;
{
	Ctfstate *CTF = (Ctfstate*)state;
	int idx;

	for (idx = 0; idx < MaxCtfs; ++idx) {

	  struct ctf_peerstate *CTFp = & CTF->peers[idx];

	  if (CTFp->replypeer != peerdata)
	    continue;

	  CTFp->replypeer = NULL;

	  CTFp->bufsize    = 0;
	  CTFp->sawhungry  = 0;
	  peerdata->inlen = 0;

	  break;
	}
	return 0;
}


static int
subdaemon_handler_ctf_prepoll (state, fdsp, fdscountp)
     struct subdaemon_state *state;
     struct zmpollfd **fdsp;
     int *fdscountp;
{
	Ctfstate *CTF = (Ctfstate*)state;
	int idx;
	struct stat stbuf;
	int rc = -1;

	if (! CTF) return 0; /* No state to monitor */

	/* If we have contentfilter underneath us,
	   check if it has something to say! */

	memset(& stbuf, 0, sizeof(stbuf));

	while (lstat(contentfilter, &stbuf) < 0) {
	  if (errno == EINTR) continue;
	  break;
	}
	if (stbuf.st_ino != CTF->proc_ino ||
	    stbuf.st_ctime != CTF->proc_ctime ||
	    stbuf.st_mtime != CTF->proc_mtime) {
	  /* CHANGED SOMEHOW ! */
	  CTF->proc_ino   = stbuf.st_ino;
	  CTF->proc_ctime = stbuf.st_ctime;
	  CTF->proc_mtime = stbuf.st_mtime;

	  for (idx = 0; idx < MaxCtfs; ++idx) {
	    struct ctf_peerstate *CTFp = & CTF->peers[idx];
	    if (CTFp->tofp)
	      fclose(CTFp->tofp);
	    CTFp->tofp = NULL;
	  }
	}
 
	for (idx = 0; idx < MaxCtfs; ++idx) {
	  struct ctf_peerstate *CTFp = & CTF->peers[idx];
	  if (CTFp->fromfd >= 0) {
	    zmpoll_addfd( fdsp, fdscountp,
			  CTFp->fromfd, -1, &CTFp->pollfd );
	    if (CTFp->fdb.rdsize)
	      rc = 1;
	  }
	}

	return rc;
}

static int
subdaemon_handler_ctf_postpoll (state, fdsp, fdscount)
     struct subdaemon_state *state;
     struct zmpollfd *fdsp;
     int fdscount;
{
	int rc = 0;
	int idx;
	int sawhungry = 0;
	Ctfstate *CTF = (Ctfstate*)state;

	if (! CTF) return -1; /* No state to monitor */

	for (idx = 0; idx < MaxCtfs; ++idx) {
	  struct ctf_peerstate *CTFp = &CTF->peers[idx];
	  if (CTFp->fromfd < 0)
	    continue; /* No contentfilter there.. */

	  if ( CTFp->fdb.rdsize ||
	       (CTFp->pollfd->revents & (ZM_POLLIN|ZM_POLLERR|ZM_POLLHUP)) ) {
	    /* We have something to read ! */
	    
	    rc = fdgets( & CTFp->buf, CTFp->inlen,
			 & CTFp->bufsize,
			 & CTFp->fdb, CTFp->fromfd, -1);
#if 0 /* Let the loop to spin... */
	    if (rc < 0 && errno == EAGAIN) return -EAGAIN;  /* */
#endif
	    if (rc == 0 || rc == -1) { /* EOF */
	      subdaemon_killctf(CTFp);
	      continue;
	    }
	    
	    if (rc > 0) {
	      CTFp->inlen = rc;
	      if (CTFp->buf[rc-1] == '\n') {
		/* Whole line accumulated, send it out! */
		
		subdaemon_send_to_peer(CTFp->replypeer, CTFp->buf, rc);
		CTFp->inlen = 0; /* Zap it.. */
	      }

	      if (strcmp( CTFp->buf, Hungry ) == 0) {
		CTFp->sawhungry = 1;
		sawhungry = 1;
		CTFp->replypeer = NULL;
	      }
	    }
	  }

	  if ((now - CTFp->last_cmd_time) > SUBSERVER_IDLE_TIMEOUT) {
	    subdaemon_killctf(CTFp);
	    continue;
	  }
	}

	return sawhungry;
}



static int
subdaemon_handler_ctf_shutdown (state)
     struct subdaemon_state *state;
{
	return -1;
}


/* --------------------------------------------------------------- */
/*  client caller interface                                        */
/* --------------------------------------------------------------- */

/* extern int  contentfilter_rdz_fd; */

struct ctf_state {
	int fd_io;
	FILE *outfp;
	int buflen;
	char *buf;
	char *pbuf;
	int sawhungry; /* remote may yield  N  lines of output, 
			  until  Hungry  */
	struct fdgets_fdbuf fdb;
};


void
smtpcontentfilter_kill ( statep )
     void * statep;
{
	struct ctf_state * state = statep;
	if (!state) return;

	if (state->outfp) fclose(state->outfp);
	state->outfp = NULL;

	if (state->fd_io >= 0) {
	  close(state->fd_io);
	  state->fd_io = -1;
	}
	if (state->buf)  free(state->buf);
	state->buf = NULL;
}


static int smtpcontentfilter_init __((struct ctf_state **pp));

static int
smtpcontentfilter_init ( statepp )
     struct ctf_state **statepp;
{
	struct ctf_state *state = *statepp;
	struct stat stbuf;

	if (!contentfilter) return -1; /* D'uh!  Not configured! */

	if (!state)
	  state = calloc(1, sizeof(*state));

	if (!state) return -1;

	*statepp = state; /* alloced! */


	memset( state, 0, sizeof(*state) );
	state->fd_io = -1;


	if (stat(contentfilter, &stbuf)) {
	  type(NULL,0,NULL, "contentfilter stat(%s) error %d",
	       contentfilter, errno);
	  return 0;
	}

	if (S_ISREG(stbuf.st_mode)) {

	  int toserver[2];
	  int rc;

	  /* Abusing the thing, to be exact, but... */
	  rc = socketpair(PF_UNIX, SOCK_STREAM, 0, toserver);
	  if (rc != 0) return -2; /* create fail */

	  state->fd_io = toserver[1];
	  rc = fdpass_sendfd(contentfilter_rdz_fd, toserver[0]);

	  if (debug)
	    type(NULL,0,NULL,"smtpcontentfilter_init: fdpass_sendfd(%d,%d) rc=%d, errno=%s",
		 contentfilter_rdz_fd, toserver[0], rc, strerror(errno));

	  if (rc != 0) {
	    /* did error somehow */
	    close(toserver[0]);
	    close(toserver[1]);
	    return -3;
	  }
	  close(toserver[0]); /* Sent or not, close the remote end
				 from our side. */

	} else {
	  /* PRESUMING HERE: If the file is not regular, it is socket.. */

	  int msgsock;
	  struct sockaddr_un server;

	  memset((char*)&server,0,sizeof(server));
	  server.sun_family = AF_UNIX;
	  strncpy(server.sun_path,contentfilter,sizeof(server.sun_path)-1);
	  server.sun_path[sizeof(server.sun_path)-1]='\0';

	  msgsock = socket(AF_UNIX,SOCK_STREAM,0);

	  if (msgsock < 0) {
	    type(NULL,0,NULL, "contentfilter socket(%s) error %d (%s)",
		 contentfilter,errno,strerror(errno));
	    return(0);
	  }
	  if (connect(msgsock,(struct sockaddr *)&server,
		      sizeof(server) - sizeof (server.sun_path)
		      + strlen(server.sun_path)+1) < 0) {
	    type(NULL,0,NULL, "contentfilter connect(%s) error %d (%s)",
		 contentfilter,errno,strerror(errno));
	    return 0;
	  }
	  state->fd_io = msgsock;
	}

	if (debug)
	  type(NULL,0,NULL,"smtpcontentfilter_init; 9");

	fd_blockingmode(state->fd_io);

	state->outfp = fdopen(state->fd_io, "w");

	if (debug)
	  type(NULL,0,NULL,"smtpcontentfilter_init; 10");
	errno = 0;

	if (state->buf) state->buf[0] = 0;
	if (fdgets( & state->buf, 0, & state->buflen, & state->fdb, state->fd_io, 10 ) <= 0) {
	  /* something failed! -- timeout in 10 secs ?? */
	  if (debug)
	    type(NULL,0,NULL,"smtpcontentfilter_init; FAILURE 10-B");
	  smtpcontentfilter_kill( state );
	  return -4;
	}

	if (debug)
	  type(NULL,0,NULL,"smtpcontentfilter_init; 11; errno=%s",
	       strerror(errno));

	if ( !state->buf  || (strcmp(state->buf, Hungry) != 0) )
	  return -5; /* Miserable failure.. Not getting proper protocol! */

	if (debug)
	  type(NULL,0,NULL,"smtpcontentfilter_init; 12");

	state->sawhungry = 1;

	return 0;

}


char *
contentfilter_proc(ctfstatep, fname)
     void **ctfstatep;
     const char *fname;
{
	int rc;
	struct ctf_state *ctfstate =  * ctfstatep;
	time_t then;

	time( &then );


	if (! ctfstate || !ctfstate->outfp) {
	  smtpcontentfilter_init( &ctfstate );
	  if (! ctfstate || !ctfstate->outfp || !ctfstate->sawhungry) {
	    type(NULL, 0, NULL, "Failed to init interactive contentfilter subsystem");
	    smtpcontentfilter_kill( ctfstate );
	    return NULL;
	  }
	}

	*ctfstatep = ctfstate;

	if (! ctfstate->sawhungry ) {
	  /* Wrong initial state at this point in call! */
	  smtpcontentfilter_kill( ctfstate );
	  type(NULL, 0, NULL, "Interactive contentfilter subsystem lost internal sync ??");
	  return NULL;
	}

	/* Free the reply state, if such exists */

	if (ctfstate->pbuf) free(ctfstate->pbuf);
	ctfstate->pbuf = NULL;

	/* We have seen  "#hungry\n",  now we go and send our little thingie
	   down the pipe... */

	fprintf(ctfstate->outfp, "%s\n", fname);
	fflush(ctfstate->outfp);
	if (ferror(ctfstate->outfp)) {
	  fclose(ctfstate->outfp);
	  ctfstate->outfp = NULL;
	  return NULL;
	}

	/* Now we collect replies, until we see "#hungry" again.. */
	/* .. we do strip the trailing newline first.. */
	ctfstate->sawhungry = 0;

	while ( ! ctfstate->sawhungry ) {

	  /* The reply is better to reach us within 60 seconds.. */

	  if (ctfstate->buf) ctfstate->buf[0] = 0;
	  rc = fdgets( & ctfstate->buf, 0, & ctfstate->buflen, & ctfstate->fdb, ctfstate->fd_io, 120 );

	  if (ctfstate->buf && (rc > 0))
	    if (ctfstate->buf[rc-1] == '\n')
	      ctfstate->buf[--rc] = 0;

	  if (debug)
	    type(NULL,0,NULL, "fdgets()->%p rc=%d buf=\"%s\"",
		 ctfstate->buf, rc, (ctfstate->buf ? ctfstate->buf : "<NULL>"));

	  if (rc <= 0) {
	    /* TIMED OUT !  BRR... (or EOFed?) */
	    smtpcontentfilter_kill( ctfstate );
	    type(NULL, 0, NULL, "Interactive contentfilter %s!",
		 (rc < 0) ? "timed out" : "EOFed" );
	    return NULL;
	  }

	  if ( strcmp(ctfstate->buf, "#hungry") == 0 ) {
	    ctfstate->sawhungry = 1;
	    /* Got "#hungry",  bail out, and yield pbuf .. */
	    if (debug)
	      type(NULL,0,NULL," GOT #hungry !");

	    return ctfstate->pbuf;
	  }

	  /* We have a reply line here.. */
	  {
	    int i;
	    rc = sscanf(ctfstate->buf, "%i %i %i", &i, &i, &i);
	  }
	  if (rc < 2) {
	    /* BAD! */
	    continue;
	  }
	  if (ctfstate->pbuf) free(ctfstate->pbuf);
	  ctfstate->pbuf = strdup(ctfstate->buf);

	}

	time( &now );
	if ((now - then) > 30)
	  type(NULL,0,NULL, "C-FILTER DELAY %d SECONDS!",
	       (int)(now - then));

	return NULL; /* very bad! */
}
