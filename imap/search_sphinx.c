/* search_sphinx.c -- glue code for searching with Sphinx
 *
 * Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "index.h"
#include "imap_err.h"
#include "global.h"
#include "retry.h"
#include "command.h"
#include "xmalloc.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"
#include "bitvector.h"
#include "mboxlist.h"
#include "xstats.h"
#include "search_engines.h"
#include "sphinxmgr_client.h"

#include <mysql/mysql.h>

struct connection
{
    MYSQL *mysql;
    char *socket_path;
};
#define CONNECTION_INITIALIZER	{ 0, 0 }

static int doquery(struct connection *, int, const struct buf *);

/* Name of columns */
#define COL_CYRUSID	"cyrusid"
static const char * const column_by_part[SEARCH_NUM_PARTS] = {
    NULL,
    "header_from",
    "header_to",
    "header_cc",
    "header_bcc",
    "header_subject",
    "headers",
    "body"
};

static int get_connection(const char *mboxname, struct connection *conn)
{
    MYSQL *c = NULL;
    char *socket_path = NULL;
    int r;

    /* note, we always go through sphinxmgr even if
     * it's the same mboxname as last time - this lets
     * sphinxmgr know that the index daemon is being
     * used and so not to expire it */
    r = sphinxmgr_getsock(mboxname, &socket_path);
    if (r) return r;

    if (conn->socket_path && !strcmp(socket_path, conn->socket_path)) {
	free(socket_path);
	return 0;
    }

    free(conn->socket_path);
    conn->socket_path = NULL;

    if (conn->mysql) {
	xstats_inc(SPHINX_CLOSE);
	mysql_close(conn->mysql);
	mysql_library_end();
	conn->mysql = NULL;
    }

    xstats_inc(SPHINX_CONNECT);
    c = mysql_init(NULL);

    if (!mysql_real_connect(c,
			   /*host*/NULL,
			   /*user*/"", /*password*/"",
			   /*database*/NULL,
			   /*port*/0, socket_path,
			   /*client_flag*/0)) {
	syslog(LOG_ERR, "IOERROR: failed to connect to Sphinx: %s",
	       mysql_error(c));
	mysql_close(c);
	mysql_library_end();
	free(socket_path);
	return IMAP_IOERROR;
    }

    conn->socket_path = socket_path;
    conn->mysql = c;
    return 0;
}

static void close_connection(struct connection *conn)
{
    free(conn->socket_path);
    conn->socket_path = NULL;

    if (conn->mysql) {
	xstats_inc(SPHINX_CLOSE);
	mysql_close(conn->mysql);
	conn->mysql = NULL;
	mysql_library_end();
    }
}

static int parse_cyrusid(const char *cyrusid,
			 const char **mboxnamep,
			 unsigned int *uidvalidityp,
			 unsigned int *uidp)
{
    // user.cassandane.1320711192.196715
    static struct buf buf = BUF_INITIALIZER;
    char *p;

    buf_reset(&buf);
    buf_appendcstr(&buf, cyrusid);

    p = strrchr(buf_cstring(&buf), '.');
    if (!p)
	return 0;
    *p++ = '\0';
    *uidp = strtoul(p, NULL, 10);

    p = strrchr(buf.s, '.');
    if (!p)
	return 0;
    *p++ = '\0';
    *uidvalidityp = strtoul(p, NULL, 10);

    *mboxnamep = buf.s;

    return 1;
}

static const struct buf *make_cyrusid(struct mailbox *mailbox, uint32_t uid)
{
    static struct buf buf = BUF_INITIALIZER;
    // user.cassandane.1320711192.196715
    buf_reset(&buf);
    buf_printf(&buf, "%s.%u.%u",
		     mailbox->name,
		     mailbox->i.uidvalidity,
		     uid);
    return &buf;
}

/*
 * Escape a string for MySQL.  Note that mysql_real_escape_string
 * requires a live connection, and we now want to be able to build a
 * query string before we have a connection.  From the MySQL
 * documentation:
 *
 *	Strictly speaking, MySQL requires only that backslash and
 *	the quote character used to quote the string in the query
 *	be escaped. mysql_real_escape_string() quotes the other
 *	characters to make them easier to read in log files.
 */
