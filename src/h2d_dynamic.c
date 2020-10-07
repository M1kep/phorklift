#include "h2d_main.h"

/* Call dynamic->get_name() which returns a name.
 * Return H2D_AGAIN, H2D_ERROR or H2D_OK. */
static int h2d_dynamic_get_name(struct h2d_dynamic_conf *dynamic,
		struct h2d_request *r, const char **p_name)
{
	struct h2d_dynamic_ctx *ctx = r->dynamic_ctx;

	const char *name;
	if (dynamic->is_name_blocking) {
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic get_name() blocking");
		name = h2d_lua_api_call_lstring(r, dynamic->get_name, NULL);

	} else {
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic get_name() non-blocking");

		if (ctx->lth == NULL) {
			ctx->lth = h2d_lua_api_thread_new(dynamic->get_name, r);
		}

		int ret = h2d_lua_api_thread_resume(ctx->lth);
		if (ret != H2D_OK) {
			return ret;
		}

		name = lua_tostring(ctx->lth->L, -1);

		h2d_lua_api_thread_free(ctx->lth);
		ctx->lth = NULL;
	}

	if (name == NULL) {
		return H2D_ERROR;
	}

	*p_name = name;
	return H2D_OK;
}

static void *h2d_dynamic_to_container(struct h2d_dynamic_conf *sub_dyn)
{
	struct h2d_dynamic_conf *dynamic = sub_dyn->father ? sub_dyn->father : sub_dyn;
	return ((char *)sub_dyn) - dynamic->container.offset;
}
static struct h2d_dynamic_conf *h2d_dynamic_from_container(void *container,
		struct h2d_dynamic_conf *dynamic)
{
	return (void *)(((char *)container) + dynamic->container.offset);
}

static void h2d_dynamic_delete(struct h2d_dynamic_conf *sub_dyn, const char *reason)
{
	struct h2d_dynamic_conf *dynamic = sub_dyn->father;

	printf("delete dynamic sub %s for %s\n", sub_dyn->name, reason);

	if (sub_dyn->is_just_holder) {
		h2d_request_active_list(&sub_dyn->holder_wait_head, "dynamic holder");
	}

	loop_timer_delete(sub_dyn->timer);
	wuy_dict_delete(dynamic->sub_dict, sub_dyn);
	dynamic->container.del(h2d_dynamic_to_container(sub_dyn));
}

static int64_t h2d_dynamic_timeout_handler(int64_t at, void *data)
{
	h2d_dynamic_delete(data, "idle timedout");
	return 0;
}
static void h2d_dynamic_timer_new(struct h2d_dynamic_conf *sub_dyn)
{
	sub_dyn->timer = loop_timer_new(h2d_loop, h2d_dynamic_timeout_handler, sub_dyn);
}
static void h2d_dynamic_timer_update(struct h2d_dynamic_conf *sub_dyn)
{
	if (sub_dyn->timer != NULL) {
		loop_timer_set_after(sub_dyn->timer, sub_dyn->idle_timeout * 1000);
	}
}

/* Call dynamic->get_conf(), which accepts 2 arguments (name, last_modify_time),
 * and returns WUY_HTTP_200 (with conf-table), WUY_HTTP_304, WUY_HTTP_404,
 * or WUY_HTTP_500. */
static int h2d_dynamic_get_conf(struct h2d_dynamic_conf *dynamic,
		struct h2d_request *r)
{
	h2d_request_log(r, H2D_LOG_DEBUG, "dynamic get_conf()");

	struct h2d_dynamic_ctx *ctx = r->dynamic_ctx;

	if (ctx->lth == NULL) {
		ctx->lth = h2d_lua_api_thread_new(dynamic->get_conf, r);
		lua_pushstring(ctx->lth->L, ctx->sub_dyn->name);
		lua_pushinteger(ctx->lth->L, ctx->sub_dyn->modify_time);
		h2d_lua_api_thread_set_argn(ctx->lth, 2);
	}

	int ret = h2d_lua_api_thread_resume(ctx->lth);
	if (ret != H2D_OK) {
		return ret;
	}

	/* not conf-table, but WUY_HTTP_200/304/404/500 */
	if (lua_isnumber(ctx->lth->L, -1)) {
		switch (lua_tointeger(ctx->lth->L, 1)) {
		case WUY_HTTP_200:
			lua_pop(ctx->lth->L, 1);
			break;
		case WUY_HTTP_304:
			if (ctx->sub_dyn->is_just_holder) {
				return H2D_ERROR;
			}
			return H2D_OK;
		case WUY_HTTP_404:
			h2d_dynamic_delete(ctx->sub_dyn, "deleted");
			return WUY_HTTP_404;
		default:
			return WUY_HTTP_500;
		}
	}

