/*   
 *   VDETELWEB: VDE telnet and WEB interface
 *
 *   vdetelweb.c: main
 *   
 *   Copyright 2005,2008 Virtual Square Team University of Bologna - Italy
 *   2005 written by Renzo Davoli 
 *   --pidfile/-p and cleanup management by Mattia Belletti (C) 2004
 *                            (copied from vde_switch code).
 *   2008 updated Renzo Davoli
 *   2008 sha1sum by Marco Dalla Via
 *   
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *   $Id$
 *
 */
#include <config.h>
#include <stdio.h>
#include <signal.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <linux/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "vdetelweb.h"
#include <lwipv6.h>
#include <limits.h>
#include <mhash.h>

int daemonize;
int telnet;
int web;
char *mgmt;
char *banner;
char *progname;
char *prompt;
int logok;
static char *passwd;
static char *pidfile = NULL;
static char pidfile_path[_POSIX_PATH_MAX];
struct stack *lwipstack;

#define MAXFD 16
#define HASH_SIZE 40
int npfd=0;
struct pollfd pfd[MAXFD];
voidfun fpfd[MAXFD];
void *status[MAXFD];

#define ROOTCONFFILE "/etc/vde/vdetelwebrc"

/* This will be prefixed by getenv("HOME") */
#define USERCONFFILE "/.vde/vdetelwebrc"

void printlog(int priority, const char *format, ...)
{
	va_list arg;

	va_start (arg, format);

	if (logok)
		vsyslog(priority,format,arg);
	else {
		fprintf(stderr,"%s: ",progname);
		vfprintf(stderr,format,arg);
		fprintf(stderr,"\n");
	}
	va_end (arg);
}


static void cleanup(void)
{
	if (lwipstack)
		lwip_stack_free(lwipstack);
	if((pidfile != NULL) && unlink(pidfile_path) < 0) {
		printlog(LOG_WARNING,"Couldn't remove pidfile '%s': %s", pidfile, strerror(errno));
	}
}

static char hex[]="0123456789abcdef";
int sha1passwdok(const char *pw) {
	unsigned char out[mhash_get_block_size(MHASH_SHA1)];
	char outstr[mhash_get_block_size(MHASH_SHA1)*2+1];
	unsigned int i;
	MHASH td;
	td=mhash_init(MHASH_SHA1);
	mhash(td, pw, strlen(pw));
	mhash_deinit(td, out);
	for (i=0; i<mhash_get_block_size(MHASH_SHA1); i++) {
		outstr[2*i]=hex[out[i] >> 4];
		outstr[2*i+1]=hex[out[i] & 0xf];
	}
	outstr[2*i]=0;
	return (memcmp(outstr,passwd,mhash_get_block_size(MHASH_SHA1))==0);
}

static void sig_handler(int sig)
{
	cleanup();
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}

static void setsighandlers(void)
{
	/* setting signal handlers.
	 *    * sets clean termination for SIGHUP, SIGINT and SIGTERM, and simply
	 *       * ignores all the others signals which could cause termination. */
	struct { int sig; const char *name; int ignore; } signals[] = {
		{ SIGHUP, "SIGHUP", 0 },
		{ SIGINT, "SIGINT", 0 },
		{ SIGPIPE, "SIGPIPE", 1 },
		{ SIGALRM, "SIGALRM", 1 },
		{ SIGTERM, "SIGTERM", 0 },
		{ SIGUSR1, "SIGUSR1", 1 },
		{ SIGUSR2, "SIGUSR2", 1 },
		{ SIGPOLL, "SIGPOLL", 1 },
		{ SIGPROF, "SIGPROF", 1 },
		{ SIGVTALRM, "SIGVTALRM", 1 },
#ifdef SIGSTKFLT
		{ SIGSTKFLT, "SIGSTKFLT", 1 },
#endif
		{ SIGIO, "SIGIO", 1 },
		{ SIGPWR, "SIGPWR", 1 },
#ifdef SIGUNUSED
		{ SIGUNUSED, "SIGUNUSED", 1 },
#endif
		{ 0, NULL, 0 }
	};

	int i;
	for(i = 0; signals[i].sig != 0; i++)
		if(signal(signals[i].sig,
					signals[i].ignore ? SIG_IGN : sig_handler) == SIG_ERR)
			perror("Setting handler");
}

