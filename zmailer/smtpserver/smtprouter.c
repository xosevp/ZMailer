/*
 *    Copyright 1988 by Rayan S. Zachariassen, all rights reserved.
 *      This will be free software, but only when it is finished.
 */
/*
 *    Several extensive changes by Matti Aarnio <mea@nic.funet.fi>
 *      Copyright 1991-1997.
 */

#include "smtpserver.h"

/*
 * The way VRFY and EXPN are implemented, and even MAIL FROM and RCPT TO
 * checking, we somehow connect to a router and ask it to do stuff for us.
 * There are three routines, one to connect to the router, one to kill it
 * off again, and the line-getting routine that gets everything the router
 * prints at us, one line at a time.
 */

static char promptbuf[30];
static int promptlen;
static FILE *tofp, *fromfp;

static char *mgets __((SmtpState * SS, int *, FILE *));

static char *mgets(SS, flagp, fp)
SmtpState *SS;
int *flagp;
FILE *fp;
{
    register int bufsize, c;
    register char *cp;
    char *buf;
    int ccnt = 0;

    if (*flagp) {
	*flagp = 0;
	return NULL;
    }
    bufsize = 16;
    cp = buf = emalloc(bufsize);
    while ((c = getc(fp)) != EOF) {
	*cp = c;
	++ccnt;
	if (c == '\n') {
	    *flagp = 0;
	    break;
	}
	++cp;
	if (strncmp(cp - promptlen, promptbuf, promptlen) == 0) {
	    free(buf);
	    buf = NULL;
	    *flagp = 1;
	    return NULL;
	}
	if (bufsize - 2 < ccnt) {
	    bufsize <<= 1;
	    buf = erealloc(buf, bufsize);
	    cp = buf + ccnt;
	}
    }

    if (buf)
	*cp = '\0';

    if (debug && buf)
	fprintf(stdout, "000 '%s'\r\n", buf);

    if (c == EOF) {
	printf("got EOF!\n");
	free(buf);
	return NULL;
    }
    return buf;
}

static const char *newenviron[] =
{"SMTPSERVER=y", NULL};

static int callr __((SmtpState * SS));
static int callr(SS)
SmtpState *SS;
{
    int sawend, rpid, to[2], from[2];
    char *bufp, *cp;

    if (pipe(to) < 0 || pipe(from) < 0)
	return -1;

#if 0
#if 0
    SIGNAL_HANDLE(SIGCHLD, SIG_DFL);	/* ?? SIG_DFL  on SVR4 ? */
#else
    SIGNAL_HANDLE(SIGCHLD, reaper);
#endif
    SIGNAL_HANDLE(SIGPIPE, SIG_IGN);
#endif

    if (routerprog == NULL) {
	if ((cp = getzenv("MAILBIN")) == NULL) {
	    zsyslog((LOG_ERR, "MAILBIN unspecified in zmailer.conf"));
	    return -1;
	}
	routerprog = emalloc(strlen(cp) + sizeof "router" + 2);
	sprintf(routerprog, "%s/router", cp);
    }
    if ((rpid = fork()) == 0) {	/* child */
	rpid = getpid();
	dup2(to[0], 0);
	dup2(from[1], 1);
	dup2(from[1], 2);
	close(to[0]);
	close(to[1]);
	close(from[0]);
	close(from[1]);
	runasrootuser();	/* XXX: security alert! */
	environ = (char **) newenviron;
	execl(routerprog, "router", "-io-i", (char *) NULL);
#define	BADEXEC	"8@$#&(\n\n"
	write(1, BADEXEC, strlen(BADEXEC));
	_exit(1);
    } else if (rpid < 0)
	return -1;

    close(to[0]);
    close(from[1]);
    tofp = fdopen(to[1], "w");
    fromfp = fdopen(from[0], "r");

    /* stuff a command to print something recognizable down the pipe */
    sprintf(promptbuf, "%d# ", getpid());
    promptlen = strlen(promptbuf);

    /*
     * Gotta be careful here: ihostaddr is generated within the program
     * from the IP address, but rhostname is specified by the remote host.
     * Although it may be caught by partridge(), lets make extra sure.
     */
    for (cp = SS->rhostname; *cp; ++cp)
	if (*cp == '\'')
	    *cp = '?';
    if (SS->rhostname[strlen(SS->rhostname) - 1] == '\\')
	SS->rhostname[strlen(SS->rhostname) - 1] = '?';

    fprintf(tofp, "PS1='%s' ; %s %s '%s' '%s'\n", promptbuf,
	    ROUTER_SERVER, RKEY_INIT, SS->rhostname, SS->ihostaddr);
    fflush(tofp);

    sawend = 0;
    while ((bufp = mgets(SS, &sawend, fromfp)) != NULL) {
	if (strncmp(cp, BADEXEC, strlen(BADEXEC) - 2) == 0) {
	    free(cp);
	    killr(SS, rpid);
	    return -1;
	}
	/*printf("241%c%s\n", sawend == 1 ? ' ' : '-', buf); */
	free(bufp);
    }

    return pid;
}