static void append_escaped_map(struct buf *buf,
			       const char *base, unsigned int len,
			       int quote)
{
    buf_ensure(buf, len+1);

    buf_putc(buf, quote);
    for ( ; len ; len--, base++) {
	int c = *(unsigned char *)base;
	if (c == '\\' || c == '\'' || c == '"')
	    buf_putc(buf, '\\');
	buf_putc(buf, c);
    }
    buf_putc(buf, quote);
    buf_cstring(buf);
}

static void append_escaped(struct buf *to, const struct buf *from, int quote)
{
    append_escaped_map(to, from->s, from->len, quote);
}

static void append_escaped_cstr(struct buf *to, const char *str, int quote)
{
    if (str)
	append_escaped_map(to, str, strlen(str), quote);
}

struct opstack {
    int idx;	/* index of next child in parent node */
    int op;	/* op of the parent node */
};

typedef struct sphinx_builder sphinx_builder_t;
struct sphinx_builder {
    search_builder_t super;
    struct mailbox *mailbox;
    search_hit_cb_t proc;
    void *rock;
    int opts;
    struct buf query;	    /* Spinx extended query syntax, not SphinxQL */
    int depth;
    int alloc;
    struct opstack *stack;
    int nmatches;
};

static struct opstack *opstack_top(sphinx_builder_t *bb)
{
    return (bb->depth ? &bb->stack[bb->depth-1] : NULL);
}

static void begin_child(sphinx_builder_t *bb)
{
    struct opstack *top = opstack_top(bb);

    if (top) {
	/* operator precedence in the Sphinx text searching language
	 * is not what we would expect, so over-compensate by always
	 * using parentheses */
	if (!top->idx)
	    buf_appendcstr(&bb->query, "(");
	else if (top->op == SEARCH_OP_AND)
	    buf_appendcstr(&bb->query, " ");
	else
	    buf_appendcstr(&bb->query, "|");
	top->idx++;
    }
}

static void begin_boolean(search_builder_t *bx, int op)
{
    sphinx_builder_t *bb = (sphinx_builder_t *)bx;
    struct opstack *top;

//     if (SEARCH_VERBOSE(bb->opts))
// 	syslog(LOG_NOTICE, "begin_boolean(%s)", search_op_as_string(op));

    begin_child(bb);

    if (op == SEARCH_OP_NOT)
	buf_appendcstr(&bb->query, "!");

    /* push a new op on the stack */
    if (bb->depth+1 > bb->alloc) {
	bb->alloc += 16;
	bb->stack = xrealloc(bb->stack, bb->alloc * sizeof(struct opstack));
    }

    top = &bb->stack[bb->depth++];
    top->op = op;
    top->idx = 0;
}

static void end_boolean(search_builder_t *bx, int op __attribute__((unused)))
{
    sphinx_builder_t *bb = (sphinx_builder_t *)bx;
    struct opstack *top = opstack_top(bb);

//     if (SEARCH_VERBOSE(bb->opts))
// 	syslog(LOG_NOTICE, "end_boolean(%s)", search_op_as_string(op));

    if (top->idx)
	buf_appendcstr(&bb->query, ")");

    /* op the last operator off the stack */
    bb->depth--;
}

