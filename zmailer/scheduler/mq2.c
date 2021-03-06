/*
 *	ZMailer 2.99.53+ Scheduler "mailq2" routines
 *
 *	Copyright Matti Aarnio <mea@nic.funet.fi> 1999-2006
 *
 */

#include "scheduler.h"
#include <ctype.h>
#include <unistd.h>
#include "zsyslog.h"
/* #include <stdlib.h> */
#include <errno.h>

#include "ta.h"
#include "libz.h"

#include "prototypes.h"

#ifdef HAVE_SYS_LOADAVG_H
#include <sys/loadavg.h>
#endif

#if	defined(BSD4_3) || defined(sun)
#include <sys/file.h>
#endif
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "zmsignal.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include "libc.h"

#include "zmpoll.h"


static void mq2interpret __((struct mailq *, char *));

static struct mailq *mq2root  = NULL;
static int           mq2count = 0;
static int	     mq2max   = 20; /* How many can live simultaneously */
static int           max_mq_life = 90; /* 90 seconds for an action */

int mq2_active __((void))
{
  return (mq2root != NULL);
}

/* INTERNAL */
static void mq2_discard(mq)
     struct mailq *mq;
{
  struct mailq **mqp = &mq2root;
  while (*mqp) {
    if (*mqp == mq) {
      *mqp = mq->nextmailq;
      break;
    }
    mqp = &((*mqp)->nextmailq);
  }

  close(mq->fd);

  if (mq->inbuf)
    free(mq->inbuf);
  if (mq->inpline)
    free(mq->inpline);
  if (mq->outbuf)
    free(mq->outbuf);
  if (mq->challenge)
    free(mq->challenge);

  free(mq);

  --mq2count;
  MIBMtaEntry->sc.MQ2sockParallel --;
}

/* EXTERNAL */
int mq2_putc(mq,c)
     struct mailq *mq;
     int c;
{
  if (!mq->outbuf) {
    mq->outbufspace = 500;
    mq->outbuf = emalloc(mq->outbufspace);
  }

  if (mq->outbufsize+2 >= mq->outbufspace) {
    mq->outbufspace *= 2;
    mq->outbuf = erealloc(mq->outbuf, mq->outbufspace);
  }

  if (mq->outbuf == NULL)
    return -2; /* Out of memory :-/ */

  /* SMTP rule: duplicate the dot at the beginning of the line */
  if (c == '.' && mq->outcol == 0)
    mq->outbuf[mq->outbufsize ++] = c;

  mq->outbuf[mq->outbufsize ++] = c;
  if (c == '\n')
    mq->outcol = 0;
  else
    mq->outcol++;

  return 0; /* Implementation ok */
}

/* EXTERNAL */
int mq2_puts(mq,s)
     struct mailq *mq;
     char *s;
{
  int rc;
  if (!mq) return -2; /* D'uh...  FD is not among MQs.. */

  for (;s && *s; ++s)
    if ((rc = mq2_putc(mq,*s)) < 0)
      return rc;

  return 0; /* Ok. */
}

int mq2_puts_(mq,s)
     struct mailq *mq;
     char *s;
{
  int rc;
  if (!mq) return -2; /* D'uh...  FD is not among MQs.. */

  mq->outcol++;

  for (;s && *s; ++s)
    if ((rc = mq2_putc(mq,*s)) < 0)
      return rc;

  return 0; /* Ok. */
}


/*
 * mq2: wflush() - return <0: error detected,
 *                        >0: write pending,
 *                       ==0: flush complete
 */
/* INTERNAL */
int mq2_wflush(mq)
     struct mailq *mq;
{
  if (verbose)
    sfprintf(sfstderr,"mq2_wflush() fd = %d", mq->fd);

  while (mq->outbufcount < mq->outbufsize) {
    int r, i;
    i = mq->outbufsize - mq->outbufcount;
    r = write(mq->fd, mq->outbuf + mq->outbufcount, i);

    if (r > 0) {

      /* Some written! */
      mq->outbufcount += r;

    } else {
      /* Error ??? */
      if (errno == EAGAIN || errno == EINTR)
	break; /* Back later .. */
      /* Err... what ?? */

      if (verbose)
	sfprintf(sfstderr, " -- failure; errno = %d\n", errno);

      mq2_discard(mq);
      MIBMtaEntry->sc.MQ2sockWriteFails ++;

      return -1;
    }
  }
  if (mq->outbufcount >= mq->outbufsize)
    mq->outbufcount = mq->outbufsize = 0;