static void usage(char *progname) {
	fprintf (stderr,"Usage: %s [-w] [-t] [-d] [-n nodename] [-p pidfile] mgmt_socket\n"
			"       %s [--web] [--telnet] [--daemon] [--nodename nodename] [--pidfile pidfile] mgmt_socket\n",progname,progname);
	exit(-1);
}

void setprompt(char *ctrl,char *nodename)
{
	char buf[BUFSIZE];
	if (nodename==NULL) {
		struct utsname un;
		uname(&un);
		snprintf(buf,BUFSIZE,"VDE2@%s[%s]: ",un.nodename,ctrl);
	} else
		snprintf(buf,BUFSIZE,"VDE2@%s[%s]: ",nodename,ctrl);
	prompt=strdup(buf);
}

int openextravdem(void)
{
	struct sockaddr_un sun;
	int fd,n;
	char buf[BUFSIZE+1];
	sun.sun_family=PF_UNIX;
	snprintf(sun.sun_path,UNIX_PATH_MAX,"%s",mgmt);
	fd=socket(PF_UNIX,SOCK_STREAM,0);
	if (connect(fd,(struct sockaddr *)(&sun),sizeof(sun)) < 0) {
		printlog(LOG_ERR,"Error connecting to the management socket '%s': %s", mgmt, strerror(errno));
		return(-1);
	}
	if ((n=read(fd,buf,BUFSIZE))<=0) {
		printlog(LOG_ERR,"banner %s",strerror(errno));
		return(-1);
	}
	return fd;
}

int openvdem(char *mgmt,char *progname, struct netif **nif,char *nodename)
{
	(void) progname;
	struct sockaddr_un sun;
	int fd,n;
	ssize_t voidn;
	(void) voidn;
	char buf[BUFSIZE+1],*line2,*ctrl;
	sun.sun_family=PF_UNIX;
	snprintf(sun.sun_path,UNIX_PATH_MAX,"%s",mgmt);
	fd=socket(PF_UNIX,SOCK_STREAM,0);
	if (connect(fd,(struct sockaddr *)(&sun),sizeof(sun)) < 0) {
		printlog(LOG_ERR,"Error connecting to the management socket '%s': %s", mgmt, strerror(errno));
		exit(-1);
	}
	if ((n=read(fd,buf,BUFSIZE))<=0) {
		printlog(LOG_ERR,"Error reading banner from VDE switch: %s",strerror(errno));
		exit(-1);
	}
	buf[n]=0;
	if ((ctrl=rindex(buf,'\n')) != NULL)
		*ctrl=0;
	banner=strdup(buf);
	voidn = write(fd,"ds/showinfo\n",12);
	if ((n=read(fd,buf,BUFSIZE))<=0) {
		printlog(LOG_ERR,"Error reading ctl socket from VDE switch: %s",strerror(errno));
		exit(-1);
	}
	buf[n]=0;
	if ((line2=index(buf,'\n')) == NULL) {
		printlog(LOG_ERR,"Error parsing first line of ctl socket information");
		exit(-1);
	}
	line2++;
	if (strncmp(line2,"ctl dir ",8) != 0) {
		printlog(LOG_ERR,"Error parsing ctl socket information");
		exit(-1);
	}
	for(ctrl=line2+8;*ctrl!='\n' && ctrl<buf+n;ctrl++)
		;
	*ctrl=0;
	ctrl=line2+8;
	setprompt(ctrl,nodename);
	strcat(ctrl,"[0]");
	*nif=lwip_vdeif_add(lwipstack,ctrl);
	if (*nif == NULL) {
		printlog(LOG_ERR,"Cannot connect to the VDE switch");
		exit(-1);
	}
	lwip_ifup(*nif);

	return fd;
}

static void bitno2mask(unsigned char *addr,int bitno,int len)
{
	int i;
	for(i=0;i<len;i++,bitno -= 8) {
		if (bitno >= 8)
			addr[i]=255;
		else if (bitno <= 0)
			addr[i]=0;
		else
			addr[i]=256 - (1<<(8-bitno));
	}
}

