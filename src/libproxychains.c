/***************************************************************************
                          libproxychains.c  -  description
                             -------------------
    begin                : Tue May 14 2002
    copyright          :  netcreature (C) 2002
    email                 : netcreature@users.sourceforge.net
 ***************************************************************************/
 /*     GPL */
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#undef _GNU_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <netdb.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>


#include "core.h"
#include "common.h"

#define     satosin(x)      ((struct sockaddr_in *) &(x))
#define     SOCKADDR(x)     (satosin(x)->sin_addr.s_addr)
#define     SOCKADDR_2(x)     (satosin(x)->sin_addr)
#define     SOCKPORT(x)     (satosin(x)->sin_port)
#define     SOCKFAMILY(x)     (satosin(x)->sin_family)
#define     MAX_CHAIN 512

#ifdef IS_SOLARIS
#undef connect
int __xnet_connect(int sock, const struct sockaddr *addr, unsigned int len);
connect_t true___xnet_connect;
#endif

close_t true_close;
connect_t true_connect;
gethostbyname_t true_gethostbyname;
getaddrinfo_t true_getaddrinfo;
freeaddrinfo_t true_freeaddrinfo;
getnameinfo_t true_getnameinfo;
gethostbyaddr_t true_gethostbyaddr;
sendto_t true_sendto;

int tcp_read_time_out;
int tcp_connect_time_out;
chain_type proxychains_ct;
proxy_data proxychains_pd[MAX_CHAIN];
unsigned int proxychains_proxy_count = 0;
unsigned int proxychains_proxy_offset = 0;
int proxychains_got_chain_data = 0;
unsigned int proxychains_max_chain = 1;
int proxychains_quiet_mode = 0;
int proxychains_resolver = 0;
localaddr_arg localnet_addr[MAX_LOCALNET];
size_t num_localnet_addr = 0;
unsigned int remote_dns_subnet = 224;

pthread_once_t init_once = PTHREAD_ONCE_INIT;

static int init_l = 0;

static void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct);

static void* load_sym(char* symname, void* proxyfunc) {

	void *funcptr = dlsym(RTLD_NEXT, symname);

	if(!funcptr) {
		fprintf(stderr, "Cannot load symbol '%s' %s\n", symname, dlerror());
		exit(1);
	} else {
		PDEBUG("loaded symbol '%s'" " real addr %p  wrapped addr %p\n", symname, funcptr, proxyfunc);
	}
	if(funcptr == proxyfunc) {
		PDEBUG("circular reference detected, aborting!\n");
		abort();
	}
	return funcptr;
}

#define INIT() init_lib_wrapper(__FUNCTION__)

#define SETUP_SYM(X) do { if (! true_ ## X ) true_ ## X = load_sym( # X, X ); } while(0)

#include "allocator_thread.h"

const char *proxychains_get_version(void);

static void setup_hooks(void) {
	SETUP_SYM(connect);
	SETUP_SYM(sendto);
	SETUP_SYM(gethostbyname);
	SETUP_SYM(getaddrinfo);
	SETUP_SYM(freeaddrinfo);
	SETUP_SYM(gethostbyaddr);
	SETUP_SYM(getnameinfo);
	SETUP_SYM(close);
#ifdef IS_SOLARIS
	SETUP_SYM(__xnet_connect);
#endif
}

static int close_fds[16];
static int close_fds_cnt = 0;

static void do_init(void) {
	srand(time(NULL));
	core_initialize();
	at_init();

	/* read the config file */
	get_chain_data(proxychains_pd, &proxychains_proxy_count, &proxychains_ct);
	DUMP_PROXY_CHAIN(proxychains_pd, proxychains_proxy_count);

	proxychains_write_log(LOG_PREFIX "DLL init: proxychains-ng %s\n", proxychains_get_version());

	setup_hooks();

	while(close_fds_cnt) true_close(close_fds[--close_fds_cnt]);

	init_l = 1;
}

static void init_lib_wrapper(const char* caller) {
#ifndef DEBUG
	(void) caller;
#endif
	if(!init_l) PDEBUG("%s called from %s\n", __FUNCTION__,  caller);
	pthread_once(&init_once, do_init);
}

/* if we use gcc >= 3, we can instruct the dynamic loader 
 * to call init_lib at link time. otherwise it gets loaded
 * lazily, which has the disadvantage that there's a potential
 * race condition if 2 threads call it before init_l is set 
 * and PTHREAD support was disabled */
