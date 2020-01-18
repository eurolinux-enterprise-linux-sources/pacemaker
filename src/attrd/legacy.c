/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>

#include <crm/crm.h>
#include <crm/cib/internal.h>
#include <crm/msg_xml.h>
#include <crm/pengine/rules.h>
#include <crm/common/ipc.h>
#include <crm/common/ipcs.h>
#include <crm/cluster/internal.h>
#include <crm/common/xml.h>
#include <crm/attrd.h>

#include <attrd_common.h>

#define OPTARGS	"hV"
#if SUPPORT_HEARTBEAT
ll_cluster_t *attrd_cluster_conn;
#endif

char *attrd_uname = NULL;
char *attrd_uuid = NULL;
uint32_t attrd_nodeid = 0;

GHashTable *attr_hash = NULL;
lrmd_t *the_lrmd = NULL;
crm_trigger_t *attrd_config_read = NULL;

/* Convenience macro for registering a CIB callback.
 * Check the_cib != NULL before using.
 */
#define register_cib_callback(call_id, data, fn, free_fn) \
    the_cib->cmds->register_callback_full(the_cib, call_id, 120, FALSE, \
                                           data, #fn, fn, free_fn)

typedef struct attr_hash_entry_s {
    char *uuid;
    char *id;
    char *set;
    char *section;

    char *value;
    char *stored_value;

    int timeout;
    char *dampen;
    guint timer_id;

    char *user;

} attr_hash_entry_t;

void attrd_local_callback(xmlNode * msg);
gboolean attrd_timer_callback(void *user_data);
gboolean attrd_trigger_update(attr_hash_entry_t * hash_entry);
void attrd_perform_update(attr_hash_entry_t * hash_entry);
static void update_local_attr(xmlNode *msg, attr_hash_entry_t *hash_entry);

static void
free_hash_entry(gpointer data)
{
    attr_hash_entry_t *entry = data;

    if (entry == NULL) {
        return;
    }
    free(entry->id);
    free(entry->set);
    free(entry->dampen);
    free(entry->section);
    free(entry->uuid);
    free(entry->value);
    free(entry->stored_value);
    free(entry->user);
    free(entry);
}

/* Exit code means? */
static int32_t
attrd_ipc_dispatch(qb_ipcs_connection_t * c, void *data, size_t size)
{
    uint32_t id = 0;
    uint32_t flags = 0;
    crm_client_t *client = crm_client_get(c);
    xmlNode *msg = crm_ipcs_recv(client, data, size, &id, &flags);

    crm_ipcs_send_ack(client, id, flags, "ack", __FUNCTION__, __LINE__);
    if (msg == NULL) {
        crm_debug("No msg from %d (%p)", crm_ipcs_client_pid(c), c);
        return 0;
    }
#if ENABLE_ACL
    CRM_ASSERT(client->user != NULL);
    crm_acl_get_set_user(msg, F_ATTRD_USER, client->user);
#endif

    crm_trace("Processing msg from %d (%p)", crm_ipcs_client_pid(c), c);
    crm_log_xml_trace(msg, __FUNCTION__);

    attrd_local_callback(msg);

    free_xml(msg);
    return 0;
}

static void
usage(const char *cmd, int exit_status)
{
    FILE *stream;

    stream = exit_status ? stderr : stdout;

    fprintf(stream, "usage: %s [-srkh] [-c configure file]\n", cmd);
/* 	fprintf(stream, "\t-d\tsets debug level\n"); */
/* 	fprintf(stream, "\t-s\tgets daemon status\n"); */
/* 	fprintf(stream, "\t-r\trestarts daemon\n"); */
/* 	fprintf(stream, "\t-k\tstops daemon\n"); */
/* 	fprintf(stream, "\t-h\thelp message\n"); */
    fflush(stream);

    crm_exit(exit_status);
}

static void
stop_attrd_timer(attr_hash_entry_t * hash_entry)
{
    if (hash_entry != NULL && hash_entry->timer_id != 0) {
        crm_trace("Stopping %s timer", hash_entry->id);
        g_source_remove(hash_entry->timer_id);
        hash_entry->timer_id = 0;
    }
}

static void
log_hash_entry(int level, attr_hash_entry_t * entry, const char *text)
{
    do_crm_log(level, "%s: Set: %s, Name: %s, Value: %s, Timeout: %s",
               text, entry->section, entry->id, entry->value, entry->dampen);
}

static attr_hash_entry_t *
find_hash_entry(xmlNode * msg)
{
    const char *value = NULL;
    const char *attr = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
    attr_hash_entry_t *hash_entry = NULL;

    if (attr == NULL) {
        crm_info("Ignoring message with no attribute name");
        return NULL;
    }

    hash_entry = g_hash_table_lookup(attr_hash, attr);

    if (hash_entry == NULL) {
        /* create one and add it */
        crm_info("Creating hash entry for %s", attr);
        hash_entry = calloc(1, sizeof(attr_hash_entry_t));
        hash_entry->id = strdup(attr);

        g_hash_table_insert(attr_hash, hash_entry->id, hash_entry);
        hash_entry = g_hash_table_lookup(attr_hash, attr);
        CRM_CHECK(hash_entry != NULL, return NULL);
    }

    value = crm_element_value(msg, F_ATTRD_SET);
    if (value != NULL) {
        free(hash_entry->set);
        hash_entry->set = strdup(value);
        crm_debug("\t%s->set: %s", attr, value);
    }

    value = crm_element_value(msg, F_ATTRD_SECTION);
    if (value == NULL) {
        value = XML_CIB_TAG_STATUS;
    }
    free(hash_entry->section);
    hash_entry->section = strdup(value);
    crm_trace("\t%s->section: %s", attr, value);

    value = crm_element_value(msg, F_ATTRD_DAMPEN);
    if (value != NULL) {
        free(hash_entry->dampen);
        hash_entry->dampen = strdup(value);

        hash_entry->timeout = crm_get_msec(value);
        crm_trace("\t%s->timeout: %s", attr, value);
    }
#if ENABLE_ACL
    free(hash_entry->user);
    hash_entry->user = NULL;

    value = crm_element_value(msg, F_ATTRD_USER);
    if (value != NULL) {
        hash_entry->user = strdup(value);
        crm_trace("\t%s->user: %s", attr, value);
    }
#endif

    log_hash_entry(LOG_DEBUG_2, hash_entry, "Found (and updated) entry:");
    return hash_entry;
}

/*!
 * \internal
 * \brief Clear failure-related attributes for local node
 *
 * \param[in] xml  XML of ATTRD_OP_CLEAR_FAILURE request
 */
static void
local_clear_failure(xmlNode *xml)
{
    const char *rsc = crm_element_value(xml, F_ATTRD_RESOURCE);
    const char *what = rsc? rsc : "all resources";
    const char *op = crm_element_value(xml, F_ATTRD_OPERATION);
    const char *interval_s = crm_element_value(xml, F_ATTRD_INTERVAL);
    int interval = crm_get_interval(interval_s);
    regex_t regex;
    GHashTableIter iter;
    attr_hash_entry_t *hash_entry = NULL;

    if (attrd_failure_regex(&regex, rsc, op, interval) != pcmk_ok) {
        crm_info("Ignoring invalid request to clear %s",
                 (rsc? rsc : "all resources"));
        return;
    }
    crm_debug("Clearing %s locally", what);

    /* Make sure value is not set, so we delete */
    if (crm_element_value(xml, F_ATTRD_VALUE)) {
        crm_xml_replace(xml, F_ATTRD_VALUE, NULL);
    }

    g_hash_table_iter_init(&iter, attr_hash);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &hash_entry)) {
        if (regexec(&regex, hash_entry->id, 0, NULL, 0) == 0) {
            crm_trace("Matched %s when clearing %s", hash_entry->id, what);
            update_local_attr(xml, hash_entry);
        }
    }
    regfree(&regex);
}

