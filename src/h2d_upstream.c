#include "h2d_main.h"

/* for getaddrinfo() */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

// TODO each upstream should has one ssl-ctx, for different ssl configs
static SSL_CTX *h2d_upstream_ssl_ctx;

static void h2d_upstream_address_free(struct h2d_upstream_address *address)
{
	if (wuy_list_empty(&address->active_head) && wuy_list_empty(&address->idle_head)) {
		free(address);
	}
}

static void h2d_upstream_connection_close(struct h2d_upstream_connection *upc)
{
	loop_stream_close(upc->loop_stream);
	upc->loop_stream = NULL;

	wuy_list_delete(&upc->list_node);

	struct h2d_upstream_address *address = upc->address;
	if (upc->request == NULL) {
		address->idle_num--;
	}

	free(upc->preread_buf);
	free(upc);

	/* free address if it was deleted */
	if (address->deleted) {
		h2d_upstream_address_free(address);
	}
}

static void h2d_upstream_address_delete(struct h2d_upstream_address *address)
{
	wuy_list_delete(&address->hostname_node);
	wuy_list_delete(&address->upstream_node);
	h2d_upstream_address_free(address);
}

static void h2d_upstream_do_resolve(struct h2d_upstream_conf *upstream)
{
	/* pick next hostname */
	char *name = NULL;
	while (1) {
		struct h2d_upstream_hostname *hostname = &upstream->hostnames[upstream->resolve_index++];
		if (hostname->name != NULL || hostname->need_resolved) {
			name = hostname->name;
			break;
		}
	}

	/* finish resolve all hostnames */
	if (name == NULL) {
		upstream->resolve_last = time(NULL);
		upstream->resolve_index = 0;
		if (!upstream->resolve_updated) {
			return;
		}

		/* clear deleted addresses, just before loadbalance->update() */
		upstream->address_num = 0;
		struct h2d_upstream_address *address, *safe;
		wuy_list_iter_safe_type(&upstream->address_head, address, safe, upstream_node) {
			if (address->deleted) {
				h2d_upstream_address_delete(address);
			} else {
				upstream->address_num++;
			}
		}

		if (upstream->address_num == 0) {
			printf("!!! no address. no update\n");
			return;
		}

		upstream->loadbalance->update(upstream);
		upstream->resolve_updated = false;
		return;
	}

	/* send resolve query */
	struct h2d_resolver_query query;
	int name_len = strlen(name);
	assert(name_len < sizeof(query.hostname));
	memcpy(query.hostname, name, name_len);
	query.expire_after = upstream->resolve_interval;
	loop_stream_write(upstream->resolve_stream, &query,
			sizeof(query.expire_after) + name_len);
}

static void h2d_upstream_address_add(struct h2d_upstream_conf *upstream,
		struct h2d_upstream_hostname *hostname, struct sockaddr *sockaddr,
		struct h2d_upstream_address *before)
{
	struct h2d_upstream_address *address = calloc(1, sizeof(struct h2d_upstream_address));

	switch (sockaddr->sa_family) {
	case AF_INET:
		address->sockaddr.sin = *((struct sockaddr_in *)sockaddr);
		if (hostname->port != 0) {
			address->sockaddr.sin.sin_port = htons(hostname->port);
		}
		break;

	case AF_INET6:
		address->sockaddr.sin6 = *((struct sockaddr_in6 *)sockaddr);
		if (hostname->port != 0) {
			address->sockaddr.sin6.sin6_port = htons(hostname->port);
		}
		break;

	case AF_UNIX:
		address->sockaddr.sun = *((struct sockaddr_un *)sockaddr);
		break;

	default:
		printf("sa_family: %d\n", sockaddr->sa_family);
		abort();
	}

	wuy_list_init(&address->idle_head);
	wuy_list_init(&address->active_head);
	address->upstream = upstream;

	if (before != NULL) {
		wuy_list_add_before(&before->upstream_node, &address->upstream_node);
		wuy_list_add_before(&before->hostname_node, &address->hostname_node);
	} else {
		wuy_list_append(&upstream->address_head, &address->upstream_node);
		wuy_list_append(&hostname->address_head, &address->hostname_node);
	}
}