void killr(SS, rpid)
SmtpState *SS;
int rpid;
{
    if (rpid > 0) {
	fclose(tofp);
	fclose(fromfp);
	kill(rpid, SIGKILL);
    }
}


/*
 * Now we can do VRFY et al using the router we have connected to.
 */

char *
 router(SS, function, holdlast, args, len)
SmtpState *SS;
const char *function, *args;
const int holdlast, len;
{
    char *bufp, *prevb = NULL;
    int sawend = 0, i;
    const char *args0 = args;

    if (args == NULL) {
	type(SS, 501, NULL, NULL);
	return NULL;
    }
    if (routerpid <= 0 && (routerpid = callr(SS)) <= 0) {
	type(SS, 451, NULL, NULL);
	return NULL;
    }
    fprintf(tofp, "%s %s \"", ROUTER_SERVER, function);

    /* Process all double-quotes and backslashes so that
       no surprises happen.. */

    for (i = 0; *args && i < len; ++args, ++i) {
	if (*args == '\\' || *args == '"')
	    putc('\\', tofp);
	putc(*args, tofp);
    }
    fprintf(tofp, "\"\n");
    fflush(tofp);

    for (;;) {
	/*
	 * We want to give the router the opportunity to report
	 * result codes, e.g. for boolean requests.  If the first
	 * three characters are digits and the 4th a space or '-',
	 * then pass through.
	 */
	bufp = mgets(SS, &sawend, fromfp);
	if (!bufp) {
	    /* Huh! Got an EOF, while propably didn't expect it ?
	       Lets find out what the subprocess status was */
	    bufp = emalloc(80 + strlen(args0) + strlen(function) +
			   sizeof(ROUTER_SERVER));
	    sprintf(bufp, "400 Huh! Router server EOFed (crashed?) on us! Help! Last cmd was: %s %s \"%*s\"", ROUTER_SERVER, function, len, args0);
	    break;
	}

	if (debug) {
	    fprintf(stdout, "001 Got string: '%s'\r\n", bufp);
	    typeflush(SS);
	}
	if (prevb != NULL) {
	    if (strlen(prevb) > 4 &&
	      isdigit(prevb[0]) && isdigit(prevb[1]) && isdigit(prevb[2])
		&& (prevb[3] == ' ' || prevb[3] == '-')) {
		printf("%s\r\n", prevb);
	    } else {
		printf("250-%s\r\n", prevb);
	    }
	    free(prevb);
	}
	prevb = bufp;
    }

    if (!holdlast && prevb != NULL) {
	/* -------- Print the last line too -------- */
	if (strlen(prevb) > 4 &&
	    isdigit(prevb[0]) && isdigit(prevb[1]) && isdigit(prevb[2])
	    && (prevb[3] == ' ')) {
	    printf("%s\r\n", prevb);
	} else {
	    printf("250 %s\r\n", prevb);
	}
	strncpy(prevb, "dummy-replacement-string", strlen(prevb));
    } else {
	/* Uhhh.... No output! */
	if (!prevb)
	    printf("500 **INTERNAL*ERROR**\r\n");
    }
    typeflush(SS);

    return prevb;		/* It may be unfreeable pointer... */
}
