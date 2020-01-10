/*
 * Copyright (c) 2015 David Vossel <davidvossel@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <crm_internal.h>

#include <glib.h>
#include <unistd.h>

#include <crm/crm.h>
#include <crm/msg_xml.h>
#include <crm/services.h>
#include <crm/common/mainloop.h>
#include <crm/common/alerts_internal.h>
#include <crm/common/iso8601_internal.h>
#include <crm/lrmd_alerts_internal.h>

#include <crm/pengine/status.h>
#include <crm/cib.h>
#include <crm/lrmd.h>

static lrmd_key_value_t *
alert_key2param(lrmd_key_value_t *head, enum crm_alert_keys_e name,
                const char *value)
{
    const char **key;

    if (value == NULL) {
        value = "";
    }
    for (key = crm_alert_keys[name]; *key; key++) {
        crm_trace("Setting alert key %s = '%s'", *key, value);
        head = lrmd_key_value_add(head, *key, value);
    }
    return head;
}

static lrmd_key_value_t *
alert_key2param_int(lrmd_key_value_t *head, enum crm_alert_keys_e name,
                    int value)
{
    char *value_s = crm_itoa(value);

    head = alert_key2param(head, name, value_s);
    free(value_s);
    return head;
}

static void
set_ev_kv(gpointer key, gpointer value, gpointer user_data)
{
    lrmd_key_value_t **head = (lrmd_key_value_t **) user_data;

    if (value) {
        crm_trace("Setting environment variable %s='%s'",
                  (char*)key, (char*)value);
        *head = lrmd_key_value_add(*head, key, value);
    }
}

static lrmd_key_value_t *
alert_envvar2params(lrmd_key_value_t *head, crm_alert_entry_t *entry)
{
    if (entry->envvars) {
        g_hash_table_foreach(entry->envvars, set_ev_kv, &head);
    }
    return head;
}

/*
 * We could use g_strv_contains() instead of this function,
 * but that has only been available since glib 2.43.2.
 */
static gboolean
is_target_alert(char **list, const char *value)
{
    int target_list_num = 0;
    gboolean rc = FALSE;

    CRM_CHECK(value != NULL, return FALSE);

    if (list == NULL) {
        return TRUE;
    }

    target_list_num = g_strv_length(list);

    for (int cnt = 0; cnt < target_list_num; cnt++) {
        if (strcmp(list[cnt], value) == 0) {
            rc = TRUE;
            break;
        }
    }
    return rc;
}

/*!
 * \internal
 * \brief Execute alert agents for an event
 *
 * \param[in]     lrmd        LRMD connection to use
 * \param[in]     alert_list  Alerts to execute
 * \param[in]     kind        Type of event that is being alerted for
 * \param[in]     attr_name   If crm_alert_attribute, the attribute name
 * \param[in,out] params      Environment variables to pass to agents
 *
 * \retval pcmk_ok on success
 * \retval -1 if some alerts failed
 * \retval -2 if all alerts failed
 */
static int
exec_alert_list(lrmd_t *lrmd, GList *alert_list, enum crm_alert_flags kind,
                const char *attr_name, lrmd_key_value_t *params)
{
    bool any_success = FALSE, any_failure = FALSE;
    const char *kind_s = crm_alert_flag2text(kind);
    crm_time_hr_t *now = NULL;

    params = alert_key2param(params, CRM_alert_kind, kind_s);
    params = alert_key2param(params, CRM_alert_version, VERSION);

    for (GList *iter = g_list_first(alert_list); iter; iter = g_list_next(iter)) {
        crm_alert_entry_t *entry = (crm_alert_entry_t *)(iter->data);
        lrmd_key_value_t *copy_params = NULL;
        lrmd_key_value_t *head = NULL;
        int rc;

        if (is_not_set(entry->flags, kind)) {
            crm_trace("Filtering unwanted %s alert to %s via %s",
                      kind_s, entry->recipient, entry->id);
            continue;
        }

        if ((kind == crm_alert_attribute)
            && !is_target_alert(entry->select_attribute_name, attr_name)) {

            crm_trace("Filtering unwanted attribute '%s' alert to %s via %s",
                      attr_name, entry->recipient, entry->id);
            continue;
        }

        if (now == NULL) {
            now = crm_time_hr_new(NULL);
        }
        crm_info("Sending %s alert via %s to %s",
                 kind_s, entry->id, entry->recipient);

        /* Make a copy of the parameters, because each alert will be unique */
        for (head = params; head != NULL; head = head->next) {
            copy_params = lrmd_key_value_add(copy_params, head->key, head->value);
        }

        copy_params = alert_key2param(copy_params, CRM_alert_recipient,
                                      entry->recipient);

        if (now) {
            char *timestamp = crm_time_format_hr(entry->tstamp_format, now);

            if (timestamp) {
                copy_params = alert_key2param(copy_params, CRM_alert_timestamp,
                                              timestamp);
                free(timestamp);
            }
        }

        copy_params = alert_envvar2params(copy_params, entry);

        rc = lrmd->cmds->exec_alert(lrmd, entry->id, entry->path,
                                    entry->timeout, copy_params);
        if (rc < 0) {
            crm_err("Could not execute alert %s: %s " CRM_XS " rc=%d",
                    entry->id, pcmk_strerror(rc), rc);
            any_failure = TRUE;
        } else {
            any_success = TRUE;
        }
    }

    if (now) {
        free(now);
    }

    if (any_failure) {
        return (any_success? -1 : -2);
    }
    return pcmk_ok;
}