static int h2d_upstream_resolve_on_read(loop_stream_t *s, void *data, int len)
{
	struct h2d_upstream_conf *upstream = loop_stream_get_app_data(s);
	struct h2d_upstream_hostname *hostname = &upstream->hostnames[upstream->resolve_index-1];

	/* diff */
	uint8_t *p = data;
	uint8_t *end = p + len;
	wuy_list_node_t *node = wuy_list_first(&hostname->address_head);
	while (p < end && node != NULL) {
		struct h2d_upstream_address *address = wuy_containerof(node,
				struct h2d_upstream_address, hostname_node);

		struct sockaddr *newaddr = (struct sockaddr *)p;

		/* compare in the same way with h2d_resolver_addrcmp() */
		int cmp = wuy_sockaddr_addrcmp(newaddr, &address->sockaddr.s);
		if (cmp == 0) {
			p += wuy_sockaddr_size(newaddr);
			node = wuy_list_next(&hostname->address_head, node);
			continue;
		}

		if (cmp < 0) { /* new address */
			h2d_upstream_address_add(upstream, hostname, newaddr, address);
			p += wuy_sockaddr_size(newaddr);

		} else {
			/* delete address.
			 * We can not free the address now because it is used
			 * by loadbalance. We just mark it here and free it
			 * just before loadbalance->update(). */
			address->deleted = true;
			node = wuy_list_next(&hostname->address_head, node);
		}

		upstream->resolve_updated = true;
	}
	while (node != NULL) {
		struct h2d_upstream_address *address = wuy_containerof(node,
				struct h2d_upstream_address, hostname_node);
		node = wuy_list_next(&hostname->address_head, node);
		address->deleted = true;
		upstream->resolve_updated = true;
	}
	while (p < end) {
		struct sockaddr *newaddr = (struct sockaddr *)p;
		h2d_upstream_address_add(upstream, hostname, newaddr, NULL);
		p += wuy_sockaddr_size(newaddr);
		upstream->resolve_updated = true;
	}

	/* resolve next hostname */
	h2d_upstream_do_resolve(upstream);

	return len;
}
static loop_stream_ops_t h2d_upstream_resolve_ops = {
	.on_read = h2d_upstream_resolve_on_read,
};

static void h2d_upstream_try_resolve(struct h2d_upstream_conf *upstream)
{
	if (upstream->resolve_last == 0) {
		/* all static addresses, no need resolve */
		return;
	}

	if (upstream->resolve_stream == NULL) {
		/* initialize the loop_stream.
		 * We can not initialize this at h2d_upstream_conf_post() because
		 * it is called in master process while we need the worker. */
		int fd = h2d_resolver_connect();
		upstream->resolve_stream = loop_stream_new(h2d_loop, fd, &h2d_upstream_resolve_ops, false);
		loop_stream_set_app_data(upstream->resolve_stream, upstream);
	}

	if (time(NULL) - upstream->resolve_last < upstream->resolve_interval) {
		return;
	}
	if (upstream->resolve_index != 0) { /* in processing already */
		return;
	}

	/* begin the resolve */
	h2d_upstream_do_resolve(upstream);
}


static void h2d_upstream_on_active(loop_stream_t *s)
{
	/* Explicit handshake is not required here because the following
	 * routine will call SSL_read/SSL_write to do the handshake.
	 * We handshake here just to avoid calling the following
	 * routine during handshake for performence. So we handle
	 * H2D_AGAIN only, but not H2D_ERROR. */
	if (h2d_ssl_stream_handshake(s) == H2D_AGAIN) {
		return;
	}

	struct h2d_upstream_connection *upc = loop_stream_get_app_data(s);
	if (upc->request != NULL) {
		h2d_request_active(upc->request);
	} else { /* idle */
		h2d_upstream_connection_close(upc);
	}
}
static loop_stream_ops_t h2d_upstream_ops = {
	.on_readable = h2d_upstream_on_active,
	.on_writable = h2d_upstream_on_active,

	H2D_SSL_LOOP_STREAM_UNDERLYINGS,
};


struct h2d_upstream_connection *
h2d_upstream_get_connection(struct h2d_upstream_conf *upstream, struct h2d_request *r)
{
	atomic_fetch_add(&upstream->stats->total, 1);

	h2d_upstream_try_resolve(upstream);

	struct h2d_upstream_address *address = upstream->loadbalance->pick(upstream, r);
	if (address == NULL) {
		printf("upstream pick fail\n");
		atomic_fetch_add(&upstream->stats->pick_fail, 1);
		return NULL;
	}

	if (h2d_upstream_address_is_down(address)) {
		printf("upstream all down!\n");
		struct h2d_upstream_address *iaddr;
		wuy_list_iter_type(&upstream->address_head, iaddr, upstream_node) {
			iaddr->fails = 0;
		}
	}

	char tmpbuf[100];
	wuy_sockaddr_ntop(&address->sockaddr.s, tmpbuf, sizeof(tmpbuf));
	printf("pick %s\n", tmpbuf);

	struct h2d_upstream_connection *upc;
	if (wuy_list_pop_type(&address->idle_head, upc, list_node)) {
		wuy_list_append(&address->active_head, &upc->list_node);
		address->idle_num--;
		atomic_fetch_add(&upstream->stats->reuse, 1);
		upc->request = r;
		return upc;
	}

	errno = 0;
	int fd = wuy_tcp_connect(&address->sockaddr.s);
	if (fd < 0) {
		perror("upstream connect");
		return NULL;
	}

	loop_stream_t *s = loop_stream_new(h2d_loop, fd, &h2d_upstream_ops, errno == EINPROGRESS);
	if (s == NULL) {
		return NULL;
	}

	if (upstream->ssl_enable) {
		h2d_ssl_stream_set(s, h2d_upstream_ssl_ctx, false);
	}

	upc = calloc(1, sizeof(struct h2d_upstream_connection));
	upc->address = address;
	upc->loop_stream = s;
	wuy_list_append(&address->active_head, &upc->list_node);
	loop_stream_set_app_data(s, upc);

	upc->request = r;
	return upc;
}