#if __GNUC__ > 2
__attribute__((constructor))
static void gcc_init(void) {
	INIT();
}
#endif


typedef enum {
	RS_PT_NONE = 0,
	RS_PT_SOCKS4,
	RS_PT_SOCKS5,
	RS_PT_HTTP
} rs_proxyType;

/*
  proxy_from_string() taken from rocksock network I/O library (C) rofl0r
  valid inputs:
	socks5://user:password@proxy.domain.com:port
	socks5://proxy.domain.com:port
	socks4://proxy.domain.com:port
	http://user:password@proxy.domain.com:port
	http://proxy.domain.com:port

	supplying port number is obligatory.
	user:pass@ part is optional for http and socks5.
	however, user:pass authentication is currently not implemented for http proxies.
  return 1 on success, 0 on error.
*/
static int proxy_from_string(const char *proxystring,
	char *type_buf,
	char* host_buf,
	int *port_n,
	char *user_buf,
	char* pass_buf)
{
	const char* p;
	rs_proxyType proxytype;

	size_t next_token = 6, ul = 0, pl = 0, hl;
	if(!proxystring[0] || !proxystring[1] || !proxystring[2] || !proxystring[3] || !proxystring[4] || !proxystring[5]) goto inv_string;
	if(*proxystring == 's') {
		switch(proxystring[5]) {
			case '5': proxytype = RS_PT_SOCKS5; break;
			case '4': proxytype = RS_PT_SOCKS4; break;
			default: goto inv_string;
		}
	} else if(*proxystring == 'h') {
		proxytype = RS_PT_HTTP;
		next_token = 4;
	} else goto inv_string;
	if(
	   proxystring[next_token++] != ':' ||
	   proxystring[next_token++] != '/' ||
	   proxystring[next_token++] != '/') goto inv_string;
	const char *at = strrchr(proxystring+next_token, '@');
	if(at) {
		if(proxytype == RS_PT_SOCKS4)
			return 0;
		p = strchr(proxystring+next_token, ':');
		if(!p || p >= at) goto inv_string;
		const char *u = proxystring+next_token;
		ul = p-u;
		p++;
		pl = at-p;
		if(proxytype == RS_PT_SOCKS5 && (ul > 255 || pl > 255))
			return 0;
		memcpy(user_buf, u, ul);
		user_buf[ul]=0;
		memcpy(pass_buf, p, pl);
		pass_buf[pl]=0;
		next_token += 2+ul+pl;
	} else {
		user_buf[0]=0;
		pass_buf[0]=0;
	}
	const char* h = proxystring+next_token;
	p = strchr(h, ':');
	if(!p) goto inv_string;
	hl = p-h;
	if(hl > 255)
		return 0;
	memcpy(host_buf, h, hl);
	host_buf[hl]=0;
	*port_n = atoi(p+1);
	switch(proxytype) {
		case RS_PT_SOCKS4:
			strcpy(type_buf, "socks4");
			break;
		case RS_PT_SOCKS5:
			strcpy(type_buf, "socks5");
			break;
		case RS_PT_HTTP:
			strcpy(type_buf, "http");
			break;
		default:
			return 0;
	}
	return 1;
inv_string:
	return 0;
}

static const char* bool_str(int bool_val) {
	if(bool_val) return "true";
	return "false";
}

static int parse_ipv4_addr(char *localnet_spec, localaddr_arg *localaddr) {
	char local_in_addr_port[32];
	char local_in_addr[32], local_in_port[32], local_netmask[32];
	if (sscanf(localnet_spec, "%21[^/]/%15s", local_in_addr_port, local_netmask) < 2) {
		fprintf(stderr, "localnet format error");
		return -1;
	}

	if (sscanf(local_in_addr_port, "%15[^:]:%5s", local_in_addr, local_in_port) < 2) {
		PDEBUG("added localnet: netaddr=%s, netmask=%s\n", local_in_addr, local_netmask);
		localaddr->port = 0;
	} else {
		PDEBUG("added localnet: netaddr=%s, port=%s, netmask=%s\n", local_in_addr, local_in_port, local_netmask);
		localaddr->port = (short)atoi(local_in_port);
	}

	int error;
	error = inet_pton(AF_INET, local_in_addr, &localaddr->ip.addr);
	if (error <= 0) {
		fprintf(stderr, "localnet address error\n");
		return -1;
	}
	localaddr->ip.is_v6 = 0;

	error = inet_pton(AF_INET, local_netmask, &localaddr->netmask.addr);
	if (error <= 0) {
		fprintf(stderr, "localnet netmask error\n");
		return -1;
	}
	localaddr->netmask.is_v6 = 0;

	return 0;
}

