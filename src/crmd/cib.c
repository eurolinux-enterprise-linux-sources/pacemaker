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

#include <unistd.h>  /* sleep */

#include <crm/common/alerts_internal.h>
#include <crm/common/xml.h>
#include <crm/crm.h>
#include <crm/msg_xml.h>

#include <crmd.h>
#include <crmd_callbacks.h>  /* crmd_cib_connection_destroy */
#include <crmd_fsa.h>
#include <crmd_messages.h>


struct crm_subsystem_s *cib_subsystem = NULL;

int cib_retries = 0;

static void
do_cib_updated(const char *event, xmlNode * msg)
{
    if (crm_patchset_contains_alert(msg, TRUE)) {
        mainloop_set_trigger(config_read);
    }
}

static void
revision_check_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    int cmp = -1;
    xmlNode *generation = NULL;
    const char *revision = NULL;

    if (rc != pcmk_ok) {
        fsa_data_t *msg_data = NULL;

        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
        return;
    }

    generation = output;
    CRM_CHECK(safe_str_eq(crm_element_name(generation), XML_TAG_CIB),
              crm_log_xml_err(output, __FUNCTION__); return);

    crm_trace("Checking our feature revision %s is allowed", CRM_FEATURE_SET);

    revision = crm_element_value(generation, XML_ATTR_CRM_VERSION);
    cmp = compare_version(revision, CRM_FEATURE_SET);

    if (cmp > 0) {
        crm_err("Shutting down because the current configuration is not supported by this version "
                CRM_XS " build=%s supported=%s current=%s",
                PACEMAKER_VERSION, CRM_FEATURE_SET, revision);
        /* go into a stall state */
        register_fsa_error_adv(C_FSA_INTERNAL, I_SHUTDOWN, NULL, NULL, __FUNCTION__);
        return;
    }
}

static void
do_cib_replaced(const char *event, xmlNode * msg)
{
    crm_debug("Updating the CIB after a replace: DC=%s", AM_I_DC ? "true" : "false");
    if (AM_I_DC == FALSE) {
        return;

    } else if (fsa_state == S_FINALIZE_JOIN && is_set(fsa_input_register, R_CIB_ASKED)) {
        /* no need to restart the join - we asked for this replace op */
        return;
    }

    /* start the join process again so we get everyone's LRM status */
    populate_cib_nodes(node_update_quick|node_update_all, __FUNCTION__);
    register_fsa_input(C_FSA_INTERNAL, I_ELECTION, NULL);
}

/* A_CIB_STOP, A_CIB_START, O_CIB_RESTART */
void
do_cib_control(long long action,
               enum crmd_fsa_cause cause,
               enum crmd_fsa_state cur_state,
               enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    CRM_ASSERT(fsa_cib_conn != NULL);

    if (action & A_CIB_STOP) {

        if (fsa_cib_conn->state != cib_disconnected && last_resource_update != 0) {
            crm_info("Waiting for resource update %d to complete", last_resource_update);
            crmd_fsa_stall(FALSE);
            return;
        }

        crm_info("Disconnecting CIB");
        clear_bit(fsa_input_register, R_CIB_CONNECTED);

        fsa_cib_conn->cmds->del_notify_callback(fsa_cib_conn, T_CIB_DIFF_NOTIFY, do_cib_updated);

        if (fsa_cib_conn->state != cib_disconnected) {
            fsa_cib_conn->cmds->set_slave(fsa_cib_conn, cib_scope_local);
            fsa_cib_conn->cmds->signoff(fsa_cib_conn);
        }
        crm_notice("Disconnected from the CIB");
    }

    if (action & A_CIB_START) {
        int rc = pcmk_ok;

        if (cur_state == S_STOPPING) {
            crm_err("Ignoring request to start the CIB after shutdown");
            return;
        }

        rc = fsa_cib_conn->cmds->signon(fsa_cib_conn, CRM_SYSTEM_CRMD, cib_command_nonblocking);

        if (rc != pcmk_ok) {
            /* a short wait that usually avoids stalling the FSA */
            sleep(1);
            rc = fsa_cib_conn->cmds->signon(fsa_cib_conn, CRM_SYSTEM_CRMD, cib_command_nonblocking);
        }

        if (rc != pcmk_ok) {
            crm_info("Could not connect to the CIB service: %s", pcmk_strerror(rc));

        } else if (pcmk_ok !=
                   fsa_cib_conn->cmds->set_connection_dnotify(fsa_cib_conn,
                                                              crmd_cib_connection_destroy)) {
            crm_err("Could not set dnotify callback");

        } else if (pcmk_ok !=
                   fsa_cib_conn->cmds->add_notify_callback(fsa_cib_conn, T_CIB_REPLACE_NOTIFY,
                                                           do_cib_replaced)) {
            crm_err("Could not set CIB notification callback (replace)");

        } else if (pcmk_ok !=
                   fsa_cib_conn->cmds->add_notify_callback(fsa_cib_conn, T_CIB_DIFF_NOTIFY,
                                                           do_cib_updated)) {
            crm_err("Could not set CIB notification callback (update)");

        } else {
            set_bit(fsa_input_register, R_CIB_CONNECTED);
        }

        if (is_set(fsa_input_register, R_CIB_CONNECTED) == FALSE) {

            cib_retries++;
            crm_warn("Couldn't complete CIB registration %d"
                     " times... pause and retry", cib_retries);

            if (cib_retries < 30) {
                crm_timer_start(wait_timer);
                crmd_fsa_stall(FALSE);

            } else {
                crm_err("Could not complete CIB"
                        " registration  %d times..." " hard error", cib_retries);
                register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
            }
        } else {
            int call_id = 0;

            crm_info("CIB connection established");

            call_id = fsa_cib_conn->cmds->query(fsa_cib_conn, NULL, NULL, cib_scope_local);

            fsa_register_cib_callback(call_id, FALSE, NULL, revision_check_callback);
            cib_retries = 0;
        }
    }
}

/*!
 * \internal
 * \brief Get CIB call options to use local scope if master unavailable
 *
 * \return CIB call options
 */
int crmd_cib_smart_opt()
{
    int call_opt = cib_quorum_override;

    if (fsa_state == S_ELECTION || fsa_state == S_PENDING) {
        crm_info("Sending update to local CIB in state: %s", fsa_state2string(fsa_state));
        call_opt |= cib_scope_local;
    }
    return call_opt;
}