struct h2d_upstream_connection *
h2d_upstream_retry_connection(struct h2d_upstream_connection *old)
{
	struct h2d_request *r = old->request;
	struct h2d_upstream_address *address = old->address;
	struct h2d_upstream_conf *upstream = address->upstream;

	atomic_fetch_add(&upstream->stats->retry, 1);

	/* mark this down temporarily to avoid picked again */
	int fails = address->fails;
	address->fails = upstream->fails;

	/* pick a new connection */
	struct h2d_upstream_connection *newc = h2d_upstream_get_connection(upstream, r);

	/* recover address's fails, if it was not cleared */
	if (address->fails != 0) {
		address->fails = fails;
	}

	/* close the old connection at last, because it would cause
	 * the address freed if address->deleted. */
	h2d_upstream_connection_close(old);

	return newc;
}

void h2d_upstream_release_connection(struct h2d_upstream_connection *upc)
{
	assert(upc->request != NULL);
	assert(upc->loop_stream != NULL);

	struct h2d_upstream_address *address = upc->address;

	/* close the connection */
	if (loop_stream_is_closed(upc->loop_stream) || upc->request->state != H2D_REQUEST_STATE_DONE) {
		h2d_upstream_connection_close(upc);
		return;
	}

	/* put the connection into idle pool */
	if (address->idle_num > address->upstream->idle_max) {
		/* close the oldest one if pool is full */
		struct h2d_upstream_connection *idle;
		wuy_list_first_type(&address->idle_head, idle, list_node);
		assert(idle != NULL);
		h2d_upstream_connection_close(idle);
	}

	upc->request = NULL;
	address->idle_num++;
	wuy_list_delete(&upc->list_node);
	wuy_list_append(&address->idle_head, &upc->list_node);

	// TODO loop_stream_set_keepalive()
}

int h2d_upstream_connection_read(struct h2d_upstream_connection *upc,
		void *buffer, int buf_len)
{
	assert(upc->loop_stream != NULL);
	uint8_t *buf_pos = buffer;

	/* upc->preread_buf was allocated in h2d_upstream_connection_read_notfinish() */
	if (upc->preread_buf != NULL) {
		if (buf_len < upc->preread_len) {
			memcpy(buffer, upc->preread_buf, buf_len);
			upc->preread_len -= buf_len;
			memmove(upc->preread_buf, upc->preread_buf + buf_len, upc->preread_len);
			return buf_len;
		}

		memcpy(buffer, upc->preread_buf, upc->preread_len);
		buf_pos += upc->preread_len;
		buf_len -= upc->preread_len;
		free(upc->preread_buf);
		upc->preread_buf = NULL;
		upc->preread_len = 0;

		if (buf_len == 0) {
			return buf_pos - (uint8_t *)buffer;
		}
	}

	int read_len = loop_stream_read(upc->loop_stream, buf_pos, buf_len);
	if (read_len < 0) {
		printf("upstream read fail %d\n", read_len);
		return H2D_ERROR;
	}

	int ret_len = buf_pos - (uint8_t *)buffer + read_len;
	return ret_len == 0 ? H2D_AGAIN : ret_len;
}
void h2d_upstream_connection_read_notfinish(struct h2d_upstream_connection *upc,
		void *buffer, int buf_len)
{
	if (buf_len == 0) {
		return;
	}
	assert(upc->preread_buf == NULL);
	upc->preread_buf = malloc(buf_len);
	memcpy(upc->preread_buf, buffer, buf_len);
	upc->preread_len = buf_len;
}

/* We assume that the writing would not lead to block here.
 * If @data==NULL, we just check if in connecting. */
int h2d_upstream_connection_write(struct h2d_upstream_connection *upc,
		void *data, int data_len)
{
	assert(upc->loop_stream != NULL);

	int write_len = loop_stream_write(upc->loop_stream, data, data_len);
	if (write_len < 0) {
		printf("upstream write fail %d\n", write_len);
		return H2D_ERROR;
	}
	if (write_len != data_len) { /* blocking happens */
		printf(" !!! upstream write block!!! %d %d\n", write_len, data_len);
		return H2D_ERROR;
	}
	return H2D_OK;
}