static void
remote_clear_callback(xmlNode *msg, int call_id, int rc, xmlNode *output,
                      void *user_data)
{
    if (rc == 0) {
        crm_debug("Successfully cleared failures using %s", (char *) user_data);
    } else {
        crm_notice("Failed to clear failures: %s " CRM_XS " call=%d xpath=%s rc=%d",
                   pcmk_strerror(rc), call_id, (char *) user_data, rc);
    }
}

/* xpath component to match an id attribute (format takes remote node name) */
#define XPATH_ID "[@" XML_ATTR_UUID "='%s']"

/* Define the start of an xpath to match a remote node transient attribute
 * (argument must be either an empty string to match for all remote nodes,
 * or XPATH_ID to match for a single remote node)
 */
#define XPATH_REMOTE_ATTR(x) "/" XML_TAG_CIB "/" XML_CIB_TAG_STATUS \
    "/" XML_CIB_TAG_STATE "[@" XML_NODE_IS_REMOTE "='true']" x \
    "/" XML_TAG_TRANSIENT_NODEATTRS "/" XML_TAG_ATTR_SETS "/" XML_CIB_TAG_NVPAIR

/* xpath component to match an attribute name exactly */
#define XPATH_NAME_IS(x) "@" XML_NVPAIR_ATTR_NAME "='" x "'"

/* xpath component to match an attribute name by prefix */
#define XPATH_NAME_START(x) "starts-with(@" XML_NVPAIR_ATTR_NAME ", '" x "')"

/* xpath ending to clear all resources */
#define XPATH_CLEAR_ALL \
    "[" XPATH_NAME_START(CRM_FAIL_COUNT_PREFIX "-") \
    " or " XPATH_NAME_START(CRM_LAST_FAILURE_PREFIX "-") "]"

/* xpath ending to clear all operations for one resource
 * (format takes resource name x 4)
 *
 * @COMPAT attributes set < 1.1.17:
 * also match older attributes that do not have the operation part
 */
#define XPATH_CLEAR_ONE \
    "[" XPATH_NAME_IS(CRM_FAIL_COUNT_PREFIX "-%s") \
    " or " XPATH_NAME_IS(CRM_LAST_FAILURE_PREFIX "-%s") \
    " or " XPATH_NAME_START(CRM_FAIL_COUNT_PREFIX "-%s#") \
    " or " XPATH_NAME_START(CRM_LAST_FAILURE_PREFIX "-%s#") "]"