  /* Shrink the outbuf, if you can.. */
  if (mq->outbufcount > 0) {
    int l = mq->outbufsize - mq->outbufcount;
    if (l)
      memcpy(mq->outbuf, mq->outbuf + mq->outbufcount, l);
    mq->outbufcount = 0;
    mq->outbufsize = l;
  }

  if (verbose && (mq->outbufsize - mq->outbufcount))
    sfprintf(sfstderr," -- ok; buf left: %d chars\n",
	    mq->outbufsize - mq->outbufcount);
  else if (verbose)
    sfprintf(sfstderr,"\n");

  return (mq->outbufcount < mq->outbufsize);
}

/* INTERNAL */
static void mq2_iputc(mq,c)
     struct mailq *mq;
     int c;
{
  if (!mq->inpline) {
    mq->inplinespace = 500;
    mq->inplinesize = 0;
    mq->inpline = emalloc(mq->inplinespace);
  }
  if (mq->inplinesize +2 >= mq->inplinespace) {
    mq->inplinespace *= 2;
    mq->inpline = erealloc(mq->inpline, mq->inplinespace);
  }
  if (mq->inpline == NULL)
    return;
  mq->inpline[mq->inplinesize ++] = c;
}


/*
 *  Copies characters from  inbuf[]  to  inpline[], and terminates
 *  when it  1) gets '\n' (appends '\000' to the string, returns
 *  pointer to begining of the string),  2) runs out of the inbuf[],
 *  and returns NULL.
 *
 */
/* INTERNAL */
static char *mq2_gets(mq)
     struct mailq *mq;
{
  int c;
  char *ret = NULL;

  while (mq->inbufcount < mq->inbufsize) {
    c = mq->inbuf[mq->inbufcount++];
    if (c == '\n') {
      /* Got a complete line */
      mq2_iputc(mq,'\000');
      mq->inplinesize = 0;
      ret = mq->inpline;
      break;
    } else if (c != '\r') {
      mq2_iputc(mq,c);
    }
  }
  /* Shrink the input buffer a bit, if you can.. */
  if (mq->inbufcount > 0) {
    c = mq->inbufsize - mq->inbufcount;
    if (c <= 0)
      mq->inbufsize = mq->inbufcount = 0;
    else {
      memcpy(mq->inbuf, mq->inbuf + mq->inbufcount, c);
      mq->inbufcount = 0;
      mq->inbufsize = c;
    }
  }
  return ret;
}



/* INTERNAL */
static void mq2_read(mq)
     struct mailq *mq;
{
  int i, spc;
  char *s;

  if (mq->fd < 0) {
    mq2_discard(mq);
    MIBMtaEntry->sc.MQ2sockReadFails ++;
    return;
    /* Zap! */
  }

  if (!mq->inbuf) {
    mq->inbufspace = 500;
    mq->inbuf = emalloc(mq->inbufspace);
  }

  if (mq->inbufsize+80 >= mq->inbufspace) {
    mq->inbufspace *= 2;
    /* Abort if line size is too large.. */
    if (mq->inbufspace > 17000) {
      free(mq->inbuf);
      mq->inbuf = NULL;
    } else
      mq->inbuf = erealloc(mq->inbuf, mq->inbufspace);
  }

  if (mq->inbuf == NULL) {
    mq2_discard(mq);  /* Out of memory :-/ */
    return;
  }

  spc = mq->inbufspace - mq->inbufsize;
  i = read(mq->fd, mq->inbuf + mq->inbufsize, spc);

  if (i == 0) {
    mq2_discard(mq);
    MIBMtaEntry->sc.MQ2sockReadEOF ++;
    return; /* ZAP! */
  }
  if (i > 0) {
    /* GOT SOMETHING! */
    mq->inbufsize += i;
  } else {
    if (errno == EINTR || errno == EAGAIN) {
      /* Ok, come back later */
    } else {
      mq2_discard(mq); /* ZAP! */
    }
    return;
  }

  /* Do some processing here! */