static void match(search_builder_t *bx, int part, const char *str)
{
    sphinx_builder_t *bb = (sphinx_builder_t *)bx;
    static struct buf f = BUF_INITIALIZER;
    static struct buf e1 = BUF_INITIALIZER;

    begin_child(bb);
    if (str) bb->nmatches++;
    if (str) xstats_inc(SPHINX_MATCH);

    if (column_by_part[part]) {
	buf_appendcstr(&bb->query, "@");
	buf_appendcstr(&bb->query, column_by_part[part]);
	buf_appendcstr(&bb->query, " ");
    }
    else if (config_getswitch(IMAPOPT_SPHINX_TEXT_EXCLUDES_ODD_HEADERS)) {
	/* This horrible hack makes TEXT searches match FROM, TO, CC, BCC
	 * and SUBJECT but not any other random headers, which is more
	 * like what users expect. */
	int i;
	const char *sep = "(";
	buf_appendcstr(&bb->query, "@");
	for (i = 0 ; i < SEARCH_NUM_PARTS ; i++) {
	    if (column_by_part[i] && i != SEARCH_PART_HEADERS) {
		buf_appendcstr(&bb->query, sep);
		buf_appendcstr(&bb->query, column_by_part[i]);
		sep = ",";
	    }
	}
	buf_appendcstr(&bb->query, ") ");
    }

    buf_init_ro_cstr(&f, str);
    buf_reset(&e1);
    append_escaped(&bb->query, &f, '"');
}

static void *get_internalised(search_builder_t *bx)
{
    sphinx_builder_t *bb = (sphinx_builder_t *)bx;
    void *internalised;

    buf_cstring(&bb->query);
    internalised = xmemdup(&bb->query, sizeof(bb->query));
    memset(&bb->query, 0, sizeof(bb->query));
    return internalised;
}

static void free_internalised(void *internalised)
{
    struct buf *query = (struct buf *)internalised;

    if (query) {
	buf_free(query);
	free(query);
    }
}

static search_builder_t *begin_search(struct mailbox *mailbox,
				      int opts,
				      search_hit_cb_t proc, void *rock)
{
    sphinx_builder_t *bb;

    bb = xzmalloc(sizeof(sphinx_builder_t));
    bb->super.begin_boolean = begin_boolean;
    bb->super.end_boolean = end_boolean;
    bb->super.match = match;
    bb->super.get_internalised = get_internalised;

    bb->mailbox = mailbox;
    bb->proc = proc;
    bb->rock = rock;
    bb->opts = opts;

    if ((opts & SEARCH_MULTIPLE))
	xstats_inc(SPHINX_MULTIPLE);
    else
	xstats_inc(SPHINX_SINGLE);

    return &bb->super;
}

/* Yes, we read the latest uid in two separate functions.  Meh */
static int read_latest_search(sphinx_builder_t *bb,
			      struct connection *conn,
			      uint32_t *latestp)
{
    struct buf query = BUF_INITIALIZER;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    int r = 0;

    buf_printf(&query, "SELECT mboxname,uid "
		       "FROM latest "
		       "WHERE uidvalidity=%u "
		       "LIMIT 10000",
		       bb->mailbox->i.uidvalidity);

    r = doquery(conn, SEARCH_VERBOSE(bb->opts), &query);
    if (r) goto out;

    res = mysql_store_result(conn->mysql);
    while ((row = mysql_fetch_row(res))) {
	if (!strcmp(bb->mailbox->name, row[0])) {
	    *latestp = strtoul(row[1], NULL, 10);
	    break;
	}
    }

out:
    if (res) mysql_free_result(res);
    buf_free(&query);
    return r;
}