static void sockaddr2ip_6addr(struct ip_addr *ipaddrp,unsigned char *addr)
{
	IP6_ADDR(ipaddrp,
			(addr[0]<<8)|addr[1],
			(addr[2]<<8)|addr[3],
			(addr[4]<<8)|addr[5],
			(addr[6]<<8)|addr[7],
			(addr[8]<<8)|addr[9],
			(addr[10]<<8)|addr[11],
			(addr[12]<<8)|addr[13],
			(addr[14]<<8)|addr[15]);
}

static void readip(char *arg,struct netif *nif,int af)
{
	char *bit=rindex(arg,'/');
	if (bit == 0) 
		printlog(LOG_ERR,"IP addresses must include the netmask i.e. addr/maskbits");
	else {
		int bitno=atoi(bit+1);
		*bit=0; 
		struct addrinfo *res,hint;
		struct ip_addr ipaddr,netmask;
		int err;
		memset(&hint,0,sizeof(hint));
		hint.ai_family=af;
		if ((err=getaddrinfo(arg,NULL,&hint,&res))!=0)
			printlog(LOG_ERR,"IP address %s error %s",arg,gai_strerror(err));
		else {
			switch(res->ai_family) {
				case PF_INET:
					{
						struct sockaddr_in *in=(struct sockaddr_in *)res->ai_addr;
						int addrh=ntohl(in->sin_addr.s_addr);
						unsigned char i,addr[4];
						for (i=0;i<4;i++,addrh>>=8)
							addr[3-i]=addrh;
						IP64_ADDR(&ipaddr, addr[0],addr[1],addr[2],addr[3]);
						bitno2mask(addr,bitno,4);
						IP64_MASKADDR(&netmask, addr[0],addr[1],addr[2],addr[3]);
						lwip_add_addr(nif,&ipaddr,&netmask);
					}
					break;
				case PF_INET6:
					{
						struct sockaddr_in6 *in=(struct sockaddr_in6 *)res->ai_addr;
						unsigned char *addr=in->sin6_addr.s6_addr;
						sockaddr2ip_6addr(&ipaddr,addr);
						bitno2mask(addr,bitno,16);
						sockaddr2ip_6addr(&netmask,addr);
						lwip_add_addr(nif,&ipaddr,&netmask);
					}
					break;
				default:
					printlog(LOG_ERR,"Unsupported Address Family: %s",arg);
			}
			freeaddrinfo(res);
		}
	}
}

static void readdefroute(char *arg,struct netif *nif,int af)
{
	struct addrinfo *res,hint;
	struct ip_addr ipaddr;
	int err;
	memset(&hint,0,sizeof(hint));
	hint.ai_family=af;
	if ((err=getaddrinfo(arg,NULL,&hint,&res))!=0)
		printlog(LOG_ERR,"IP address %s error %s",arg,gai_strerror(err));
	else {
		switch(res->ai_family) {
			case PF_INET:
				{
					struct sockaddr_in *in=(struct sockaddr_in *)res->ai_addr;
					int addrh=ntohl(in->sin_addr.s_addr);
					unsigned char i,addr[4];
					for (i=0;i<4;i++,addrh>>=8)
						addr[3-i]=addrh;
					IP64_ADDR(&ipaddr, addr[0],addr[1],addr[2],addr[3]);
					lwip_add_route(lwipstack,IP_ADDR_ANY,IP_ADDR_ANY,&ipaddr,nif,0);
				}
				break;
			case PF_INET6:
				{
					struct sockaddr_in6 *in=(struct sockaddr_in6 *)res->ai_addr;
					sockaddr2ip_6addr(&ipaddr,in->sin6_addr.s6_addr);
					lwip_add_route(lwipstack,IP_ADDR_ANY,IP_ADDR_ANY,&ipaddr,nif,0);
				}
				break;
			default:
				printlog(LOG_ERR,"Unsupported Address Family: %s",arg);
		}
		freeaddrinfo(res);
	}
}

static void readpassword(char *arg,int unused)
{
	(void) unused;
	passwd=strdup(arg);
}

