/*
 *	      PPP Routing related Module
 *
 *	    Written by Toshiharu OHNO (tony-o@iij.ad.jp)
 *
 *   Copyright (C) 1994, Internet Initiative Japan, Inc. All rights reserverd.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the Internet Initiative Japan, Inc.  The name of the
 * IIJ may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * $Id: route.c,v 1.42.2.11 1998/03/16 22:52:50 brian Exp $
 *
 */

#include <sys/param.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <net/if_types.h>
#include <net/route.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if_dl.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>

#include <errno.h>
#include <machine/endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <termios.h>
#include <unistd.h>

#include "command.h"
#include "mbuf.h"
#include "log.h"
#include "loadalias.h"
#include "defs.h"
#include "vars.h"
#include "id.h"
#include "iplist.h"
#include "timer.h"
#include "throughput.h"
#include "lqr.h"
#include "hdlc.h"
#include "link.h"
#include "fsm.h"
#include "slcompress.h"
#include "ipcp.h"
#include "filter.h"
#include "bundle.h"
#include "route.h"
#include "descriptor.h"
#include "prompt.h"

static void
p_sockaddr(struct sockaddr *phost, struct sockaddr *pmask, int width)
{
  char buf[29];
  struct sockaddr_in *ihost = (struct sockaddr_in *)phost;
  struct sockaddr_in *mask = (struct sockaddr_in *)pmask;
  struct sockaddr_dl *dl = (struct sockaddr_dl *)phost;

  switch (phost->sa_family) {
  case AF_INET:
    if (!phost)
      buf[0] = '\0';
    else if (ihost->sin_addr.s_addr == INADDR_ANY)
      strcpy(buf, "default");
    else if (!mask) 
      strcpy(buf, inet_ntoa(ihost->sin_addr));
    else {
      u_int msk = ntohl(mask->sin_addr.s_addr);
      u_int tst;
      int bits;
      int len;
      struct sockaddr_in net;

      for (tst = 1, bits=32; tst; tst <<= 1, bits--)
        if (msk & tst)
          break;

      for (tst <<=1; tst; tst <<= 1)
        if (!(msk & tst))
          break;

      net.sin_addr.s_addr = ihost->sin_addr.s_addr & mask->sin_addr.s_addr;
      strcpy(buf, inet_ntoa(net.sin_addr));
      for (len = strlen(buf); len > 3; buf[len-=2] = '\0')
        if (strcmp(buf+len-2, ".0"))
          break;

      if (tst)    /* non-contiguous :-( */
        sprintf(buf+strlen(buf),"&0x%08x", msk);
      else
        sprintf(buf+strlen(buf), "/%d", bits);
    }
    break;

  case AF_LINK:
    if (dl->sdl_nlen)
      snprintf(buf, sizeof buf, "%.*s", dl->sdl_nlen, dl->sdl_data);
    else if (dl->sdl_alen)
      if (dl->sdl_type == IFT_ETHER)
        if (dl->sdl_alen < sizeof buf / 3) {
          int f;
          u_char *MAC;

          MAC = (u_char *)dl->sdl_data + dl->sdl_nlen;
          for (f = 0; f < dl->sdl_alen; f++)
            sprintf(buf+f*3, "%02x:", MAC[f]);
          buf[f*3-1] = '\0';
        } else
	  strcpy(buf, "??:??:??:??:??:??");
      else
        sprintf(buf, "<IFT type %d>", dl->sdl_type);
    else if (dl->sdl_slen)
      sprintf(buf, "<slen %d?>", dl->sdl_slen);
    else
      sprintf(buf, "link#%d", dl->sdl_index);
    break;

  default:
    sprintf(buf, "<AF type %d>", phost->sa_family);
    break;
  }

  prompt_Printf(&prompt, "%-*s ", width-1, buf);
}

static struct bits {
  u_long b_mask;
  char b_val;
} bits[] = {
  { RTF_UP, 'U' },
  { RTF_GATEWAY, 'G' },
  { RTF_HOST, 'H' },
  { RTF_REJECT, 'R' },
  { RTF_DYNAMIC, 'D' },
  { RTF_MODIFIED, 'M' },
  { RTF_DONE, 'd' },
  { RTF_CLONING, 'C' },
  { RTF_XRESOLVE, 'X' },
  { RTF_LLINFO, 'L' },
  { RTF_STATIC, 'S' },
  { RTF_PROTO1, '1' },
  { RTF_PROTO2, '2' },
  { RTF_BLACKHOLE, 'B' },
#ifdef RTF_WASCLONED
  { RTF_WASCLONED, 'W' },
#endif
#ifdef RTF_PRCLONING
  { RTF_PRCLONING, 'c' },
#endif
#ifdef RTF_PROTO3
  { RTF_PROTO3, '3' },
#endif
#ifdef RTF_BROADCAST
  { RTF_BROADCAST, 'b' },
#endif
  { 0, '\0' }
};

