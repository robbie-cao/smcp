/*	@file smcp-plat-bsd.c
**	@author Robert Quattlebaum <darco@deepdarc.com>
**
**	Copyright (C) 2014 Robert Quattlebaum
**
**	Permission is hereby granted, free of charge, to any person
**	obtaining a copy of this software and associated
**	documentation files (the "Software"), to deal in the
**	Software without restriction, including without limitation
**	the rights to use, copy, modify, merge, publish, distribute,
**	sublicense, and/or sell copies of the Software, and to
**	permit persons to whom the Software is furnished to do so,
**	subject to the following conditions:
**
**	The above copyright notice and this permission notice shall
**	be included in all copies or substantial portions of the
**	Software.
**
**	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
**	KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
**	WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
**	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
**	OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
**	OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
**	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
**	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERBOSE_DEBUG
#define VERBOSE_DEBUG 0
#endif

#ifndef DEBUG
#define DEBUG VERBOSE_DEBUG
#endif

#include "assert-macros.h"

#include "smcp.h"

#if SMCP_USE_BSD_SOCKETS

#include "smcp-internal.h"
#include "smcp-logging.h"

#include <stdio.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/cdefs.h>
#include <time.h>
#include <sys/select.h>
#include <poll.h>


#ifndef SOCKADDR_HAS_LENGTH_FIELD
#if defined(__KAME__)
#define SOCKADDR_HAS_LENGTH_FIELD 1
#endif
#endif


static smcp_status_t
smcp_internal_join_multicast_group(smcp_t self, const char* group)
{
	smcp_status_t ret = SMCP_STATUS_ERRNO;

	if (self->plat.mcfd == -1) {
		self->plat.mcfd = socket(SMCP_BSD_SOCKETS_NET_FAMILY, SOCK_DGRAM, 0);
		require(self->plat.mcfd >= 0, bail);
	}

	struct hostent *tmp = gethostbyname2(group, SMCP_BSD_SOCKETS_NET_FAMILY);

	require(!h_errno && tmp, bail);
	require(tmp->h_length > 1, bail);

#if SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
	{
		struct ipv6_mreq imreq;
		int btrue = 1;
		memset(&imreq, 0, sizeof(imreq));
		memcpy(&imreq.ipv6mr_multiaddr.s6_addr, tmp->h_addr_list[0], 16);

		require(0 ==
			setsockopt(self->plat.mcfd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
				&btrue,
				sizeof(btrue)), bail);

		// Do a precautionary leave group, to clear any stake kernel data.
		setsockopt(self->plat.mcfd,
			IPPROTO_IPV6,
			IPV6_LEAVE_GROUP,
			&imreq,
			sizeof(imreq));

		require_quiet(0 ==
			setsockopt(self->plat.mcfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &imreq,
				sizeof(imreq)), bail);
	}
	ret = SMCP_STATUS_OK;
#else
#warning TODO: Implement joining the multicast group for this network family!
	ret = SMCP_STATUS_NOT_IMPLEMENTED;
#endif

bail:

	return ret;
}

smcp_t
smcp_plat_init(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;

	self->plat.mcfd = -1;
	self->plat.fd_udp = -1;
#if SMCP_DTLS
	self->plat.fd_dtls = -1;
#endif

#if SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
	smcp_internal_join_multicast_group(self, COAP_MULTICAST_IP6_LL_ALLDEVICES);
#endif

	return self;
}

void
smcp_plat_finalize(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;

	if(self->plat.fd_udp>=0) {
		close(self->plat.fd_udp);
	}
#if SMCP_DTLS
	if(self->plat.fd_dtls>=0) {
		close(self->plat.fd_dtls);
	}
#endif
	if(self->plat.mcfd>=0) {
		close(self->plat.mcfd);
	}
}

int
smcp_plat_get_fd(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;
	return self->plat.fd_udp;
}

uint16_t
smcp_plat_get_port(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_sockaddr_t saddr;
	socklen_t socklen = sizeof(saddr);
	if (self->plat.fd_udp < 0) {
		return 0;
	}
	getsockname(self->plat.fd_udp, (struct sockaddr*)&saddr, &socklen);
	return ntohs(saddr.smcp_port);
}


static smcp_cms_t
monotonic_get_time_ms(void)
{
#if HAVE_CLOCK_GETTIME
	struct timespec tv = { 0 };
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC, &tv);

	return (smcp_cms_t)(tv.tv_sec * MSEC_PER_SEC) + (smcp_cms_t)(tv.tv_nsec / NSEC_PER_MSEC);
#else
	struct timeval tv = { 0 };
	gettimeofday(&tv, NULL);
	return (smcp_cms_t)(tv.tv_sec * MSEC_PER_SEC) + (smcp_cms_t)(tv.tv_usec / USEC_PER_MSEC);
#endif
}
smcp_timestamp_t
smcp_plat_cms_to_timestamp(
	smcp_cms_t cms
) {
	return monotonic_get_time_ms() + cms;
}

smcp_cms_t
smcp_plat_timestamp_diff(smcp_timestamp_t lhs, smcp_timestamp_t rhs) {
	return lhs - rhs;
}

smcp_cms_t
smcp_plat_timestamp_to_cms(smcp_timestamp_t ts) {
	return smcp_plat_timestamp_diff(ts, monotonic_get_time_ms());
}

smcp_status_t
smcp_plat_bind_to_sockaddr(
	smcp_t self,
	smcp_session_type_t type,
	const smcp_sockaddr_t* sockaddr
) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = SMCP_STATUS_FAILURE;
	int fd = -1;

	switch(type) {
	case SMCP_SESSION_TYPE_UDP:
#if SMCP_DTLS
	case SMCP_SESSION_TYPE_DTLS:
#endif
#if SMCP_TCP
	case SMCP_SESSION_TYPE_TCP:
#endif
#if SMCP_TLS
	case SMCP_SESSION_TYPE_TLS:
#endif
		break;

	default:
		ret = SMCP_STATUS_NOT_IMPLEMENTED;
		// Unsupported session type.
		goto bail;
	}

	fd = socket(SMCP_BSD_SOCKETS_NET_FAMILY, SOCK_DGRAM, IPPROTO_UDP);

	require_action_string(fd >= 0, bail, ret = SMCP_STATUS_ERRNO, strerror(errno));

#if defined(IPV6_V6ONLY) && SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
	{
		int value = 0; /* explicitly allow ipv4 traffic too (required on bsd and some debian installations) */
		if (setsockopt(self->plat.fd_udp, IPPROTO_IPV6, IPV6_V6ONLY, &value, sizeof(value)) < 0)
		{
			DEBUG_PRINTF("Setting IPV6_V6ONLY=0 on socket failed (%s)",strerror(errno));
		}
	}
