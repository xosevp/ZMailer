/*
 *	Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *	This will be free software, but only when it is finished.
 *	Copyright 1991-1999 by Matti Aarnio -- modifications, including MIME
 */

#define	RFC974		/* If BIND, check that TCP SMTP service is enabled */

#define	TIMEOUT		(5*60)	/* timeout in seconds, per exchange */
#define ALARM_BLOCKSIZE (6*560) /* less than 'smtp_bufsize' */
#define ALARM_DOTTOOK   (60*60)
			/* RFC 1123 recommends:
			     - Initial 220: 5 minutes
			     - MAIL, RCPT : 5 minutes
			     - DATA initialization (until "354.."): 2 minutes
			     - While writing data, a block
			       at the time: 3 minutes  (How large a block ?)
			     - From "." to "250 OK": 10 minutes

			   I think we simplify:  5 minutes each, except "."
			   to "250 ok" which is 60 (!) minutes. (sendmail's
			   default value.)
			   Block-size is 1kB.   4-Feb-95: [mea@utu.fi]

			 */

#define DefCharset "ISO-8859-1"


#define CHUNK_MAX_SIZE 64000

#define DO_CHUNKING 1

#include "hostenv.h"

#ifdef linux_xx
#define __USE_BSD 1
#endif
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include "zmsignal.h"
#include <sysexits.h>
/* #include <strings.h> */ /* poorly portable.. */
#ifdef HAVE_STDARG_H
# include <stdarg.h>
#else
# include <varargs.h>
#endif
#include <fcntl.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <string.h>

#include "ta.h"

#include "mail.h"
#include "zsyslog.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_NETINET_IN6_H
# include <netinet/in6.h>
#endif
#ifdef HAVE_NETINET6_IN6_H
# include <netinet6/in6.h>
#endif
#ifdef HAVE_LINUX_IN6_H
# include <linux/in6.h>
#endif
#include <netdb.h>
#ifndef EAI_AGAIN
# include "netdb6.h"
#endif

#include "zmalloc.h"
#include "libz.h"
#include "libc.h"

#include "dnsgetrr.h"


#ifdef _AIX /* Defines NFDBITS, et.al. */
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <sys/time.h>

#ifndef	NFDBITS
/*
 * This stuff taken from the 4.3bsd /usr/include/sys/types.h, but on the
 * assumption we are dealing with pre-4.3bsd select().
 */

/* #error "FDSET macro susceptible" */

typedef long	fd_mask;

#ifndef	NBBY
#define	NBBY	8
#endif	/* NBBY */
#define	NFDBITS		((sizeof fd_mask) * NBBY)

/* SunOS 3.x and 4.x>2 BSD already defines this in /usr/include/sys/types.h */
#ifdef	notdef
typedef	struct fd_set { fd_mask	fds_bits[1]; } fd_set;
#endif	/* notdef */

#ifndef	_Z_FD_SET
/* #warning "_Z_FD_SET[1]" */
#define	_Z_FD_SET(n, p)   ((p)->fds_bits[0] |= (1 << (n)))
#define	_Z_FD_CLR(n, p)   ((p)->fds_bits[0] &= ~(1 << (n)))
#define	_Z_FD_ISSET(n, p) ((p)->fds_bits[0] & (1 << (n)))
#define _Z_FD_ZERO(p)	  memset((char *)(p), 0, sizeof(*(p)))
#endif	/* !FD_SET */
#endif	/* !NFDBITS */

#ifdef FD_SET
/* #warning "_Z_FD_SET[2]" */
#define _Z_FD_SET(sock,var) FD_SET(sock,&var)
#define _Z_FD_CLR(sock,var) FD_CLR(sock,&var)
#define _Z_FD_ZERO(var) FD_ZERO(&var)
#define _Z_FD_ISSET(i,var) FD_ISSET(i,&var)
#else
/* #warning "_Z_FD_SET[3]" */
#define _Z_FD_SET(sock,var) var |= (1 << sock)
#define _Z_FD_CLR(sock,var) var &= ~(1 << sock)
#define _Z_FD_ZERO(var) var = 0
#define _Z_FD_ISSET(i,var) ((var & (1 << i)) != 0)
#endif


#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif /* - HAVE_OPENSSL */

#if	defined(TRY_AGAIN) && defined(HAVE_RESOLVER)
#define	BIND		/* Want BIND (named) nameserver support enabled */
#endif	/* TRY_AGAIN */
#ifdef	BIND
#ifdef NOERROR
#undef NOERROR		/* Several SysV-streams using systems have NOERROR,
			   which is not the same as  <arpa/nameser.h> has! */
