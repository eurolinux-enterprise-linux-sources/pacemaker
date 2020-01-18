/*
 * Copyright 2004-2019 Andrew Beekhof <andrew@beekhof.net>
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

#include <sys/time.h>
#include <sys/resource.h>

#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crm/cluster/internal.h>
#include <crm/cluster/election.h>
#include <crm/crm.h>
#include <crmd.h>
#include <crmd_fsa.h>
#include <crmd_messages.h>
#include <crmd_callbacks.h>
#include <tengine.h>

static election_t *fsa_election = NULL;

static gboolean
election_win_cb(gpointer data)
{
    register_fsa_input(C_FSA_INTERNAL, I_ELECTION_DC, NULL);
    return FALSE;
}

void
controld_election_init(const char *uname)
{
    fsa_election = election_init("DC", uname, 60000 /*60s*/, election_win_cb);
}

void
controld_remove_voter(const char *uname)
{
    election_remove(fsa_election, uname);

    if (safe_str_eq(uname, fsa_our_dc)) {
        /* Clear any election dampening in effect. Otherwise, if the lost DC had
         * just won, an immediate new election could fizzle out with no new DC.
         */
        election_clear_dampening(fsa_election);
    }
}

void
controld_stop_election_timeout()
{
    election_timeout_stop(fsa_election);
}

void
controld_election_fini()
{
    election_fini(fsa_election);
    fsa_election = NULL;
}

void
controld_set_election_period(const char *value)
{
    election_timeout_set_period(fsa_election, crm_get_msec(value));
}

void
controld_stop_election_timer()
{
    election_timeout_stop(fsa_election);
}

/*	A_ELECTION_VOTE	*/
void
do_election_vote(long long action,
                 enum crmd_fsa_cause cause,
                 enum crmd_fsa_state cur_state,
                 enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    gboolean not_voting = FALSE;

    /* don't vote if we're in one of these states or wanting to shut down */
    switch (cur_state) {
        case S_STARTING:
        case S_RECOVERY:
        case S_STOPPING:
        case S_TERMINATE:
            crm_warn("Not voting in election, we're in state %s", fsa_state2string(cur_state));
            not_voting = TRUE;
            break;
        case S_ELECTION:
        case S_INTEGRATION:
        case S_RELEASE_DC:
            break;
        default:
            crm_err("Broken? Voting in state %s", fsa_state2string(cur_state));
            break;
    }

    if (not_voting == FALSE) {
        if (is_set(fsa_input_register, R_STARTING)) {
            not_voting = TRUE;
        }
    }

    if (not_voting) {
        if (AM_I_DC) {
            register_fsa_input(C_FSA_INTERNAL, I_RELEASE_DC, NULL);

        } else {
            register_fsa_input(C_FSA_INTERNAL, I_PENDING, NULL);
        }
        return;
    }

    election_vote(fsa_election);
    return;
}

void
do_election_check(long long action,
                  enum crmd_fsa_cause cause,
                  enum crmd_fsa_state cur_state,
                  enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (fsa_state == S_ELECTION) {
        election_check(fsa_election);
    } else {
        crm_debug("Ignoring election check because we are not in an election");
    }
}

#define loss_dampen 2           /* in seconds */

/*	A_ELECTION_COUNT	*/
void
do_election_count_vote(long long action,
                       enum crmd_fsa_cause cause,
                       enum crmd_fsa_state cur_state,
                       enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    enum election_result rc = 0;
    ha_msg_input_t *vote = fsa_typed_data(fsa_dt_ha_msg);

    if(crm_peer_cache == NULL) {
        if(is_not_set(fsa_input_register, R_SHUTDOWN)) {
            crm_err("Internal error, no peer cache");
        }
        return;
    }

    rc = election_count_vote(fsa_election, vote->msg, cur_state != S_STARTING);
    switch(rc) {
        case election_start:
            election_reset(fsa_election);
            register_fsa_input(C_FSA_INTERNAL, I_ELECTION, NULL);
            break;

        case election_lost:
            update_dc(NULL);

            if (fsa_input_register & R_THE_DC) {
                register_fsa_input(C_FSA_INTERNAL, I_RELEASE_DC, NULL);
                fsa_cib_conn->cmds->set_slave(fsa_cib_conn, cib_scope_local);

            } else if (cur_state != S_STARTING) {
                register_fsa_input(C_FSA_INTERNAL, I_PENDING, NULL);
            }
            break;

        default:
            crm_trace("Election message resulted in state %d", rc);
    }
}