#endif

	require_action_string(
		bind(fd, (struct sockaddr*)sockaddr, sizeof(*sockaddr)) == 0,
		bail,
		ret = SMCP_STATUS_ERRNO,
		strerror(errno)
	);

#ifdef SMCP_RECVPKTINFO
	{	// Handle sockopts.
		int value = 1;
		setsockopt(fd, SMCP_IPPROTO, SMCP_RECVPKTINFO, &value, sizeof(value));
	}
#endif

	// TODO: Fix this!
	switch(type) {
	case SMCP_SESSION_TYPE_UDP:
		self->plat.fd_udp = fd;
		break;
#if SMCP_DTLS
	case SMCP_SESSION_TYPE_DTLS:
		self->plat.fd_dtls = fd;
		break;
#endif

	default:
		ret = SMCP_STATUS_NOT_IMPLEMENTED;
		// Unsupported session type.
		goto bail;
	}

	fd = -1;

	ret = SMCP_STATUS_OK;

bail:
	if (fd >= 0) {
		close(fd);
	}
	return ret;
}


smcp_status_t
smcp_plat_bind_to_port(
	smcp_t self,
	smcp_session_type_t type,
	uint16_t port
) {
	SMCP_EMBEDDED_SELF_HOOK;

	smcp_sockaddr_t saddr = {
#if SOCKADDR_HAS_LENGTH_FIELD
		.___smcp_len		= sizeof(smcp_sockaddr_t),
#endif
		.___smcp_family	= SMCP_BSD_SOCKETS_NET_FAMILY,
		.smcp_port		= htons(port),
	};

	return smcp_plat_bind_to_sockaddr(self, type, &saddr);
}