  while ((s = mq2_gets(mq)) != NULL) {
    mq2interpret(mq, s);
  }

  mq2_wflush(mq);
}


/* EXTERNAL */
void mq2_register(fd, addr)
     int fd;
     Usockaddr *addr;
{
  struct mailq *mq;
  void *mq2test;

  static int cnt = 0;
  char buf[200];
  struct timeval tv;

  if (mq2count > mq2max) {
    close(fd); /* TOO MANY! */
    return;
  }
  
  mq = emalloc(sizeof(*mq));
  if (!mq) {
    close(fd);
    return;
  }
  memset(mq, 0, sizeof(*mq));

  ++mq2count;
  MIBMtaEntry->sc.MQ2sockParallel ++;
  
  mq->fd = fd;
  mq->apoptosis = now + max_mq_life;
  mq->qaddr = *addr;

  mq->nextmailq = mq2root;
  mq2root = mq;

  /* 
     Scheduler writes following to the interface socket:

	"version zmailer 2.0\n"
	"some magic random gunk used as challenge\n"
  */

  mq2_puts(mq,"version zmailer 2.0\n");

  mq2test = mq2_authuser(mq, mq2authfile, NULL);

  if (! mq2test) {

    fd_blockingmode(fd);

    mq2_puts(mq, "550 NO ACCESS FOR YOU\n");
    mq2_wflush(mq);
  mq2_abort:;
    MIBMtaEntry->sc.MQ2sockAuthRej ++;
    mq2_discard(mq);

  } else {
  
    fd_nonblockingmode(fd);

    gettimeofday(&tv,NULL);

    sprintf(buf,"220 MAILQ-V2-CHALLENGE: %ld.%ld.%d",
	    (long)tv.tv_sec, (long)tv.tv_usec, ++cnt);

    mq->challenge = strdup(buf); /* This MAY yield NULL */
    if (!mq->challenge) goto mq2_abort;

    mq2_puts(mq, buf);
    mq2_puts(mq, "\n");
    mq2_wflush(mq);

    mq->auth = 0;
  }
}

/* EXTERNAL */
void mq2add_to_poll(fds, fdscountp)
     struct zmpollfd **fds;
     int *fdscountp;
{
  struct mailq *mq = mq2root;

  for ( ; mq ; mq = mq->nextmailq ) {
    mq->fds = NULL;

    if (mq->fd < 0)
      continue;

    /* _Z_FD_SET(mq->fd, *rdmaskp);
       if (mq->outbufcount < mq->outbufsize)
       _Z_FD_SET(mq->fd, *wrmaskp);
    */

    zmpoll_addfd(fds, fdscountp, mq->fd,
		 (mq->outbufcount < mq->outbufsize ? mq->fd : -1),
		 &mq->fds);

  }
}


/* EXTERNAL */
void mq2_areinsets(fds)
     struct zmpollfd *fds;
{
    struct mailq *mq;

    timed_log_reinit();

    /* The mq-queue may change while we are going it over,
       thus we *must not* try to do write and read things
       at the same time! and be *very* carefull at following
       the 'next' pointers... */

    mq = mq2root;
    while ( mq ) {
      struct mailq *mq2 = mq->nextmailq;
      if ( mq->fds && (mq->fds->revents & ZM_POLLOUT) ) {
	mq2_wflush(mq);
      }

      /* Time of forced death ? */
      if (now > mq->apoptosis) {
	MIBMtaEntry->sc.MQ2sockTimedOut ++;
	mq2_discard(mq);
      }

      mq = mq2;
    }

    mq = mq2root;
    while ( mq ) {
      struct mailq *mq2 = mq->nextmailq;
      if ( mq->fds &&
	   (mq->fds->revents & (ZM_POLLIN|ZM_POLLERR|ZM_POLLHUP)) ) {
	mq->fds = NULL;
	mq2_read(mq);
      }
      mq = mq2;
    }
}

/*
 * The  thread_report()  is Sfio_t stream based printout routine,
 * and we want to use special discipline routine which translates
 * the write backend function to 'mq2' buffering mode.
 *
 */

int zsfsetfd(fp, fd)
     Sfio_t *fp;
     int fd;
{
  /* This is *NOT* the SFIO's sfsetfd() -- we do no sfsync() at any point.. */
  fp->file = fd;
  return fd;
}