/* xpath ending to clear one operation for one resource
 * (format takes resource name x 2, resource name + operation + interval x 2)
 *
 * @COMPAT attributes set < 1.1.17:
 * also match older attributes that do not have the operation part
 */
#define XPATH_CLEAR_OP \
    "[" XPATH_NAME_IS(CRM_FAIL_COUNT_PREFIX "-%s") \
    " or " XPATH_NAME_IS(CRM_LAST_FAILURE_PREFIX "-%s") \
    " or " XPATH_NAME_IS(CRM_FAIL_COUNT_PREFIX "-%s#%s_%d") \
    " or " XPATH_NAME_IS(CRM_LAST_FAILURE_PREFIX "-%s#%s_%d") "]"

/*!
 * \internal
 * \brief Clear failure-related attributes for Pacemaker Remote node(s)
 *
 * \param[in] xml  XML of ATTRD_OP_CLEAR_FAILURE request
 */
static void
remote_clear_failure(xmlNode *xml)
{
    const char *rsc = crm_element_value(xml, F_ATTRD_RESOURCE);
    const char *host = crm_element_value(xml, F_ATTRD_HOST);
    const char *op = crm_element_value(xml, F_ATTRD_OPERATION);
    int rc = pcmk_ok;
    char *xpath;

    if (the_cib == NULL) {
        crm_info("Ignoring request to clear %s on %s because not connected to CIB",
                 (rsc? rsc : "all resources"),
                 (host? host: "all remote nodes"));
        return;
    }

    /* Build an xpath to clear appropriate attributes */

    if (rsc == NULL) {
        /* No resource specified, clear all resources */

        if (host == NULL) {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR("") XPATH_CLEAR_ALL);
        } else {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR(XPATH_ID) XPATH_CLEAR_ALL,
                                      host);
        }

    } else if (op == NULL) {
        /* Resource but no operation specified, clear all operations */

        if (host == NULL) {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR("") XPATH_CLEAR_ONE,
                                      rsc, rsc, rsc, rsc);
        } else {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR(XPATH_ID) XPATH_CLEAR_ONE,
                                      host, rsc, rsc, rsc, rsc);
        }

    } else {
        /* Resource and operation specified */

        const char *interval_s = crm_element_value(xml, F_ATTRD_INTERVAL);
        int interval = crm_get_interval(interval_s);

        if (host == NULL) {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR("") XPATH_CLEAR_OP,
                                      rsc, rsc, rsc, op, interval,
                                      rsc, op, interval);
        } else {
            xpath = crm_strdup_printf(XPATH_REMOTE_ATTR(XPATH_ID) XPATH_CLEAR_OP,
                                      host, rsc, rsc, rsc, op, interval,
                                      rsc, op, interval);
        }
    }

    crm_trace("Clearing attributes matching %s", xpath);
    rc = the_cib->cmds->delete(the_cib, xpath, NULL, cib_xpath|cib_multiple);
    register_cib_callback(rc, xpath, remote_clear_callback, free);
}

static void
process_xml_request(xmlNode *xml)
{
    attr_hash_entry_t *hash_entry = NULL;
    const char *from = crm_element_value(xml, F_ORIG);
    const char *op = crm_element_value(xml, F_ATTRD_TASK);
    const char *host = crm_element_value(xml, F_ATTRD_HOST);
    const char *ignore = crm_element_value(xml, F_ATTRD_IGNORE_LOCALLY);

    if (host && safe_str_eq(host, attrd_uname)) {
        crm_info("%s relayed from %s", (op? op : "Request"), from);
        attrd_local_callback(xml);

    } else if (safe_str_eq(op, ATTRD_OP_PEER_REMOVE)) {
        CRM_CHECK(host != NULL, return);
        crm_debug("Removing %s from peer caches for %s", host, from);
        crm_remote_peer_cache_remove(host);
        reap_crm_member(0, host);

    } else if (safe_str_eq(op, ATTRD_OP_CLEAR_FAILURE)) {
        local_clear_failure(xml);

    } else if ((ignore == NULL) || safe_str_neq(from, attrd_uname)) {
        crm_trace("%s message from %s", op, from);
        hash_entry = find_hash_entry(xml);
        stop_attrd_timer(hash_entry);
        attrd_perform_update(hash_entry);
    }
}

#if SUPPORT_HEARTBEAT
static void
attrd_ha_connection_destroy(gpointer user_data)
{
    crm_trace("Invoked");
    if (attrd_shutting_down()) {
        /* we signed out, so this is expected */
        crm_info("Heartbeat disconnection complete");
        return;
    }

    crm_crit("Lost connection to heartbeat service!");
    if (attrd_mainloop_running()) {
        attrd_quit_mainloop();
        return;
    }
    crm_exit(pcmk_ok);
}

static void
attrd_ha_callback(HA_Message * msg, void *private_data)
{
    xmlNode *xml = convert_ha_message(NULL, msg, __FUNCTION__);

    process_xml_request(xml);
    free_xml(xml);
}

#endif