int
smcp_plat_update_pollfds(
	smcp_t self,
	struct pollfd fds[],
	int maxfds
) {
	int ret = 1;

	require_quiet(maxfds > 0, bail);

	assert(fds != NULL);

	if (self->plat.fd_udp > 0) {
		fds->fd = self->plat.fd_udp;
		fds->events = POLLIN | POLLHUP;
		fds->revents = 0;
		fds++;
		maxfds--;
	}

#if SMCP_DTLS
	if (self->plat.fd_dtls > 0) {
		fds->fd = self->plat.fd_dtls;
		fds->events = POLLIN | POLLHUP;
		fds->revents = 0;
		fds++;
		maxfds--;
	}
#endif // SMCP_DTLS

bail:
	return ret;
}

smcp_status_t
smcp_plat_update_fdsets(
	smcp_t self,
	fd_set *read_fd_set,
	fd_set *write_fd_set,
	fd_set *error_fd_set,
	int *fd_count,
	smcp_cms_t *timeout
) {
	smcp_status_t ret = SMCP_STATUS_OK;

	if (self->plat.fd_udp > 0) {
		if (read_fd_set) {
			FD_SET(self->plat.fd_udp, read_fd_set);
		}

		if (error_fd_set) {
			FD_SET(self->plat.fd_udp, error_fd_set);
		}

		if (fd_count && (*fd_count <= self->plat.fd_udp)) {
			*fd_count = self->plat.fd_udp + 1;
		}
	}

#if SMCP_DTLS
	if (self->plat.fd_dtls > 0) {
		if (read_fd_set) {
			FD_SET(self->plat.fd_dtls, read_fd_set);
		}

		if (error_fd_set) {
			FD_SET(self->plat.fd_dtls, error_fd_set);
		}

		if (fd_count && (*fd_count <= self->plat.fd_dtls)) {
			*fd_count = self->plat.fd_dtls + 1;
		}
	}
#endif

	if (timeout) {
		smcp_cms_t tmp = smcp_get_timeout(self);

		if (tmp <= *timeout) {
			*timeout = tmp;
		}
	}

	return ret;
}


static ssize_t
sendtofrom(
	int fd,
	const void *data, size_t len, int flags,
	const struct sockaddr * saddr_to, socklen_t socklen_to,
	const struct sockaddr * saddr_from, socklen_t socklen_from
)
{
	ssize_t ret = -1;
	if ((socklen_from == 0)
		|| (saddr_from == NULL)
		|| (saddr_from->sa_family != saddr_to->sa_family)
	) {
		ret = sendto(
			fd,
			data,
			len,
			0,
			(struct sockaddr *)saddr_to,
			socklen_to
		);
		check(ret>0);
	} else {
		struct iovec iov = { (void *)data, len };
		uint8_t cmbuf[CMSG_SPACE(sizeof (struct in6_pktinfo))];
		struct cmsghdr *scmsgp;
		struct msghdr msg = {
			.msg_name = (void*)saddr_to,
			.msg_namelen = socklen_to,
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = cmbuf,
			.msg_controllen = sizeof(cmbuf),
		};

#if defined(AF_INET6)
		if (saddr_to->sa_family == AF_INET6) {
			struct in6_pktinfo *pktinfo;
			scmsgp = CMSG_FIRSTHDR(&msg);
			scmsgp->cmsg_level = IPPROTO_IPV6;
			scmsgp->cmsg_type = IPV6_PKTINFO;
			scmsgp->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
			pktinfo = (struct in6_pktinfo *)(CMSG_DATA(scmsgp));

			pktinfo->ipi6_addr = ((struct sockaddr_in6*)saddr_from)->sin6_addr;
			pktinfo->ipi6_ifindex = ((struct sockaddr_in6*)saddr_from)->sin6_scope_id;
		} else
#endif

		if (saddr_to->sa_family == AF_INET) {
			struct in_pktinfo *pktinfo;
			scmsgp = CMSG_FIRSTHDR(&msg);
			scmsgp->cmsg_level = IPPROTO_IP;
			scmsgp->cmsg_type = IP_PKTINFO;
			scmsgp->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
			pktinfo = (struct in_pktinfo *)(CMSG_DATA(scmsgp));

			pktinfo->ipi_spec_dst = ((struct sockaddr_in*)saddr_to)->sin_addr;
			pktinfo->ipi_addr = ((struct sockaddr_in*)saddr_from)->sin_addr;
			pktinfo->ipi_ifindex = 0;
		}

		ret = sendmsg(fd, &msg, flags);

		check(ret > 0);
		check_string(ret >= 0, strerror(errno));
	}

	return ret;
}

