#include <stdlib.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <math.h>

#include "h2d_main.h"

enum {
	H2D_MODULE_FILTER_PROCESS_HEADERS = 0,
	H2D_MODULE_FILTER_PROCESS_BODY,
	H2D_MODULE_FILTER_RESPONSE_HEADERS,
	H2D_MODULE_FILTER_RESPONSE_BODY,
	H2D_MODULE_FILTER_NUM,
};
struct h2d_module_filters {
	double			ranks[H2D_MODULE_FILTER_NUM][H2D_MODULE_MAX];
	struct h2d_module	*modules[H2D_MODULE_FILTER_NUM][H2D_MODULE_MAX];
};


/* only used in worker process */
int h2d_module_number;

#define X(m) extern struct h2d_module m;
H2D_MODULE_X_LIST
#undef X
static struct h2d_module *h2d_module_statics[] =
{
	#define X(m) &m,
	H2D_MODULE_X_LIST
	#undef X
};

/* iteration on static and dynamic modules */
struct h2d_module *h2d_module_next(struct h2d_module *m)
{
	if (m == NULL) {
		return h2d_module_statics[0];
	}
	int next = m->index + 1;
	if (next < H2D_MODULE_STATIC_NUMBER) {
		return h2d_module_statics[next];
	}
	if (h2d_conf_runtime == NULL || h2d_conf_runtime->dynamic_modules == NULL) {
		return NULL;
	}
	return h2d_conf_runtime->dynamic_modules[next - H2D_MODULE_STATIC_NUMBER].sym;
}

#define H2D_MODULE_TO_MEMBER(m, offset) (void *)((char *)m + offset)
#define H2D_MODULE_FROM_MEMBER(m, offset) (void *)((char *)m - offset)
static struct wuy_cflua_command *h2d_module_next_command(
		struct wuy_cflua_command *cmd, off_t offset)
{
	struct h2d_module *m = NULL;
	if (cmd->type != WUY_CFLUA_TYPE_END || cmd->u.next == NULL) {
		m = H2D_MODULE_FROM_MEMBER(cmd, offset);
	}

	while ((m = h2d_module_next(m)) != NULL) {
		struct wuy_cflua_command *next = H2D_MODULE_TO_MEMBER(m, offset);
		if (next->type != WUY_CFLUA_TYPE_END) {
			return next;
		}
	}
	return NULL;
}
struct wuy_cflua_command *h2d_module_next_listen_command(struct wuy_cflua_command *cmd)
{
	return h2d_module_next_command(cmd, offsetof(struct h2d_module, command_listen));
}
struct wuy_cflua_command *h2d_module_next_host_command(struct wuy_cflua_command *cmd)
{
	return h2d_module_next_command(cmd, offsetof(struct h2d_module, command_host));
}
struct wuy_cflua_command *h2d_module_next_path_command(struct wuy_cflua_command *cmd)
{
	return h2d_module_next_command(cmd, offsetof(struct h2d_module, command_path));
}

static void h2d_module_stats(void **confs, wuy_json_t *json, off_t offset)
{
	struct h2d_module *m = NULL;
	while ((m = h2d_module_next(m)) != NULL) {
		void (*stats)(void *, wuy_json_t *) = *(void **)H2D_MODULE_TO_MEMBER(m, offset);
		if (stats != NULL && confs[m->index] != NULL) {
			stats(confs[m->index], json);
		}
	}
}
void h2d_module_stats_listen(struct h2d_conf_listen *conf_listen, wuy_json_t *json)
{
	h2d_module_stats(conf_listen->module_confs, json, offsetof(struct h2d_module, stats_listen));
}
void h2d_module_stats_host(struct h2d_conf_host *conf_host, wuy_json_t *json)
{
	h2d_module_stats(conf_host->module_confs, json, offsetof(struct h2d_module, stats_host));
}
void h2d_module_stats_path(struct h2d_conf_path *conf_path, wuy_json_t *json)
{
	h2d_module_stats(conf_path->module_confs, json, offsetof(struct h2d_module, stats_path));
}

