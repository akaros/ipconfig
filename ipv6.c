/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * ipconfig for IPv6
 *	RS means Router Solicitation
 *	RA means Router Advertisement
 */

#include <benchutil/alarm.h>
#include <parlib/parlib.h>
#include <iplib/iplib.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "icmp.h"
#include "ipconfig.h"

#pragma varargck argpos ralog 1

#define RALOG "v6routeradv"

#define NetS(x) (((uint8_t*)x)[0]<< 8 | ((uint8_t*)x)[1])
#define NetL(x) (((uint8_t*)x)[0]<<24 | ((uint8_t*)x)[1]<<16 | \
		 ((uint8_t*)x)[2]<< 8 | ((uint8_t*)x)[3])

enum {
	ICMP6LEN=	4,
};

typedef struct Hdr Hdr;
struct Hdr			/* ICMP v4 & v6 header */
{
	uint8_t	type;
	uint8_t	code;
	uint8_t	cksum[2];	/* Checksum */
	uint8_t	data[];
};

char *icmpmsg6[Maxtype6+1] =
{
[EchoReply]		"EchoReply",
[UnreachableV6]		"UnreachableV6",
[PacketTooBigV6]	"PacketTooBigV6",
[TimeExceedV6]		"TimeExceedV6",
[Redirect]		"Redirect",
[EchoRequest]		"EchoRequest",
[TimeExceed]		"TimeExceed",
[InParmProblem]		"InParmProblem",
[Timestamp]		"Timestamp",
[TimestampReply]	"TimestampReply",
[InfoRequest]		"InfoRequest",
[InfoReply]		"InfoReply",
[AddrMaskRequest]	"AddrMaskRequest",
[AddrMaskReply]		"AddrMaskReply",
[EchoRequestV6]		"EchoRequestV6",
[EchoReplyV6]		"EchoReplyV6",
[RouterSolicit]		"RouterSolicit",
[RouterAdvert]		"RouterAdvert",
[NbrSolicit]		"NbrSolicit",
[NbrAdvert]		"NbrAdvert",
[RedirectV6]		"RedirectV6",
};

static char *icmp6opts[] =
{
[0]			"unknown option",
[V6nd_srclladdr]	"srcll_addr",
[V6nd_targlladdr]	"targll_addr",
[V6nd_pfxinfo]		"prefix",
[V6nd_redirhdr]		"redirect",
[V6nd_mtu]		"mtu",
[V6nd_home]		"home",
[V6nd_srcaddrs]		"src_addrs",
[V6nd_ip]		"ip",
[V6nd_rdns]		"rdns",
[V6nd_9fs]		"9fs",
[V6nd_9auth]		"9auth",
};

uint8_t v6allroutersL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x02
};

uint8_t v6allnodesL[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0x01
};