#ifndef RTF_WASCLONED
#define RTF_WASCLONED (0)
#endif

static void
p_flags(u_long f, int max)
{
  char name[33], *flags;
  register struct bits *p = bits;

  if (max > sizeof name - 1)
    max = sizeof name - 1;

  for (flags = name; p->b_mask && flags - name < max; p++)
    if (p->b_mask & f)
      *flags++ = p->b_val;
  *flags = '\0';
  prompt_Printf(&prompt, "%-*.*s", max, max, name);
}

const char *
Index2Nam(int idx)
{
  static char **ifs;
  static int nifs, debug_done;

  if (!nifs) {
    int mib[6], have, had;
    size_t needed;
    char *buf, *ptr, *end;
    struct sockaddr_dl *dl;
    struct if_msghdr *ifm;

    mib[0] = CTL_NET;
    mib[1] = PF_ROUTE;
    mib[2] = 0;
    mib[3] = 0;
    mib[4] = NET_RT_IFLIST;
    mib[5] = 0;

    if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
      LogPrintf(LogERROR, "Index2Nam: sysctl: estimate: %s\n", strerror(errno));
      return "???";
    }
    if ((buf = malloc(needed)) == NULL)
      return "???";
    if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0) {
      free(buf);
      return "???";
    }
    end = buf + needed;

    have = 0;
    for (ptr = buf; ptr < end; ptr += ifm->ifm_msglen) {
      ifm = (struct if_msghdr *)ptr;
      dl = (struct sockaddr_dl *)(ifm + 1);
      if (ifm->ifm_index > 0) {
        if (ifm->ifm_index > have) {
          had = have;
          have = ifm->ifm_index + 5;
          if (had)
            ifs = (char **)realloc(ifs, sizeof(char *) * have);
          else
            ifs = (char **)malloc(sizeof(char *) * have);
          if (!ifs) {
            LogPrintf(LogDEBUG, "Index2Nam: %s\n", strerror(errno));
            nifs = 0;
            return "???";
          }
          memset(ifs + had, '\0', sizeof(char *) * (have - had));
        }
        if (ifs[ifm->ifm_index-1] == NULL) {
          ifs[ifm->ifm_index-1] = (char *)malloc(dl->sdl_nlen+1);
          memcpy(ifs[ifm->ifm_index-1], dl->sdl_data, dl->sdl_nlen);
          ifs[ifm->ifm_index-1][dl->sdl_nlen] = '\0';
          if (nifs < ifm->ifm_index)
            nifs = ifm->ifm_index;
        }
      } else if (LogIsKept(LogDEBUG))
        LogPrintf(LogDEBUG, "Skipping out-of-range interface %d!\n",
                  ifm->ifm_index);
    }
    free(buf);
  }

  if (LogIsKept(LogDEBUG) && !debug_done) {
    int f;

    LogPrintf(LogDEBUG, "Found the following interfaces:\n");
    for (f = 0; f < nifs; f++)
      if (ifs[f] != NULL)
        LogPrintf(LogDEBUG, " Index %d, name \"%s\"\n", f+1, ifs[f]);
    debug_done = 1;
  }

  if (idx < 1 || idx > nifs || ifs[idx-1] == NULL)
    return "???";

  return ifs[idx-1];
}

int
ShowRoute(struct cmdargs const *arg)
{
  struct rt_msghdr *rtm;
  struct sockaddr *sa_dst, *sa_gw, *sa_mask;
  char *sp, *ep, *cp, *wp;
  size_t needed;
  int mib[6];

  mib[0] = CTL_NET;
  mib[1] = PF_ROUTE;
  mib[2] = 0;
  mib[3] = 0;
  mib[4] = NET_RT_DUMP;
  mib[5] = 0;
  if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
    LogPrintf(LogERROR, "ShowRoute: sysctl: estimate: %s\n", strerror(errno));
    return (1);
  }
  if (needed < 0)
    return (1);
  sp = malloc(needed);
  if (sp == NULL)
    return (1);
  if (sysctl(mib, 6, sp, &needed, NULL, 0) < 0) {
    LogPrintf(LogERROR, "ShowRoute: sysctl: getroute: %s\n", strerror(errno));
    free(sp);
    return (1);
  }
  ep = sp + needed;

  prompt_Printf(&prompt, "%-20s%-20sFlags  Netif\n", "Destination", "Gateway");
  for (cp = sp; cp < ep; cp += rtm->rtm_msglen) {
    rtm = (struct rt_msghdr *) cp;
    wp = (char *)(rtm+1);

    if (rtm->rtm_addrs & RTA_DST) {
      sa_dst = (struct sockaddr *)wp;
      wp += sa_dst->sa_len;
    } else
      sa_dst = NULL;

    if (rtm->rtm_addrs & RTA_GATEWAY) {
      sa_gw = (struct sockaddr *)wp;
      wp += sa_gw->sa_len;
    } else
      sa_gw = NULL;

    if (rtm->rtm_addrs & RTA_NETMASK) {
      sa_mask = (struct sockaddr *)wp;
      wp += sa_mask->sa_len;
    } else
      sa_mask = NULL;

    p_sockaddr(sa_dst, sa_mask, 20);
    p_sockaddr(sa_gw, NULL, 20);

    p_flags(rtm->rtm_flags, 6);
    prompt_Printf(&prompt, " %s\n", Index2Nam(rtm->rtm_index));
  }
  free(sp);
  return 0;
}

