/*
 * Generalized adaptation to ZMailer libc fill-in use by
 * Matti Aarnio <mea@nic.funet.fi> 2000
 *
 * The original version taken from  glibc-2.1.92 on 1-Aug-2000
 *
 * This is SERIOUSLY LOBOTIMIZED to be usable in environments WITHOUT
 * threaded versions of getservbyname(), and friends, plus ridding
 * __alloca()  calls as they are VERY GCC specific, which isn't a good
 * thing for ZMailer.  (Also DE-ANSIfied to K&R style..)
 *
 * Original reason for having   getaddrinfo()  API in ZMailer was
 * to support IPv6 universe -- and that is still the reason.  This
 * adaptation module is primarily for those systems which don't have
 * this IPv6 API, but there are also some systems (all those using
 * the original INNER NET code -- glibc 2.0/2.1/2.2(?) especially)
 * which have faulty error condition processing in them. Specifically
 * plain simple TIMEOUTS on queries are not handled properly!
 *
 * Now that Linuxes have caught up at libc level, we no longer have
 * a reason to support kernel things which don't exist at libc level.
 * (Running ZMailer on Linux with libc5 is not supported in sense of
 * supporting IPv6 at the kernel..)
 * 
 */

/* The Inner Net License, Version 2.00

  The author(s) grant permission for redistribution and use in source and
binary forms, with or without modification, of the software and documentation
provided that the following conditions are met:

0. If you receive a version of the software that is specifically labelled
   as not being for redistribution (check the version message and/or README),
   you are not permitted to redistribute that version of the software in any
   way or form.
1. All terms of the all other applicable copyrights and licenses must be
   followed.
2. Redistributions of source code must retain the authors' copyright
   notice(s), this list of conditions, and the following disclaimer.
3. Redistributions in binary form must reproduce the authors' copyright
   notice(s), this list of conditions, and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
4. All advertising materials mentioning features or use of this software
   must display the following acknowledgement with the name(s) of the
   authors as specified in the copyright notice(s) substituted where
   indicated:

	This product includes software developed by <name(s)>, The Inner
	Net, and other contributors.

5. Neither the name(s) of the author(s) nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY ITS AUTHORS AND CONTRIBUTORS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  If these license terms cause you a real problem, contact the author.  */

/* This software is Copyright 1996 by Craig Metz, All Rights Reserved.  */

# include "hostenv.h"  /* ZMailer autoconfig environment */

#include <sys/param.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h> /* Sol 2.6 barfs without this.. */
#include <resolv.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <sys/utsname.h>
#include <netdb.h>
#if !defined(EAI_AGAIN) || !defined(AI_NUMERICHOST)
# include "netdb6.h"
#endif

#include "libc.h" /* More of ZMailer environment */

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif
#ifndef PF_LOCAL
#define PF_LOCAL PF_UNIX
#endif


#define GAIH_OKIFUNSPEC 0x0100
#define GAIH_EAI        ~(GAIH_OKIFUNSPEC)

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX  108
#endif

struct gaih_service {
  const char *name;
  int num;
};

struct gaih_servtuple {
  struct gaih_servtuple *next;
  int socktype;
  int protocol;
  int port;
};

static struct gaih_servtuple nullserv = { NULL, 0, 0, 0 };

static void freeservtuple(st)
     struct gaih_servtuple *st;
{
  while (st && st != &nullserv) {
    struct gaih_servtuple *stn = st->next;
    free(st);
    st = stn;
  }
}


struct gaih_addrtuple {
  struct gaih_addrtuple *next;
  int family;
  char addr[16];
  unsigned int scopeid;
};

static void freeaddrtuple(at)
     struct gaih_addrtuple *at;
{
  while (at) {
    struct gaih_addrtuple *atn = at->next;
    free(at);
    at = atn;
  }
}


struct gaih_typeproto {
  int socktype;
  int protocol;
  char *name;
  int protoflag;
};

/* Values for `protoflag'.  */
#define GAI_PROTO_NOSERVICE	1