#if SUPPORT_COROSYNC
static void
attrd_cs_dispatch(cpg_handle_t handle,
                 const struct cpg_name *groupName,
                 uint32_t nodeid, uint32_t pid, void *msg, size_t msg_len)
{
    uint32_t kind = 0;
    xmlNode *xml = NULL;
    const char *from = NULL;
    char *data = pcmk_message_common_cs(handle, nodeid, pid, msg, &kind, &from);

    if(data == NULL) {
        return;
    }
    if (kind == crm_class_cluster) {
        xml = string2xml(data);
        if (xml == NULL) {
            crm_err("Bad message received: '%.120s'", data);
        }
    }

    if (xml != NULL) {
        /* crm_xml_add_int(xml, F_SEQ, wrapper->id); */
        crm_xml_add(xml, F_ORIG, from);
        process_xml_request(xml);
        free_xml(xml);
    }

    free(data);
}

static void
attrd_cs_destroy(gpointer unused)
{
    if (attrd_shutting_down()) {
        /* we signed out, so this is expected */
        crm_info("Corosync disconnection complete");
        return;
    }

    crm_crit("Lost connection to Corosync service!");
    if (attrd_mainloop_running()) {
        attrd_quit_mainloop();
        return;
    }
    crm_exit(EINVAL);
}
#endif

static void
attrd_cib_connection_destroy(gpointer user_data)
{
    cib_t *conn = user_data;

    conn->cmds->signoff(conn);  /* Ensure IPC is cleaned up */

    if (attrd_shutting_down()) {
        crm_info("Connection to the CIB terminated...");

    } else {
        /* eventually this will trigger a reconnect, not a shutdown */
        crm_err("Connection to the CIB terminated...");
        crm_exit(ENOTCONN);
    }

    return;
}

static void
update_for_hash_entry(gpointer key, gpointer value, gpointer user_data)
{
    attr_hash_entry_t *entry = value;

    if (entry->value != NULL || entry->stored_value != NULL) {
        attrd_timer_callback(value);
    }
}

static void
local_update_for_hash_entry(gpointer key, gpointer value, gpointer user_data)
{
    attr_hash_entry_t *entry = value;

    if (entry->timer_id == 0) {
        crm_trace("Performing local-only update after replace for %s", entry->id);
        attrd_perform_update(entry);
        /* } else {
         *     just let the timer expire and attrd_timer_callback() will do the right thing
         */
    }
}

static void
do_cib_replaced(const char *event, xmlNode * msg)
{
    crm_info("Updating all attributes after %s event", event);
    g_hash_table_foreach(attr_hash, local_update_for_hash_entry, NULL);
}

static gboolean
cib_connect(void *user_data)
{
    static int attempts = 1;
    static int max_retry = 20;
    gboolean was_err = FALSE;
    static cib_t *local_conn = NULL;

    if (local_conn == NULL) {
        local_conn = cib_new();
    }

    if (was_err == FALSE) {
        int rc = -ENOTCONN;

        if (attempts < max_retry) {
            crm_debug("CIB signon attempt %d", attempts);
            rc = local_conn->cmds->signon(local_conn, T_ATTRD, cib_command);
        }

        if (rc != pcmk_ok && attempts > max_retry) {
            crm_err("Signon to CIB failed: %s", pcmk_strerror(rc));
            was_err = TRUE;

        } else if (rc != pcmk_ok) {
            attempts++;
            return TRUE;
        }
    }

    crm_info("Connected to the CIB after %d signon attempts", attempts);

    if (was_err == FALSE) {
        int rc = local_conn->cmds->set_connection_dnotify(local_conn, attrd_cib_connection_destroy);

        if (rc != pcmk_ok) {
            crm_err("Could not set dnotify callback");
            was_err = TRUE;
        }
    }

    if (was_err == FALSE) {
        if (pcmk_ok !=
            local_conn->cmds->add_notify_callback(local_conn, T_CIB_REPLACE_NOTIFY,
                                                  do_cib_replaced)) {
            crm_err("Could not set CIB notification callback");
            was_err = TRUE;
        }
        if (was_err == FALSE) {
            if (pcmk_ok != local_conn->cmds->add_notify_callback(local_conn, T_CIB_DIFF_NOTIFY, attrd_cib_updated_cb)) {
                crm_err("Could not set CIB notification callback (update)");
                was_err = TRUE;
            }

        }
        attrd_config_read = mainloop_add_trigger(G_PRIORITY_HIGH, attrd_read_options, NULL);

        /* Reading of cib(Alert section) after the start */
        mainloop_set_trigger(attrd_config_read);
    }

    if (was_err) {
        crm_err("Aborting startup");
        crm_exit(DAEMON_RESPAWN_STOP);
    }

    the_cib = local_conn;

    crm_info("Sending full refresh now that we're connected to the cib");
    g_hash_table_foreach(attr_hash, local_update_for_hash_entry, NULL);

    return FALSE;
}