uint8_t v6Unspecified[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6loopback[IPaddrlen] = {
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 1
};

uint8_t v6glunicast[IPaddrlen] = {
	0x08, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6linklocal[IPaddrlen] = {
	0xfe, 0x80, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 0
};

uint8_t v6solpfx[IPaddrlen] = {
	0xff, 0x02, 0, 0,
	0, 0, 0, 0,
	0, 0, 0, 1,
	/* last 3 bytes filled with low-order bytes of addr being solicited */
	0xff, 0, 0, 0,
};

uint8_t v6defmask[IPaddrlen] = {
	0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff,
	0, 0, 0, 0,
	0, 0, 0, 0
};

enum
{
	Vadd,
	Vremove,
	Vunbind,
	Vaddpref6,
	Vra6,
};

static void
ralog(char *fmt, ...)
{
	char msg[512];
	va_list arg;

	va_start(arg, fmt);
	vsnprintf(msg, sizeof msg, fmt, arg);
	va_end(arg);
	syslog(debug, RALOG, msg);
}

extern void
ea2lla(uint8_t *lla, uint8_t *ea)
{
	assert(IPaddrlen == 16);
	memset(lla, 0, IPaddrlen);
	lla[0]  = 0xFE;
	lla[1]  = 0x80;
	lla[8]  = ea[0] | 0x2;
	lla[9]  = ea[1];
	lla[10] = ea[2];
	lla[11] = 0xFF;
	lla[12] = 0xFE;
	lla[13] = ea[3];
	lla[14] = ea[4];
	lla[15] = ea[5];
}

extern void
ipv62smcast(uint8_t *smcast, uint8_t *a)
{
	assert(IPaddrlen == 16);
	memset(smcast, 0, IPaddrlen);
	smcast[0]  = 0xFF;
	smcast[1]  = 0x02;
	smcast[11] = 0x1;
	smcast[12] = 0xFF;
	smcast[13] = a[13];
	smcast[14] = a[14];
	smcast[15] = a[15];
}

void
v6paraminit(Conf *cf)
{
	cf->sendra = cf->recvra = 0;
	cf->mflag = 0;
	cf->oflag = 0;
	cf->maxraint = Maxv6initraintvl;
	cf->minraint = Maxv6initraintvl / 4;
	cf->linkmtu = 1500;
	cf->reachtime = V6reachabletime;
	cf->rxmitra = V6retranstimer;
	cf->ttl = MAXTTL;

	cf->routerlt = 0;

	cf->prefixlen = 64;
	cf->onlink = 0;
	cf->autoflag = 0;
	cf->validlt = cf->preflt = ~0L;
}

static char *
optname(unsigned opt)
{
	static char buf[32];

	if (opt >= COUNT_OF(icmp6opts) || icmp6opts[opt] == NULL) {
		snprintf(buf, sizeof buf, "unknown option %d", opt);
		return buf;
	} else
		return icmp6opts[opt];
}

void
opt_sprint(uint8_t *ps, uint8_t *pe, char *buf, size_t size)
{
	int otype, osz, pktsz;
	uint8_t *a;
	char p[256];

	a = ps;
	for (pktsz = pe - ps; pktsz > 0; pktsz -= osz) {
		otype = a[0];
		osz = a[1] * 8;

		switch (otype) {
		default:
			snprintf(p, sizeof p, " option=%s ", optname(otype));
			strlcat(buf, p, size);
		case V6nd_srclladdr:
		case V6nd_targlladdr:
			if (pktsz < osz || osz != 8) {
				snprintf(p, sizeof p,
				         " option=%s bad size=%d",
					 optname(otype), osz);
				strlcat(buf, p, size);
				return;
			}
			snprintf(p, sizeof p,
			         " option=%s maddr=%E",
			         optname(otype), a+2);
			strlcat(buf, p, size);
			break;
		case V6nd_pfxinfo:
			if (pktsz < osz || osz != 32) {
				snprintf(p, sizeof p,
				         " option=%s: bad size=%d",
					 optname(otype), osz);
				strlcat(buf, p, size);
				return;
			}
			snprintf(p, sizeof p,
			         " option=%s pref=%i preflen=%3.3d"
				 " lflag=%1.1d aflag=%1.1d unused1=%1.1d"
				 " validlt=%ud preflt=%ud unused2=%1.1d",
				 optname(otype), a+16, (int)(*(a+2)),
				 (*(a+3) & (1 << 7)) != 0,
				 (*(a+3) & (1 << 6)) != 0,
				 (*(a+3) & 63) != 0,
				 NetL(a+4), NetL(a+8), NetL(a+12)!=0);
			strlcat(buf, p, size);
			break;
		}
		a += osz;
	}
}

static void
pkt2str(uint8_t *ps, uint8_t *pe, char *buf, size_t size)
{
	int pktlen;
	char *tn;
	uint8_t *a;
	Hdr *h;
	char p[256];

	h = (Hdr*)ps;
	a = ps + 4;

	pktlen = pe - ps;
	if(pktlen < ICMP6LEN) {
		strlcpy(buf, "short pkt", size);
		return;
	}

	tn = icmpmsg6[h->type];
	if(tn == NULL)
		snprintf(p, sizeof p, "t=%ud c=%d ck=%4.4ux", h->type,
			 h->code, (uint16_t)NetS(h->cksum));
	else
		snprintf(p, sizeof p, "t=%s c=%d ck=%4.4ux", tn,
			 h->code, (uint16_t)NetS(h->cksum));
	strlcat(buf, p, size);

	switch(h->type){
	case RouterSolicit:
		ps += 8;
		snprintf(p, sizeof p, " unused=%1.1d ", NetL(a)!=0);
		strlcat(buf, p, size);
		opt_sprint(ps, pe, buf, size);
		break;
	case RouterAdvert:
		ps += 16;
		snprintf(p, sizeof p, " hoplim=%3.3d mflag=%1.1d oflag=%1.1d"
			 " unused=%1.1d routerlt=%d reachtime=%d rxmtimer=%d",
			 a[0],
			 (*(a+1) & (1 << 7)) != 0,
			 (*(a+1) & (1 << 6)) != 0,
			 (*(a+1) & 63) != 0,
			 NetS(a+2), NetL(a+4), NetL(a+8));
		strlcat(buf, p, size);
		opt_sprint(ps, pe, buf, size);
		break;
	default:
		strlcat(buf, " unexpected icmp6 pkt type", size);
		break;
	}
}

/*
 * based on libthread's threadsetname, but drags in less library code.
 * actually just sets the arguments displayed.
 */
void
procsetname(char *fmt, ...)
{
	int fd;
	char *cmdname;
	char buf[128];
	va_list arg;

	va_start(arg, fmt);
	vasprintf(&cmdname, fmt, arg);
	va_end(arg);
	if (cmdname == NULL)
		return;
	snprintf(buf, sizeof buf, "#p/%d/args", getpid());
	if((fd = open(buf, O_WRONLY)) >= 0){
		write(fd, cmdname, strlen(cmdname)+1);
		close(fd);
	}
	free(cmdname);
}

int
dialicmp(uint8_t *dst, int dport, int *ctlfd)
{
	int fd, cfd, n, m;
	char cmsg[100], name[128], connind[40];
	char hdrs[] = "headers";

	snprintf(name, sizeof name, "%s/icmpv6/clone", conf.mpoint);
	cfd = open(name, O_RDWR);
	if(cfd < 0)
		error(1, 0, "dialicmp: can't open %s: %r", name);

	n = snprintf(cmsg, sizeof cmsg, "connect %i!%d!r %d", dst, dport, dport);
	m = write(cfd, cmsg, n);
	if (m < n)
		error(1, 0, "dialicmp: can't write %s to %s: %r", cmsg, name);

	lseek(cfd, 0, 0);
	n = read(cfd, connind, sizeof connind);
	if (n < 0)
		connind[0] = 0;
	else if (n < sizeof connind)
		connind[n] = 0;
	else
		connind[sizeof connind - 1] = 0;

	snprintf(name, sizeof name, "%s/icmpv6/%s/data", conf.mpoint, connind);
	fd = open(name, O_RDWR);
	if(fd < 0)
		error(1, 0, "dialicmp: can't open %s: %r", name);

	n = sizeof hdrs - 1;
	if(write(cfd, hdrs, n) < n)
		error(1, 0, "dialicmp: can't write `%s' to %s: %r", hdrs, name);
	*ctlfd = cfd;
	return fd;
}

/* add ipv6 addr to an interface */
int
ip6cfg(int autoconf)
{
	int dupfound = 0, n;
	char *p;
	char buf[256], line[1024];
	uint8_t ethaddr[6];
	FILE *bp;

	if (autoconf) {			/* create link-local addr */
		if (myetheraddr(ethaddr, conf.dev) < 0)
			error(1, 0, "myetheraddr w/ %s failed: %r", conf.dev);
		ea2lla(conf.laddr, ethaddr);
	}

	if (dupl_disc)
		n = sprintf(buf, "try");
	else
		n = sprintf(buf, "add");

	n += snprintf(buf+n, sizeof buf-n, " %i", conf.laddr);
	if(!validip(conf.mask))
		ipmove(conf.mask, v6defmask);
	n += snprintf(buf+n, sizeof buf-n, " %M", conf.mask);
	if(validip(conf.raddr)){
		n += snprintf(buf+n, sizeof buf-n, " %i", conf.raddr);
		if(conf.mtu != 0)
			n += snprintf(buf+n, sizeof buf-n, " %d", conf.mtu);
	}

	if(write(conf.cfd, buf, n) < 0){
		warning("write(%s): %r", buf);
		return -1;
	}

	if (!dupl_disc)
		return 0;

	sleep(3000);

	/* read arp table, look for addr duplication */
	snprintf(buf, sizeof buf, "%s/arp", conf.mpoint);
	bp = fopen(buf, "r");
	if (bp == NULL) {
		warning("couldn't open %s: %r", buf);
		return -1;
	}

	snprintf(buf, sizeof buf, "%i", conf.laddr);
	while((p = fgets(line, sizeof(line), bp)) != NULL){
		p[strlen(line)-1] = 0;
		if(strcasestr(p, buf) != 0) {
			warning("found dup entry in arp cache");
			dupfound = 1;
			break;
		}
	}
	fclose(bp);

	if (dupfound)
		doremove();
	else {
		n = sprintf(buf, "add %i %M", conf.laddr, conf.mask);
		if(validip(conf.raddr)){
			n += snprintf(buf+n, sizeof buf-n, " %i", conf.raddr);
			if(conf.mtu != 0)
				n += snprintf(buf+n, sizeof buf-n, " %d",
					conf.mtu);
		}
		write(conf.cfd, buf, n);
	}
	return 0;
}

static int
recvra6on(char *net, int conn)
{
	struct ipifc* ifc;

	ifc = readipifc(net, NULL, conn);
	if (ifc == NULL)
		return 0;
	else if (ifc->sendra6 > 0)
		return IsRouter;
	else if (ifc->recvra6 > 0)
		return IsHostRecv;
	else
		return IsHostNoRecv;
}

/* send icmpv6 router solicitation to multicast address for all routers */
static void
sendrs(int fd)
{
	Routersol *rs;
	uint8_t buff[sizeof *rs];

	memset(buff, 0, sizeof buff);
	rs = (Routersol *)buff;
	memmove(rs->dst, v6allroutersL, IPaddrlen);
	memmove(rs->src, v6Unspecified, IPaddrlen);
	rs->type = ICMP6_RS;

	if(write(fd, rs, sizeof buff) < sizeof buff)
		ralog("sendrs: write failed, pkt size %d", sizeof buff);
	else
		ralog("sendrs: sent solicitation to %i from %i on %s",
			rs->dst, rs->src, conf.dev);
}

/*
 * a router receiving a router adv from another
 * router calls this; it is basically supposed to
 * log the information in the ra and raise a flag
 * if any parameter value is different from its configured values.
 *
 * doing nothing for now since I don't know where to log this yet.
 */
static void
recvrarouter(uint8_t buf[], int pktlen)
{
	ralog("i am a router and got a router advert");
}

/* host receiving a router advertisement calls this */

static void
ewrite(int fd, char *str)
{
	int n;

	n = strlen(str);
	if (write(fd, str, n) != n)
		ralog("write(%s) failed: %r", str);
}

static void
issuebasera6(Conf *cf)
{
	char *cfg;

	asprintf(&cfg, "ra6 mflag %d oflag %d reachtime %d rxmitra %d "
		 "ttl %d routerlt %d",
		 cf->mflag, cf->oflag, cf->reachtime, cf->rxmitra,
		 cf->ttl, cf->routerlt);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
issuerara6(Conf *cf)
{
	char *cfg;

	asprintf(&cfg, "ra6 sendra %d recvra %d maxraint %d minraint %d "
		 "linkmtu %d",
		 cf->sendra, cf->recvra, cf->maxraint, cf->minraint,
		 cf->linkmtu);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
issueadd6(Conf *cf)
{
	char *cfg;

	asprintf(&cfg, "add6 %i %d %d %d %lud %lud", cf->v6pref, cf->prefixlen,
		 cf->onlink, cf->autoflag, cf->validlt, cf->preflt);
	ewrite(cf->cfd, cfg);
	free(cfg);
}

static void
recvrahost(uint8_t buf[], int pktlen)
{
	int arpfd, m, n;
	char abuf[100], *msg;
	uint8_t optype;
	Lladdropt *llao;
	Mtuopt *mtuo;
	Prefixopt *prfo;
	Routeradv *ra;
	static int first = 1;

	ra = (Routeradv*)buf;
//	memmove(conf.v6gaddr, ra->src, IPaddrlen);
	conf.ttl = ra->cttl;
	conf.mflag = (MFMASK & ra->mor);
	conf.oflag = (OCMASK & ra->mor);
	conf.routerlt =  nhgets(ra->routerlt);
	conf.reachtime = nhgetl(ra->rchbltime);
	conf.rxmitra =   nhgetl(ra->rxmtimer);

//	issueadd6(&conf);		/* for conf.v6gaddr? */
	msg = "ra6 recvra 1";
	if (write(conf.cfd, msg, strlen(msg)) < 0)
		ralog("write(ra6 recvra 1) failed: %r");
	issuebasera6(&conf);

	m = sizeof *ra;
	while (pktlen - m > 0) {
		optype = buf[m];
		switch (optype) {
		case V6nd_srclladdr:
			llao = (Lladdropt *)&buf[m];
			m += 8 * buf[m+1];
			if (llao->len != 1) {
				ralog("recvrahost: illegal len (%d) for source "
					"link layer address option", llao->len);
				return;
			}
			if (!ISIPV6LINKLOCAL(ra->src)) {
				ralog("recvrahost: non-link-local src addr for "
					"router adv %i", ra->src);
				return;
			}

			snprintf(abuf, sizeof abuf, "%s/arp", conf.mpoint);
			arpfd = open(abuf, O_WRONLY);
			if (arpfd < 0) {
				ralog("recvrahost: couldn't open %s to write: %r",
					abuf);
				return;
			}

			n = snprintf(abuf, sizeof abuf, "add ether %i %E",
				ra->src, llao->lladdr);
			if (write(arpfd, abuf, n) < n)
				ralog("recvrahost: couldn't write to %s/arp",
					conf.mpoint);
			close(arpfd);
			break;
		case V6nd_targlladdr:
		case V6nd_redirhdr:
			m += 8 * buf[m+1];
			ralog("ignoring unexpected option type `%s' in Routeradv",
				optname(optype));
			break;
		case V6nd_mtu:
			mtuo = (Mtuopt*)&buf[m];
			m += 8 * mtuo->len;
			conf.linkmtu = nhgetl(mtuo->mtu);
			break;
		case V6nd_pfxinfo:
			prfo = (Prefixopt*)&buf[m];
			m += 8 * prfo->len;
			if (prfo->len != 4) {
				ralog("illegal len (%d) for prefix option",
					prfo->len);
				return;
			}
			memmove(conf.v6pref, prfo->pref, IPaddrlen);
			conf.prefixlen = prfo->plen;
			conf.onlink =   ((prfo->lar & OLMASK) != 0);
			conf.autoflag = ((prfo->lar & AFMASK) != 0);
			conf.validlt = nhgetl(prfo->validlt);
			conf.preflt =  nhgetl(prfo->preflt);
			issueadd6(&conf);
			if (first) {
				first = 0;
				ralog("got initial RA from %i on %s; pfx %i",
					ra->src, conf.dev, prfo->pref);
			}
			break;
		default:
			if (debug)
				ralog("ignoring optype %d in Routeradv from %i",
					optype, ra->src);
			/* fall through */
		case V6nd_srcaddrs:
			/* netsbd sends this, so quietly ignore it for now */
			m += 8 * buf[m+1];
			break;
		}
	}
}

/*
 * daemon to receive router advertisements from routers
 */
void
recvra6(void)
{
	int fd, cfd, n, sendrscnt, sleepfor;
	uint8_t buf[4096];

	/* TODO: why not v6allroutersL? */
	fd = dialicmp(v6allnodesL, ICMP6_RA, &cfd);
	if (fd < 0)
		error(1, 0, "can't open icmp_ra connection: %r");

	sendrscnt = Maxv6rss;

	switch(fork()) {
	case -1:
		error(1, 0, "can't fork: %r");
	default:
		return;
	case 0:
		break;
	}

	procsetname("recvra6 on %s", conf.dev);
	ralog("recvra6 on %s", conf.dev);
	sleepfor = jitter();
	for (;;) {
		/*
		 * We only get 3 (Maxv6rss) tries, so make sure we
		 * wait long enough to be certain that at least one RA
		 * will be transmitted.
		 */
		if (sleepfor < 7000)
			sleepfor = 7000;
		alarm(sleepfor);
		n = read(fd, buf, sizeof buf);
		alarm(0);
		if (n <= 0) {
			if (sendrscnt > 0) {
				sendrscnt--;
				if (recvra6on(conf.mpoint, myifc) == IsHostRecv)
					sendrs(fd);
				sleepfor = V6rsintvl + (lrand48() % 100);
			}
			if (sendrscnt == 0) {
				sendrscnt--;
				sleepfor = 0;
				ralog("recvra6: no router advs after %d sols on %s",
					Maxv6rss, conf.dev);
			}
			continue;
		}

		sleepfor = 0;
		sendrscnt = -1;		/* got at least initial ra; no whining */
		switch (recvra6on(conf.mpoint, myifc)) {
		case IsRouter:
			recvrarouter(buf, n);
			break;
		case IsHostRecv:
			recvrahost(buf, n);
			break;
		case IsHostNoRecv:
			ralog("recvra6: recvra off, quitting on %s", conf.dev);
			close(fd);
			exit(0);
		default:
			ralog("recvra6: unable to read router status on %s",
				conf.dev);
			break;
		}
	}
}

/*
 * return -1 -- error, reading/writing some file,
 *         0 -- no arp table updates
 *         1 -- successful arp table update
 */
int
recvrs(uint8_t *buf, int pktlen, uint8_t *sol)
{
	int n, optsz, arpfd;
	char abuf[256];
	Routersol *rs;
	Lladdropt *llao;

	rs = (Routersol *)buf;
	n = sizeof *rs;
	optsz = pktlen - n;
	pkt2str(buf, buf+pktlen, abuf, sizeof abuf);

	if (optsz != sizeof *llao)
		return 0;
	if (buf[n] != V6nd_srclladdr || 8*buf[n+1] != sizeof *llao) {
		ralog("rs opt err %s", abuf);
		return -1;
	}

	ralog("rs recv %s", abuf);

	if (memcmp(rs->src, v6Unspecified, IPaddrlen) == 0)
		return 0;

	snprintf(abuf, sizeof abuf, "%s/arp", conf.mpoint);
	arpfd = open(abuf, O_WRONLY);
	if (arpfd < 0) {
		ralog("recvrs: can't open %s/arp to write: %r", conf.mpoint);
		return -1;
	}

	llao = (Lladdropt *)&buf[n];
	n = snprintf(abuf, sizeof abuf, "add ether %i %E", rs->src, llao->lladdr);
	if (write(arpfd, abuf, n) < n) {
		ralog("recvrs: can't write to %s/arp: %r", conf.mpoint);
		close(arpfd);
		return -1;
	}

	memmove(sol, rs->src, IPaddrlen);
	close(arpfd);
	return 1;
}

void
sendra(int fd, uint8_t *dst, int rlt)
{
	int pktsz, preflen;
	char abuf[1024], tmp[40];
	uint8_t buf[1024], macaddr[6], src[IPaddrlen];
	struct ipifc *ifc = NULL;
	struct iplifc *lifc, *nlifc;
	Lladdropt *llao;
	Prefixopt *prfo;
	Routeradv *ra;

	memset(buf, 0, sizeof buf);
	ra = (Routeradv *)buf;

	myetheraddr(macaddr, conf.dev);
	ea2lla(src, macaddr);
	memmove(ra->src, src, IPaddrlen);
	memmove(ra->dst, dst, IPaddrlen);
	ra->type = ICMP6_RA;
	ra->cttl = conf.ttl;

	if (conf.mflag > 0)
		ra->mor |= MFMASK;
	if (conf.oflag > 0)
		ra->mor |= OCMASK;
	if (rlt > 0)
		hnputs(ra->routerlt, conf.routerlt);
	else
		hnputs(ra->routerlt, 0);
	hnputl(ra->rchbltime, conf.reachtime);
	hnputl(ra->rxmtimer, conf.rxmitra);

	pktsz = sizeof *ra;

	/* include all global unicast prefixes on interface in prefix options */
	ifc = readipifc(conf.mpoint, ifc, myifc);
	for (lifc = (ifc? ifc->lifc: NULL); lifc; lifc = nlifc) {
		nlifc = lifc->next;
		prfo = (Prefixopt *)(buf + pktsz);
		/* global unicast address? */
		if (!ISIPV6LINKLOCAL(lifc->ip) && !ISIPV6MCAST(lifc->ip) &&
		    memcmp(lifc->ip, IPnoaddr, IPaddrlen) != 0 &&
		    memcmp(lifc->ip, v6loopback, IPaddrlen) != 0 &&
		    !isv4(lifc->ip)) {
			memmove(prfo->pref, lifc->net, IPaddrlen);

			/* hack to find prefix length */
			snprintf(tmp, sizeof tmp, "%M", lifc->mask);
			preflen = atoi(&tmp[1]);
			prfo->plen = preflen & 0xff;
			if (prfo->plen == 0)
				continue;

			prfo->type = V6nd_pfxinfo;
			prfo->len = 4;
			prfo->lar = AFMASK;
			hnputl(prfo->validlt, lifc->validlt);
			hnputl(prfo->preflt, lifc->preflt);
			pktsz += sizeof *prfo;
		}
	}
	/*
	 * include link layer address (mac address for now) in
	 * link layer address option
	 */
	llao = (Lladdropt *)(buf + pktsz);
	llao->type = V6nd_srclladdr;
	llao->len = 1;
	memmove(llao->lladdr, macaddr, sizeof macaddr);
	pktsz += sizeof *llao;

	pkt2str(buf+40, buf+pktsz, abuf, sizeof abuf);
	if(write(fd, buf, pktsz) < pktsz)
		ralog("sendra fail %s: %r", abuf);
	else if (debug)
		ralog("sendra succ %s", abuf);
}

/*
 * daemon to send router advertisements to hosts
 */
void
sendra6(void)
{
	int fd, cfd, n, dstknown = 0, sendracnt, sleepfor, nquitmsgs;
	long lastra, now;
	uint8_t buf[4096], dst[IPaddrlen];
	struct ipifc *ifc = NULL;

	fd = dialicmp(v6allnodesL, ICMP6_RS, &cfd);
	if (fd < 0)
		error(1, 0, "can't open icmp_rs connection: %r");

	sendracnt = Maxv6initras;
	nquitmsgs = Maxv6finalras;

	switch(fork()) {
	case -1:
		error(1, 0, "can't fork: %r");
	default:
		return;
	case 0:
		break;
	}

	procsetname("sendra6 on %s", conf.dev);
	ralog("sendra6 on %s", conf.dev);
	sleepfor = jitter();
	for (;;) {
		lastra = time(0);
		if (sleepfor < 0)
			sleepfor = 0;
		alarm(sleepfor);
		n = read(fd, buf, sizeof buf);
		alarm(0);

		ifc = readipifc(conf.mpoint, ifc, myifc);
		if (ifc == NULL) {
			ralog("sendra6: can't read router params on %s",
				conf.mpoint);
			continue;
		}

		if (ifc->sendra6 <= 0)
			if (nquitmsgs > 0) {
				sendra(fd, v6allnodesL, 0);
				nquitmsgs--;
				sleepfor = Minv6interradelay + jitter();
				continue;
			} else {
				ralog("sendra6: sendra off, quitting on %s",
					conf.dev);
				exit(0);
			}

		nquitmsgs = Maxv6finalras;

		if (n <= 0) {			/* no RS */
			if (sendracnt > 0)
				sendracnt--;
		} else {			/* respond to RS */
			dstknown = recvrs(buf, n, dst);
			now = time(0);

			if (now - lastra < Minv6interradelay) {
				/* too close, skip */
				sleepfor = lastra + Minv6interradelay +
					jitter() - now;
				continue;
			}
			sleep(jitter());
		}
		sleepfor = randint(ifc->rp.minraint, ifc->rp.maxraint);
		if (dstknown > 0)
			sendra(fd, dst, 1);
		else
			sendra(fd, v6allnodesL, 1);
	}
}

void
startra6(void)
{
	static char routeon[] = "iprouting 1";

	if (conf.recvra > 0)
		recvra6();

	if (conf.sendra > 0) {
		if (write(conf.cfd, routeon, sizeof routeon - 1) < 0) {
			warning("write (iprouting 1) failed: %r");
			return;
		}
		sendra6();
		if (conf.recvra <= 0)
			recvra6();
	}
}

void
doipv6(int what)
{
	nip = nipifcs(conf.mpoint);
	if(!noconfig){
		lookforip(conf.mpoint);
		controldevice();
		binddevice();
	}

	switch (what) {
	default:
		error(1, 0, "unknown IPv6 verb");
	case Vaddpref6:
		issueadd6(&conf);
		break;
	case Vra6:
		issuebasera6(&conf);
		issuerara6(&conf);
		dolog = 1;
		startra6();
		break;
	}
}