static int end_search(search_builder_t *bx)
{
    sphinx_builder_t *bb = (sphinx_builder_t *)bx;
    struct connection conn = CONNECTION_INITIALIZER;
    struct buf query = BUF_INITIALIZER;	/* SphinxQL query */
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    uint32_t uid;
    uint32_t latest = 0;
    int r = 0;

    if ((bb->opts & SEARCH_DRYRUN)) goto out;

    if (!bb->nmatches) {
	/* The search expression has no match clauses, which means it
	 * won't be using Sphinx's text search capabilities.  The best
	 * we can hope for from Sphinx is that it will tell us every
	 * indexed message, and our caller will add the unindexed
	 * messages and post-filter all of that to enforce the actual
	 * search criteria.  So let's just short-circuit all that by
	 * returning an error, which forces our caller to fall back. */
	r = IMAP_TRIVIAL_SEARCH;
	goto out;
    }

    r = get_connection(bb->mailbox->name, &conn);
    if (r) goto out;

    if ((bb->opts & SEARCH_UNINDEXED)) {
	/* To avoid races, we want the 'latest' uid we use to be
	 * an underestimate, because the caller can handle false
	 * positives but not false negatives.  So we fetch it
	 * first before the main query. */
	r = read_latest_search(bb, &conn, &latest);
	if (r) goto out;
    }

    buf_init_ro_cstr(&query, "SELECT "COL_CYRUSID" FROM rt WHERE MATCH(");
    append_escaped(&query, &bb->query, '\'');
    buf_appendcstr(&query, ")");
    // get sphinx to sort by most recent date first
    buf_appendcstr(&query, " ORDER BY "COL_CYRUSID" DESC "
			       " LIMIT " SPHINX_MAX_MATCHES
			       " OPTION max_matches=" SPHINX_MAX_MATCHES);
    buf_cstring(&query);

    if (SEARCH_VERBOSE(bb->opts))
	syslog(LOG_NOTICE, "Sphinx query %s", query.s);
    xstats_inc(SPHINX_QUERY);

    r = mysql_real_query(conn.mysql, query.s, query.len);
    if (r) {
	syslog(LOG_ERR, "IOERROR: Sphinx query %s failed: %s",
	       query.s, mysql_error(conn.mysql));
	r = IMAP_IOERROR;
	goto out;
    }

    res = mysql_use_result(conn.mysql);
    while ((row = mysql_fetch_row(res))) {
	const char *mboxname;
	unsigned int uidvalidity;
	unsigned int uid;
	if (SEARCH_VERBOSE(bb->opts) > 1)
	    syslog(LOG_NOTICE, "Sphinx row cyrusid=%s", row[0]);
	xstats_inc(SPHINX_ROW);
	if (!parse_cyrusid(row[0], &mboxname, &uidvalidity, &uid))
	    // TODO: whine
	    continue;
	if (!(bb->opts & SEARCH_MULTIPLE)) {
	    if (strcmp(mboxname, bb->mailbox->name))
		continue;
	    if (uidvalidity != bb->mailbox->i.uidvalidity)
		continue;
	}
	xstats_inc(SPHINX_RESULT);
	r = bb->proc(mboxname, uidvalidity, uid, bb->rock);
	if (r) goto out;
    }
    r = 0;

    if ((bb->opts & SEARCH_UNINDEXED)) {
	/* add in the unindexed uids as false positives */
	for (uid = latest+1 ; uid <= bb->mailbox->i.last_uid ; uid++) {
	    xstats_inc(SPHINX_UNINDEXED);
	    r = bb->proc(bb->mailbox->name, bb->mailbox->i.uidvalidity, uid, bb->rock);
	    if (r) goto out;
	}
    }

out:
    if (res) mysql_free_result(res);
    close_connection(&conn);
    free(bb->stack);
    buf_free(&bb->query);
    buf_free(&query);
    free(bx);
    return r;
}

typedef struct sphinx_receiver sphinx_receiver_t;
struct sphinx_receiver
{
    search_text_receiver_t super;
    int verbose;
    struct connection conn;
    struct mailbox *mailbox;
    uint32_t uid;
    int part;
    unsigned int parts_total;
    int truncate_warning;
    struct buf parts[SEARCH_NUM_PARTS];
    struct buf query;
    unsigned int uncommitted;
    uint32_t latest;
    uint32_t latest_id;	    /* The 'id' attribute of the row in the
			     * 'latest' table which describes the
			     * current mailbox, or 0 */
    uint32_t latest_lastid; /* The largest document ID in the 'latest'
			     * table, used when INSERTing */
    uint32_t lastid;	    /* largest document ID in the 'tr' table,
			     * used to assign new document IDs when
			     * INSERTing into the table */
    struct {
	struct buf *query;
	search_snippet_cb_t proc;
	void *rock;
    } snippet;
};

/* This is carefully aligned with the default search_batchsize so that
 * we get the minimum number of commits with default parameters */
#define MAX_UNCOMMITTED	    20