static void h2d_module_fix(struct h2d_module *m, int i)
{
	m->index = i;

	off_t offset = sizeof(void *) * i;
	m->command_listen.offset = offsetof(struct h2d_conf_listen, module_confs) + offset;
	m->command_host.offset = offsetof(struct h2d_conf_host, module_confs) + offset;
	m->command_path.offset = offsetof(struct h2d_conf_path, module_confs) + offset;

	m->command_path.inherit_count_offset = offsetof(struct h2d_conf_path, content_inherit_counts) + sizeof(int) * i;
}

void h2d_module_master_init(void)
{
	/* init static modules only */
	for (int i = 0; i < H2D_MODULE_STATIC_NUMBER; i++) {
		struct h2d_module *m = h2d_module_statics[i];
		h2d_module_fix(m, i);
		if (m->master_init != NULL) {
			m->master_init();
		}
	}
}

void h2d_module_worker_init(void)
{
	h2d_module_number = 0; /* this can be used only after this function */

	struct h2d_module *m = NULL;
	while ((m = h2d_module_next(m)) != NULL) {
		h2d_module_number++;

		if (h2d_module_number > H2D_MODULE_STATIC_NUMBER) {
			/* fix dynamic module again, because it may be changed
			 * because of failure in reloading configuration */
			h2d_module_fix(m, h2d_module_number - 1);
		}

		if (m->worker_init != NULL) {
			m->worker_init();
		}
	}
}

bool h2d_module_command_is_set(struct wuy_cflua_command *cmd, void *conf)
{
	if (cmd->type == WUY_CFLUA_TYPE_END) {
		return false;
	}

	/* simple type, not table */
	if (cmd->type != WUY_CFLUA_TYPE_TABLE) {
		return conf != NULL;
	}

	/* table type */
	if (conf == NULL) {
		return false;
	}

	/* check the first command */
	struct wuy_cflua_command *first = &cmd->u.table->commands[0];
	if (first->name != NULL) {
		/* must be array-member */
		abort();
	}

	void *ptr = (char *)conf + first->offset;

	/* it's multi-value array */
	if (!first->is_single_array) {
		return *(char **)ptr != NULL;
	}

	/* it's single value */
	switch (first->type) {
	case WUY_CFLUA_TYPE_BOOLEAN:
		return *(bool *)ptr;
	case WUY_CFLUA_TYPE_DOUBLE:
		return *(double *)ptr != 0;
	case WUY_CFLUA_TYPE_INTEGER:
		return *(int *)ptr != 0;
	case WUY_CFLUA_TYPE_STRING:
		return *(char **)ptr != NULL;
	case WUY_CFLUA_TYPE_FUNCTION:
		return wuy_cflua_is_function_set(*(wuy_cflua_function_t *)ptr);
	case WUY_CFLUA_TYPE_TABLE:
		return h2d_module_command_is_set(first, *(char **)ptr);
	default:
		abort();
	}
}

void h2d_module_request_ctx_free(struct h2d_request *r)
{
	struct h2d_module *m = NULL;
	while ((m = h2d_module_next(m)) != NULL) {
		if (m->ctx_free != NULL && r->module_ctxs[m->index] != NULL) {
			m->ctx_free(r);
		}
		r->module_ctxs[m->index] = NULL;
	}
}

static int h2d_module_filter_run(struct h2d_request *r, int index)
{
	struct h2d_module **modules = r->conf_path->filters->modules[index];
	while (1) {
		struct h2d_module *m = modules[r->filter_indexs[index]];
		if (m == NULL) {
			return H2D_OK;
		}
		int ret = (*(&m->filters.process_headers + index))(r);
		if (ret != H2D_OK) {
			if (ret > 0) { /* HTTP status_code */
				r->filter_terminal = m;
			}
			return ret;
		}
		r->filter_indexs[index]++;
	}
}
int h2d_module_filter_process_headers(struct h2d_request *r)
{
	return h2d_module_filter_run(r, H2D_MODULE_FILTER_PROCESS_HEADERS);
}
int h2d_module_filter_process_body(struct h2d_request *r)
{
	return h2d_module_filter_run(r, H2D_MODULE_FILTER_PROCESS_BODY);
}
int h2d_module_filter_response_headers(struct h2d_request *r)
{
	return h2d_module_filter_run(r, H2D_MODULE_FILTER_RESPONSE_HEADERS);
}
int h2d_module_filter_response_body(struct h2d_request *r, uint8_t *data,
		int data_len, int buf_len, bool *p_is_last)
{
	struct h2d_module **modules = r->conf_path->filters->modules[H2D_MODULE_FILTER_RESPONSE_BODY];

	int i = 0;
	while (1) {
		struct h2d_module *m = modules[i++];
		if (m == NULL) {
			break;
		}
		data_len = m->filters.response_body(r, data, data_len, buf_len, p_is_last);
		if (data_len < 0) {
			return data_len;
		}
	}
	return data_len;
}