#endif
#include <arpa/nameser.h>
#include <resolv.h>

#ifndef	BIND_VER
#ifdef	GETLONG
/* 4.7.3 introduced the {GET,PUT}{LONG,SHORT} macros in nameser.h */
#define	BIND_VER	473
#else	/* !GETLONG */
#define	BIND_VER	472
#endif	/* GETLONG */
#endif	/* !BIND_VER */
#endif	/* BIND */

/* Define all those things which exist on newer BINDs, and which may
   get returned to us, when we make a query with  T_ANY ... */

#ifndef	T_TXT
# define T_TXT 16	/* Text strings */
#endif
#ifndef T_RP
# define T_RP 17	/* Responsible person */
#endif
#ifndef T_AFSDB
# define T_AFSDB 18	/* AFS cell database */
#endif
#ifndef T_X25
# define T_X25 19	/* X.25 calling address */
#endif
#ifndef T_ISDN
# define T_ISDN 20	/* ISDN calling address */
#endif
#ifndef T_RT
# define T_RT 21	/* router */
#endif
#ifndef T_NSAP
# define T_NSAP 22	/* NSAP address */
#endif
#ifndef T_NSAP_PTR
# define T_NSAP_PTR 23	/* reverse NSAP lookup (depreciated) */
#endif
#ifndef	T_UINFO
# define T_UINFO 100
#endif
#ifndef T_UID
# define T_UID 101
#endif
#ifndef T_GID
# define T_GID 102
#endif
#ifndef T_UNSPEC
# define T_UNSPEC 103
#endif
#ifndef T_SA
# define T_SA 200		/* Shuffle addresses */
#endif



#ifndef	SEEK_SET
#define	SEEK_SET	0
#endif	/* SEEK_SET */
#ifndef SEEK_CUR
#define SEEK_CUR   1
#endif
#ifndef SEEK_XTND
#define SEEK_XTND  2
#endif

#ifndef	IPPORT_SMTP
#define	IPPORT_SMTP	25
#endif 	/* IPPORT_SMTP */

#define	PROGNAME	"smtpclient"	/* for logging */
#define	CHANNEL		"smtp"	/* the default channel name we deliver for */

#ifndef	MAXHOSTNAMELEN
#define	MAXHOSTNAMELEN 64
#endif	/* MAXHOSTNAMELEN */

#define MAXFORWARDERS	128	/* Max number of MX rr's that can be listed */

#define GETADDRINFODEBUG	0 /* XXX: Only w/ bundled libc/getaddrinfo.c */
#define GETMXRRDEBUG		1


extern char *defcharset;
extern char myhostname[MAXHOSTNAMELEN+1];
extern int myhostnameopt;
extern char errormsg[ZBUFSIZ]; /* Global for the use of  dnsgetrr.c */
extern const char *progname;
extern const char *cmdline, *eocmdline, *logfile, *msgfile;
extern int pid;
extern int debug;
extern int verbosity;
extern int conndebug;
extern int dotmode;		/* At the SMTP '.' phase, DON'T LEAVE IMMEDIATELY!. */
extern int getout;		/* signal handler turns this on when we are wanted to abort! */
extern int timeout;		/* how long do we wait for response? (sec.) */
extern int conntimeout;		/* connect() timeout */
extern int gotalarm;		/* indicate that alarm happened! */
extern int noalarmjmp;		/* Don't long-jmp on alarm! */
extern jmp_buf alarmjmp;
extern jmp_buf procabortjmp;
extern int procabortset;
#if !(defined(HAVE_MMAP) && defined(TA_USE_MMAP))
extern int readalready;		/* does buffer contain valid message data? */
#endif
extern int wantreserved;	/* open connection on secure (reserved) port */
extern int statusreport;	/* put status reports on the command line */
extern int force_8bit;		/* Claim to the remote to be 8-bit system, even
				   when it doesn't report itself as such..*/
extern int force_7bit;		/* and reverse the previous.. */
extern int keep_header8;	/* Don't do "MIME-2" to the headers */
extern int checkwks;
extern FILE *logfp;
extern int nobody;
extern char *localidentity;	/* If we are wanted to bind some altenate
				   interface than what the default is thru
				   normal kernel mechanisms.. */
extern int daemon_uid;
extern int first_uid;		/* Make the opening connect with the UID of the
				   sender (atoi(rp->addr->misc)), unless it is
				   "nobody", in which case use "daemon"      */