/* Maximum size of a query, determined empirically, is a little bit
 * under 8MB.  That seems like more than enough, so let's limit the
 * total amount of parts text to 4 MB. */
#define MAX_PARTS_SIZE	    (4*1024*1024)

static const char *describe_query(struct buf *desc,
				  const struct buf *query,
				  unsigned maxlen)
{
    buf_reset(desc);
    buf_appendcstr(desc, "Sphinx query ");
    if (maxlen && query->len > maxlen) {
	buf_appendmap(desc, query->s, maxlen);
	buf_appendcstr(desc, "...");
    }
    else {
	append_escaped(desc, query, '"');
    }
    return buf_cstring(desc);
}

static int doquery(struct connection *conn, int verbose, const struct buf *query)
{
    int r;
    struct buf desc = BUF_INITIALIZER;
    unsigned int maxlen = verbose > 2 ? /*unlimited*/0 : 128;

    if (verbose > 1)
	syslog(LOG_NOTICE, "%s", describe_query(&desc, query, maxlen));

    r = mysql_real_query(conn->mysql, query->s, query->len);
    if (r) {
	syslog(LOG_ERR, "IOERROR: %s failed: %s",
			describe_query(&desc, query, maxlen),
			mysql_error(conn->mysql));
	r = IMAP_IOERROR;
    }

    buf_free(&desc);
    return r;
}

#if 0
/* Dump a result which has had mysql_store_result() called on it */
static void dump_result(MYSQL_RES *res)
{
    uint64_t nrows = mysql_num_rows(res);
    unsigned int nfields = mysql_num_fields(res);
    unsigned int i;
    unsigned int j;
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    MYSQL_ROW *row;
    struct buf buf = BUF_INITIALIZER;

    syslog(LOG_NOTICE, "Sphinx result: %u rows", (unsigned int)nrows);
    i = 0;
    while ((row = mysql_fetch_row(res))) {
	buf_reset(&buf);
	for (j = 0 ; j < nfields ; j++)
	    buf_printf(&buf, " %s=\"%s\"", fields[j].name, row[j]);
	syslog(LOG_NOTICE, "    [%u]%s", ++i, buf_cstring(&buf));
    }

    buf_free(&buf);
    mysql_data_seek(res, 0);	/* rewind */
}
#endif


/*
 * Read the most recently indexed UID for the current mailboxfrom the
 * 'latest' table in the Sphinx searchd.  This is a bit of a shemozzle
 * because Sphinx does not let us write a WHERE clause in a SELECT or
 * UPDATE statement which matches against a string attribute, so we
 * can't just do the obvious SQL statements.  Instead we have to SELECT
 * on the uidvalidity only and then filter the results manually for
 * mboxname.  The same limitation makes write_latest() a real challange
 * too.
 * Updates tr->latest, tr->latest_id, tr->latest_lastid
 * Returns 0 on success or an IMAP error code.
 */
static int read_latest(sphinx_receiver_t *tr)
{
    struct buf query = BUF_INITIALIZER;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    int r = 0;

    tr->latest = 0;
    tr->latest_id = 0;
    tr->latest_lastid = 0;

    buf_printf(&query, "SELECT id,mboxname,uid "
		       "FROM latest "
		       "WHERE uidvalidity=%u "
		       "LIMIT 10000",
		       tr->mailbox->i.uidvalidity);

    r = doquery(&tr->conn, tr->verbose, &query);
    if (r) goto out;

    res = mysql_store_result(tr->conn.mysql);
    while ((row = mysql_fetch_row(res))) {
	if (!strcmp(tr->mailbox->name, row[1])) {
	    tr->latest_id = strtoul(row[0], NULL, 10);
	    tr->latest = strtoul(row[2], NULL, 10);
	    break;
	}
    }

    mysql_free_result(res);
    res = NULL;

    buf_reset(&query);
    /* Guess what.. the query 'SELECT MAX(id) FROM latest' returns N
     * rows with all N valid ids..., rather than one row with the max */
    buf_appendcstr(&query, "SELECT max(id) FROM latest ORDER BY id DESC LIMIT 1;");

    r = doquery(&tr->conn, tr->verbose, &query);
    if (r) goto out;

    res = mysql_store_result(tr->conn.mysql);
    if (!res) goto out;
    row = mysql_fetch_row(res);
    if (row)
	tr->latest_lastid = strtoul(row[0], NULL, 10);

out:
    if (res) mysql_free_result(res);
    buf_free(&query);
    return r;
}