	/* conf-table */
	if (!lua_istable(ctx->lth->L, -1)) {
		printf("invalid get_conf() return\n");
		return H2D_ERROR;
	}

	lua_xmove(ctx->lth->L, h2d_L, 1);
	h2d_lua_api_thread_free(ctx->lth);
	ctx->lth = NULL;

	/* parse */
	void *container = NULL;
	struct wuy_cflua_table *sub_table = wuy_cflua_copy_table_default(
			dynamic->container.conf_table,
			h2d_dynamic_to_container(dynamic));
	int err = wuy_cflua_parse(h2d_L, sub_table, &container);
	wuy_cflua_free_copied_table(sub_table);

	if (err < 0) {
		printf("parse dynamic sub error: %s\n", wuy_cflua_strerror(h2d_L, err));
		return H2D_ERROR;
	}

	/* replace */
	struct h2d_dynamic_conf *new_sub = h2d_dynamic_from_container(container, dynamic);
	new_sub->name = ctx->sub_dyn->name;
	ctx->sub_dyn->name = NULL;
	new_sub->create_time = ctx->sub_dyn->create_time;
	h2d_dynamic_delete(ctx->sub_dyn, "replaced");

	new_sub->father = dynamic;
	new_sub->modify_time = time(NULL);
	new_sub->check_time = new_sub->modify_time;
	wuy_dict_add(dynamic->sub_dict, new_sub);
	if (new_sub->idle_timeout > 0) {
		h2d_dynamic_timer_new(new_sub);
		h2d_dynamic_timer_update(new_sub);
	}

	ctx->sub_dyn = new_sub;

	return H2D_OK;
}

static bool h2d_dynamic_need_check_conf(struct h2d_dynamic_conf *sub_dyn,
		struct h2d_request *r)
{
	if (sub_dyn->is_just_holder) {
		return false;
	}
	if (sub_dyn->check_interval == 0) {
		return false;
	}
	time_t now = time(NULL);
	if (now - sub_dyn->check_time < sub_dyn->check_interval) {
		return false;
	}
	if (wuy_cflua_is_function_set(sub_dyn->check_filter)) {
		if (h2d_lua_api_call_boolean(r, sub_dyn->check_filter) != 1) {
			return false;
		}
	}
	sub_dyn->check_time = now;
	return true;
}

void *h2d_dynamic_get(struct h2d_dynamic_conf *dynamic, struct h2d_request *r)
{
	h2d_request_log(r, H2D_LOG_DEBUG, "h2d_dynamic_get()");

	struct h2d_dynamic_ctx *ctx = r->dynamic_ctx;
	if (ctx == NULL) {
		ctx = calloc(1, sizeof(struct h2d_dynamic_ctx));
		r->dynamic_ctx = ctx;
	}

	if (ctx->sub_dyn != NULL) { /* in processing already*/
		goto state_get_conf;
	}

	/* get name to ctx->name */
	const char *name;
	int ret = h2d_dynamic_get_name(dynamic, r, &name);
	if (ret != H2D_OK) {
		goto not_ok;
	}

	/* search cache by name */
	ctx->sub_dyn = wuy_dict_get(dynamic->sub_dict, name);

	if (ctx->sub_dyn == NULL) {
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic new holder %s", name);

		/* No container, just hold this name. So the following requests
		 * will be pending here on its holder_wait_head, before get_conf()
		 * returning. */
		ctx->sub_dyn = calloc(1, sizeof(struct h2d_dynamic_conf));
		ctx->sub_dyn->father = dynamic;
		ctx->sub_dyn->name = strdup(name);
		ctx->sub_dyn->create_time = time(NULL);
		ctx->sub_dyn->is_just_holder = true;
		wuy_list_init(&ctx->sub_dyn->holder_wait_head);
		wuy_dict_add(dynamic->sub_dict, ctx->sub_dyn);

	} else if (ctx->sub_dyn->is_just_holder) {
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic just_holder hit %s %d",
				name, ctx->sub_dyn->error_ret);

		/* this name was error in get_conf() */
		ret = ctx->sub_dyn->error_ret;
		if (ret != H2D_OK) {
			goto not_ok;
		}

		/* this name is in get_conf() processing */
		wuy_list_append(&ctx->sub_dyn->holder_wait_head, &r->list_node);
		ctx->sub_dyn = NULL;
		return NULL;

	} else {
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic hit %s", name);

		h2d_dynamic_timer_update(ctx->sub_dyn);

		if (!h2d_dynamic_need_check_conf(ctx->sub_dyn, r)) {
			/* here is the most passed way to end this function! */
			return h2d_dynamic_to_container(ctx->sub_dyn);
		}

		/* also get_conf() to check */
		h2d_request_log(r, H2D_LOG_DEBUG, "dynamic check %s", name);
	}