void
smcp_plat_set_remote_sockaddr(const smcp_sockaddr_t* addr)
{
	smcp_t const self = smcp_get_current_instance();

	if (addr) {
		self->plat.sockaddr_remote = *addr;
	} else {
		memset(&self->plat.sockaddr_remote,0,sizeof(self->plat.sockaddr_remote));
	}
}

void
smcp_plat_set_local_sockaddr(const smcp_sockaddr_t* addr)
{
	smcp_t const self = smcp_get_current_instance();

	if (addr) {
		self->plat.sockaddr_local = *addr;
	} else {
		memset(&self->plat.sockaddr_local,0,sizeof(self->plat.sockaddr_local));
	}
}

void
smcp_plat_set_session_type(smcp_session_type_t type)
{
	smcp_t const self = smcp_get_current_instance();

	self->plat.session_type = type;
}


const smcp_sockaddr_t*
smcp_plat_get_remote_sockaddr(void)
{
	return &smcp_get_current_instance()->plat.sockaddr_remote;
}

const smcp_sockaddr_t*
smcp_plat_get_local_sockaddr(void)
{
	return &smcp_get_current_instance()->plat.sockaddr_local;
}

smcp_session_type_t
smcp_plat_get_session_type(void)
{
	return smcp_get_current_instance()->plat.session_type;
}

smcp_status_t
smcp_plat_outbound_start(smcp_t self, uint8_t** data_ptr, coap_size_t *data_len)
{
	SMCP_EMBEDDED_SELF_HOOK;
	if (data_ptr) {
		*data_ptr = (uint8_t*)self->plat.outbound_packet_bytes;
	}
	if (data_len) {
		*data_len = sizeof(self->plat.outbound_packet_bytes);
	}
	self->outbound.packet = (struct coap_header_s*)self->plat.outbound_packet_bytes;
	return SMCP_STATUS_OK;
}


smcp_status_t
smcp_plat_outbound_finish(smcp_t self,const uint8_t* data_ptr, coap_size_t data_len, int flags)
{
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = SMCP_STATUS_FAILURE;
	ssize_t sent_bytes = -1;
	const int fd = smcp_get_current_instance()->plat.fd_udp;

	assert(fd >= 0);

	require(data_len > 0, bail);

#if VERBOSE_DEBUG
	{
		char addr_str[50] = "???";
		uint16_t port = ntohs(smcp_plat_get_remote_sockaddr()->smcp_port);
		SMCP_ADDR_NTOP(addr_str,sizeof(addr_str),&smcp_plat_get_remote_sockaddr()->smcp_addr);
		DEBUG_PRINTF("smcp(%p): Outbound packet to [%s]:%d", self,addr_str,(int)port);
		coap_dump_header(
			SMCP_DEBUG_OUT_FILE,
			"Outbound:\t",
			(struct coap_header_s*)data_ptr,
			(coap_size_t)data_len
		);
	}
#endif

	sent_bytes = sendtofrom(
		fd,
		data_ptr,
		data_len,
		0,
		(struct sockaddr *)smcp_plat_get_remote_sockaddr(),
		sizeof(smcp_sockaddr_t),
		(struct sockaddr *)smcp_plat_get_local_sockaddr(),
		sizeof(smcp_sockaddr_t)
	);

	require_action_string(
		(sent_bytes >= 0),
		bail, ret = SMCP_STATUS_ERRNO, strerror(errno)
	);

	require_action_string(
		(sent_bytes == data_len),
		bail, ret = SMCP_STATUS_FAILURE, "sendto() returned less than len"
	);

	ret = SMCP_STATUS_OK;
bail:
	return ret;
}