static int write_latest(sphinx_receiver_t *tr)
{
    struct buf query = BUF_INITIALIZER;
    int r;
    uint32_t id = tr->latest_id;

    if (id) {
	buf_printf(&query, "UPDATE latest "
			   "SET uid=%u "
			   "WHERE id=%u",
			   tr->latest, id);
    }
    else {
	id = tr->latest_lastid+1;
	buf_appendcstr(&query, "INSERT INTO latest "
			       "(id,mboxname,uidvalidity,uid) "
			       "VALUES (");
	buf_printf(&query, "%u,", id);
	append_escaped_cstr(&query, tr->mailbox->name, '\'');
	buf_printf(&query, ",%u,%u)",
		   tr->mailbox->i.uidvalidity, tr->latest);
    }

    r = doquery(&tr->conn, tr->verbose, &query);
    if (r) goto out;

    tr->latest_id = id;

out:
    buf_free(&query);
    return 0;
}

/*
 * Read the last document ID from Sphinx.  Currently this is very dumb
 * and just SELECTs MAX(id), in the hope that this is efficient on the
 * server side (the documentation does not make that clear).  This has
 * the behaviour that document IDs might get re-used if the last
 * document is DELETEd; we don't really care because the only thing we
 * use the document IDs for is INSERTing a new row.
 *
 * Updates tr->lastid
 * Returns: 0 on success or an IMAP error code.
 */
static int read_lastid(sphinx_receiver_t *tr)
{
    struct buf query = BUF_INITIALIZER;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    int r = 0;

    tr->lastid = 0;

    buf_appendcstr(&query, "SELECT max(id) FROM rt ORDER BY id DESC LIMIT 1;");

    r = doquery(&tr->conn, tr->verbose, &query);
    if (r) goto out;

    res = mysql_store_result(tr->conn.mysql);
    if (!res) goto out;
#if 0
    if (tr->verbose > 1) dump_result(res);
#endif
    row = mysql_fetch_row(res);

    if (row)
	tr->lastid = strtoul(row[0], NULL, 10);

    if (tr->verbose > 1)
	syslog(LOG_NOTICE, "Sphinx read_lastid: %u", tr->lastid);

out:
    if (res) mysql_free_result(res);
    buf_free(&query);
    return r;
}


static int flush(sphinx_receiver_t *tr, int force)
{
    int r = 0;

    if (!force && tr->uncommitted < MAX_UNCOMMITTED) return 0;

    if (tr->uncommitted) {
	r = write_latest(tr);
	if (r) return r;
    }

    if (tr->verbose > 1)
	syslog(LOG_NOTICE, "Sphinx committing");

    r = mysql_commit(tr->conn.mysql);
    if (r) {
	syslog(LOG_ERR, "IOERROR: Sphinx COMMIT failed for "
			"mailbox %s, %u messages ending at uid %u: %s",
			tr->mailbox->name, tr->uncommitted, tr->uid,
			mysql_error(tr->conn.mysql));
	return IMAP_IOERROR;
    }
    tr->uncommitted = 0;

    return r;
}

static void begin_message(search_text_receiver_t *rx, uint32_t uid)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int i;

    tr->uid = uid;
    for (i = 0 ; i < SEARCH_NUM_PARTS ; i++)
	buf_reset(&tr->parts[i]);
    tr->parts_total = 0;
    tr->truncate_warning = 0;
}

static void begin_part(search_text_receiver_t *rx, int part)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    tr->part = part;
}