int
main(int argc, char **argv)
{
    int flag = 0;
    int argerr = 0;
    crm_cluster_t cluster;
    gboolean was_err = FALSE;
    qb_ipcs_connection_t *c = NULL;
    qb_ipcs_service_t *ipcs = NULL;

    crm_log_init(T_ATTRD, LOG_NOTICE, TRUE, FALSE, argc, argv, FALSE);
    mainloop_add_signal(SIGTERM, attrd_shutdown);

    while ((flag = getopt(argc, argv, OPTARGS)) != EOF) {
        switch (flag) {
            case 'V':
                crm_bump_log_level(argc, argv);
                break;
            case 'h':          /* Help message */
                usage(T_ATTRD, EX_OK);
                break;
            default:
                ++argerr;
                break;
        }
    }

    if (optind > argc) {
        ++argerr;
    }

    if (argerr) {
        usage(T_ATTRD, EX_USAGE);
    }

    attr_hash = g_hash_table_new_full(crm_str_hash, g_str_equal, NULL, free_hash_entry);

    crm_info("Starting up");

    if (was_err == FALSE) {

#if SUPPORT_COROSYNC
        if (is_openais_cluster()) {
            cluster.destroy = attrd_cs_destroy;
            cluster.cpg.cpg_deliver_fn = attrd_cs_dispatch;
            cluster.cpg.cpg_confchg_fn = pcmk_cpg_membership;
        }
#endif

#if SUPPORT_HEARTBEAT
        if (is_heartbeat_cluster()) {
            cluster.hb_conn = NULL;
            cluster.hb_dispatch = attrd_ha_callback;
            cluster.destroy = attrd_ha_connection_destroy;
        }
#endif

        if (FALSE == crm_cluster_connect(&cluster)) {
            crm_err("HA Signon failed");
            was_err = TRUE;
        }

        attrd_uname = cluster.uname;
        attrd_uuid = cluster.uuid;
        attrd_nodeid = cluster.nodeid;
#if SUPPORT_HEARTBEAT
        attrd_cluster_conn = cluster.hb_conn;
#endif
    }

    crm_info("Cluster connection active");

    if (was_err == FALSE) {
        attrd_init_ipc(&ipcs, attrd_ipc_dispatch);
    }

    crm_info("Accepting attribute updates");

    attrd_init_mainloop();

    if (0 == g_timeout_add_full(G_PRIORITY_LOW + 1, 5000, cib_connect, NULL, NULL)) {
        crm_info("Adding timer failed");
        was_err = TRUE;
    }

    if (was_err) {
        crm_err("Aborting startup");
        return 100;
    }

    crm_notice("Starting mainloop...");
    attrd_run_mainloop();
    crm_notice("Exiting...");

#if SUPPORT_HEARTBEAT
    if (is_heartbeat_cluster()) {
        attrd_cluster_conn->llc_ops->signoff(attrd_cluster_conn, TRUE);
        attrd_cluster_conn->llc_ops->delete(attrd_cluster_conn);
    }
#endif

    c = qb_ipcs_connection_first_get(ipcs);
    while (c != NULL) {
        qb_ipcs_connection_t *last = c;

        c = qb_ipcs_connection_next_get(ipcs, last);

        /* There really shouldn't be anyone connected at this point */
        crm_notice("Disconnecting client %p, pid=%d...", last, crm_ipcs_client_pid(last));
        qb_ipcs_disconnect(last);
        qb_ipcs_connection_unref(last);
    }

    qb_ipcs_destroy(ipcs);

    attrd_lrmd_disconnect();
    attrd_cib_disconnect();

    g_hash_table_destroy(attr_hash);
    free(attrd_uuid);

    return crm_exit(pcmk_ok);
}

struct attrd_callback_s {
    char *attr;
    char *value;
};

/*!
 * \internal
 * \brief Free an attrd callback structure
 */
static void
free_attrd_callback(void *user_data)
{
    struct attrd_callback_s *data = user_data;

    free(data->attr);
    free(data->value);
    free(data);
}

static void
attrd_cib_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    attr_hash_entry_t *hash_entry = NULL;
    struct attrd_callback_s *data = user_data;

    if (data->value == NULL && rc == -ENXIO) {
        rc = pcmk_ok;

    } else if (call_id < 0) {
        crm_warn("Update %s=%s failed: %s", data->attr, data->value, pcmk_strerror(call_id));
        return;
    }

    switch (rc) {
        case pcmk_ok:
            crm_debug("Update %d for %s=%s passed", call_id, data->attr, data->value);
            hash_entry = g_hash_table_lookup(attr_hash, data->attr);

            if (hash_entry) {
                free(hash_entry->stored_value);
                hash_entry->stored_value = NULL;
                if (data->value != NULL) {
                    hash_entry->stored_value = strdup(data->value);
                }
            }
            break;
        case -pcmk_err_diff_failed:    /* When an attr changes while the CIB is syncing */
        case -ETIME:           /* When an attr changes while there is a DC election */
        case -ENXIO:           /* When an attr changes while the CIB is syncing a
                                 *   newer config from a node that just came up
                                 */
            crm_warn("Update %d for %s=%s failed: %s",
                     call_id, data->attr, data->value, pcmk_strerror(rc));
            break;
        default:
            crm_err("Update %d for %s=%s failed: %s",
                    call_id, data->attr, data->value, pcmk_strerror(rc));
    }
}