state_get_conf:

	ret = h2d_dynamic_get_conf(dynamic, r);
	if (ret != H2D_OK) {
		goto not_ok;
	}

	return h2d_dynamic_to_container(ctx->sub_dyn);

not_ok:
	if (ret == H2D_AGAIN) {
		return NULL;
	}
	if (ctx->sub_dyn == NULL) {
		goto out;
	}
	if (ctx->sub_dyn->is_just_holder) {
		/* cache error */
		ctx->sub_dyn->error_ret = ret;

		h2d_dynamic_timer_new(ctx->sub_dyn);
		loop_timer_set_after(ctx->sub_dyn->timer,
				ctx->sub_dyn->father->error_expire * 1000);

		h2d_request_active_list(&ctx->sub_dyn->holder_wait_head, "dynamic holder");
	} else {
		/* ignore error */
		return h2d_dynamic_to_container(ctx->sub_dyn);
	}
out:
	r->resp.status_code = (ret == H2D_ERROR) ? WUY_HTTP_500 : ret;
	return NULL;
}

void h2d_dynamic_ctx_free(struct h2d_request *r)
{
	struct h2d_dynamic_ctx *ctx = r->dynamic_ctx;
	if (ctx == NULL) {
		return;
	}

	if (ctx->lth != NULL) {
		h2d_lua_api_thread_free(ctx->lth);
	}
	if (ctx->sub_dyn != NULL && ctx->sub_dyn->is_just_holder
			&& ctx->sub_dyn->timer == NULL) {
		h2d_dynamic_delete(ctx->sub_dyn, "just_holder");
	}
	free(ctx);
	r->dynamic_ctx = NULL;
}

static bool h2d_dynamic_sub_begin = false;
void h2d_dynamic_init(void)
{
	h2d_dynamic_sub_begin = true;
}

void h2d_dynamic_set_container(struct h2d_dynamic_conf *dynamic,
		struct wuy_cflua_table *conf_table,
		off_t offset, void (*del)(void *))
{
	dynamic->container.conf_table = conf_table;
	dynamic->container.offset = offset;
	dynamic->container.del = del;
}

static bool h2d_dynamic_conf_post(void *data)
{
	struct h2d_dynamic_conf *dynamic = data;

	/* created dynamic-sub */
	if (h2d_dynamic_sub_begin) {
		if (dynamic->get_name_meta_level != -1) {
			printf("get_name is not allowed in dynamic sub\n");
			return false;
		}
		dynamic->get_name = 0;
		return true;
	}

	/* defined in configuration file */
	if (!wuy_cflua_is_function_set(dynamic->get_name)) {
		return true;
	}
	if (!wuy_cflua_is_function_set(dynamic->get_conf)) {
		printf("dynamic get_conf must be set too\n");
		return false;
	}
	dynamic->sub_dict = wuy_dict_new_type(WUY_DICT_KEY_STRING,
			offsetof(struct h2d_dynamic_conf, name),
			offsetof(struct h2d_dynamic_conf, dict_node));

	return true;
}

static struct wuy_cflua_command h2d_dynamic_conf_commands[] = {
	/* father only */
	{	.name = "get_name",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_dynamic_conf, get_name),
		.meta_level_offset = offsetof(struct h2d_dynamic_conf, get_name_meta_level),
	},
	{	.name = "get_conf",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_dynamic_conf, get_conf),
	},
	{	.name = "is_name_blocking",
		.type = WUY_CFLUA_TYPE_BOOLEAN,
		.offset = offsetof(struct h2d_dynamic_conf, is_name_blocking),
	},
	{	.name = "sub_max",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_dynamic_conf, sub_max),
		.default_value.n = 1000,
		.limits.n = WUY_CFLUA_LIMITS_NON_NEGATIVE,
	},
	{	.name = "error_expire",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_dynamic_conf, error_expire),
		.default_value.n = 3,
		.limits.n = WUY_CFLUA_LIMITS_NON_NEGATIVE,
	},

	/* sub */
	{	.name = "check_interval",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_dynamic_conf, check_interval),
		.default_value.n = 0,
		.limits.n = WUY_CFLUA_LIMITS_NON_NEGATIVE,
	},
	{	.name = "check_filter",
		.type = WUY_CFLUA_TYPE_FUNCTION,
		.offset = offsetof(struct h2d_dynamic_conf, check_filter),
	},
	{	.name = "idle_timeout",
		.type = WUY_CFLUA_TYPE_INTEGER,
		.offset = offsetof(struct h2d_dynamic_conf, idle_timeout),
		.default_value.n = 3600,
		.limits.n = WUY_CFLUA_LIMITS_NON_NEGATIVE,
	},
	{ NULL },
};

struct wuy_cflua_table h2d_dynamic_conf_table = {
	.commands = h2d_dynamic_conf_commands,
	.post = h2d_dynamic_conf_post,
};
