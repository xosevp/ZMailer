/*
 *	Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *	This will be free software, but only when it is finished.
 *	Copyright 1992-2003 Matti Aarnio -- this & that smaller, and
 *	larger changes...
 */

#include "hostenv.h"
#include <stdio.h>
#include <ctype.h>
#include "zmalloc.h"
#include <pwd.h>
#include <sysexits.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "ta.h"
#include "mail.h"
#include "zmsignal.h"
#include "zsyslog.h"

#include "libz.h"
#include "libc.h"

#include "shmmib.h"

#define	PROGNAME	"errormail"
#define	CHANNEL		"error"	/* the default channel name we deliver for */

const char *progname;

extern char *optarg;
extern int  optind;
extern void process __((struct ctldesc *));

FILE *verboselog = NULL;

#ifndef strchr
extern char *strrchr(), *strchr();
#endif

#ifndef	MAXPATHLEN
#define	MAXPATHLEN 1024
#endif	/* MAXPATHLEN */

int D_alloc = 0; /* For tmalloc() from libz.a ... */


static void MIBcountCleanup __((void))
{
	MIBMtaEntry->taerrm.TaProcCountG -= 1;
	if (MIBMtaEntry->taerrm.TaProcCountG > 99999U)
	  MIBMtaEntry->taerrm.TaProcCountG = 0;

}

static void SHM_MIB_diag(rc)
     const int rc;
{
  switch (rc) {
  case EX_OK:
    /* OK */
    MIBMtaEntry->taerrm.TaRcptsOk ++;
    break;
  case EX_TEMPFAIL:
  case EX_IOERR:
  case EX_OSERR:
  case EX_CANTCREAT:
  case EX_SOFTWARE:
  case EX_DEFERALL:
    /* DEFER */
    MIBMtaEntry->taerrm.TaRcptsRetry ++;
    break;
  case EX_NOPERM:
  case EX_PROTOCOL:
  case EX_USAGE:
  case EX_NOUSER:
  case EX_NOHOST:
  case EX_UNAVAILABLE:
  default:
    /* FAIL */
    MIBMtaEntry->taerrm.TaRcptsFail ++;
    break;
  }
}



int
main(argc, argv)
	int argc;
	char *argv[];
{
	const char *channel;
	char msgfilename[MAXPATHLEN+1];
	int errflg, c;
	struct ctldesc *dp;
	RETSIGTYPE (*oldsig) __((int));

	SIGNAL_HANDLESAVE(SIGINT, SIG_IGN, oldsig);
	if (oldsig != SIG_IGN)
	  SIGNAL_HANDLE(SIGINT, wantout);
	SIGNAL_HANDLESAVE(SIGHUP, SIG_IGN, oldsig);
	if (oldsig != SIG_IGN)
	  SIGNAL_HANDLE(SIGHUP, wantout);
	SIGNAL_HANDLESAVE(SIGTERM, SIG_IGN, oldsig);
	if (oldsig != SIG_IGN)
	  SIGNAL_HANDLE(SIGTERM, wantout);
	SIGNAL_HANDLESAVE(SIGQUIT, SIG_IGN, oldsig);
	if (oldsig != SIG_IGN)
	  SIGNAL_HANDLE(SIGQUIT, wantout);
	SIGNAL_IGNORE(SIGPIPE);


	/* This assignment MUST precede readzenv() ! */
	progname = strrchr(argv[0], '/');
	if (progname == NULL)
	  progname = argv[0];
	else
	  ++progname;

	if (getenv("ZCONFIG")) readzenv(getenv("ZCONFIG"));


	Z_SHM_MIB_Attach(1); /* we don't care if it succeeds or fails.. */

	MIBMtaEntry->taerrm.TaProcessStarts += 1;
	MIBMtaEntry->taerrm.TaProcCountG    += 1;

	atexit(MIBcountCleanup);

	errflg = 0;
	channel = CHANNEL;
	while (1) {
	  c = getopt(argc, argv, "c:V");
	  if (c == EOF)
	    break;
	  switch (c) {
	  case 'c':		/* specify channel scanned for */
	    channel = optarg;
	    break;
	  case 'V':
	    prversion(PROGNAME);
	    exit(EX_OK);
	    break;
	  default:
	    ++errflg;
	    break;
	  }
	}
	if (errflg || optind != argc) {
	  fprintf(stderr, "Usage: %s [-V] [-c channel]\n",
		  argv[0]);
	  exit(EX_USAGE);
	}

	/* We need this later on .. */
	zopenlog("errormail", LOG_PID, LOG_MAIL);

	while (!getout) {
	  char *host;

	  printf("#hungry\n");
	  fflush(stdout);

	  if (fgets(msgfilename, sizeof msgfilename, stdin) == NULL) break;
	  if (strchr(msgfilename, '\n') == NULL) break; /* No ending '\n' !
							   Must have been
							   partial input! */
	  if (strcmp(msgfilename, "#idle\n") == 0) {
	    MIBMtaEntry->taerrm.TaIdleStates += 1;
	    continue; /* Ah well, we can stay idle.. */
	  }

	  /* Input:
	       spool/file/name [ \t host.info ] \n
	   */

	  if (emptyline(msgfilename, sizeof msgfilename))
	    break;

	  MIBMtaEntry->taerrm.TaMessages += 1;

	  host = strchr(msgfilename,'\t');
	  if (host != NULL)
	    *host++ = 0;

	  dp = ctlopen(msgfilename, channel, host, &getout, ctlsticky, NULL);
	  if (dp != NULL) {
	    process(dp);
	    ctlclose(dp);
	  } else {
	    printf("#resync %s\n",msgfilename);
	    fflush(stdout);
	  }
	}
	return 0;
}