void h2d_upstream_init(void)
{
	h2d_upstream_ssl_ctx = h2d_ssl_ctx_new_client();
}


/* configration */

extern struct h2d_upstream_loadbalance h2d_upstream_loadbalance_hash;
extern struct h2d_upstream_loadbalance h2d_upstream_loadbalance_roundrobin;
static struct h2d_upstream_loadbalance *
h2d_upstream_conf_loadbalance_select(struct h2d_upstream_conf *conf)
{
	/* We have 2 loadbalances now, roundrobin and hash.
	 * Use hash if conf->hash is set, otherwise roundrobin. */
	if (!h2d_conf_is_zero_function(conf->hash)) {
		return &h2d_upstream_loadbalance_hash;
	} else {
		return &h2d_upstream_loadbalance_roundrobin;
	}
}

static bool h2d_upstream_conf_post(void *data)
{
	struct h2d_upstream_conf *conf = data;

	if (conf->hostnames == NULL) {
		return true;
	}

	conf->stats = wuy_shmem_alloc(sizeof(struct h2d_upstream_stats));

	wuy_list_init(&conf->address_head);

	bool need_resolved = false;
	for (int i = 0; conf->hostnames[i].name != NULL; i++) {
		struct h2d_upstream_hostname *hostname = &conf->hostnames[i];

		wuy_list_init(&hostname->address_head);

		/* it's static address, no need resolve */
		struct sockaddr sockaddr;
		if (wuy_sockaddr_pton(hostname->name, &sockaddr, conf->default_port)) {
			hostname->need_resolved = false;
			hostname->port = 0;
			h2d_upstream_address_add(conf, hostname, &sockaddr, NULL);
			conf->address_num++;
			continue;
		}

		/* it's hostname, resolve it */
		need_resolved = true;
		hostname->need_resolved = true;
		hostname->port = conf->default_port;

		/* parse the port */
		char *pport = strchr(hostname->name, ':');
		if (pport != NULL) {
			hostname->port = atoi(pport + 1);
			if (hostname->port == 0) {
				printf("invalid port %s\n", hostname->name);
				return false;
			}
			*pport = '\0';
		}

		/* resolve the hostname */
		int length;
		uint8_t *buffer = h2d_resolver_hostname(hostname->name, &length);
		if (buffer == NULL) {
			printf("resolve fail %s\n", hostname->name);
			return false;
		}

		uint8_t *p = buffer;
		while (p < buffer + length) {
			struct sockaddr *sa = (struct sockaddr *)p;
			h2d_upstream_address_add(conf, hostname, sa, NULL);
			p += wuy_sockaddr_size(sa);
			conf->address_num++;
		}

		free(buffer);
	}

	if (conf->address_num == 0) {
		printf("no address for upstream\n");
		return false;
	}

	/* resolve stream */
	if (need_resolved && conf->resolve_interval > 0) {
		conf->resolve_last = time(NULL);
	}

	/* loadbalance */
	conf->loadbalance = h2d_upstream_conf_loadbalance_select(conf);
	if (conf->loadbalance->init != NULL) {
		conf->loadbalance->init(conf);
	}
	conf->loadbalance->update(conf);

	return true;
}

int h2d_upstream_conf_stats(void *data, char *buf, int len)
{
	struct h2d_upstream_conf *conf = data;
	struct h2d_upstream_stats *stats = conf->stats;
	if (stats == NULL) {
		return 0;
	}
	return snprintf(buf, len, "upstream: %d %d %d %d\n",
			atomic_load(&stats->total),
			atomic_load(&stats->reuse),
			atomic_load(&stats->retry),
			atomic_load(&stats->pick_fail));
}

static struct wuy_cflua_command h2d_upstream_conf_commands[] = {
	{	.name = "idle_max",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, idle_max),
	},
	{	.name = "fails",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, fails),
	},
	{	.name = "recv_buffer_size",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, recv_buffer_size),
	},
	{	.name = "send_buffer_size",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, send_buffer_size),
	},
	{	.name = "default_port",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, default_port),
	},
	{	.name = "resolve_interval",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_upstream_conf, resolve_interval),
	},
	{	.name = "ssl_enable",
		.type = WUY_CFLUA_TYPE_BOOLEAN,
		.offset = offsetof(struct h2d_upstream_conf, ssl_enable),
	},

	/* loadbalances */
	{	.name = "hash",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_upstream_conf, hash),
	},
	{ NULL }
};

struct wuy_cflua_table h2d_upstream_conf_table = {
	.commands = h2d_upstream_conf_commands,
	.post = h2d_upstream_conf_post,
};