/*
 *  Delete routes associated with our interface
 */
void
DeleteIfRoutes(struct bundle *bundle, int all)
{
  struct rt_msghdr *rtm;
  struct sockaddr *sa;
  struct in_addr sa_dst, sa_none;
  int pass;
  size_t needed;
  char *sp, *cp, *ep;
  int mib[6];

  LogPrintf(LogDEBUG, "DeleteIfRoutes (%d)\n", bundle->ifIndex);
  sa_none.s_addr = INADDR_ANY;

  mib[0] = CTL_NET;
  mib[1] = PF_ROUTE;
  mib[2] = 0;
  mib[3] = 0;
  mib[4] = NET_RT_DUMP;
  mib[5] = 0;
  if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
    LogPrintf(LogERROR, "DeleteIfRoutes: sysctl: estimate: %s\n",
	      strerror(errno));
    return;
  }
  if (needed < 0)
    return;

  sp = malloc(needed);
  if (sp == NULL)
    return;

  if (sysctl(mib, 6, sp, &needed, NULL, 0) < 0) {
    LogPrintf(LogERROR, "DeleteIfRoutes: sysctl: getroute: %s\n",
	      strerror(errno));
    free(sp);
    return;
  }
  ep = sp + needed;

  for (pass = 0; pass < 2; pass++) {
    /*
     * We do 2 passes.  The first deletes all cloned routes.  The second
     * deletes all non-cloned routes.  This is necessary to avoid
     * potential errors from trying to delete route X after route Y where
     * route X was cloned from route Y (which is no longer there).
     */
    if (RTF_WASCLONED == 0 && pass == 0)
      /* So we can't tell ! */
      continue;
    for (cp = sp; cp < ep; cp += rtm->rtm_msglen) {
      rtm = (struct rt_msghdr *) cp;
      sa = (struct sockaddr *) (rtm + 1);
      LogPrintf(LogDEBUG, "DeleteIfRoutes: addrs: %x, Netif: %d (%s),"
                " flags: %x, dst: %s ?\n", rtm->rtm_addrs, rtm->rtm_index,
                Index2Nam(rtm->rtm_index), rtm->rtm_flags,
	        inet_ntoa(((struct sockaddr_in *) sa)->sin_addr));
      if (rtm->rtm_addrs & RTA_DST && rtm->rtm_addrs & RTA_GATEWAY &&
	  rtm->rtm_index == bundle->ifIndex &&
	  (all || (rtm->rtm_flags & RTF_GATEWAY))) {
        sa_dst.s_addr = ((struct sockaddr_in *)sa)->sin_addr.s_addr;
        sa = (struct sockaddr *)((char *)sa + sa->sa_len);
        if (sa->sa_family == AF_INET || sa->sa_family == AF_LINK) {
          if ((pass == 0 && (rtm->rtm_flags & RTF_WASCLONED)) ||
              (pass == 1 && !(rtm->rtm_flags & RTF_WASCLONED))) {
            LogPrintf(LogDEBUG, "DeleteIfRoutes: Remove it (pass %d)\n", pass);
            bundle_SetRoute(bundle, RTM_DELETE, sa_dst, sa_none, sa_none, 0);
          } else
            LogPrintf(LogDEBUG, "DeleteIfRoutes: Skip it (pass %d)\n", pass);
        } else
          LogPrintf(LogDEBUG,
                    "DeleteIfRoutes: Can't remove routes of %d family !\n",
                    sa->sa_family);
      }
    }
  }
  free(sp);
}

int
GetIfIndex(char *name)
{
  int idx;
  const char *got;

  idx = 1;
  while (strcmp(got = Index2Nam(idx), "???"))
    if (!strcmp(got, name))
      return idx;
    else
      idx++;
  return -1;
}