/*
#ifndef MALLOC_TRACE
univptr_t
tmalloc(n)
	size_t n;
{
	return emalloc((u_int)n);
}
#endif
*/

void encodeXtext __((Sfio_t *, const char *, int));
void encodeXtext(mfp,s,len)
     Sfio_t *mfp;
     const char *s;
     int len;
{
  while (*s && len > 0) {
    int c = (*s) & 0xFF;
    if ('!' <= c && c <= '~' && c != '+' && c != '=')
      sfputc(mfp,c);
    else
      sfprintf(mfp,"+%02X",c);
    ++s;
    --len;
  }
}

static void decodeXtext __((Sfio_t *, const char *));
static void
decodeXtext(mfp,xtext)
	Sfio_t *mfp;
	const char *xtext;
{
	for (;*xtext;++xtext) {
	  if (*xtext == '+') {
	    int c = '?';
	    sscanf(xtext+1,"%02X",&c);
	    sfputc(mfp,c);
	    if (*xtext) ++xtext;
	    if (*xtext) ++xtext;
	  } else
	    sfputc(mfp,*xtext);
	}
}


/* Pick recipient address from the input line.
   EXTREMELY Simple minded parsing.. */
static void pick_env_addr __((const char *buf, Sfio_t *mfp));
static void pick_env_addr(buf,mfp)
     const char *buf;
     Sfio_t *mfp;
{
	const char *s = buf;

	while (*s != 0 && *s != ' ' && *s != '\t' && *s != ':') ++s;
	if (*s != ':') return; /* BAD INPUT! */
	buf = ++s; /* We have skipped the initial header.. */

	s = strchr(buf,'<');
	if (s != NULL) {
	  /*  Cc:  The Postoffice managers <postoffice> */
	  buf = ++s;
	  s = strrchr(buf,'>');
	  if (s == NULL)
	    return; /* No trailing '>' ? BAD BAD! */
	  sfprintf(mfp,"todsn NOTIFY=NEVER ORCPT=rfc822;");
	  encodeXtext(mfp, buf, (int)(s - buf));
	  sfprintf(mfp,"\nto <%.*s>\n",(int)(s - buf), buf);
	} else {
	  /*  Cc: some-address  */
	  sfprintf(mfp,"todsn NOTIFY=NEVER ORCPT=rfc822;");
	  s = buf + strlen(buf) - 1;
	  encodeXtext(mfp,buf,strlen(buf));
	  sfprintf(mfp,"\nto <%s>\n",buf);
	}
}