/*	A_ELECT_TIMER_START, A_ELECTION_TIMEOUT 	*/
/* we won */
void
do_election_timer_ctrl(long long action,
                       enum crmd_fsa_cause cause,
                       enum crmd_fsa_state cur_state,
                       enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
}

static void
feature_update_callback(xmlNode * msg, int call_id, int rc, xmlNode * output, void *user_data)
{
    if (rc != pcmk_ok) {
        fsa_data_t *msg_data = NULL;

        crm_notice("Feature update failed: %s "CRM_XS" rc=%d",
                   pcmk_strerror(rc), rc);
        register_fsa_error(C_FSA_INTERNAL, I_ERROR, NULL);
    }
}

/*	 A_DC_TAKEOVER	*/
void
do_dc_takeover(long long action,
               enum crmd_fsa_cause cause,
               enum crmd_fsa_state cur_state,
               enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    int rc = pcmk_ok;
    xmlNode *cib = NULL;
    const char *cluster_type = name_for_cluster_type(get_cluster_type());
    pid_t watchdog = pcmk_locate_sbd();

    crm_info("Taking over DC status for this partition");
    set_bit(fsa_input_register, R_THE_DC);
    execute_stonith_cleanup();

#if SUPPORT_COROSYNC
    if (is_classic_ais_cluster()) {
        send_cluster_text(crm_class_quorum, NULL, TRUE, NULL, crm_msg_ais);
    }
#endif

    election_reset(fsa_election);
    set_bit(fsa_input_register, R_JOIN_OK);
    set_bit(fsa_input_register, R_INVOKE_PE);

    fsa_cib_conn->cmds->set_master(fsa_cib_conn, cib_scope_local);

    cib = create_xml_node(NULL, XML_TAG_CIB);
    crm_xml_add(cib, XML_ATTR_CRM_VERSION, CRM_FEATURE_SET);
    fsa_cib_update(XML_TAG_CIB, cib, cib_quorum_override, rc, NULL);
    fsa_register_cib_callback(rc, FALSE, NULL, feature_update_callback);

    update_attr_delegate(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG, NULL, NULL, NULL, NULL,
                         XML_ATTR_HAVE_WATCHDOG, watchdog?"true":"false", FALSE, NULL, NULL);

    update_attr_delegate(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG, NULL, NULL, NULL, NULL,
                         "dc-version", PACEMAKER_VERSION "-" BUILD_VERSION, FALSE, NULL, NULL);

    update_attr_delegate(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG, NULL, NULL, NULL, NULL,
                         "cluster-infrastructure", cluster_type, FALSE, NULL, NULL);

#if SUPPORT_COROSYNC
#  if !SUPPORT_PLUGIN
    if (fsa_cluster_name == NULL && is_corosync_cluster()) {
        char *cluster_name = corosync_cluster_name();

        if (cluster_name) {
            update_attr_delegate(fsa_cib_conn, cib_none, XML_CIB_TAG_CRMCONFIG, NULL, NULL, NULL, NULL,
                                 "cluster-name", cluster_name, FALSE, NULL, NULL);
        }
        free(cluster_name);
    }
#  endif
#endif

    mainloop_set_trigger(config_read);
    free_xml(cib);
}

/*	 A_DC_RELEASE	*/
void
do_dc_release(long long action,
              enum crmd_fsa_cause cause,
              enum crmd_fsa_state cur_state,
              enum crmd_fsa_input current_input, fsa_data_t * msg_data)
{
    if (action & A_DC_RELEASE) {
        crm_debug("Releasing the role of DC");
        clear_bit(fsa_input_register, R_THE_DC);

    } else if (action & A_DC_RELEASED) {
        crm_info("DC role released");
#if 0
        if (are there errors) {
            /* we can't stay up if not healthy */
            /* or perhaps I_ERROR and go to S_RECOVER? */
            result = I_SHUTDOWN;
        }
#endif
        if (is_set(fsa_input_register, R_SHUTDOWN)) {
            xmlNode *update = NULL;
            crm_node_t *node = crm_get_peer(0, fsa_our_uname);

            crm_update_peer_expected(__FUNCTION__, node, CRMD_JOINSTATE_DOWN);
            update = create_node_state_update(node, node_update_expected, NULL,
                                              __FUNCTION__);
            fsa_cib_anon_update(XML_CIB_TAG_STATUS, update);
            free_xml(update);
        }
        register_fsa_input(C_FSA_INTERNAL, I_RELEASE_SUCCESS, NULL);

    } else {
        crm_err("Unknown DC action %s", fsa_action2string(action));
    }

    crm_trace("Am I still the DC? %s", AM_I_DC ? XML_BOOLEAN_YES : XML_BOOLEAN_NO);

}