struct mq2discipline {
  Sfdisc_t D;
  struct mailq *mq;
};

ssize_t mq2_sfwrite(sfp, vp, len, discp)
     Sfio_t *sfp;
     const void * vp;
     size_t len;
     Sfdisc_t *discp;
{
    struct mq2discipline *mqd = (struct mq2discipline *)discp;
    struct mailq *mq = mqd->mq;
    int rc, i;
    char *p = (char *)vp;

    for (i = 0; len > 0; ++p, --len, ++i)
      if ((rc = mq2_putc(mq,*p)) < 0)
	return rc;

    return i;
}

static void mq2_thread_report __((struct mailq *mq, int mode, char *channel, char *host));
static void mq2_thread_report(mq, mode, channel, host)
     struct mailq *mq;
     int mode;
     char *channel, *host;
{
  Sfio_t *fp;
  struct mq2discipline mq2d;

  fp = sfnew(NULL, NULL, 0, 0, SF_LINE|SF_WRITE);
  if (!fp) {
    mq2_puts(mq, " *** FAILURE: sfnew(NULL, NULL, 0, -1, SF_LINE|SF_STRING|SF_WRITE);\n");
    return;
  }

  memset(&mq2d, 0, sizeof(mq2d));
  mq2d.mq = mq;
  mq2d.D.writef  = mq2_sfwrite;

  sfdisc(fp, &mq2d.D);


  if (channel && host)
    thread_detail_report(fp, mode, channel, host);
  else
    thread_report(fp, mode);

  zsfsetfd(fp, -1);
  sfclose(fp);
}


/* INTERNAL */
static int mq2cmd_etrn(mq,s)
     struct mailq *mq;
     char *s;
{
  char *t = s;
  int rc;

  if (!(mq->auth & MQ2MODE_ETRN)) {
    mq2_puts(mq, "-No ETRN allowed for you.\n");
    return 0;
  }

  while (*t && (*t != ' ') && (*t != '\t')) ++t;
  if (*t) *t++ = '\000';
  while (*t == ' ' || *t == '\t') ++t;

  /* 's' points to the first arg, 't' points to string after
     separating white-space has been skipped. */
  sfprintf(sfstdout,"%s MQ2 ETRN: %s %s\n", timestring(), s, t);
  rc = turnme(s);
  if (rc)
    mq2_puts(mq, "+OK; an ETRN started something.\n");
  else
    mq2_puts(mq, "+OK; an ETRN didn't start anything.\n");

  return 0;
}

static void
scheduler_mailq_extra(fp)
     Sfio_t *fp;
{
  int pid;
  int pipe_fd[2];
  const char *scheduler_M_extra_cmd = getzenv("SCHEDULER_M_EXTRA");

  if (!scheduler_M_extra_cmd) return; /* Nothing to do.. */

  if (pipe(pipe_fd) != 0)
    return; /* Sigh.. We don't worry about e.g. EINTR here. */

  pid = fork();

  if (pid > 0) {
    /* Parent! */

    /* We are ourselves going to die rather quickly... */
# ifdef SIGCLD
    SIGNAL_HANDLE(SIGCLD,SIG_IGN);		/* Auto-reap the kids.. */
# else
    SIGNAL_HANDLE(SIGCHLD,SIG_IGN);
# endif

    close(pipe_fd[1]); /* Parent does not write */

    for (;;) {
      char c;
      int rc = read(pipe_fd[0], &c, 1);
      if (rc == 0) break; /* EOF */
      if (rc == 1) {
	sfputc(fp, c);
	continue;
      }
      if (rc < 0 && errno == EINTR)
	continue;
      break; /* ANYTHING AT ALL: break out! */
    }
    close(pipe_fd[0]);

    return;
  }
  if (pid == 0) {
    int i;
    /* Child! */
    close(pipe_fd[0]); /* Child does not read.. */

    if (pipe_fd[1] != 1)
      dup2(pipe_fd[1],1); /* STDOUT */
    if (pipe_fd[1] != 2)
      dup2(pipe_fd[1],2); /* STDERR */

    close(0);
    i = open("/dev/null",O_RDONLY,0);
    if (i > 0) dup2(i, 0);

    execl(scheduler_M_extra_cmd, scheduler_M_extra_cmd, NULL);
    _exit(255);
 }
}