extern int D_alloc;		/* Memory usage debug */
extern int no_pipelining;	/* In case the system just doesn't cope with it */
extern int prefer_ip6;


#ifdef	lint
#undef	putc
#define	putc	fputc
#endif	/* lint */

/* Extended SMTP flags -- can downgrade from 8-bit to 7-bit while in transport
   IF  MIME-Version: is present, AND Content-Transfer-Encoding: 8BIT
   For selected "force_8bit" remotes can also DECODE Q-P MIME MSGS! */
/* If there is header:  Content-Conversion: prohibited
   DO NOT do conversions no matter what
   (even when it violates the protocol..) */

/* Following options can be declared in ESMTP  EHLO response  */
#define ESMTP_SIZEOPT    0x0001 /* RFC 1427/1653/1870 */
#define ESMTP_8BITMIME   0x0002 /* RFC 1426/1652 */
#define ESMTP_DSN        0x0004 /* RFC 1891	 */
#define ESMTP_PIPELINING 0x0008 /* RFC 1854/2197 */
#define ESMTP_ENHSTATUS  0x0010 /* RFC 2034	 */
#define ESMTP_CHUNKING   0x0020 /* RFC 1830	 */
#ifdef HAVE_OPENSSL
#define ESMTP_STARTTLS   0x0040 /* RFC 2487	 */
#endif /* - HAVE_OPENSSL */


# ifdef RFC974

#if	defined(BIND_VER) && (BIND_VER >= 473)
typedef u_char msgdata;
#else	/* !defined(BIND_VER) || (BIND_VER < 473) */
typedef char msgdata;
#endif	/* defined(BIND_VER) && (BIND_VER >= 473) */

struct mxdata {
	msgdata		*host;
	int		 pref;
	time_t		 expiry;
	struct addrinfo *ai;
};
# endif /* RFC974 */

struct smtpdisc {
  Sfdisc_t D;		/* Sfio Discipline structure		*/
  void *SS;		/* Ptr to SS context			*/
};

typedef struct {
  int  ehlo_capabilities;	/* Capabilities of the remote system */
  int  esmtp_on_banner;
  int  within_ehlo;
  int  main_esmtp_on_banner;
  int  servport;
  int  literalport;
  int  firstmx;			/* error in smtpwrite("HELO"..) */
# ifdef RFC974
  int  mxcount;
  struct mxdata mxh[MAXFORWARDERS];
# endif /* RFC974 */
  int  smtp_bufsize;		/* Size of the buffer; this should be large
				   enough to contain MOST cases of pipelined
				   SMTP information, AND still fit within
				   roundtrip TCP buffers */
  int  smtp_outcount;		/* we used this much.. */
  int  block_written;		/* written anything in async phase */
  long ehlo_sizeval;
  int  rcpt_limit;		/* Number of recipients that can be sent on
				   one session.. */

  Sfio_t *smtpfp;		/* Sfio_t* to the remote host           */
  struct smtpdisc smtpdisc;	/* SMTP outstream discipline data	*/

  char *smtphost;		/* strdup()ed name of the remote host */
  char *myhostname;		/* strdup()ed name of my outbound interface */

  FILE *verboselog;		/* verboselogfile */

  int hsize;			/* Output state variables */
  int msize;

  int pipelining;		/* Are we pipelining ? */
  int pipebufsize;		/* Response collection buffering */
  int pipebufspace;
  char *pipebuf;
  int pipeindex;		/* commands associated w/ those responses */
  int pipespace;
  int pipereplies;		/* Replies handled so far */
  char **pipecmds;
  struct rcpt **pipercpts;	/* recipients -""- */

  int rcptcnt;			/* PIPELINING variables */
  int rcptstates;
#define RCPTSTATE_OK  0x01 /* At least one OK   state   */
#define RCPTSTATE_400 0x02 /* At least one TEMP failure */
#define RCPTSTATE_500 0x04 /* At least one PERM failure */
#define FROMSTATE_400 0x08 /* MAIL FROM --> 4XX code */
#define FROMSTATE_500 0x10 /* MAIL FROM --> 5XX code */
#define DATASTATE_OK  0x20 /* DATA/BDAT --> 2/3XX code */
#define DATASTATE_400 0x40 /* DATA/BDAT --> 4XX code */
#define DATASTATE_500 0x80 /* DATA/BDAT --> 5XX code */
  int state;
  int alarmcnt;
  int column;
  int lastch;
  int chunking;

  char *chunkbuf;		/* CHUNKING, RFC-1830 */
  int   chunksize, chunkspace;

  char remotemsg[2*ZBUFSIZ];
  char remotehost[MAXHOSTNAMELEN+1];
  char *mailfrommsg;
  char ipaddress[200];

  struct addrinfo ai;		/* Lattest active connection */
  union {
    struct sockaddr_in  v4;
#if defined(AF_INET6) && defined(INET6)
    struct sockaddr_in6 v6;
#endif
  } ai_addr;
  int ismx;

  char stdinbuf[8192];
  int  stdinsize; /* Available */
  int  stdincurs; /* Consumed  */

  int   sslmode;		/* Set, when SSL/TLS in running */
#ifdef HAVE_OPENSSL
  SSL * ssl;
  SSL_CTX * ctx;

  char *sslwrbuf;
  int   sslwrspace, sslwrin, sslwrout;
  /* space, how much stuff in, where the output cursor is */
#endif /* - HAVE_OPENSSL */
  char *tls_cipher_info;
  char *tls_ccert_subject;
  char *tls_ccert_issuer;
  char *tls_ccert_fingerprint;

} SmtpState;