static void append_text(search_text_receiver_t *rx,
			const struct buf *text)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    if (tr->part) {
	unsigned len = text->len;
	if (tr->parts_total + len > MAX_PARTS_SIZE) {
	    if (!tr->truncate_warning++)
		syslog(LOG_ERR, "Sphinx: truncating text from "
				"message mailbox %s uid %u",
				tr->mailbox->name, tr->uid);
	    len = MAX_PARTS_SIZE - tr->parts_total;
	}
	if (len) {
	    tr->parts_total += len;
	    buf_appendmap(&tr->parts[tr->part], text->s, len);
	}
    }
}

static void end_part(search_text_receiver_t *rx,
		     int part __attribute__((unused)))
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    if (tr->verbose > 1)
	syslog(LOG_NOTICE, "Sphinx: %u bytes in part %d",
	       tr->parts[tr->part].len, tr->part);

    tr->part = 0;
}

static int end_message(search_text_receiver_t *rx)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int i;
    int r;

    if (!tr->conn.mysql) return IMAP_INTERNAL;

    buf_reset(&tr->query);
    buf_appendcstr(&tr->query, "INSERT INTO rt (id,"COL_CYRUSID);
    for (i = 0 ; i < SEARCH_NUM_PARTS ; i++) {
	if (tr->parts[i].len) {
	    buf_appendcstr(&tr->query, ",");
	    buf_appendcstr(&tr->query, column_by_part[i]);
	}
    }
    buf_appendcstr(&tr->query, ") VALUES (");
    buf_printf(&tr->query, "%u,", ++tr->lastid);
    append_escaped(&tr->query, make_cyrusid(tr->mailbox, tr->uid), '\'');
    for (i = 0 ; i < SEARCH_NUM_PARTS ; i++) {
	if (tr->parts[i].len) {
	    buf_appendcstr(&tr->query, ",");
	    append_escaped(&tr->query, &tr->parts[i], '\'');
	}
    }
    /* apparently Sphinx doesn't let you explicitly INSERT a NULL */
    buf_appendcstr(&tr->query, ")");

    r = doquery(&tr->conn, tr->verbose, &tr->query);
    if (r) goto out; /* TODO: propagate error to the user */

    ++tr->uncommitted;
    tr->latest = tr->uid;

    r = flush(tr, /*force*/0);
    /* TODO: propagate error to the user */

out:
    tr->uid = 0;
    return 0;
}

static int begin_mailbox(search_text_receiver_t *rx,
			 struct mailbox *mailbox,
			 int incremental __attribute__((unused)))
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int r;

    r = get_connection(mailbox->name, &tr->conn);
    if (r) return r;

    tr->mailbox = mailbox;

    r = read_lastid(tr);
    if (r) return r;

    r = read_latest(tr);
    if (r) return r;

    return 0;
}

static uint32_t first_unindexed_uid(search_text_receiver_t *rx)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    return tr->latest+1;
}

static int is_indexed(search_text_receiver_t *rx, uint32_t uid)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    return (uid <= tr->latest);
}

static int end_mailbox(search_text_receiver_t *rx,
		       struct mailbox *mailbox
			    __attribute__((unused)))
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int r = 0;

    if (tr->conn.mysql) {
	r = flush(tr, /*force*/1);
	close_connection(&tr->conn);
    }

    tr->mailbox = NULL;

    return r;
}

static search_text_receiver_t *begin_update(int verbose)
{
    sphinx_receiver_t *tr;

    tr = xzmalloc(sizeof(sphinx_receiver_t));
    tr->super.begin_mailbox = begin_mailbox;
    tr->super.first_unindexed_uid = first_unindexed_uid;
    tr->super.is_indexed = is_indexed;
    tr->super.begin_message = begin_message;
    tr->super.begin_part = begin_part;
    tr->super.append_text = append_text;
    tr->super.end_part = end_part;
    tr->super.end_message = end_message;
    tr->super.end_mailbox = end_mailbox;

    tr->verbose = verbose;

    return &tr->super;
}