struct cf {
	char *tag;
	void (*f)();
	int arg;
} cft[]= {
	{"ip4",readip,PF_INET},
	{"ip6",readip,PF_INET6},
	{"ip",readip,0},
	{"defroute4",readdefroute,PF_INET},
	{"defroute6",readdefroute,PF_INET6},
	{"defroute",readdefroute,0},
	{"password",readpassword,0},
	{NULL,NULL,0}};

int readconffile(char *path,struct netif *nif)
{
	FILE *f;
	char buf[BUFSIZE],*s;
	int line = 0;

	if (path==NULL)
		return -1;
	if((f=fopen(path,"r"))==NULL)
		return -1;
	while (fgets(buf,BUFSIZE,f) != NULL)
	{
		line++;

		if ((s=rindex(buf,'\n')) != NULL)
			*s=0;

		for(s=buf;*s == ' ' || *s == '\t';s++);

		if (*s != '#' && *s != '\n' && *s != '\0')
		{
			struct cf *scf;
			for (scf=cft;scf->tag != NULL;scf++)
				if(strncmp(s,scf->tag,strlen(scf->tag)) == 0)
				{
					s+=strlen(scf->tag);
					for(;*s == ' ' || *s == '\t';s++);
					if (*s == '=')
						s++;
					for(;*s == ' ' || *s == '\t';s++);
					scf->f(s,nif,scf->arg);
					break;
				}
			if (scf->tag == NULL) 
				printlog(LOG_ERR,"Error parsing configuration file: line %d: %s", line, buf);
		}
	}
	return 0;
}

int addpfd(int fd,voidfun cb)
{
	if (npfd < MAXFD) {
		pfd[npfd].fd=fd;
		pfd[npfd].events=POLLIN|POLLHUP;
		pfd[npfd].revents=0;
		fpfd[npfd]=cb;
		npfd++;
	}
	return npfd-1;
}

void delpfd(int fn)
{
	int i=fn;
	for (i=fn;i<npfd-1;i++) {
		pfd[i]=pfd[i+1];
		fpfd[i]=fpfd[i+1];
		status[i]=status[i+1];
	}
	npfd--;
}

int pfdsearch(int fd)
{
	int i;
	for (i=0;i<npfd && pfd[i].fd!=fd;i++)
		;
	return i;
}

#if 0
int setfds(fd_set *rds, fd_set *exc)
{
	int i,max=0;
	FD_ZERO(rds);
	FD_ZERO(exc);
	for (i=0;i<npfd;i++) {
		FD_SET(pfd[i].fd,rds);
		FD_SET(pfd[i].fd,exc);
		if (pfd[i].fd>max) max=pfd[i].fd;
	}
	return max+1;
}
#endif