// MARK: -

smcp_status_t
smcp_plat_wait(
	smcp_t self, smcp_cms_t cms
) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = SMCP_STATUS_OK;
	struct pollfd pollee = { self->plat.fd_udp, POLLIN | POLLHUP, 0 };
	int descriptors_ready;

	if(cms >= 0) {
		cms = MIN(cms, smcp_get_timeout(self));
	} else {
		cms = smcp_get_timeout(self);
	}

	errno = 0;

	descriptors_ready = poll(&pollee, 1, cms);

	// Ensure that poll did not fail with an error.
	require_action_string(descriptors_ready != -1,
		bail,
		ret = SMCP_STATUS_ERRNO,
		strerror(errno)
	);

	if (descriptors_ready == 0) {
		ret = SMCP_STATUS_TIMEOUT;
	}

bail:
	return ret;
}

smcp_status_t
smcp_plat_process(
	smcp_t self
) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = 0;

	int tmp;
	struct pollfd polls[2];
	int poll_count;

	poll_count = smcp_plat_update_pollfds(self, polls, sizeof(polls)/sizeof(polls[0]));

	errno = 0;

	tmp = poll(polls, poll_count, 0);

	// Ensure that poll did not fail with an error.
	require_action_string(
		errno == 0,
		bail,
		ret = SMCP_STATUS_ERRNO,
		strerror(errno)
	);

	if(tmp > 0) {
		for (tmp = 0; tmp < poll_count; tmp++) {
			if (!polls[tmp].revents) {
				continue;
			} else {
				char packet[SMCP_MAX_PACKET_LENGTH+1];
				smcp_sockaddr_t remote_saddr = {};
				smcp_sockaddr_t local_saddr = {};
				ssize_t packet_len = 0;
				char cmbuf[0x100];
				struct iovec iov = { packet, SMCP_MAX_PACKET_LENGTH };
				struct msghdr msg = {
					.msg_name = &remote_saddr,
					.msg_namelen = sizeof(remote_saddr),
					.msg_iov = &iov,
					.msg_iovlen = 1,
					.msg_control = cmbuf,
					.msg_controllen = sizeof(cmbuf),
				};
				struct cmsghdr *cmsg;

				packet_len = recvmsg(polls[tmp].fd, &msg, 0);

				require_action(packet_len > 0, bail, ret = SMCP_STATUS_ERRNO);

				packet[packet_len] = 0;

				for (
					cmsg = CMSG_FIRSTHDR(&msg);
					cmsg != NULL;
					cmsg = CMSG_NXTHDR(&msg, cmsg)
				) {
					if (cmsg->cmsg_level != SMCP_IPPROTO
						|| cmsg->cmsg_type != SMCP_PKTINFO
					) {
						continue;
					}

					// Preinitialize some of the fields.
					local_saddr = remote_saddr;

#if SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
					struct in6_pktinfo *pi = (struct in6_pktinfo *)CMSG_DATA(cmsg);
					local_saddr.smcp_addr = pi->ipi6_addr;
					local_saddr.sin6_scope_id = pi->ipi6_ifindex;

#elif SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET
					struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
					local_saddr.smcp_addr = pi->ipi_addr;
#endif

					local_saddr.smcp_port = htons(smcp_plat_get_port(self));

					self->plat.pktinfo = *pi;
				}

				smcp_set_current_instance(self);
				smcp_plat_set_remote_sockaddr(&remote_saddr);
				smcp_plat_set_local_sockaddr(&local_saddr);

				if (self->plat.fd_udp == polls[tmp].fd) {
					smcp_plat_set_session_type(SMCP_SESSION_TYPE_UDP);

					ret = smcp_inbound_packet_process(self, packet, (coap_size_t)packet_len, 0);
					require_noerr(ret, bail);

#if SMCP_DTLS
				} else if (self->plat.fd_dtls == polls[tmp].fd) {
					// TODO: Feed it into dtls, see if anything pops out.

#endif
				}
			}
		}
	}

	smcp_handle_timers(self);