/* configuration */

static int h2d_module_filters_rank_index;
static double *h2d_module_filters_ranks;
static double h2d_module_filter_get_rank(const struct h2d_module *m)
{
	double rank = h2d_module_filters_ranks[m->index];
	if (!isnan(rank)) {
		return rank;
	}
	return m->filters.ranks[h2d_module_filters_rank_index];
}
static int h2d_module_filter_cmp(const void *a, const void *b)
{
	const struct h2d_module *ma = *(const struct h2d_module **)a;
	const struct h2d_module *mb = *(const struct h2d_module **)b;
	double ranka = h2d_module_filter_get_rank(ma);
	double rankb = h2d_module_filter_get_rank(mb);
	if (ranka == rankb) {
		return ma->index - mb->index;
	} else {
		return ranka < rankb ? -1 : 1;
	}
}

static const char *h2d_module_filters_arbitrary(lua_State *L, void *data)
{
	struct h2d_module_filters *conf = data;

	if (!lua_istable(L, -1)) {
		return "expect array";
	}

	const char *name = lua_tostring(L, -2);

	struct h2d_module *m = NULL;
	while ((m = h2d_module_next(m)) != NULL) {
		if (strcmp(m->name, name) == 0) {
			break;
		}
	}
	if (m == NULL) {
		return "unknown module name";
	}

	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		int index = lua_tointeger(L, -2);
		if (index == 0 || index > H2D_MODULE_FILTER_NUM) {
			return "invalid key";
		}
		if (!lua_isnumber(L, -1)) {
			return "invalid rank value";
		}
		conf->ranks[index - 1][m->index] = lua_tonumber(L, -1);
		lua_pop(L, 1);
	}

	return WUY_CFLUA_OK;
}
static void h2d_module_filters_init(void *data)
{
	struct h2d_module_filters *conf = data;

	for (int i = 0; i < H2D_MODULE_FILTER_NUM; i++) {
		int j = 0;
		struct h2d_module *m = NULL;
		while ((m = h2d_module_next(m)) != NULL) {
			conf->ranks[i][j++] = NAN;
		}
	}
}
static const char *h2d_module_filters_post(void *data)
{
	struct h2d_module_filters *conf = data;

	int counts[H2D_MODULE_FILTER_NUM] = {0};
	struct h2d_module *m = NULL;
	while ((m = h2d_module_next(m)) != NULL) {
		if (m->filters.process_headers) {
			int j = H2D_MODULE_FILTER_PROCESS_HEADERS;
			conf->modules[j][counts[j]++] = m;
		}
		if (m->filters.process_body) {
			int j = H2D_MODULE_FILTER_PROCESS_BODY;
			conf->modules[j][counts[j]++] = m;
		}
		if (m->filters.response_headers) {
			int j = H2D_MODULE_FILTER_RESPONSE_HEADERS;
			conf->modules[j][counts[j]++] = m;
		}
		if (m->filters.response_body) {
			int j = H2D_MODULE_FILTER_RESPONSE_BODY;
			conf->modules[j][counts[j]++] = m;
		}
	}

	for (int i = 0; i < H2D_MODULE_FILTER_NUM; i++) {
		h2d_module_filters_rank_index = i;
		h2d_module_filters_ranks = conf->ranks[i];
		qsort(conf->modules[i], counts[i], sizeof(struct h2d_module *), h2d_module_filter_cmp);
	}

	return WUY_CFLUA_OK;
}