static void save_pidfile(void)
{
	if(pidfile[0] != '/')
		strncat(pidfile_path, pidfile, _POSIX_PATH_MAX - strlen(pidfile_path));
	else
		strcpy(pidfile_path, pidfile);

	int fd = open(pidfile_path,
			O_WRONLY | O_CREAT | O_EXCL,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	FILE *f;

	if(fd == -1) {
		printlog(LOG_ERR, "Error in pidfile creation: %s", strerror(errno));
		exit(1);
	}

	if((f = fdopen(fd, "w")) == NULL) {
		printlog(LOG_ERR, "Error in FILE* construction: %s", strerror(errno));
		exit(1);
	}

	if(fprintf(f, "%ld\n", (long int)getpid()) <= 0) {
		printlog(LOG_ERR, "Error in writing pidfile");
		exit(1);
	}

	fclose(f);
}

/* this custom version of daemon(3) continue to receive stderr messages
 * until the end of the startup phase, the foreground process terminates
 * when stderr gets closed*/
static int special_daemon(void)
{
	int fd;
	int errorpipe[2];
	char buf[256];
	int n;
	ssize_t voidn;
	(void) voidn;

	if (pipe(errorpipe))
		return -1;

	switch (fork()) {
		case -1:
			return (-1);
		case 0:
			break;
		default:
			close(errorpipe[1]);
			while ((n=read(errorpipe[0],buf,128)) > 0) {
				voidn = write(STDERR_FILENO,buf,n);
			}
			_exit(0);
	}
	close(errorpipe[0]);

	if (setsid() == -1)
		return (-1);

	voidn = chdir("/");

	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(errorpipe[1], STDERR_FILENO);
		close(errorpipe[1]);
		if (fd > 2)
			(void)close (fd);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	struct netif *nif;
	int vdefd;
	char *conffile=NULL;
	char *nodename=NULL;
	int c;
	
	progname=argv[0];

	while (1) {
		int option_index = 0;

		static struct option long_options[] = {
			{"daemon", 0, 0, 'd'},
			{"mgmt", 1, 0, 'M'},
			{"telnet", 0, 0, 't'},
			{"web", 0, 0, 'w'},
			{"help",0,0,'h'},
			{"rcfile",1,0,'f'},
			{"nodename",1,0,'n'},
			{"pidfile", 1, 0, 'p'},
			{0, 0, 0, 0}
		};
		c = getopt_long_only (argc, argv, "hdwtM:f:n:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
			case 'M':
				mgmt=strdup(optarg);
				break;
			case 'f':
				conffile=strdup(optarg);
				break;
			case 'n':
				nodename=strdup(optarg);
				break;
			case 't':
				telnet=1;
				break;
			case 'w':
				web=1;
				break;
			case 'd':
				daemonize=1;
				break;
			case 'p':
				pidfile=strdup(optarg);
				break;
			case 'h':
				usage(argv[0]); //implies exit
				break;
		}
	}
	if (optind < argc && mgmt==NULL)
		mgmt=argv[optind];

	if (mgmt==NULL) {
		printlog(LOG_ERR,"mgmt_socket not defined");
		exit(-1);
	}
	if (telnet==0 && web==0) {
		printlog(LOG_ERR,"at least one service option (-t -w) must be specified");
		exit(-1);
	}

	atexit(cleanup);
	setsighandlers();

	/* saves current path in pidfile_path, because otherwise with daemonize() we
	 * forget it */
	if(getcwd(pidfile_path, _POSIX_PATH_MAX-1) == NULL) {
		printlog(LOG_ERR, "getcwd: %s", strerror(errno));
		exit(1);
	}
	strcat(pidfile_path, "/");
	
	/* call daemon before starting the stack otherwise the stack threads
	 * does not get inherited by the forked process */
	if (daemonize && special_daemon()) {
		printlog(LOG_ERR,"daemon: %s",strerror(errno));
		exit(1);
	}

	lwipstack=lwip_stack_new();
	lwip_stack_set(lwipstack);
	
	vdefd = openvdem(mgmt, argv[0], &nif, nodename);
	
	/* If rcfile is specified, try it and nothing else */
	if (conffile && readconffile(conffile,nif) < 0)
	{
		printlog(LOG_ERR, "Error reading configuration file '%s': %s", conffile, strerror(errno));
		exit(1);
	}
	/* Else try default ones */
	else if (!conffile)
	{
		int rv;
		char *homedir = getenv("HOME");
		if (homedir)
		{
			int len = strlen(homedir) + strlen(USERCONFFILE) + 1;
			conffile = malloc(len);
			snprintf(conffile, len, "%s%s", homedir, USERCONFFILE);
			if ((rv = readconffile(conffile, nif)) >= 0)
				free(conffile);
		}
		if (!homedir || rv < 0)
			rv = readconffile(conffile = ROOTCONFFILE, nif);

		if (rv < 0)
		{
			printlog(LOG_ERR, "Error reading configuration file '%s': %s", conffile, strerror(errno));
			exit(1);
		}
	}
	
	/* once here, we're sure we're the true process which will continue as a
	 * server: save PID file if needed */
	if(pidfile) save_pidfile();

	if (telnet)
		telnet_init(vdefd);
	if (web)
		web_init(vdefd);

	if (daemonize) {
		int fd;
		if ((fd=open("/dev/null",O_RDWR)) >= 0) {
			close(STDERR_FILENO);
			dup2(fd,STDERR_FILENO);
			close(fd);
			openlog(basename(argv[0]), LOG_PID, 0);
			logok=1;
		}
		printlog(LOG_INFO,"VDETELWEB started");
	}

	while (1)
	{
		int i;
		int m=lwip_poll(pfd,npfd,-1);
		for (i=0;i<npfd && m>0;i++) {
			if (pfd[i].revents) {
				m--;
				fpfd[i](i,pfd[i].fd,vdefd);
			}
		}
	}
}