static struct gaih_typeproto gaih_inet_typeproto[] = {
  { 0, 0, NULL, 0 },
  { SOCK_STREAM, IPPROTO_TCP, (char *) "tcp", 0 },
  { SOCK_DGRAM, IPPROTO_UDP, (char *) "udp", 0 },
  { SOCK_RAW, IPPROTO_RAW, (char *) "raw", GAI_PROTO_NOSERVICE },
  { 0, 0, NULL, 0 }
};

struct gaih {
  int family;
  int (*gaih)__((const char *name, const struct gaih_service *service,
		 const struct addrinfo *req, struct addrinfo **pai,
		 FILE *));
  char *famname;
};

static struct addrinfo default_hints =
	{ 0, PF_UNSPEC, 0, 0, 0, NULL, NULL, NULL };


#if 0 /* NO SUPPORT FOR "LOCAL" ADDRESS SPACE ENTITIES! */

static int gaih_local __((const char *name, const struct gaih_service *service,
			  const struct addrinfo *req, struct addrinfo **pai,
			  FILE *vlog));

static int
gaih_local (name, service, req, pai, vlog)
     const char *name;
     const struct gaih_service *service;
     const struct addrinfo *req;
     struct addrinfo **pai;
     FILE *vlog;
{
  struct utsname utsname;

  if ((name != NULL) && (req->ai_flags & AI_NUMERICHOST))
    return GAIH_OKIFUNSPEC | -EAI_NONAME;

  if ((name != NULL) || (req->ai_flags & AI_CANONNAME))
    if (uname (&utsname) < 0) return -EAI_SYSTEM;

  if (name != NULL) {
    if (strcmp(name, "localhost") &&
	strcmp(name, "local") &&
	strcmp(name, "unix") &&
	strcmp(name, utsname.nodename))
      return GAIH_OKIFUNSPEC | -EAI_NONAME;
  }

  if (req->ai_protocol || req->ai_socktype) {
    struct gaih_typeproto *tp = gaih_inet_typeproto + 1;

    while (tp->name != NULL
	   && ((tp->protoflag & GAI_PROTO_NOSERVICE) != 0
	       || (req->ai_socktype != 0 && req->ai_socktype != tp->socktype)
	       || (req->ai_protocol != 0
		   && req->ai_protocol != tp->protocol)))
      ++tp;

    if (tp->name == NULL) {
      if (req->ai_socktype)
	return (GAIH_OKIFUNSPEC | -EAI_SOCKTYPE);
      else
	return (GAIH_OKIFUNSPEC | -EAI_SERVICE);
    }
  }

  *pai = malloc (sizeof (struct addrinfo) + sizeof (struct sockaddr_un)
		 + ((req->ai_flags & AI_CANONNAME)
		    ? (strlen(utsname.nodename) + 1): 0));
  if (*pai == NULL)
    return -EAI_MEMORY;

  (*pai)->ai_next = NULL;
  (*pai)->ai_flags = req->ai_flags;
  (*pai)->ai_family = AF_LOCAL;
  (*pai)->ai_socktype = req->ai_socktype ? req->ai_socktype : SOCK_STREAM;
  (*pai)->ai_protocol = req->ai_protocol;
  (*pai)->ai_addrlen = sizeof (struct sockaddr_un);
  (*pai)->ai_addr = (void*)((char *) (*pai) + sizeof (struct addrinfo));

#if HAVE_SA_LEN
  ((struct sockaddr_un *) (*pai)->ai_addr)->sun_len =
    sizeof (struct sockaddr_un);
#endif /* SALEN */

  ((struct sockaddr_un *)(*pai)->ai_addr)->sun_family = AF_LOCAL;
  memset(((struct sockaddr_un *)(*pai)->ai_addr)->sun_path, 0, UNIX_PATH_MAX);

  if (service) {
    struct sockaddr_un *sunp = (struct sockaddr_un *) (*pai)->ai_addr;

    if (strchr (service->name, '/') != NULL) {
      if (strlen (service->name) >= sizeof (sunp->sun_path)) {
	return GAIH_OKIFUNSPEC | -EAI_SERVICE;
      }
      strcpy (sunp->sun_path, service->name);

    } else {

      if (strlen (P_tmpdir "/") + 1 + strlen (service->name) >=
	  sizeof (sunp->sun_path))
	return GAIH_OKIFUNSPEC | -EAI_SERVICE;
      
      strcpy(sunp->sun_path, P_tmpdir);
      strcat(sunp->sun_path, "/");
      strcat(sunp->sun_path, service->name);
    }

  } else {

    if (tmpnam (((struct sockaddr_un *) (*pai)->ai_addr)->sun_path) == NULL)
      return -EAI_SYSTEM;
    }

    if (req->ai_flags & AI_CANONNAME)
      (*pai)->ai_canonname = strcpy ((char *) *pai + sizeof (struct addrinfo)
				     + sizeof (struct sockaddr_un),
				     utsname.nodename);
    else
      (*pai)->ai_canonname = NULL;
    return 0;
}
#endif /* GAIH_LOCAL() KILLED... */