struct wuy_cflua_table h2d_module_filters_conf_table = {
	.commands = (struct wuy_cflua_command[1]) { { NULL } },
	.size = sizeof(struct h2d_module_filters),
	.arbitrary = h2d_module_filters_arbitrary,
	.init = h2d_module_filters_init,
	.post = h2d_module_filters_post,
};


static const char *h2d_module_dynamic_load(struct h2d_module_dynamic *d)
{
	h2d_conf_log(H2D_LOG_INFO, "load dynamic module %s", d->filename);

	/* make the module name */
	const char *p = strrchr(d->filename, '/');
	if (p == NULL) {
		p = d->filename;
	} else {
		p++;
	}

	int len = strlen(p);
	if (memcmp(p, "h2d_", 4) != 0 || memcmp(p + len - 3, ".so", 3) != 0) {
		return "invalid dynamic module filename format; must be 'h2d_*.so'";
	}

	char mod_name[len + sizeof("_module") - 3];
	memcpy(mod_name, p, len - 3);
	memcpy(mod_name + len - 3, "_module", 7);
	mod_name[len + 7 - 3] = '\0';

	/* load */
	d->dl_handle = dlopen(d->filename, RTLD_NOW);
	if (d->dl_handle == NULL) {
		return "fail in open dynamic module";
	}
	d->sym = dlsym(d->dl_handle, mod_name);
	if (d->sym == NULL) {
		return "fail in load dynamic module";
	}

	return WUY_CFLUA_OK;
}

static const char *h2d_module_dynamic_post(void *data)
{
	struct h2d_module_dynamic *modules = *(struct h2d_module_dynamic **)data;
	if (modules == NULL) {
		return WUY_CFLUA_OK;
	}

	for (struct h2d_module_dynamic *d = modules; d->filename != NULL; d++) {
		const char *err = h2d_module_dynamic_load(d);
		if (err != WUY_CFLUA_OK) {
			wuy_cflua_post_arg = d->filename;
			return err;
		}

		struct h2d_module *m = d->sym;
		h2d_module_fix(m, H2D_MODULE_STATIC_NUMBER + (d - modules));

		if (m->master_init != NULL) {
			m->master_init();
			m->master_init = NULL;
		}
	}

	return WUY_CFLUA_OK;
}

static const char *h2d_module_dynamic_upstream_post(void *data)
{
	struct h2d_module_dynamic *modules = *(struct h2d_module_dynamic **)data;
	if (modules == NULL) {
		return WUY_CFLUA_OK;
	}

	for (struct h2d_module_dynamic *d = modules; d->filename != NULL; d++) {
		const char *err = h2d_module_dynamic_load(d);
		if (err != WUY_CFLUA_OK) {
			wuy_cflua_post_arg = d->filename;
			return err;
		}

		h2d_upstream_dynamic_module_fix(d->sym, d - modules);
	}

	return WUY_CFLUA_OK;
}

static void h2d_module_dynamic_free(void *data)
{
	struct h2d_module_dynamic *modules = *(struct h2d_module_dynamic **)data;
	if (modules == NULL) {
		return;
	}

	for (struct h2d_module_dynamic *d = modules; d->filename != NULL; d++) {
		dlclose(d->dl_handle);
	}
}

static struct wuy_cflua_command h2d_module_dynamic_commands[] = {
	{	.type = WUY_CFLUA_TYPE_STRING,
		.offset = 0,
		.array_member_size = sizeof(struct h2d_module_dynamic),
	},
	{ NULL }
};
struct wuy_cflua_table h2d_module_dynamic_table = {
	.commands = h2d_module_dynamic_commands,
	.post = h2d_module_dynamic_post,
	.free = h2d_module_dynamic_free,
};

struct wuy_cflua_table h2d_module_dynamic_upstream_table = {
	.commands = h2d_module_dynamic_commands,
	.post = h2d_module_dynamic_upstream_post,
	.free = h2d_module_dynamic_free,
};