/*!
 * \internal
 * \brief Send an alert for a node attribute change
 *
 * \param[in] lrmd        LRMD connection to use
 * \param[in] alert_list  List of alert agents to execute
 * \param[in] node        Name of node with attribute change
 * \param[in] nodeid      Node ID of node with attribute change
 * \param[in] attr_name   Name of attribute that changed
 * \param[in] attr_value  New value of attribute that changed
 *
 * \retval pcmk_ok on success
 * \retval -1 if some alert agents failed
 * \retval -2 if all alert agents failed
 */
int
lrmd_send_attribute_alert(lrmd_t *lrmd, GList *alert_list,
                          const char *node, uint32_t nodeid,
                          const char *attr_name, const char *attr_value)
{
    int rc = pcmk_ok;
    lrmd_key_value_t *params = NULL;

    if (lrmd == NULL) {
        return -2;
    }

    params = alert_key2param(params, CRM_alert_node, node);
    params = alert_key2param_int(params, CRM_alert_nodeid, nodeid);
    params = alert_key2param(params, CRM_alert_attribute_name, attr_name);
    params = alert_key2param(params, CRM_alert_attribute_value, attr_value);

    rc = exec_alert_list(lrmd, alert_list, crm_alert_attribute, attr_name,
                         params);
    lrmd_key_value_freeall(params);
    return rc;
}

/*!
 * \internal
 * \brief Send an alert for a node membership event
 *
 * \param[in] lrmd        LRMD connection to use
 * \param[in] alert_list  List of alert agents to execute
 * \param[in] node        Name of node with change
 * \param[in] nodeid      Node ID of node with change
 * \param[in] state       New state of node with change
 *
 * \retval pcmk_ok on success
 * \retval -1 if some alert agents failed
 * \retval -2 if all alert agents failed
 */
int
lrmd_send_node_alert(lrmd_t *lrmd, GList *alert_list,
                     const char *node, uint32_t nodeid, const char *state)
{
    int rc = pcmk_ok;
    lrmd_key_value_t *params = NULL;

    if (lrmd == NULL) {
        return -2;
    }

    params = alert_key2param(params, CRM_alert_node, node);
    params = alert_key2param(params, CRM_alert_desc, state);
    params = alert_key2param_int(params, CRM_alert_nodeid, nodeid);

    rc = exec_alert_list(lrmd, alert_list, crm_alert_node, NULL, params);
    lrmd_key_value_freeall(params);
    return rc;
}

/*!
 * \internal
 * \brief Send an alert for a fencing event
 *
 * \param[in] lrmd        LRMD connection to use
 * \param[in] alert_list  List of alert agents to execute
 * \param[in] target      Name of fence target node
 * \param[in] task        Type of fencing event that occurred
 * \param[in] desc        Readable description of event
 * \param[in] op_rc       Result of fence action
 *
 * \retval pcmk_ok on success
 * \retval -1 if some alert agents failed
 * \retval -2 if all alert agents failed
 */
int
lrmd_send_fencing_alert(lrmd_t *lrmd, GList *alert_list,
                        const char *target, const char *task, const char *desc,
                        int op_rc)
{
    int rc = pcmk_ok;
    lrmd_key_value_t *params = NULL;

    if (lrmd == NULL) {
        return -2;
    }

    params = alert_key2param(params, CRM_alert_node, target);
    params = alert_key2param(params, CRM_alert_task, task);
    params = alert_key2param(params, CRM_alert_desc, desc);
    params = alert_key2param_int(params, CRM_alert_rc, op_rc);

    rc = exec_alert_list(lrmd, alert_list, crm_alert_fencing, NULL, params);
    lrmd_key_value_freeall(params);
    return rc;
}

/*!
 * \internal
 * \brief Send an alert for a resource operation
 *
 * \param[in] lrmd        LRMD connection to use
 * \param[in] alert_list  List of alert agents to execute
 * \param[in] node        Name of node that executed operation
 * \param[in] op          Resource operation
 *
 * \retval pcmk_ok on success
 * \retval -1 if some alert agents failed
 * \retval -2 if all alert agents failed
 */
int
lrmd_send_resource_alert(lrmd_t *lrmd, GList *alert_list,
                         const char *node, lrmd_event_data_t *op)
{
    int rc = pcmk_ok;
    int target_rc = pcmk_ok;
    lrmd_key_value_t *params = NULL;

    if (lrmd == NULL) {
        return -2;
    }

    target_rc = rsc_op_expected_rc(op);
    if ((op->interval == 0) && (target_rc == op->rc)
        && safe_str_eq(op->op_type, RSC_STATUS)) {

        /* Don't send alerts for probes with the expected result. Leave it up to
         * the agent whether to alert for 'failed' probes. (Even if we find a
         * resource running, it was probably because someone did a clean-up of
         * the status section.)
         */
        return pcmk_ok;
    }

    params = alert_key2param(params, CRM_alert_node, node);
    params = alert_key2param(params, CRM_alert_rsc, op->rsc_id);
    params = alert_key2param(params, CRM_alert_task, op->op_type);
    params = alert_key2param_int(params, CRM_alert_interval, op->interval);
    params = alert_key2param_int(params, CRM_alert_target_rc, target_rc);
    params = alert_key2param_int(params, CRM_alert_status, op->op_status);
    params = alert_key2param_int(params, CRM_alert_rc, op->rc);

    if (op->op_status == PCMK_LRM_OP_DONE) {
        params = alert_key2param(params, CRM_alert_desc, services_ocf_exitcode_str(op->rc));
    } else {
        params = alert_key2param(params, CRM_alert_desc, services_lrm_status_str(op->op_status));
    }

    rc = exec_alert_list(lrmd, alert_list, crm_alert_resource, NULL, params);
    lrmd_key_value_freeall(params);
    return rc;
}
