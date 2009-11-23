#include "cache.h"
#include "transport.h"
#include "quote.h"
#include "run-command.h"
#include "commit.h"
#include "diff.h"
#include "revision.h"
#include "quote.h"

struct helper_data
{
	const char *name;
	struct child_process *helper;
	FILE *out;
	unsigned fetch : 1,
		option : 1,
		push : 1;
};

static struct child_process *get_helper(struct transport *transport)
{
	struct helper_data *data = transport->data;
	struct strbuf buf = STRBUF_INIT;
	struct child_process *helper;

	if (data->helper)
		return data->helper;

	helper = xcalloc(1, sizeof(*helper));
	helper->in = -1;
	helper->out = -1;
	helper->err = 0;
	helper->argv = xcalloc(4, sizeof(*helper->argv));
	strbuf_addf(&buf, "remote-%s", data->name);
	helper->argv[0] = strbuf_detach(&buf, NULL);
	helper->argv[1] = transport->remote->name;
	helper->argv[2] = transport->url;
	helper->git_cmd = 1;
	if (start_command(helper))
		die("Unable to run helper: git %s", helper->argv[0]);
	data->helper = helper;

	write_str_in_full(helper->in, "capabilities\n");

	data->out = xfdopen(helper->out, "r");
	while (1) {
		if (strbuf_getline(&buf, data->out, '\n') == EOF)
			exit(128); /* child died, message supplied already */

		if (!*buf.buf)
			break;
		if (!strcmp(buf.buf, "fetch"))
			data->fetch = 1;
		if (!strcmp(buf.buf, "option"))
			data->option = 1;
		if (!strcmp(buf.buf, "push"))
			data->push = 1;
	}
	return data->helper;
}

static int disconnect_helper(struct transport *transport)
{
	struct helper_data *data = transport->data;
	if (data->helper) {
		write_str_in_full(data->helper->in, "\n");
		close(data->helper->in);
		fclose(data->out);
		finish_command(data->helper);
		free((char *)data->helper->argv[0]);
		free(data->helper->argv);
		free(data->helper);
		data->helper = NULL;
	}
	free(data);
	return 0;
}

static const char *unsupported_options[] = {
	TRANS_OPT_UPLOADPACK,
	TRANS_OPT_RECEIVEPACK,
	TRANS_OPT_THIN,
	TRANS_OPT_KEEP
	};
static const char *boolean_options[] = {
	TRANS_OPT_THIN,
	TRANS_OPT_KEEP,
	TRANS_OPT_FOLLOWTAGS
	};

static int set_helper_option(struct transport *transport,
			  const char *name, const char *value)
{
	struct helper_data *data = transport->data;
	struct child_process *helper = get_helper(transport);
	struct strbuf buf = STRBUF_INIT;
	int i, ret, is_bool = 0;

	if (!data->option)
		return 1;

	for (i = 0; i < ARRAY_SIZE(unsupported_options); i++) {
		if (!strcmp(name, unsupported_options[i]))
			return 1;
	}

	for (i = 0; i < ARRAY_SIZE(boolean_options); i++) {
		if (!strcmp(name, boolean_options[i])) {
			is_bool = 1;
			break;
		}
	}

	strbuf_addf(&buf, "option %s ", name);
	if (is_bool)
		strbuf_addstr(&buf, value ? "true" : "false");
	else
		quote_c_style(value, &buf, NULL, 0);
	strbuf_addch(&buf, '\n');

	if (write_in_full(helper->in, buf.buf, buf.len) != buf.len)
		die_errno("cannot send option to %s", data->name);

	strbuf_reset(&buf);
	if (strbuf_getline(&buf, data->out, '\n') == EOF)
		exit(128); /* child died, message supplied already */

	if (!strcmp(buf.buf, "ok"))
		ret = 0;
	else if (!prefixcmp(buf.buf, "error")) {
		ret = -1;
	} else if (!strcmp(buf.buf, "unsupported"))
		ret = 1;
	else {
		warning("%s unexpectedly said: '%s'", data->name, buf.buf);
		ret = 1;
	}
	strbuf_release(&buf);
	return ret;
}