void
process(dp)
	struct ctldesc *dp;
{
	char buf[BUFSIZ];
	const char *mailshare, *mfpath;
	Sfio_t *mfp, *efp;
	int n;
	struct rcpt *rp;
	char boundarystr[400];
	char lastchar;
	int reportcnt = 0;
	struct stat stbuf;
	long inum, mtimems;
	time_t mtime;
	const char *fromuser;

	if (fstat(dp->msgfd, &stbuf) != 0)
	  abort(); /* This is a "CAN'T FAIL" case.. */

	MIBMtaEntry->taerrm.TaDeliveryStarts += 1;

	/* recipient host field is the error message file name in FORMSDIR */
	/* recipient user field is the address causing the error */

	rp = dp->recipients;

	if ( dp->senders == NULL ||
	     STREQ(rp->addr->link->channel,"error") ) {
	  /*
	   * If there was no error return address
	   * it might be because this message was
	   * an error message being bounced back.
	   * We do NOT want to bounce this, but
	   * instead just drop it on the floor.
	   */
	  for (rp = dp->recipients; rp != NULL; rp = rp->next) {
	    diagnostic(verboselog, rp, EX_OK, 0, "error bounce dropped");
	    SHM_MIB_diag(EX_OK);
	  }
	  return;
	}

	if ((mfp = sfmail_open(MSG_RFC822)) == NULL) {
	  for (rp = dp->recipients; rp != NULL; rp = rp->next) {
	    diagnostic(verboselog, rp, EX_TEMPFAIL, 0, "sfmail_open failure");
	    SHM_MIB_diag(EX_TEMPFAIL);
	  }
	  warning("Cannot open mail file!");
	  return;
	}

	{
	  char *dom = mydomain(); /* transports/libta/buildbndry.c */
	  struct stat stbuf;

	  /* This is NOT spoolid, the mtime will change yet... */
	  fstat(sffileno(mfp),&stbuf);
	  taspoolid(boundarystr,  (long)stbuf.st_ino, stbuf.st_mtime,
#ifdef HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC
		    stbuf.st_mtim.tv_nsec
#else
#ifdef HAVE_STRUCT_STAT_ST_ATIM___TV_NSEC
		    stbuf.st_mtim.__tv_nsec
#else
#ifdef HAVE_STRUCT_STAT_ST_ATIMENSEC
		    stbuf.st_mtimensec
#else
		    0
#endif
#endif
#endif
		    );
	  strcat(boundarystr, "=_/errmail/");
	  strcat(boundarystr, dom);
	}

	sfprintf(mfp, "channel error\n");
	sfprintf(mfp, "errormsg\n");

	rp = dp->recipients;

	sfprintf(mfp, "todsn NOTIFY=NEVER ORCPT=RFC822;");
	encodeXtext(mfp, rp->addr->link->user, strlen(rp->addr->link->user));

	if (STREQ(rp->addr->link->channel,"error"))
	  rp->addr->link->user = "postmaster";

	sfprintf(mfp, "\nto <%s>\n",rp->addr->link->user);

	/* copy error message file itself */
	mailshare = getzenv("MAILSHARE");
	if (mailshare == NULL)
	  mailshare = MAILSHARE;

	mfpath = emalloc(3 + strlen(mailshare) + strlen(FORMSDIR) +
			 strlen(rp->addr->host));
	sprintf((char*)mfpath, "%s/%s/%s",
		mailshare, FORMSDIR, rp->addr->host);