bail:
	smcp_set_current_instance(NULL);
	self->is_responding = false;
	return ret;
}

smcp_status_t
smcp_plat_lookup_hostname(const char* hostname, smcp_sockaddr_t* saddr)
{
	smcp_status_t ret;
	struct addrinfo hint = {
		.ai_flags		= AI_ADDRCONFIG,
		.ai_family		= AF_UNSPEC,
	};

	struct addrinfo *results = NULL;
	struct addrinfo *iter = NULL;

	memset(saddr, 0, sizeof(*saddr));
	saddr->___smcp_family = SMCP_BSD_SOCKETS_NET_FAMILY;

#if SOCKADDR_HAS_LENGTH_FIELD
	saddr->___smcp_len = sizeof(*saddr);
#endif

	int error = getaddrinfo(hostname, NULL, &hint, &results);

#if SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
	if(error && (inet_addr(hostname) != INADDR_NONE)) {
		char addr_v4mapped_str[8 + strlen(hostname)];
		hint.ai_family = AF_INET6;
		hint.ai_flags = AI_ALL | AI_V4MAPPED,
		strcpy(addr_v4mapped_str,"::ffff:");
		strcat(addr_v4mapped_str,hostname);
		error = getaddrinfo(addr_v4mapped_str,
			NULL,
			&hint,
			&results
		);
	}
#endif

	if (EAI_AGAIN == error) {
		ret = SMCP_STATUS_WAIT_FOR_DNS;
		goto bail;
	}

#ifdef TM_EWOULDBLOCK
	if (TM_EWOULDBLOCK == error) {
		ret = SMCP_STATUS_WAIT_FOR_DNS;
		goto bail;
	}
#endif

	require_action_string(
		!error,
		bail,
		ret = SMCP_STATUS_HOST_LOOKUP_FAILURE,
		gai_strerror(error)
	);

	// Move to the first recognized result
	for(iter = results;iter && (iter->ai_family!=AF_INET6 && iter->ai_family!=AF_INET);iter=iter->ai_next);

	require_action(
		iter,
		bail,
		ret = SMCP_STATUS_HOST_LOOKUP_FAILURE
	);

#if SMCP_BSD_SOCKETS_NET_FAMILY==AF_INET6
	if(iter->ai_family == AF_INET) {
		struct sockaddr_in *v4addr = (void*)iter->ai_addr;
		saddr->sin6_addr.s6_addr[10] = 0xFF;
		saddr->sin6_addr.s6_addr[11] = 0xFF;
		memcpy(&saddr->sin6_addr.s6_addr[12], &v4addr->sin_addr.s_addr, 4);
	} else
#endif
	if(iter->ai_family == SMCP_BSD_SOCKETS_NET_FAMILY) {
		memcpy(saddr, iter->ai_addr, iter->ai_addrlen);
	}

	if(SMCP_IS_ADDR_MULTICAST(&saddr->smcp_addr)) {
		smcp_t const self = smcp_get_current_instance();
		check(self->outbound.packet->tt != COAP_TRANS_TYPE_CONFIRMABLE);
		if(self->outbound.packet->tt == COAP_TRANS_TYPE_CONFIRMABLE) {
			self->outbound.packet->tt = COAP_TRANS_TYPE_NONCONFIRMABLE;
		}
	}

	ret = SMCP_STATUS_OK;

bail:
	if(results)
		freeaddrinfo(results);
	return ret;
}


#endif // #if SMCP_USE_BSD_SOCKETS
