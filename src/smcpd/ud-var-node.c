/*	@file ud-var-node.c
**	@brief Unix-Domain Variable Node
**	@author Robert Quattlebaum <darco@deepdarc.com>
**
**	Copyright (C) 2016 Robert Quattlebaum
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

#define DEBUG 1
#define VERBOSE_DEBUG 1

#ifndef ASSERT_MACROS_USE_SYSLOG
#define ASSERT_MACROS_USE_SYSLOG 1
#endif

#include <smcp/assert-macros.h>

#include <stdio.h>
#include <smcp/smcp.h>
#include <smcp/smcp-node-router.h>

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "ud-var-node.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <smcp/fasthash.h>

#ifndef UD_VAR_NODE_MAX_REQUESTS
#define UD_VAR_NODE_MAX_REQUESTS		(20)
#endif

struct ud_var_node_s {
	struct smcp_node_s node;
	struct smcp_observable_s observable;
	int fd;
	const char* path;
	uint32_t last_etag;
	smcp_timestamp_t next_refresh;
	smcp_cms_t refresh_period;
	smcp_timestamp_t next_poll;
	smcp_cms_t poll_period;
};

coap_ssize_t
ud_var_node_get_content(
	ud_var_node_t self,
	char* buffer_ptr,
	coap_size_t buffer_len
) {
	coap_ssize_t ret = 0;
	ssize_t lseek_ret;

	lseek_ret = lseek(self->fd, 0, SEEK_SET);

	check_string(lseek_ret >= 0, strerror(errno));

	ret = read(self->fd, buffer_ptr, buffer_len);

	require_string(ret >= 0, bail, strerror(errno));

bail:
	return ret;
}

uint32_t
ud_var_node_calc_etag(
	const char* buffer_ptr,
	coap_size_t buffer_len
) {
	struct fasthash_state_s state;

	fasthash_start(&state, 0x12341234);

	while (buffer_len > 255) {
		fasthash_feed(&state, (const uint8_t*)buffer_ptr, 255);
		buffer_len -= 255;
		buffer_ptr += 255;
	}
	fasthash_feed(&state, (const uint8_t*)buffer_ptr, buffer_len);

	return fasthash_finish_uint32(&state);
}

smcp_status_t
ud_var_node_request_handler(
	ud_var_node_t self
) {
	smcp_status_t ret = SMCP_STATUS_NOT_ALLOWED;
	smcp_method_t method = smcp_inbound_get_code();
	coap_content_type_t accept_type = COAP_CONTENT_TYPE_TEXT_PLAIN;
	coap_content_type_t content_type = COAP_CONTENT_TYPE_TEXT_PLAIN;
	uint8_t buffer[256];
	coap_ssize_t buffer_len = 0;
	int write_fd = -1;
	off_t lseek_ret;
	uint32_t value_etag;

	buffer_len = ud_var_node_get_content(self, (char*)buffer, sizeof(buffer));
	require_action_string(buffer_len >= 0, bail, ret = SMCP_STATUS_ERRNO, strerror(errno));
	value_etag = ud_var_node_calc_etag((const char*)buffer, buffer_len);

	{
		const uint8_t* value;
		coap_size_t value_len;
		coap_option_key_t key;
		while ((key = smcp_inbound_next_option(&value, &value_len)) != COAP_OPTION_INVALID) {
			if (key == COAP_OPTION_ACCEPT) {
				accept_type = coap_decode_uint32(value, value_len);

			} else if (key == COAP_OPTION_CONTENT_TYPE) {
				accept_type = coap_decode_uint32(value, value_len);

			} else {
				require_action(key != COAP_OPTION_URI_PATH, bail, ret = SMCP_STATUS_NOT_FOUND);

				require_action(!COAP_OPTION_IS_CRITICAL(key), bail, ret = SMCP_STATUS_BAD_OPTION);
			}
		}
	}

	// Make sure this is a supported method.
	switch (method) {
	case COAP_METHOD_GET:
	case COAP_METHOD_PUT:
		break;

	case COAP_METHOD_POST:
		// We should not get a request at this point in the state machine.
		if (smcp_inbound_is_dupe()) {
			smcp_outbound_drop();
			ret = SMCP_STATUS_DUPE;
			goto bail;
		}
		break;

	default:
		ret = SMCP_STATUS_NOT_IMPLEMENTED;
		goto bail;
		break;
	}

	switch (accept_type) {
	case SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED:
	case COAP_CONTENT_TYPE_TEXT_PLAIN:
		break;

	default:
		ret = smcp_outbound_quick_response(HTTP_RESULT_CODE_UNSUPPORTED_MEDIA_TYPE, "Unsupported Media Type");
		check_noerr(ret);
		goto bail;
	}

	if (COAP_METHOD_PUT == method) {
		const char* content_ptr = smcp_inbound_get_content_ptr();
		coap_size_t content_len = smcp_inbound_get_content_len();
		ssize_t bytes_written = 0;

		write_fd = open(self->path, O_WRONLY | O_NONBLOCK);

		require_action_string(write_fd >= 0, bail, ret = SMCP_STATUS_ERRNO, strerror(errno));

		bytes_written = write(write_fd, content_ptr, content_len);

		require_action_string(bytes_written >= 0, bail, ret = SMCP_STATUS_ERRNO, strerror(errno));

		// Must close to commit the change
		close(write_fd);
		write_fd = -1;

		// Refresh the value
		buffer_len = ud_var_node_get_content(self, (char*)buffer, sizeof(buffer));
		require_action_string(buffer_len >= 0, bail, ret = SMCP_STATUS_ERRNO, strerror(errno));
		value_etag = ud_var_node_calc_etag((const char*)buffer, buffer_len);
	}

	ret = smcp_outbound_begin_response(COAP_RESULT_205_CONTENT);

	require_noerr(ret, bail);

	ret = smcp_outbound_add_option_uint(COAP_OPTION_ETAG, value_etag);

	require_noerr(ret, bail);

	ret = smcp_observable_update(&self->observable, 0);

	check_noerr_string(ret, smcp_status_to_cstr(ret));

	ret = smcp_outbound_add_option_uint(COAP_OPTION_MAX_AGE, (self->refresh_period)/MSEC_PER_SEC + 1);

	require_noerr(ret, bail);

	ret = smcp_outbound_add_option_uint(
		COAP_OPTION_CONTENT_TYPE,
		accept_type
	);

	require_noerr(ret, bail);

	if (SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED == accept_type) {
		ret = smcp_outbound_append_content("v=", SMCP_CSTR_LEN);
		require_noerr(ret, bail);
	}

	ret = smcp_outbound_append_content((char*)buffer, (coap_size_t)buffer_len);

	require_noerr(ret, bail);

	// Send the response we hae created, passing the return value
	// to our caller.
	ret = smcp_outbound_send();

	require_noerr(ret, bail);

bail:

	if (write_fd >= 0) {
		close(write_fd);
	}

	return ret;
}

void
ud_var_node_dealloc(ud_var_node_t x) {
	close(x->fd);
	free((void*)x->path);
	free(x);
}

ud_var_node_t
ud_var_node_alloc() {
	ud_var_node_t ret = (ud_var_node_t)calloc(sizeof(struct ud_var_node_s), 1);

	ret->node.finalize = (void (*)(smcp_node_t)) &ud_var_node_dealloc;
	return ret;
}

ud_var_node_t
ud_var_node_init(
	ud_var_node_t self,
	smcp_node_t parent,
	const char* name,
	const char* path
) {
	int i;
	int fd = -1;

	require(path != NULL, bail);
	require(path[0] != 0, bail);
	require(name != NULL, bail);

	fd = open(path, O_RDONLY | O_NONBLOCK);

	require(fd >= 0, bail);
	require(self || (self = ud_var_node_alloc()), bail);
	require(smcp_node_init(
			&self->node,
			(void*)parent,
			name
	), bail);

	((smcp_node_t)&self->node)->request_handler = (void*)&ud_var_node_request_handler;

	self->path = strdup(path);
	self->fd = fd;
	self->node.is_observable = 1;

	// TODO: Figure out how to set these from the configuration.
	self->refresh_period = 30*MSEC_PER_SEC;
	self->poll_period = 1*MSEC_PER_SEC;

	fd = -1;

bail:
	if (fd >= 0) {
		close(fd);
	}
	return self;
}

smcp_status_t
ud_var_node_update_fdset(
	ud_var_node_t self,
    fd_set *read_fd_set,
    fd_set *write_fd_set,
    fd_set *error_fd_set,
    int *fd_count,
	smcp_cms_t *timeout
) {
	int i;

	syslog(LOG_DEBUG, "ud_var_node_update_fdset: %d observers", smcp_observable_observer_count(&self->observable, 0));

	if (smcp_observable_observer_count(&self->observable, 0)) {

		if (error_fd_set) {
			FD_SET(self->fd, error_fd_set);
		}

		if (fd_count) {
			*fd_count = MAX(*fd_count, self->fd + 1);
		}

		if (timeout) {
			smcp_cms_t next_refresh = smcp_plat_timestamp_to_cms(self->next_refresh);
			smcp_cms_t next_poll = smcp_plat_timestamp_to_cms(self->next_poll);

			if (next_refresh > self->refresh_period) {
				next_refresh = self->refresh_period;
				self->next_refresh = smcp_plat_cms_to_timestamp(next_refresh);
			}

			if (next_poll > self->poll_period) {
				next_poll = self->poll_period;
				self->next_poll = smcp_plat_cms_to_timestamp(next_poll);
			}

			*timeout = MIN(*timeout, next_refresh);
			*timeout = MIN(*timeout, next_poll);
		}
	}

	return SMCP_STATUS_OK;
}

smcp_status_t
ud_var_node_process(ud_var_node_t self) {
	int i;
	bool trigger_it = false;
	struct timeval tv = {0,0};

	syslog(LOG_DEBUG, "ud_var_node_process: %d observers", smcp_observable_observer_count(&self->observable, 0));

	if (smcp_observable_observer_count(&self->observable, 0)) {
		smcp_cms_t next_refresh = smcp_plat_timestamp_to_cms(self->next_refresh);
		smcp_cms_t next_poll = smcp_plat_timestamp_to_cms(self->next_poll);

		struct pollfd fdset[1];

		if (next_refresh <= 0 || next_refresh > self->refresh_period) {
			self->next_refresh = smcp_plat_cms_to_timestamp(self->refresh_period);
			trigger_it = true;
		}

		fdset[0].fd = self->fd;
		fdset[0].events = POLLPRI|POLLERR;

		if (poll(fdset, 1, 0) >= 0) {
			if ( ((fdset[0].revents & POLLPRI) == POLLPRI)
			  || ((fdset[0].revents & POLLIN)  == POLLERR)
			) {
				trigger_it = true;
			}
		}

		if (next_poll <= 0 || next_poll > self->poll_period) {
			uint8_t buffer[256];
			coap_ssize_t buffer_len = 0;
			uint32_t etag;

			self->next_poll = smcp_plat_cms_to_timestamp(self->poll_period);

			buffer_len = ud_var_node_get_content(self, (char*)buffer, sizeof(buffer));

			check_string(buffer_len >= 0, strerror(errno));

			if (buffer_len >= 0) {
				etag = ud_var_node_calc_etag((const char*)buffer, buffer_len);

				if (etag != self->last_etag) {
					self->last_etag = etag;
					trigger_it = true;
				}
			}
		}

		if (trigger_it) {
			smcp_observable_trigger(
				&self->observable,
				SMCP_OBSERVABLE_BROADCAST_KEY,
				0
			);
		}
	}
	return SMCP_STATUS_OK;
}



smcp_status_t
SMCPD_module__ud_var_node_process(ud_var_node_t self) {
	return ud_var_node_process(self);
}

smcp_status_t
SMCPD_module__ud_var_node_update_fdset(
	ud_var_node_t self,
    fd_set *read_fd_set,
    fd_set *write_fd_set,
    fd_set *error_fd_set,
    int *fd_count,
	smcp_cms_t *timeout
) {
	return ud_var_node_update_fdset(self, read_fd_set, write_fd_set, error_fd_set, fd_count, timeout);
}

ud_var_node_t
SMCPD_module__ud_var_node_init(
	ud_var_node_t	self,
	smcp_node_t			parent,
	const char*			name,
	const char*			cmd
) {
	return ud_var_node_init(self, parent, name, cmd);
}