	efp = sfopen(NULL, mfpath, "r");
	if (efp != NULL) {
	  int inhdr = 1;
	  buf[sizeof(buf)-1] = 0;
	  while (csfgets(buf,sizeof(buf)-1,efp) >= 0) {
	    if (strncmp(buf,"HDR",3)==0)
	      continue;
	    else if (strncmp(buf,"ADR",3)==0)
	      pick_env_addr(buf+4,mfp);
	    else if (strncmp(buf,"SUB",3)==0)
	      continue;
	    else
	      break;
	  }
	  sfseek(efp,(Sfoff_t)0,0); /* Rewind! */
	  sfprintf(mfp,"env-end\n");

	  /* copy To: from error return address */
	  sfprintf(mfp, "To: <%s>\n", rp->addr->link->user);

	  while (csfgets(buf,sizeof(buf)-1,efp) >= 0) {
	    if (strncmp(buf,"HDR",3)==0) {
	      sfprintf(mfp, "%s", buf+4);
	    } else if (strncmp(buf,"ADR",3)==0) {
	      sfprintf(mfp, "%s", buf+4);
	    } else if (strncmp(buf,"SUB",3)==0) {
	      sfprintf(mfp, "%s", buf+4);
	    } else {
	      if (inhdr) {
		inhdr = 0;
		sfprintf(mfp, "MIME-Version: 1.0\n");
		sfprintf(mfp, "Content-Type: multipart/report;\n");
		sfprintf(mfp, "\treport-type=delivery-status;\n");
		sfprintf(mfp, "\tboundary=\"%s\"\n\n\n",boundarystr);
		sfprintf(mfp, "--%s\n", boundarystr);
		sfprintf(mfp, "Content-Transfer-Encoding: 7BIT\n");
		sfprintf(mfp, "Content-Type: text/plain; charset=US-ASCII\n");
	      }
	      sfprintf(mfp, "%s", buf);
	    }
	  } /* ... while() ends.. */
	  sfclose(efp);
	} else {
	  sfprintf(mfp,"to <postmaster>\n"); /* Default-form Cc: ... */
	  sfprintf(mfp,"env-end\n");

	  sfprintf(mfp, "From: The Post Office <postmaster>\n");
	  sfprintf(mfp, "Subject: email delivery error\n");

	  sfprintf(mfp, "To: <%s>\n",rp->addr->link->user);

	  sfprintf(mfp, "Precedence: junk\n"); /* BSD sendmail */
	  sfprintf(mfp,"MIME-Version: 1.0\n");
	  sfprintf(mfp, "Content-Type: multipart/report;\n");
	  sfprintf(mfp, "\treport-type=delivery-status;\n");
	  sfprintf(mfp, "\tboundary=\"%s\"\n\n\n", boundarystr);
	  sfprintf(mfp, "--%s\n", boundarystr);
	  sfprintf(mfp, "Content-Transfer-Encoding: 7BIT\n");
	  sfprintf(mfp, "Content-Type: text/plain; charset=US-ASCII\n");
	  sfprintf(mfp, "\nProcessing your mail message caused the following errors:\n");
	}
	/* print out errors in standard format */
	sfputc(mfp, '\n');
	for (rp = dp->recipients; rp != NULL; rp = rp->next) {
	  /* If not prohibited, print it! */
	  if ( rp->notifyflgs & _DSN_NOTIFY_FAILURE ) {
	    sfprintf(mfp, "error: %s: %s\n", rp->addr->host, rp->addr->user);
	    ++reportcnt;
	  }
	}

	/* Did we report anything ? */
	if (reportcnt == 0){
	  /* No, throw it away and ack success. */
	  sfmail_abort(mfp);
	  for (rp = dp->recipients; rp != NULL; rp = rp->next)
	    if (!(rp->notifyflgs & _DSN_NOTIFY_FAILURE)) {
	      diagnostic(verboselog, rp, EX_OK, 0, "discarded, reportcnt=0");
	      SHM_MIB_diag(EX_OK);
	    }
	  return;
	}

	sfprintf(mfp, "\n--%s\n", boundarystr);
	sfprintf(mfp, "Content-Type: message/delivery-status\n\n");

	/* Print out errors in IETF-NOTARY format as well! */