static int end_update(search_text_receiver_t *rx)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int i;
    int r = 0;

    for (i = 0 ; i < SEARCH_NUM_PARTS ; i++)
	buf_free(&tr->parts[i]);
    buf_free(&tr->query);
    free(tr);

    return r;
}

static int begin_mailbox_snippets(search_text_receiver_t *rx,
			 struct mailbox *mailbox,
			 int incremental __attribute__((unused)))
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    int r;

    r = get_connection(mailbox->name, &tr->conn);
    if (r) return r;

    tr->mailbox = mailbox;

    return 0;
}

static int end_message_snippets(search_text_receiver_t *rx)
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;
    struct buf query = BUF_INITIALIZER;
    MYSQL_RES *res = NULL;
    MYSQL_ROW row;
    int i;
    int r;

    if (!tr->conn.mysql) {
	r = IMAP_INTERNAL;	    /* need to call begin_mailbox() */
	goto out;
    }
    if (!tr->snippet.query) {
	r = 0;
	goto out;
    }

    buf_appendcstr(&query, "CALL SNIPPETS((");
    for (i = 0 ; i < SEARCH_NUM_PARTS ; i++) {
	if (i) buf_putc(&query, ',');
	append_escaped(&query, &tr->parts[i], '\'');
    }
    buf_appendcstr(&query, "), 'rt', '");
    buf_append(&query, tr->snippet.query);
    buf_appendcstr(&query, "', 1 AS query_mode, 1 AS allow_empty)");
    r = doquery(&tr->conn, tr->verbose, &query);
    if (r) goto out;

    res = mysql_use_result(tr->conn.mysql);
    i = 0;
    while ((row = mysql_fetch_row(res))) {
	if (tr->verbose > 1)
	    syslog(LOG_ERR, "snippet [%d] \"%s\"", i, row[0]);
	if (row[0][0]) {
	    r = tr->snippet.proc(tr->mailbox, tr->uid, i, row[0], tr->snippet.rock);
	    if (r) break;
	}
	i++;
    }

out:
    if (res) mysql_free_result(res);
    buf_free(&query);
    return r;
}

static int end_mailbox_snippets(search_text_receiver_t *rx,
			       struct mailbox *mailbox
				    __attribute__((unused)))
{
    sphinx_receiver_t *tr = (sphinx_receiver_t *)rx;

    if (tr->conn.mysql)
	close_connection(&tr->conn);

    tr->mailbox = NULL;

    return 0;
}

static search_text_receiver_t *begin_snippets(void *snippet_state,
					      int verbose,
					      search_snippet_cb_t proc,
					      void *rock)
{
    sphinx_receiver_t *tr;

    tr = xzmalloc(sizeof(sphinx_receiver_t));
    tr->super.begin_mailbox = begin_mailbox_snippets;
    tr->super.begin_message = begin_message;
    tr->super.begin_part = begin_part;
    tr->super.append_text = append_text;
    tr->super.end_part = end_part;
    tr->super.end_message = end_message_snippets;
    tr->super.end_mailbox = end_mailbox_snippets;

    tr->verbose = verbose;
    tr->snippet.query = (struct buf *)snippet_state;
    tr->snippet.proc = proc;
    tr->snippet.rock = rock;

    return &tr->super;
}

static int end_snippets(search_text_receiver_t *rx)
{
    return end_update(rx);
}

static int start_daemon(int verbose __attribute__((unused)),
			const char *mboxname)
{
    char *socket_path = NULL;
    int r;

    r = sphinxmgr_getsock(mboxname, &socket_path);
    if (r) return r;

    free(socket_path);
    return 0;
}

static int stop_daemon(int verbose __attribute__((unused)),
		       const char *mboxname)
{
    return sphinxmgr_stop(mboxname);
}

const struct search_engine sphinx_search_engine = {
    "Sphinx",
    SEARCH_FLAG_CAN_BATCH,
    begin_search,
    end_search,
    begin_update,
    end_update,
    begin_snippets,
    end_snippets,
    free_internalised,
    start_daemon,
    stop_daemon
};