static int
gaih_inet_serv (const char *servicename, struct gaih_typeproto *tp,
	       struct gaih_servtuple *st)
{
  struct servent *s;

  s = getservbyname (servicename, tp->name);

  if (!s) return GAIH_OKIFUNSPEC | -EAI_SERVICE;


  st->next     = NULL;
  st->socktype = tp->socktype;
  st->protocol = tp->protocol;
  st->port     = s->s_port;

  return 0;
}


static int
gaih_inet (const char *name, const struct gaih_service *service,
	   const struct addrinfo *req, struct addrinfo **pai, FILE *vlog)
{
  struct gaih_typeproto *tp = gaih_inet_typeproto;
  struct gaih_servtuple *st = &nullserv;
  struct gaih_addrtuple *at = NULL;

  if (req->ai_protocol || req->ai_socktype) {
    ++tp;

    while (tp->name != NULL
	   && ((req->ai_socktype != 0 && req->ai_socktype != tp->socktype)
	       || (req->ai_protocol != 0
		   && req->ai_protocol != tp->protocol)))
      ++tp;

    if (tp->name == NULL) {
      if (req->ai_socktype)
	return (GAIH_OKIFUNSPEC | -EAI_SOCKTYPE);
      else
	return (GAIH_OKIFUNSPEC | -EAI_SERVICE);
    }
  }

  if (service) {

    if ((tp->protoflag & GAI_PROTO_NOSERVICE) != 0)
      return (GAIH_OKIFUNSPEC | -EAI_SERVICE);

    if (service->num < 0) {
      if (tp->name) {

	int rc;

	st = (struct gaih_servtuple *)
	  malloc (sizeof (struct gaih_servtuple));
	if (!st) return -EAI_MEMORY;

	rc = gaih_inet_serv (service->name, tp, st);
	if (rc) {
	  freeservtuple(st);
	  return rc;
	}

      } else {

	struct gaih_servtuple **pst = &st;
	for (tp++; tp->name; tp++) {
	  struct gaih_servtuple *newp;
	  int rc;

	  if ((tp->protoflag & GAI_PROTO_NOSERVICE) != 0)
	    continue;

	  newp = (struct gaih_servtuple *)
	    malloc (sizeof (struct gaih_servtuple));
	  if (!newp) {
	    freeservtuple(st);
	    return -EAI_MEMORY;
	  }

	  rc = gaih_inet_serv (service->name, tp, newp);
	  if (rc) {
	    if (rc & GAIH_OKIFUNSPEC)
	      continue;
	    free(newp);
	    freeservtuple(st);
	    return rc;
	  }

	  *pst = newp;
	  pst = &(newp->next);
	  free(newp);
	}
	if (st == &nullserv)
	  return (GAIH_OKIFUNSPEC | -EAI_SERVICE);
      }

    } else {

      st = malloc (sizeof (struct gaih_servtuple));
      if (!st) return -EAI_MEMORY;

      st->next     = NULL;
      st->socktype = tp->socktype;
      st->protocol = tp->protocol;
      st->port     = htons (service->num);

    }
  }

  if (name != NULL) {

    at = malloc (sizeof (struct gaih_addrtuple));
    if (!at) {
      freeservtuple(st);
      return -EAI_MEMORY;
    }

    at->family = AF_UNSPEC;
    at->scopeid = 0;
    at->next = NULL;

    if (inet_pton (AF_INET, name, at->addr) > 0) {
      if (req->ai_family == AF_UNSPEC ||
	  req->ai_family == AF_INET)
	at->family = AF_INET;
      else {
	freeaddrtuple(at);
	freeservtuple(st);
	return -EAI_ADDRFAMILY;
      }
    }

#if defined(INET6) && defined(AF_INET6)
    /* INET6 code */
    if (at->family == AF_UNSPEC) {
      char *namebuf = strdup (name);
      char *scope_delim;

      scope_delim = strchr (namebuf, '%');
      if (scope_delim != NULL)
	*scope_delim = '\0';

      if (inet_pton (AF_INET6, namebuf, at->addr) > 0) {
	if (req->ai_family == AF_UNSPEC || req->ai_family == AF_INET6)
	  at->family = AF_INET6;
	else {
	  free(namebuf);
	  freeaddrtuple(at);
	  freeservtuple(st);
	  return -EAI_ADDRFAMILY;
	}
#if 0 /* IPv6 scope-id ... */
	if (scope_delim != NULL) {
	  int try_numericscope = 0;
	  if (IN6_IS_ADDR_LINKLOCAL (at->addr)
	      || IN6_IS_ADDR_MC_LINKLOCAL (at->addr)) {

	    at->scopeid = if_nametoindex (scope_delim + 1);
	    if (at->scopeid == 0)
	      try_numericscope = 1;

	  } else
	    try_numericscope = 1;

	  if (try_numericscope != 0) {
	    char *end;
	    assert (sizeof (uint32_t) <= sizeof (unsigned long));
	    at->scopeid = (uint32_t) strtoul (scope_delim + 1, &end, 10);
	    if (*end != '\0') {
	      free(namebuf);
	      freeaddrtuple(at);
	      freeservtuple(st);
	      return GAIH_OKIFUNSPEC | -EAI_NONAME;
	    }
	  }
	}
#endif
      }
      free(namebuf);
    }
#endif

    if (at->family == AF_UNSPEC && (req->ai_flags & AI_NUMERICHOST) == 0) {

      struct hostent *h;
      struct gaih_addrtuple **pat = &at;
      int no_data = 0;

#if defined(INET6) && defined(AF_INET6)
      int no_inet6_data = 0;
      int old_res_options = _res.options;

      /* If we are looking for both IPv4 and IPv6 address we don't
	 want the lookup functions to automatically promote IPv4
	 addresses to IPv6 addresses.  Currently this is decided
	 by setting the RES_USE_INET6 bit in _res.options.  */
      if (req->ai_family == AF_UNSPEC)
	_res.options &= ~RES_USE_INET6;

      if (req->ai_family == AF_UNSPEC || req->ai_family == AF_INET6) {
	int i;

	h = gethostbyname2 (name, AF_INET6);


	if (vlog)
	  fprintf(vlog, " gaih_inet('%s') gethostbyname2(INET6) h=%p  h_errno=%d\n",
		  name, h, h_errno);

	if (!h) {
	  switch (h_errno) {
	  case NETDB_INTERNAL:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_SYSTEM;
	    break;
	  case HOST_NOT_FOUND:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_NONAME;
	    break;
	  case TRY_AGAIN:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_AGAIN;
	    break;
	  case NO_RECOVERY:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_FAIL;
	    break;
	  case NO_DATA:
	    /* 
	       freeaddrtuple(at);
	       freeservtuple(st);
	       return -EAI_NODATA; */
	    break;
	  default:
	    /* 
	       freeaddrtuple(at);
	       freeservtuple(st);
	       return -EAI_SYSTEM; */
	    break;
	  }
	}
	
	if (h) {
	  for (i = 0; h->h_addr_list[i]; i++) {
	    if (*pat == NULL)
	      *pat = malloc (sizeof(struct gaih_addrtuple));
	    if (*pat == NULL) {
	      freeaddrtuple(at);
	      freeservtuple(st);
	      return -EAI_MEMORY;
	    }

	    (*pat)->next = NULL;
	    (*pat)->family = AF_INET6;
	    memcpy ((*pat)->addr, h->h_addr_list[i],
		    sizeof(struct in6_addr));
	    pat = &((*pat)->next);
	  }
	}
	if (!h)
	  no_inet6_data = (h_errno == NO_DATA);
      }


      if (req->ai_family == AF_UNSPEC)
	_res.options = old_res_options;
#endif

      if (req->ai_family == AF_UNSPEC ||
	  req->ai_family == AF_INET      ) {

	int i;

	h = gethostbyname (name);

	if (vlog)
	  fprintf(vlog, " gaih_inet('%s') gethostbyname() h=%p  h_errno=%d\n",
		  name, h, h_errno);

	if (!h) {
	  switch (h_errno) {
	  case NETDB_INTERNAL:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_SYSTEM;
	    break;
	  case HOST_NOT_FOUND:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_NONAME;
	    break;
	  case TRY_AGAIN:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_AGAIN;
	    break;
	  case NO_RECOVERY:
	    freeaddrtuple(at);
	    freeservtuple(st);
	    return -EAI_FAIL;
	    break;
	  case NO_DATA:
	    /* 
	       freeaddrtuple(at);
	       freeservtuple(st);
	       return -EAI_NODATA; */
	    break;
	  default:
	    /* 
	       freeaddrtuple(at);
	       freeservtuple(st);
	       return -EAI_SYSTEM; */
	    break;
	  }
	}
	
	if (h) {
	  for (i = 0; h->h_addr_list[i]; i++) {
	    if (*pat == NULL)
	      *pat = malloc (sizeof(struct gaih_addrtuple));
	    if (*pat == NULL) {
	      freeaddrtuple(at);
	      freeservtuple(st);
	      return -EAI_MEMORY;
	    }

	    (*pat)->next = NULL;
	    (*pat)->family = AF_INET;
	    memcpy ((*pat)->addr, h->h_addr_list[i],
		    sizeof(struct in_addr));
	    pat = &((*pat)->next);
	  }
	}
	no_data = !h && h_errno == NO_DATA;
      }

#if defined(INET6) && defined(AF_INET6)
      if (no_data && no_inet6_data)
#else
      if (no_data)
#endif
	{
	  /* We made requests but they turned out no data.  The name
	     is known, though.  */
	  freeaddrtuple(at);
	  freeservtuple(st);
	  return (GAIH_OKIFUNSPEC | -EAI_NODATA);
	}
    }

    if (at->family == AF_UNSPEC) {
      freeaddrtuple(at);
      freeservtuple(st);
      return (GAIH_OKIFUNSPEC | -EAI_NONAME);
    }

  } else {   /* name == NULL */

    struct gaih_addrtuple *atr;
    atr = at = malloc (sizeof (struct gaih_addrtuple));
    if (!at) {
      freeservtuple(st);
      return -EAI_MEMORY;
    }

    memset (at, '\0', sizeof (struct gaih_addrtuple));

    if (req->ai_family == 0) {
      at->next = malloc (sizeof (struct gaih_addrtuple));
      if (!at->next) {
	freeservtuple(st);
	free(at);
	return -EAI_MEMORY;
      }
      memset (at->next, '\0', sizeof (struct gaih_addrtuple));
    }

#if defined(INET6) && defined(AF_INET6)
    if (req->ai_family == 0 || req->ai_family == AF_INET6) {
      at->family = AF_INET6;
      if ((req->ai_flags & AI_PASSIVE) == 0)
	memcpy (at->addr, &in6addr_loopback, sizeof (struct in6_addr));
      atr = at->next;
    }
#endif

    if (req->ai_family == 0 || req->ai_family == AF_INET) {
      atr->family = AF_INET;
      if ((req->ai_flags & AI_PASSIVE) == 0)
	*(unsigned int *) atr->addr = htonl (INADDR_LOOPBACK);
    }
  }

  if (pai == NULL) {
    freeaddrtuple(at);
    freeservtuple(st);
    return 0;
  }

  {
    const char *c = NULL;
    struct gaih_servtuple *st2;
    struct gaih_addrtuple *at2 = at;
    size_t socklen, namelen;

    /*
      buffer is the size of an unformatted IPv6 address in printable format.
    */
    char buffer[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];

    while (at2 != NULL) {
      if (req->ai_flags & AI_CANONNAME) {
	struct hostent *h = NULL;

#if defined(INET6) && defined(AF_INET6)
	if (at2->family == AF_INET6)
	  h = gethostbyaddr (at2->addr, sizeof(struct in6_addr), AF_INET6);
	else
#endif
	  h = gethostbyaddr (at2->addr, sizeof(struct in_addr), AF_INET);

	if (!h && h_errno == NETDB_INTERNAL) {
	  freeaddrtuple(at);
	  freeservtuple(st);
	  return -EAI_SYSTEM;
	}

	if (h == NULL)
	  c = inet_ntop (at2->family, at2->addr, buffer, sizeof(buffer));
	else
	  c = h->h_name;

	if (c == NULL) {
	  freeaddrtuple(at);
	  freeservtuple(st);
	  return GAIH_OKIFUNSPEC | -EAI_NONAME;
	}

	namelen = strlen (c) + 1;

      } else

	namelen = 0;

#if defined(INET6) && defined(AF_INET6)
      if (at2->family == AF_INET6)
	socklen = sizeof (struct sockaddr_in6);
      else
#endif
	socklen = sizeof (struct sockaddr_in);

      for (st2 = st; st2 != NULL; st2 = st2->next) {

	*pai = malloc (sizeof (struct addrinfo) + socklen + namelen);
	if (*pai == NULL) {
	  freeaddrtuple(at);
	  freeservtuple(st);
	  return -EAI_MEMORY;
	}

	(*pai)->ai_flags    = req->ai_flags;
	(*pai)->ai_family   = at2->family;
	(*pai)->ai_socktype = st2->socktype;
	(*pai)->ai_protocol = st2->protocol;
	(*pai)->ai_addrlen  = socklen;
	(*pai)->ai_addr = (void *)((char*) (*pai) + sizeof(struct addrinfo));
#if HAVE_SA_LEN
	(*pai)->ai_addr->sa_len = socklen;
#endif /* SALEN */
	(*pai)->ai_addr->sa_family = at2->family;

#if defined(INET6) && defined(AF_INET6)
	if (at2->family == AF_INET6) {
	  struct sockaddr_in6 *sin6p = (struct sockaddr_in6 *) (*pai)->ai_addr;

	  sin6p->sin6_flowinfo = 0;
	  memcpy (&sin6p->sin6_addr, at2->addr, sizeof (struct in6_addr));
	  sin6p->sin6_port = st2->port;
#if 0
	  sin6p->sin6_scope_id = at2->scopeid;
#endif
	} else
#endif
	  {
	    struct sockaddr_in *sinp = (struct sockaddr_in *) (*pai)->ai_addr;
	    memcpy (&sinp->sin_addr, at2->addr, sizeof (struct in_addr));
	    sinp->sin_port = st2->port;
	    memset (sinp->sin_zero, '\0', sizeof (sinp->sin_zero));
	  }

	if (c) {
	  (*pai)->ai_canonname = (void *)((char *) (*pai) +
					  sizeof (struct addrinfo) + socklen);
	  strcpy ((*pai)->ai_canonname, c);

	} else

	  (*pai)->ai_canonname = NULL;

	(*pai)->ai_next = NULL;
	pai = &((*pai)->ai_next);
      }

      at2 = at2->next;
    }
  }

  freeaddrtuple(at);
  freeservtuple(st);

  return 0;
}