/* INTERNAL */
static void mq2_show_snmp(mq)
     struct mailq *mq;
{
  int r, i;
  Sfio_t *fp;
  struct mq2discipline mq2d;

  fp = sfnew(NULL, NULL, 0, 0, SF_LINE|SF_WRITE);
  if (!fp) {
    mq2_puts(mq, " *** FAILURE: sfnew(NULL, NULL, 0, -1, SF_LINE|SF_STRING|SF_WRITE);\n");
    return;
  }

  memset(&mq2d, 0, sizeof(mq2d));
  mq2d.mq = mq;
  mq2d.D.writef  = mq2_sfwrite;

  sfdisc(fp, &mq2d.D);


  r = (Z_SHM_MIB_is_attached() > 0); /* Attached and WRITABLE ? */


#define SCHEDULER_MAILQ_EXTRA scheduler_mailq_extra(fp);

#include "mailq.inc" /* Shared stuff with  mailq.c  program */


  zsfsetfd(fp, -1);
  sfclose(fp);


}



/* INTERNAL */
static int mq2cmd_reroute(mq,s)
     struct mailq *mq;
     char *s;
{
  char *t = s, *u;

  while (*t && (*t != ' ') && (*t != '\t')) ++t;
  if (*t) *t++ = '\000';
  while (*t == ' ' || *t == '\t') ++t;
  u = t;
  while (*u && (*u != ' ') && (*u != '\t')) ++u;
  if (*u) *u++ = '\000';
  while (*u == ' ' || *u == '\t') ++u;

  /* 's' points to the first arg, 't' points to string after
     separating white-space has been skipped. */

  return -1;
}


/* INTERNAL */
static int mq2cmd_kill(mq,s)
     struct mailq *mq;
     char *s;
{
  char *t = s;

  if (!(mq->auth & MQ2MODE_KILL)) {
    mq2_puts(mq, "-No KILL allowed for you.\n");
    return 0;
  }
  while (*t && (*t != ' ') && (*t != '\t')) ++t;
  if (*t) *t++ = '\000';
  while (*t == ' ' || *t == '\t') ++t;

  /* 's' points to the first arg, 't' points to string after
     separating white-space has been skipped. */
  if (strcmp(s, "MSG") == 0) {
	sfprintf(sfstdout,"%s MQ2 KILL MSG: %s %s\n", timestring(), t);
	deletemsg(t, NULL);
	mq2_puts(mq, "+OK; KILLed something.\n\n");
  } else {
        mq2_puts(mq, "-Unknown request.\n\n");
  }

  return 0;
}