extern const char *FAILED;
extern time_t now;

extern int errno;
#ifndef MALLOC_TRACE
extern void * emalloc __((size_t));
extern void * erealloc __((void *, size_t));
#endif
/*
   extern int  atoi __((char*));
   extern long atol __((char*));
 */
extern char *strerror();
#ifndef strchr
extern char *strchr();
extern char *strrchr();
#endif
extern char *dottedquad();
extern char *optarg;
extern int optind;

extern char **environ;

extern int deliver    __((SmtpState *SS, struct ctldesc *dp, struct rcpt *startrp, struct rcpt *endrp));
extern int writebuf   __((SmtpState *SS, const char *buf, int len));
extern int writemimeline __((SmtpState *SS, const char *buf, int len, int cvtmode));
extern int appendlet  __((SmtpState *SS, struct ctldesc *dp, int convertmode));
extern int smtpopen   __((SmtpState *SS, const char *host, int noMX));
extern int smtpconn   __((SmtpState *SS, const char *host, int noMX));
extern int smtp_ehlo  __((SmtpState *SS, const char *strbuf));
extern int ehlo_check __((SmtpState *SS, const char *buf));
extern void smtp_flush __((SmtpState *SS));
extern int smtp_sync  __((SmtpState *SS, int, int));
extern int smtpwrite  __((SmtpState *SS, int saverpt, const char *buf, int pipelining, struct rcpt *syncrp));
extern int process    __((SmtpState *SS, struct ctldesc*, int, const char*, int));

extern int check_7bit_cleanness __((struct ctldesc *dp));
extern void notarystatsave __((SmtpState *SS, char *smtpstatline, char *status));

extern int makeconn  __((SmtpState *SS, struct addrinfo *, int));
extern int makereconn __((SmtpState *SS));
extern int vcsetup  __((SmtpState *SS, struct sockaddr *, int*, char*));
#ifdef	BIND
extern int rightmx  __((const char*, const char*, void*));
extern int h_errno;
extern int res_mkquery(), res_send(), dn_skipname(), dn_expand();
# ifdef RFC974
extern int getmxrr __((SmtpState *, const char*, struct mxdata*, int, int));
# endif /* RFC974 */
#endif	/* BIND */
extern int matchroutermxes __((const char*, struct taddress*, void*));
extern RETSIGTYPE sig_pipe __((int));
extern int getmyhostname();
extern void stashmyaddresses();
extern void getdaemon();
extern int  has_readable __((int));
extern int  bdat_flush __((SmtpState *SS, int lastflg));
extern void smtpclose __((SmtpState *SS));
extern void pipeblockread __((SmtpState *SS));
extern ssize_t smtp_sfwrite __((Sfio_t *, const void *, size_t, Sfdisc_t *));
extern int smtp_nbread      __((SmtpState *, void *, int));

#if defined(HAVE_STDARG_H) && defined(__STDC__)
extern void report __((SmtpState *SS, char *fmt, ...));
#else
extern void report();
#endif

#ifdef HAVE_OPENSSL
extern int  tls_init_clientengine __((SmtpState *SS, char *cfgpath));
extern int  tls_start_clienttls __((SmtpState *SS, const char *peername));
#endif /* - HAVE_OPENSSL */

extern char *logtag __((void));

extern time_t starttime, endtime;