/*
 * Parse an ipv6 subnet in the format of 2001:db8:a::/64.
 * A port number can be specified as [2001:db8:a::]:8080/64.
 */
static int parse_ipv6_addr(char *localnet_spec, localaddr_arg *localaddr) {
	char local_in_addr_port[64];
	char local_in_addr[64], local_in_port[32];
	unsigned int local_in_prefix;
	if (sscanf(localnet_spec, "%47[^/]/%3d", local_in_addr_port, &local_in_prefix) < 2) {
		fprintf(stderr, "localnet format error");
		return -1;
	}

	if (sscanf(local_in_addr_port, "[%39s]:%5s", local_in_addr, local_in_port) == 2) {
		PDEBUG("added localnet: netaddr=%s, port=%s, prefix=%d\n", local_in_addr, local_in_port, local_in_prefix);
		localaddr->port = (short)atoi(local_in_port);
	} else if (sscanf(local_in_addr_port, "%39s", local_in_addr) == 1) {
		PDEBUG("added localnet: netaddr=%s, prefix=%d\n", local_in_addr, local_in_prefix);
		localaddr->port = 0;
	}

	int error;
	error = inet_pton(AF_INET6, local_in_addr, &localaddr->ip.addr);
	if (error <= 0) {
		fprintf(stderr, "localnet address error\n");
		return -1;
	}
	localaddr->ip.is_v6 = 1;

	if (local_in_prefix > 128) {
		fprintf(stderr, "invalid ipv6 prefix: %d\n", local_in_prefix);
		return -1;
	}

	int i;
	memset(localaddr->netmask.addr.v6, 0, sizeof(localaddr->netmask.addr));
	for (i = 0; local_in_prefix >= 8; local_in_prefix -= 8, ++i) {
		localaddr->netmask.addr.v6[i] = 0xff;
	}
	localaddr->netmask.addr.v6[i] = ~0 << (4 - local_in_prefix);

	return 0;
}