void
attrd_perform_update(attr_hash_entry_t * hash_entry)
{
    int rc = pcmk_ok;
    struct attrd_callback_s *data = NULL;
    const char *user_name = NULL;

    if (hash_entry == NULL) {
        return;

    } else if (the_cib == NULL) {
        crm_info("Delaying operation %s=%s: cib not connected", hash_entry->id,
                 crm_str(hash_entry->value));
        return;

    }
#if ENABLE_ACL
    if (hash_entry->user) {
        user_name = hash_entry->user;
        crm_trace("Performing request from user '%s'", hash_entry->user);
    }
#endif

    if (hash_entry->value == NULL) {
        /* delete the attr */
        rc = delete_attr_delegate(the_cib, cib_none, hash_entry->section, attrd_uuid, NULL,
                                  hash_entry->set, hash_entry->uuid, hash_entry->id, NULL, FALSE,
                                  user_name);

        if (rc >= 0 && hash_entry->stored_value) {
            crm_notice("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
                       rc, attrd_uuid, hash_entry->id,
                       hash_entry->uuid ? hash_entry->uuid : "<n/a>", hash_entry->set,
                       hash_entry->section);

        } else if (rc < 0 && rc != -ENXIO) {
            crm_notice
                ("Delete operation failed: node=%s, attr=%s, id=%s, set=%s, section=%s: %s (%d)",
                 attrd_uuid, hash_entry->id, hash_entry->uuid ? hash_entry->uuid : "<n/a>",
                 hash_entry->set, hash_entry->section, pcmk_strerror(rc), rc);

        } else {
            crm_trace("Sent delete %d: node=%s, attr=%s, id=%s, set=%s, section=%s",
                      rc, attrd_uuid, hash_entry->id,
                      hash_entry->uuid ? hash_entry->uuid : "<n/a>", hash_entry->set,
                      hash_entry->section);
        }
    } else {
        /* send update */
        rc = update_attr_delegate(the_cib, cib_none, hash_entry->section,
                                  attrd_uuid, NULL, hash_entry->set, hash_entry->uuid,
                                  hash_entry->id, hash_entry->value, FALSE, user_name, NULL);
        if (rc < 0) {
            crm_notice("Could not update %s=%s: %s (%d)", hash_entry->id,
                       hash_entry->value, pcmk_strerror(rc), rc);
        } else if (safe_str_neq(hash_entry->value, hash_entry->stored_value)) {
            crm_notice("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
        } else {
            crm_trace("Sent update %d: %s=%s", rc, hash_entry->id, hash_entry->value);
        }
    }
    attrd_send_attribute_alert(attrd_uname, attrd_nodeid,
                               hash_entry->id, hash_entry->value);

    data = calloc(1, sizeof(struct attrd_callback_s));
    data->attr = strdup(hash_entry->id);
    if (hash_entry->value != NULL) {
        data->value = strdup(hash_entry->value);
    }
    register_cib_callback(rc, data, attrd_cib_callback, free_attrd_callback);
    return;
}

/*!
 * \internal
 * \brief Expand attribute values that use "++" or "+="
 *
 * \param[in] value      Attribute value to expand
 * \param[in] old_value  Previous value of attribute
 *
 * \return Newly allocated string with expanded value, or NULL if not expanded
 */
static char *
expand_attr_value(const char *value, const char *old_value)
{
    char *expanded = NULL;

    if (attrd_value_needs_expansion(value)) {
        expanded = crm_itoa(attrd_expand_value(value, old_value));
    }
    return expanded;
}

/*!
 * \internal
 * \brief Update a single node attribute for this node
 *
 * \param[in]     msg         XML message with update
 * \param[in,out] hash_entry  Node attribute structure
 */
static void
update_local_attr(xmlNode *msg, attr_hash_entry_t *hash_entry)
{
    const char *value = crm_element_value(msg, F_ATTRD_VALUE);
    char *expanded = NULL;

    if (hash_entry->uuid == NULL) {
        const char *key = crm_element_value(msg, F_ATTRD_KEY);

        if (key) {
            hash_entry->uuid = strdup(key);
        }
    }

    crm_debug("Request to update %s (%s) to %s from %s (stored: %s)",
              hash_entry->id, (hash_entry->uuid? hash_entry->uuid : "no uuid"),
              value, hash_entry->value, hash_entry->stored_value);

    if (safe_str_eq(value, hash_entry->value)
        && safe_str_eq(value, hash_entry->stored_value)) {
        crm_trace("Ignoring non-change");
        return;

    } else if (value) {
        expanded = expand_attr_value(value, hash_entry->value);
        if (expanded) {
            crm_info("Expanded %s=%s to %s", hash_entry->id, value, expanded);
            value = expanded;
        }
    }

    if (safe_str_eq(value, hash_entry->value) && hash_entry->timer_id) {
        /* We're already waiting to set this value */
        free(expanded);
        return;
    }

    free(hash_entry->value);
    hash_entry->value = NULL;
    if (value != NULL) {
        hash_entry->value = (expanded? expanded : strdup(value));
        crm_debug("New value of %s is %s", hash_entry->id, value);
    }

    stop_attrd_timer(hash_entry);

    if (hash_entry->timeout > 0) {
        hash_entry->timer_id = g_timeout_add(hash_entry->timeout, attrd_timer_callback, hash_entry);
    } else {
        attrd_trigger_update(hash_entry);
    }
}

/*!
 * \internal
 * \brief Log the result of a CIB operation for a remote attribute
 *
 * \param[in] msg     ignored
 * \param[in] id      CIB operation ID
 * \param[in] rc      CIB operation result
 * \param[in] output  ignored
 * \param[in] data    User-friendly string describing operation
 */
static void
remote_attr_callback(xmlNode *msg, int id, int rc, xmlNode *output, void *data)
{
    if (rc == pcmk_ok) {
        crm_debug("%s succeeded " CRM_XS " call=%d", (char *) data, id);
    } else {
        crm_notice("%s failed: %s " CRM_XS " call=%d rc=%d",
                   (char *) data, pcmk_strerror(rc), id, rc);
    }
}

/*!
 * \internal
 * \brief Update a Pacemaker Remote node attribute via CIB only
 *
 * \param[in] host       Pacemaker Remote node name
 * \param[in] name       Attribute name
 * \param[in] value      New attribute value
 * \param[in] section    CIB section to update (defaults to status if NULL)
 * \param[in] user_name  User to perform operation as
 *
 * \note Legacy attrd does not track remote node attributes, so such requests
 *       are only sent to the CIB. This means that dampening is ignored, and
 *       updates for the same attribute submitted to different nodes cannot be
 *       reliably ordered. This is not ideal, but allows remote nodes to
 *       be supported, and should be acceptable in practice.
 */
static void
update_remote_attr(const char *host, const char *name, const char *value,
                   const char *section, const char *user_name)
{
    int rc = pcmk_ok;
    char *desc;

    if (value == NULL) {
        desc = crm_strdup_printf("Delete of %s in %s for %s",
                                 name, section, host);
    } else {
        desc = crm_strdup_printf("Update of %s=%s in %s for %s",
                                 name, value, section, host);
    }

    if (name == NULL) {
        rc = -EINVAL;
    } else if (the_cib == NULL) {
        rc = -ENOTCONN;
    }
    if (rc != pcmk_ok) {
        remote_attr_callback(NULL, rc, rc, NULL, desc);
        free(desc);
        return;
    }

    if (value == NULL) {
        rc = delete_attr_delegate(the_cib, cib_none, section,
                                  host, NULL, NULL, NULL, name, NULL,
                                  FALSE, user_name);
    } else {
        rc = update_attr_delegate(the_cib, cib_none, section,
                                  host, NULL, NULL, NULL, name, value,
                                  FALSE, user_name, "remote");
    }

    attrd_send_attribute_alert(host, 0, name, (value? value : ""));

    crm_trace("%s submitted as CIB call %d", desc, rc);
    register_cib_callback(rc, desc, remote_attr_callback, free);
}

/*!
 * \internal
 * \brief Handle a client request to clear failures
 *
 * \param[in] msg  XML of request
 *
 * \note Handling is according to the host specified in the request:
 *       NULL: Relay to all cluster nodes (which do local_clear_failure())
 *          and also handle all remote nodes here, using remote_clear_failure();
 *       Our uname: Handle here, using local_clear_failure();
 *       Known peer: Relay to that peer, which (via process_xml_message() then
 *          attrd_local_callback()) comes back here as previous case;
 *       Unknown peer: Handle here as remote node, using remote_clear_failure()
 */
static void
attrd_client_clear_failure(xmlNode *msg)
{
    const char *host = crm_element_value(msg, F_ATTRD_HOST);

    if (host == NULL) {
        /* Clear failure on all cluster nodes */
        crm_notice("Broadcasting request to clear failure on all hosts");
        send_cluster_message(NULL, crm_msg_attrd, msg, FALSE);

        /* Clear failure on all remote nodes */
        remote_clear_failure(msg);

    } else if (safe_str_eq(host, attrd_uname)) {
        local_clear_failure(msg);

    } else {
        int is_remote = FALSE;
        crm_node_t *peer = crm_find_peer(0, host);

        crm_element_value_int(msg, F_ATTRD_IS_REMOTE, &is_remote);

        if (is_remote || (peer == NULL)) {
            /* If request is not for a known cluster node, assume remote */
            remote_clear_failure(msg);
        } else {
            /* Relay request to proper node */
            crm_notice("Relaying request to clear failure to %s", host);
            send_cluster_message(peer, crm_msg_attrd, msg, FALSE);
        }
    }
}

void
attrd_local_callback(xmlNode * msg)
{
    attr_hash_entry_t *hash_entry = NULL;
    const char *from = crm_element_value(msg, F_ORIG);
    const char *op = crm_element_value(msg, F_ATTRD_TASK);
    const char *attr = crm_element_value(msg, F_ATTRD_ATTRIBUTE);
    const char *pattern = crm_element_value(msg, F_ATTRD_REGEX);
    const char *value = crm_element_value(msg, F_ATTRD_VALUE);
    const char *host = crm_element_value(msg, F_ATTRD_HOST);
    int is_remote = FALSE;

    crm_element_value_int(msg, F_ATTRD_IS_REMOTE, &is_remote);

    if (safe_str_eq(op, ATTRD_OP_REFRESH)) {
        crm_notice("Sending full refresh (origin=%s)", from);
        g_hash_table_foreach(attr_hash, update_for_hash_entry, NULL);
        return;

    } else if (safe_str_eq(op, ATTRD_OP_PEER_REMOVE)) {
        if (host) {
            crm_notice("Broadcasting removal of peer %s", host);
            send_cluster_message(NULL, crm_msg_attrd, msg, FALSE);
        }
        return;

    } else if (safe_str_eq(op, ATTRD_OP_CLEAR_FAILURE)) {
        attrd_client_clear_failure(msg);
        return;

    } else if (op && safe_str_neq(op, ATTRD_OP_UPDATE)) {
        crm_notice("Ignoring unsupported %s request from %s", op, from);
        return;
    }

    /* Handle requests for Pacemaker Remote nodes specially */
    if (host && is_remote) {
        const char *section = crm_element_value(msg, F_ATTRD_SECTION);
        const char *user_name = crm_element_value(msg, F_ATTRD_USER);

        if (section == NULL) {
            section = XML_CIB_TAG_STATUS;
        }
        if ((attr == NULL) && (pattern != NULL)) {
            /* Attribute(s) specified by regular expression */
            /* @TODO query, iterate and update_remote_attr() for matches? */
            crm_notice("Update of %s for %s failed: regular expressions "
                       "are not supported with Pacemaker Remote nodes",
                       pattern, host);
        } else {
            /* Single attribute specified by exact name */
            update_remote_attr(host, attr, value, section, user_name);
        }
        return;
    }

    /* Redirect requests for another cluster node to that node */
    if (host != NULL && safe_str_neq(host, attrd_uname)) {
        send_cluster_message(crm_get_peer(0, host), crm_msg_attrd, msg, FALSE);
        return;
    }

    if (attr != NULL) {
        /* Single attribute specified by exact name */
        crm_debug("%s message from %s: %s=%s", op, from, attr, crm_str(value));
        hash_entry = find_hash_entry(msg);
        if (hash_entry != NULL) {
            update_local_attr(msg, hash_entry);
        }

    } else if (pattern != NULL) {
        /* Attribute(s) specified by regular expression */
        regex_t regex;
        GHashTableIter iter;

        if (regcomp(&regex, pattern, REG_EXTENDED|REG_NOSUB)) {
            crm_err("Update from %s failed: invalid pattern %s",
                    from, pattern);
            return;
        }

        crm_debug("%s message from %s: %s=%s",
                  op, from, pattern, crm_str(value));
        g_hash_table_iter_init(&iter, attr_hash);
        while (g_hash_table_iter_next(&iter, NULL, (gpointer *) &hash_entry)) {
            int rc = regexec(&regex, hash_entry->id, 0, NULL, 0);

            if (rc == 0) {
                crm_trace("Attribute %s matches %s", hash_entry->id, pattern);
                update_local_attr(msg, hash_entry);
            }
        }
        regfree(&regex);

    } else {
        crm_info("Ignoring message with no attribute name or expression");
    }
}

gboolean
attrd_timer_callback(void *user_data)
{
    stop_attrd_timer(user_data);
    attrd_trigger_update(user_data);
    return TRUE;                /* Always return true, removed cleanly by stop_attrd_timer() */
}

gboolean
attrd_trigger_update(attr_hash_entry_t * hash_entry)
{
    xmlNode *msg = NULL;

    /* send HA message to everyone */
    crm_notice("Sending flush op to all hosts for: %s (%s)",
               hash_entry->id, crm_str(hash_entry->value));
    log_hash_entry(LOG_DEBUG_2, hash_entry, "Sending flush op to all hosts for:");

    msg = create_xml_node(NULL, __FUNCTION__);
    crm_xml_add(msg, F_TYPE, T_ATTRD);
    crm_xml_add(msg, F_ORIG, attrd_uname);
    crm_xml_add(msg, F_ATTRD_TASK, "flush");
    crm_xml_add(msg, F_ATTRD_ATTRIBUTE, hash_entry->id);
    crm_xml_add(msg, F_ATTRD_SET, hash_entry->set);
    crm_xml_add(msg, F_ATTRD_SECTION, hash_entry->section);
    crm_xml_add(msg, F_ATTRD_DAMPEN, hash_entry->dampen);
    crm_xml_add(msg, F_ATTRD_VALUE, hash_entry->value);
#if ENABLE_ACL
    if (hash_entry->user) {
        crm_xml_add(msg, F_ATTRD_USER, hash_entry->user);
    }
#endif

    if (hash_entry->timeout <= 0) {
        crm_xml_add(msg, F_ATTRD_IGNORE_LOCALLY, hash_entry->value);
        attrd_perform_update(hash_entry);
    }

    send_cluster_message(NULL, crm_msg_attrd, msg, FALSE);
    free_xml(msg);

    return TRUE;
}