/* INTERNAL */
static int mq2cmd_show(mq,s)
     struct mailq *mq;
     char *s;
{
  char *t = s, *u;

  while (*t && (*t != ' ') && (*t != '\t')) ++t;
  if (*t) *t++ = '\000';
  while (*t == ' ' || *t == '\t') ++t;
  u = t;
  while (*u && (*u != ' ') && (*u != '\t')) ++u;
  if (*u) *u++ = '\000';
  while (*u == ' ' || *u == '\t') ++u;

  /* 's' points to the first arg, 't' points to string after
     separating white-space has been skipped. */

  /* This really should be "SHOW SNMP", but that command
     was reserved for other use... */
  if (strcmp(s,"COUNTERS") == 0) {

    MIBMtaEntry->sc.MQ2sockCommandShowCounters ++;

    mq2_puts(mq, "+OK until LF.LF\n");
    mq2_show_snmp(mq);
    mq2_puts_(mq, ".\n");
    return 0;
  }

  if (strcmp(s,"SNMP") == 0) {

    if (! (MQ2MODE_SNMP & mq->auth)) /* If not allowed operation, exit! */
      return -1;

    MIBMtaEntry->sc.MQ2sockCommandShowQueueVeryShort ++;

    mq2_puts(mq, "+OK until LF.LF\n");
    mq2_thread_report(mq, MQ2MODE_SNMP, NULL, NULL);
    mq2_puts_(mq, ".\n");
    return 0;
  }

  if (strcmp(s,"QUEUE") == 0) {

    if (strcmp(t,"SHORT") == 0) {

      if (! (MQ2MODE_QQ & mq->auth)) /* If not allowed operation, exit! */
	return -1;

      MIBMtaEntry->sc.MQ2sockCommandShowQueueShort ++;

      mq2_puts(mq, "+OK until LF.LF\n");
      mq2_thread_report(mq, MQ2MODE_QQ, NULL, NULL);
      mq2_puts_(mq, ".\n");
      return 0;

    } else if (strcmp(t,"THREADS2") == 0) {

      if (! (MQ2MODE_FULL & mq->auth)) /* If not allowed operation, exit! */
	return -1;

      MIBMtaEntry->sc.MQ2sockCommandShowQueueThreads2 ++;

      mq2_puts(mq, "+OK until LF.LF\n");
      mq2_thread_report(mq, MQ2MODE_FULL2, NULL, NULL);
      mq2_puts_(mq, ".\n");
      return 0;

    } else if (strcmp(t,"THREADS") == 0) {

      if (! (MQ2MODE_FULL & mq->auth)) /* If not allowed operation, exit! */
	return -1;

      MIBMtaEntry->sc.MQ2sockCommandShowQueueThreads2 ++;

      mq2_puts(mq, "+OK until LF.LF\n");
      mq2_thread_report(mq, MQ2MODE_FULL, NULL, NULL);
      mq2_puts_(mq, ".\n");
      return 0;

    }
    /* Unknown! */
    return -1;
  }

  if (strcmp(s,"THREAD") == 0) {
    /* SHOW THREAD 'channel' 'host' */
    char *channel = t;
    char *host    = u;

    if (! (MQ2MODE_FULL & mq->auth)) /* If not allowed operation, exit! */
      return -1;

    MIBMtaEntry->sc.MQ2sockCommandShowThread ++;

    mq2_puts(mq, "+OK until LF.LF\n");
    mq2_thread_report(mq, MQ2MODE_FULL, channel, host);
    mq2_puts_(mq, ".\n");
    return 0;
  }

  return -1;

}


/* INTERNAL */
static void mq2interpret(mq,s)
     struct mailq *mq;
     char *s;
{
  char *t = s;

  MIBMtaEntry->sc.MQ2sockCommands ++;

  while (*t && (*t != ' ') && (*t != '\t')) ++t;
  if (*t) *t++ = '\000';
  while (*t == ' ' || *t == '\t') ++t;

  /* 's' points to the initial verb, 't' points to string after
     separating white-space has been skipped. */

  if (cistrcmp(s,"QUIT")==0 || cistrcmp(s,"EXIT") == 0) {
    mq2_puts(mq, "+Bye bye\n");
    mq2_wflush(mq);
    mq2_discard(mq);
    MIBMtaEntry->sc.MQ2sockCommandQUIT ++;
    return;
  }

  if (mq->auth == 0 && strcmp(s,"AUTH") == 0) {
    MIBMtaEntry->sc.MQ2sockCommandAUTH ++;
    mq2auth(mq, mq2authfile, t);
    if (! mq->auth)
      MIBMtaEntry->sc.MQ2sockAuthRej ++;
    return;
  }

  if (!mq->auth) {
    mq2_puts(mq,"-BAD; USER MUST AUTHENTICATE\n");
    MIBMtaEntry->sc.MQ2sockCommandsRej ++;
    return;
  }

  if (strcmp(s,"SHOW") == 0) {
    if (mq2cmd_show(mq,t) == 0)
      return;
  }
  if (strcmp(s,"KILL") == 0) {
    if (mq2cmd_kill(mq,t) == 0)
      return;
  }
  if (strcmp(s,"REROUTE") == 0) {
    if (mq2cmd_reroute(mq,t) == 0)
      return;
  }
  if (strcmp(s,"ETRN") == 0) {
    MIBMtaEntry->sc.MQ2sockCommandETRN ++;
    mq2cmd_etrn(mq,t);
    return;
  }

  MIBMtaEntry->sc.MQ2sockCommandsRej ++;

  mq2_puts(mq, "-MAILQ2 Unknown command, or refused by access control; VERB='");
  mq2_puts(mq, s);
  mq2_puts(mq, "' REST='");
  mq2_puts(mq, t);
  mq2_puts(mq, "'\n");
}