/* get configuration from config file */
static void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct) {
	int count = 0, port_n = 0, list = 0;
	char buff[1024], type[1024], host[1024], user[1024];
	char *env;
	char localnet_spec[64];
	FILE *file = NULL;

	if(proxychains_got_chain_data)
		return;

	//Some defaults
	tcp_read_time_out = 4 * 1000;
	tcp_connect_time_out = 10 * 1000;
	*ct = DYNAMIC_TYPE;

	env = get_config_path(getenv(PROXYCHAINS_CONF_FILE_ENV_VAR), buff, sizeof(buff));
	if( ( file = fopen(env, "r") ) == NULL )
	{
		perror("couldnt read configuration file");
		exit(1);
	}

	env = getenv(PROXYCHAINS_QUIET_MODE_ENV_VAR);
	if(env && *env == '1')
		proxychains_quiet_mode = 1;

	while(fgets(buff, sizeof(buff), file)) {
		if(buff[0] != '\n' && buff[strspn(buff, " ")] != '#') {
			/* proxylist has to come last */
			if(list) {
				if(count >= MAX_CHAIN)
					break;

				memset(&pd[count], 0, sizeof(proxy_data));

				pd[count].ps = PLAY_STATE;
				port_n = 0;

				int ret = sscanf(buff, "%s %s %d %s %s", type, host, &port_n, pd[count].user, pd[count].pass);
				if(ret < 3 || ret == EOF) {
					if(!proxy_from_string(buff, type, host, &port_n, pd[count].user, pd[count].pass)) {
						inv:
						fprintf(stderr, "error: invalid item in proxylist section: %s", buff);
						exit(1);
					}
				}

				memset(&pd[count].ip, 0, sizeof(pd[count].ip));
				pd[count].ip.is_v6 = !!strchr(host, ':');
				pd[count].port = htons((unsigned short) port_n);
				ip_type* host_ip = &pd[count].ip;
				if(1 != inet_pton(host_ip->is_v6 ? AF_INET6 : AF_INET, host, host_ip->addr.v6)) {
					if(*ct == STRICT_TYPE && proxychains_resolver && count > 0) {
						/* we can allow dns hostnames for all but the first proxy in the list if chaintype is strict, as remote lookup can be done */
						ip_type4 internal_ip = at_get_ip_for_host(host, strlen(host));
						pd[count].ip.is_v6 = 0;
						host_ip->addr.v4 = internal_ip;
						if(internal_ip.as_int == ip_type_invalid.addr.v4.as_int)
							goto inv_host;
					} else {
inv_host:
						fprintf(stderr, "proxy %s has invalid value or is not numeric\n", host);
						fprintf(stderr, "non-numeric ips are only allowed under the following circumstances:\n");
						fprintf(stderr, "chaintype == strict (%s), proxy is not first in list (%s), proxy_dns active (%s)\n\n", bool_str(*ct == STRICT_TYPE), bool_str(count > 0), bool_str(proxychains_resolver));
						exit(1);
					}
				}

				if(!strcmp(type, "http")) {
					pd[count].pt = HTTP_TYPE;
				} else if(!strcmp(type, "socks4")) {
					pd[count].pt = SOCKS4_TYPE;
				} else if(!strcmp(type, "socks5")) {
					pd[count].pt = SOCKS5_TYPE;
				} else
					goto inv;

				if(port_n)
					count++;
			} else {
				if(strstr(buff, "[ProxyList]")) {
					list = 1;
				} else if(strstr(buff, "random_chain")) {
					*ct = RANDOM_TYPE;
				} else if(strstr(buff, "strict_chain")) {
					*ct = STRICT_TYPE;
				} else if(strstr(buff, "dynamic_chain")) {
					*ct = DYNAMIC_TYPE;
				} else if(strstr(buff, "round_robin_chain")) {
					*ct = ROUND_ROBIN_TYPE;
				} else if(strstr(buff, "tcp_read_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_read_time_out);
				} else if(strstr(buff, "tcp_connect_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_connect_time_out);
				} else if(strstr(buff, "remote_dns_subnet")) {
					sscanf(buff, "%s %u", user, &remote_dns_subnet);
					if(remote_dns_subnet >= 256) {
						fprintf(stderr,
							"remote_dns_subnet: invalid value. requires a number between 0 and 255.\n");
						exit(1);
					}
				} else if(strstr(buff, "localnet")) {
					if(sscanf(buff, "%s %s", user, localnet_spec) < 2){
						fprintf(stderr, "localnet format error");
						exit(1);
					}
					if (num_localnet_addr >= MAX_LOCALNET) {
						fprintf(stderr, "# of localnet exceed %d.\n", MAX_LOCALNET);
						continue;
					}
					if (strchr(buff, '.')) {
						if (parse_ipv4_addr(localnet_spec, &localnet_addr[num_localnet_addr]) == 0) {
							++num_localnet_addr;
						} else {
							fprintf(stderr, "unable to parse ipv4 addr %s.\n", localnet_spec);
							exit(1);
						}
					} else {
						if (parse_ipv6_addr(localnet_spec, &localnet_addr[num_localnet_addr]) == 0) {
							++num_localnet_addr;
						} else {
							fprintf(stderr, "unable to parse ipv6 addr %s.\n", localnet_spec);
							exit(1);
						}
					}
				} else if(strstr(buff, "chain_len")) {
					char *pc;
					int len;
					pc = strchr(buff, '=');
					if(!pc) {
						fprintf(stderr, "error: missing equals sign '=' in chain_len directive.\n");
						exit(1);
					}
					len = atoi(++pc);
					proxychains_max_chain = (len ? len : 1);
				} else if(strstr(buff, "quiet_mode")) {
					proxychains_quiet_mode = 1;
				} else if(strstr(buff, "proxy_dns")) {
					proxychains_resolver = 1;
				}
			}
		}
	}
#ifndef BROKEN_FCLOSE
	fclose(file);
#endif
	if(!count) {
		fprintf(stderr, "error: no valid proxy found in config\n");
		exit(1);
	}
	*proxy_count = count;
	proxychains_got_chain_data = 1;
	PDEBUG("proxy_dns: %s\n", proxychains_resolver ? "ON" : "OFF");
}

/*******  HOOK FUNCTIONS  *******/

int close(int fd) {
	if(!init_l) {
		if(close_fds_cnt>=(sizeof close_fds/sizeof close_fds[0])) goto err;
		close_fds[close_fds_cnt++] = fd;
		errno = 0;
		return 0;
	}
	/* prevent rude programs (like ssh) from closing our pipes */
	if(fd != req_pipefd[0]  && fd != req_pipefd[1] &&
	   fd != resp_pipefd[0] && fd != resp_pipefd[1]) {
		return true_close(fd);
	}
	err:
	errno = EBADF;
	return -1;
}
static int is_v4inv6(const struct in6_addr *a) {
	return !memcmp(a->s6_addr, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
}
static inline int
ipv6_masked_addr_cmp(const struct in6_addr *a1, const struct in6_addr *m, const struct in6_addr *a2)
{
	for (int i = 0; i < 16; ++i) {
		if (!((a1->s6_addr[i] ^ a2->s6_addr[i]) & m->s6_addr[i]))
			return 0;
	}
	return 1;
}
int connect(int sock, const struct sockaddr *addr, unsigned int len) {
	INIT();
	PFUNC();

	int socktype = 0, flags = 0, ret = 0;
	socklen_t optlen = 0;
	ip_type dest_ip;
	DEBUGDECL(char str[256]);

	struct in_addr *p_addr_in;
	struct in6_addr *p_addr_in6;
	unsigned short port;
	size_t i;
	int remote_dns_connect = 0;
	optlen = sizeof(socktype);
	sa_family_t fam = SOCKFAMILY(*addr);
	getsockopt(sock, SOL_SOCKET, SO_TYPE, &socktype, &optlen);
	if(!((fam  == AF_INET || fam == AF_INET6) && socktype == SOCK_STREAM))
		return true_connect(sock, addr, len);

	int v6 = dest_ip.is_v6 = fam == AF_INET6;

	p_addr_in = &((struct sockaddr_in *) addr)->sin_addr;
	p_addr_in6 = &((struct sockaddr_in6 *) addr)->sin6_addr;
	port = !v6 ? ntohs(((struct sockaddr_in *) addr)->sin_port)
			   : ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
	struct in_addr v4inv6;
	if(v6 && is_v4inv6(p_addr_in6)) {
		memcpy(&v4inv6.s_addr, &p_addr_in6->s6_addr[12], 4);
		v6 = dest_ip.is_v6 = 0;
		p_addr_in = &v4inv6;
	}
	if(!v6 && !memcmp(p_addr_in, "\0\0\0\0", 4)) {
		errno = ECONNREFUSED;
		return -1;
	}

//	PDEBUG("localnet: %s; ", inet_ntop(AF_INET, &in_addr_localnet, str, sizeof(str)));
//	PDEBUG("netmask: %s; " , inet_ntop(AF_INET, &in_addr_netmask, str, sizeof(str)));
	PDEBUG("target: %s\n", inet_ntop(v6 ? AF_INET6 : AF_INET, v6 ? (void*)p_addr_in6 : (void*)p_addr_in, str, sizeof(str)));
	PDEBUG("port: %d\n", port);

	// check if connect called from proxydns
	remote_dns_connect = !v6 && (ntohl(p_addr_in->s_addr) >> 24 == remote_dns_subnet);

	for (i = 0; i < num_localnet_addr && !remote_dns_connect; i++) {
		if (!v6 && !localnet_addr[i].ip.is_v6) {
			if ((localnet_addr[i].ip.addr.v4.as_int & localnet_addr[i].netmask.addr.v4.as_int)
				== (p_addr_in->s_addr & localnet_addr[i].netmask.addr.v4.as_int)) {
				if (!localnet_addr[i].port || localnet_addr[i].port == port) {
					PDEBUG("accessing localnet using true_connect\n");
					return true_connect(sock, addr, len);
				}
			}
		} else if (v6 && localnet_addr[i].ip.is_v6) {
			for (int j = 0; j < 16; ++j) {
				if (ipv6_masked_addr_cmp((struct in6_addr *) &localnet_addr[i].ip.addr,
										 (struct in6_addr *) &localnet_addr[i].netmask, p_addr_in6)) {
					PDEBUG("accessing localnet using true_connect\n");
					return true_connect(sock, addr, len);
				}
			}
		}
	}

	flags = fcntl(sock, F_GETFL, 0);
	if(flags & O_NONBLOCK)
		fcntl(sock, F_SETFL, !O_NONBLOCK);

	memcpy(dest_ip.addr.v6, v6 ? (void*)p_addr_in6 : (void*)p_addr_in, v6?16:4);

	ret = connect_proxy_chain(sock,
				  dest_ip,
				  htons(port),
				  proxychains_pd, proxychains_proxy_count, proxychains_ct, proxychains_max_chain);

	fcntl(sock, F_SETFL, flags);
	if(ret != SUCCESS)
		errno = ECONNREFUSED;
	return ret;
}

#ifdef IS_SOLARIS
int __xnet_connect(int sock, const struct sockaddr *addr, unsigned int len) {
	return connect(sock, addr, len);
}
#endif

static struct gethostbyname_data ghbndata;
struct hostent *gethostbyname(const char *name) {
	INIT();
	PDEBUG("gethostbyname: %s\n", name);

	if(proxychains_resolver)
		return proxy_gethostbyname(name, &ghbndata);
	else
		return true_gethostbyname(name);

	return NULL;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
	INIT();
	PDEBUG("getaddrinfo: %s %s\n", node ? node : "null", service ? service : "null");

	if(proxychains_resolver)
		return proxy_getaddrinfo(node, service, hints, res);
	else
		return true_getaddrinfo(node, service, hints, res);
}

void freeaddrinfo(struct addrinfo *res) {
	INIT();
	PDEBUG("freeaddrinfo %p \n", (void *) res);

	if(!proxychains_resolver)
		true_freeaddrinfo(res);
	else
		proxy_freeaddrinfo(res);
}

int pc_getnameinfo(const struct sockaddr *sa, socklen_t salen,
	           char *host, socklen_t hostlen, char *serv,
	           socklen_t servlen, int flags)
{
	INIT();
	PFUNC();

	if(!proxychains_resolver) {
		return true_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
	} else {
		if(!salen || !(SOCKFAMILY(*sa) == AF_INET || SOCKFAMILY(*sa) == AF_INET6))
			return EAI_FAMILY;
		int v6 = SOCKFAMILY(*sa) == AF_INET6;
		if(salen < (v6?sizeof(struct sockaddr_in6):sizeof(struct sockaddr_in)))
			return EAI_FAMILY;
		if(hostlen) {
			unsigned char v4inv6buf[4];
			const void *ip = v6 ? (void*)&((struct sockaddr_in6*)sa)->sin6_addr
			                    : (void*)&((struct sockaddr_in*)sa)->sin_addr;
			unsigned scopeid = 0;
			if(v6) {
				if(is_v4inv6(&((struct sockaddr_in6*)sa)->sin6_addr)) {
					memcpy(v4inv6buf, &((struct sockaddr_in6*)sa)->sin6_addr.s6_addr[12], 4);
					ip = v4inv6buf;
					v6 = 0;
				} else
					scopeid = ((struct sockaddr_in6 *)sa)->sin6_scope_id;
			}
			if(!inet_ntop(v6?AF_INET6:AF_INET,ip,host,hostlen))
				return EAI_OVERFLOW;
			if(scopeid) {
				size_t l = strlen(host);
				if(snprintf(host+l, hostlen-l, "%%%u", scopeid) >= hostlen-l)
					return EAI_OVERFLOW;
			}
		}
		if(servlen) {
			if(snprintf(serv, servlen, "%d", ntohs(SOCKPORT(*sa))) >= servlen)
				return EAI_OVERFLOW;
		}
	}
	return 0;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {
	INIT();
	PDEBUG("TODO: proper gethostbyaddr hook\n");

	static char buf[16];
	static char ipv4[4];
	static char *list[2];
	static char *aliases[1];
	static struct hostent he;

	if(!proxychains_resolver)
		return true_gethostbyaddr(addr, len, type);
	else {

		PDEBUG("len %u\n", len);
		if(len != 4)
			return NULL;
		he.h_name = buf;
		memcpy(ipv4, addr, 4);
		list[0] = ipv4;
		list[1] = NULL;
		he.h_addr_list = list;
		he.h_addrtype = AF_INET;
		aliases[0] = NULL;
		he.h_aliases = aliases;
		he.h_length = 4;
		pc_stringfromipv4((unsigned char *) addr, buf);
		return &he;
	}
	return NULL;
}

#ifndef MSG_FASTOPEN
#   define MSG_FASTOPEN 0x20000000
#endif

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
	       const struct sockaddr *dest_addr, socklen_t addrlen) {
	INIT();
	PFUNC();
	if (flags & MSG_FASTOPEN) {
		if (!connect(sockfd, dest_addr, addrlen) && errno != EINPROGRESS) {
			return -1;
		}
		dest_addr = NULL;
		addrlen = 0;
		flags &= ~MSG_FASTOPEN;
	}
	return true_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
}