static struct gaih gaih[] = {
#if defined(INET6) && defined(AF_INET6)
  { PF_INET6, gaih_inet, "INET6" },
#endif
  { PF_INET,  gaih_inet, "INET"  },
  /*  { PF_LOCAL, gaih_local, "LOCAL" }, */
  { PF_UNSPEC, NULL }
};

int
_getaddrinfo_ __((const char *name, const char *service,
		  const struct addrinfo *hints, struct addrinfo **pai,
		  FILE *vlog));
int
_getaddrinfo_ (name, service, hints, pai, vlog)
     const char *name;
     const char *service;
     const struct addrinfo *hints;
     struct addrinfo **pai;
     FILE *vlog;
{
  int i = 0, j = 0, last_i = 0;
  struct addrinfo *p = NULL, **end;
  struct gaih *g = gaih, *pg = NULL;
  struct gaih_service gaih_service, *pservice;

  if (name != NULL && name[0] == '*' && name[1] == 0)
    name = NULL;

  if (service != NULL && service[0] == '*' && service[1] == 0)
    service = NULL;

  if (name == NULL && service == NULL)
    return EAI_NONAME;

  if (hints == NULL)
    hints = &default_hints;

  if (hints->ai_flags & ~(AI_PASSIVE|AI_CANONNAME|AI_NUMERICHOST))
    return EAI_BADFLAGS;

  if ((hints->ai_flags & AI_CANONNAME) && name == NULL)
    return EAI_BADFLAGS;

  if (service && service[0]) {
      char *c;
      gaih_service.name = service;
      gaih_service.num  = strtoul (gaih_service.name, &c, 10);
      if (*c)
	gaih_service.num = -1;
      else
	/* Can't specify a numerical socket unless a protocol family was
	   given. */
        if (hints->ai_socktype == 0)
          return EAI_SERVICE;
      pservice = &gaih_service;
  } else
    pservice = NULL;

  if (pai)
    end = &p;
  else
    end = NULL;

  while (g->gaih) {

    if (hints->ai_family == g->family ||
	hints->ai_family == AF_UNSPEC    ) {

      j++;
      if (pg == NULL || pg->gaih != g->gaih) {
	pg = g;
	i = g->gaih (name, pservice, hints, end, vlog);
	if (vlog)
	  fprintf(vlog," g->gaih[%s]('%s',...) rc=0%o\n",g->famname,name,i);

	if (i != 0) {
	  /* EAI_NODATA is a more specific result as it says that
	     we found a result but it is not usable.  */
	  if (last_i != (GAIH_OKIFUNSPEC | -EAI_NODATA))
	    last_i = i;

	  if (hints->ai_family == AF_UNSPEC && (i & GAIH_OKIFUNSPEC))
	    continue;

	  if (p)
	    freeaddrinfo (p);

	  return -(i & GAIH_EAI);
	}
	if (end)
	  while(*end) end = &((*end)->ai_next);
      }
    }
    ++g;

  } /* .. while */

  if (j == 0)
    return EAI_FAMILY;

  if (p) {
    *pai = p;
    return 0;
  }

  if (pai == NULL && last_i == 0)
    return 0;

  if (p)
    freeaddrinfo (p);

  return last_i ? -(last_i & GAIH_EAI) : EAI_NONAME;
}

int
getaddrinfo __((const char *name, const char *service,
		  const struct addrinfo *hints, struct addrinfo **pai));
int
getaddrinfo (name, service, hints, pai)
     const char *name;
     const char *service;
     const struct addrinfo *hints;
     struct addrinfo **pai;
{
  return _getaddrinfo_(name, service, hints, pai, NULL);
}

void freeaddrinfo __((ai))
     struct addrinfo *ai;
{
  struct addrinfo *p;

  while (ai != NULL) {
    p = ai;
    ai = ai->ai_next;
    free (p);
  }
}