static void standard_options(struct transport *t)
{
	char buf[16];
	int n;
	int v = t->verbose;
	int no_progress = v < 0 || (!t->progress && !isatty(1));

	set_helper_option(t, "progress", !no_progress ? "true" : "false");

	n = snprintf(buf, sizeof(buf), "%d", v + 1);
	if (n >= sizeof(buf))
		die("impossibly large verbosity value");
	set_helper_option(t, "verbosity", buf);
}

static int fetch_with_fetch(struct transport *transport,
			    int nr_heads, const struct ref **to_fetch)
{
	struct helper_data *data = transport->data;
	int i;
	struct strbuf buf = STRBUF_INIT;

	standard_options(transport);

	for (i = 0; i < nr_heads; i++) {
		const struct ref *posn = to_fetch[i];
		if (posn->status & REF_STATUS_UPTODATE)
			continue;

		strbuf_addf(&buf, "fetch %s %s\n",
			    sha1_to_hex(posn->old_sha1), posn->name);
	}

	strbuf_addch(&buf, '\n');
	if (write_in_full(data->helper->in, buf.buf, buf.len) != buf.len)
		die_errno("cannot send fetch to %s", data->name);

	while (1) {
		strbuf_reset(&buf);
		if (strbuf_getline(&buf, data->out, '\n') == EOF)
			exit(128); /* child died, message supplied already */

		if (!prefixcmp(buf.buf, "lock ")) {
			const char *name = buf.buf + 5;
			if (transport->pack_lockfile)
				warning("%s also locked %s", data->name, name);
			else
				transport->pack_lockfile = xstrdup(name);
		}
		else if (!buf.len)
			break;
		else
			warning("%s unexpectedly said: '%s'", data->name, buf.buf);
	}
	strbuf_release(&buf);
	return 0;
}

static int fetch(struct transport *transport,
		 int nr_heads, const struct ref **to_fetch)
{
	struct helper_data *data = transport->data;
	int i, count;

	count = 0;
	for (i = 0; i < nr_heads; i++)
		if (!(to_fetch[i]->status & REF_STATUS_UPTODATE))
			count++;

	if (!count)
		return 0;

	if (data->fetch)
		return fetch_with_fetch(transport, nr_heads, to_fetch);

	return -1;
}

static int push_refs(struct transport *transport,
		struct ref *remote_refs, int flags)
{
	int force_all = flags & TRANSPORT_PUSH_FORCE;
	int mirror = flags & TRANSPORT_PUSH_MIRROR;
	struct helper_data *data = transport->data;
	struct strbuf buf = STRBUF_INIT;
	struct child_process *helper;
	struct ref *ref;

	if (!remote_refs)
		return 0;

	helper = get_helper(transport);
	if (!data->push)
		return 1;

	for (ref = remote_refs; ref; ref = ref->next) {
		if (ref->peer_ref)
			hashcpy(ref->new_sha1, ref->peer_ref->new_sha1);
		else if (!mirror)
			continue;

		ref->deletion = is_null_sha1(ref->new_sha1);
		if (!ref->deletion &&
			!hashcmp(ref->old_sha1, ref->new_sha1)) {
			ref->status = REF_STATUS_UPTODATE;
			continue;
		}

		if (force_all)
			ref->force = 1;

		strbuf_addstr(&buf, "push ");
		if (!ref->deletion) {
			if (ref->force)
				strbuf_addch(&buf, '+');
			if (ref->peer_ref)
				strbuf_addstr(&buf, ref->peer_ref->name);
			else
				strbuf_addstr(&buf, sha1_to_hex(ref->new_sha1));
		}
		strbuf_addch(&buf, ':');
		strbuf_addstr(&buf, ref->name);
		strbuf_addch(&buf, '\n');
	}
	if (buf.len == 0)
		return 0;

	transport->verbose = flags & TRANSPORT_PUSH_VERBOSE ? 1 : 0;
	standard_options(transport);