	if (mydomain() != NULL) {
	  sfprintf(mfp, "Reporting-MTA: dns; %s\n", mydomain() );
	} else {
	  sfprintf(mfp, "Reporting-MTA: x-local-hostname; -unknown-\n");
	}
	if (dp->envid != NULL) {
	  sfprintf(mfp, "Original-Envelope-Id: ");
	  decodeXtext(mfp,dp->envid);
	  sfputc(mfp,'\n');
	}

	rp = dp->recipients;
	fromuser = rp->addr->link->user;
	if (*fromuser == 0 ||
	    STREQ(rp->addr->link->channel, "error"))
	  fromuser = "";

	/* rfc822date() returns a string with trailing newline! */
	sfprintf(mfp, "Arrival-Date: %s", rfc822date(&stbuf.st_ctime));
	sfprintf(mfp, "Return-Path: <%.999s>\n", fromuser);
	sfprintf(mfp, "\n");

	for (rp = dp->recipients; rp != NULL; rp = rp->next) {
	  /* If not prohibited, print it! */
	  const char *typetag;
	  const char *rcpt;
	  static const char *type_rfc   = "RFC822";
	  static const char *type_local = "X-LOCAL";

	  if (rp->notify != NULL && CISTREQN(rp->notify,"NEVER",5))
	    continue;
	  rcpt = rp->addr->user;
	  if (strchr(rcpt,'@') != NULL) {
	    typetag = type_rfc;
	    if (strncmp(rcpt,"ns:",3)==0) /* 'hold'-channel stuff */
	      typetag = type_local;
	  } else
	    typetag = type_local;
	  if (rp->orcpt != NULL) {
	    sfprintf(mfp, "Original-Recipient: ");
	    decodeXtext(mfp,rp->orcpt);
	    sfputc(mfp,'\n');
	  }
	  sfprintf(mfp, "Final-Recipient: %s; %s\n", typetag, rcpt);
	  sfprintf(mfp, "Action: failed\n");
	  sfprintf(mfp, "Status: 5.0.0\n");
	  sfprintf(mfp, "Diagnostic-Code: X-LOCAL; 500 (%s)\n", rp->addr->host );
	  sfprintf(mfp, "\n");
	}

	sfprintf(mfp, "--%s\n", boundarystr);
	sfprintf(mfp, "Content-Type: message/rfc822\n\n");

	/* Skip over the delivery envelope lines! */

	rp = dp->recipients;
	/* copy original message file */

	/* seek to message body -- try it anyway */
	lseek(dp->msgfd, (off_t)(dp->msgbodyoffset), SEEK_SET);

	/* write the (new) headers with local "Received:"-line.. */

	header_received_for_clause(rp, 0, verboselog);

	swriteheaders(rp, mfp, "\n", 0, 0, NULL);
	sfprintf(mfp,"\n");

	/* If the DSN RET=HDRS is in effect, don't copy the msg body! */
	if (!dp->dsnretmode || CISTREQN(dp->dsnretmode,"FULL",4)) {

	  /* Copy out the rest with somewhat more efficient method */
	  lastchar = 0;
	  while ((n = read(dp->msgfd, buf, sizeof(buf))) > 0) {
	    sfwrite(mfp, buf, n);
	    lastchar = buf[n-1];
	  }
	  if (lastchar != '\n')
	    sfputc(mfp, '\n');
	}

	sfprintf(mfp, "--%s--\n", boundarystr);
	if (sferror(mfp)) {
	  sfmail_abort(mfp);
	  n = EX_IOERR;
	} else if (_sfmail_close_(mfp, &inum, &mtime, &mtimems) == EOF)
	  n = EX_IOERR;
	else
	  n = EX_OK;
	{
	  /* Ok, build response with proper "spoolid" */
	  char taspid[30];
	  taspoolid(taspid, inum, mtime, mtimems);

	  for (rp = dp->recipients; rp != NULL; rp = rp->next) {
	    diagnostic(verboselog, rp, n, 0, taspid);
	    SHM_MIB_diag(n);
	  }
	}
}