	if (flags & TRANSPORT_PUSH_DRY_RUN) {
		if (set_helper_option(transport, "dry-run", "true") != 0)
			die("helper %s does not support dry-run", data->name);
	}

	strbuf_addch(&buf, '\n');
	if (write_in_full(helper->in, buf.buf, buf.len) != buf.len)
		exit(128);

	ref = remote_refs;
	while (1) {
		char *refname, *msg;
		int status;

		strbuf_reset(&buf);
		if (strbuf_getline(&buf, data->out, '\n') == EOF)
			exit(128); /* child died, message supplied already */
		if (!buf.len)
			break;

		fprintf(stderr, "helper said '%s'\n", buf.buf);

		if (!prefixcmp(buf.buf, "ok ")) {
			status = REF_STATUS_OK;
			refname = buf.buf + 3;
		} else if (!prefixcmp(buf.buf, "error ")) {
			status = REF_STATUS_REMOTE_REJECT;
			refname = buf.buf + 6;
		} else
			die("expected ok/error, helper said '%s'\n", buf.buf);

		msg = strchr(refname, ' ');
		if (msg) {
			struct strbuf msg_buf = STRBUF_INIT;
			const char *end;

			*msg++ = '\0';
			if (!unquote_c_style(&msg_buf, msg, &end))
				msg = strbuf_detach(&msg_buf, NULL);
			else
				msg = xstrdup(msg);
			strbuf_release(&msg_buf);

			if (!strcmp(msg, "no match")) {
				status = REF_STATUS_NONE;
				free(msg);
				msg = NULL;
			}
			else if (!strcmp(msg, "up to date")) {
				status = REF_STATUS_UPTODATE;
				free(msg);
				msg = NULL;
			}
			else if (!strcmp(msg, "non-fast forward")) {
				status = REF_STATUS_REJECT_NONFASTFORWARD;
				free(msg);
				msg = NULL;
			}
		}

		if (ref)
			ref = find_ref_by_name(ref, refname);
		if (!ref)
			ref = find_ref_by_name(remote_refs, refname);
		if (!ref) {
			warning("helper reported unexpected status of %s", refname);
			continue;
		}

		ref->status = status;
		ref->remote_status = msg;
	}
	strbuf_release(&buf);
	return 0;
}

static struct ref *get_refs_list(struct transport *transport, int for_push)
{
	struct helper_data *data = transport->data;
	struct child_process *helper;
	struct ref *ret = NULL;
	struct ref **tail = &ret;
	struct ref *posn;
	struct strbuf buf = STRBUF_INIT;

	helper = get_helper(transport);

	if (data->push && for_push)
		write_str_in_full(helper->in, "list for-push\n");
	else
		write_str_in_full(helper->in, "list\n");

	while (1) {
		char *eov, *eon;
		if (strbuf_getline(&buf, data->out, '\n') == EOF)
			exit(128); /* child died, message supplied already */

		if (!*buf.buf)
			break;

		eov = strchr(buf.buf, ' ');
		if (!eov)
			die("Malformed response in ref list: %s", buf.buf);
		eon = strchr(eov + 1, ' ');
		*eov = '\0';
		if (eon)
			*eon = '\0';
		*tail = alloc_ref(eov + 1);
		if (buf.buf[0] == '@')
			(*tail)->symref = xstrdup(buf.buf + 1);
		else if (buf.buf[0] != '?')
			get_sha1_hex(buf.buf, (*tail)->old_sha1);
		tail = &((*tail)->next);
	}
	strbuf_release(&buf);

	for (posn = ret; posn; posn = posn->next)
		resolve_remote_symref(posn, ret);

	return ret;
}

int transport_helper_init(struct transport *transport, const char *name)
{
	struct helper_data *data = xcalloc(sizeof(*data), 1);
	data->name = name;

	transport->data = data;
	transport->set_option = set_helper_option;
	transport->get_refs_list = get_refs_list;
	transport->fetch = fetch;
	transport->push_refs = push_refs;
	transport->disconnect = disconnect_helper;
	return 0;
}
