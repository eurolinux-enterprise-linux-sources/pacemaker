/*
 * Copyright 2004-2018 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#include <crm/crm.h>
#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <stonith/stonith.h>

#define LHA_STONITH_LIBRARY "libstonith.so.1"

static void *lha_agents_lib = NULL;

static const char META_TEMPLATE[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE resource-agent SYSTEM \"ra-api-1.dtd\">\n"
    "<resource-agent name=\"%s\">\n"
    "  <version>1.0</version>\n"
    "  <longdesc lang=\"en\">\n"
    "%s\n"
    "  </longdesc>\n"
    "  <shortdesc lang=\"en\">%s</shortdesc>\n"
    "%s\n"
    "  <actions>\n"
    "    <action name=\"start\"   timeout=\"20\" />\n"
    "    <action name=\"stop\"    timeout=\"15\" />\n"
    "    <action name=\"status\"  timeout=\"20\" />\n"
    "    <action name=\"monitor\" timeout=\"20\" interval=\"3600\"/>\n"
    "    <action name=\"meta-data\"  timeout=\"15\" />\n"
    "  </actions>\n"
    "  <special tag=\"heartbeat\">\n"
    "    <version>2.0</version>\n" "  </special>\n" "</resource-agent>\n";

/*!
 * \brief Determine namespace of a fence agent
 *
 * \param[in] agent        Fence agent type
 * \param[in] namespace_s  Name of agent namespace as string, if known
 *
 * \return Namespace of specified agent, as enum value
 */
bool
stonith__agent_is_lha(const char *agent)
{
    Stonith *stonith_obj = NULL;

    static gboolean need_init = TRUE;
    static Stonith *(*st_new_fn) (const char *) = NULL;
    static void (*st_del_fn) (Stonith *) = NULL;

    if (need_init) {
        need_init = FALSE;
        st_new_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                          "stonith_new", FALSE);
        st_del_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                          "stonith_delete", FALSE);
    }

    if (lha_agents_lib && st_new_fn && st_del_fn) {
        stonith_obj = (*st_new_fn) (agent);
        if (stonith_obj) {
            (*st_del_fn) (stonith_obj);
            return TRUE;
        }
    }
    return FALSE;
}

int
stonith__list_lha_agents(stonith_key_value_t **devices)
{
    static gboolean need_init = TRUE;

    int count = 0;
    char **entry = NULL;
    char **type_list = NULL;
    static char **(*type_list_fn) (void) = NULL;
    static void (*type_free_fn) (char **) = NULL;

    if (need_init) {
        need_init = FALSE;
        type_list_fn =
            find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY, "stonith_types", FALSE);
        type_free_fn =
            find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY, "stonith_free_hostlist",
                                  FALSE);
    }

    if (type_list_fn) {
        type_list = (*type_list_fn) ();
    }

    for (entry = type_list; entry != NULL && *entry; ++entry) {
        crm_trace("Added: %s", *entry);
        *devices = stonith_key_value_add(*devices, NULL, *entry);
        count++;
    }
    if (type_list && type_free_fn) {
        (*type_free_fn) (type_list);
    }
    return count;
}

static inline char *
strdup_null(const char *val)
{
    if (val) {
        return strdup(val);
    }
    return NULL;
}

static void
stonith_plugin(int priority, const char *fmt, ...) __attribute__((__format__ (__printf__, 2, 3)));

static void
stonith_plugin(int priority, const char *format, ...)
{
    int err = errno;

    va_list ap;
    int len = 0;
    char *string = NULL;

    va_start(ap, format);

    len = vasprintf (&string, format, ap);
    va_end(ap);
    CRM_ASSERT(len > 0);

    do_crm_log_alias(priority, __FILE__, __func__, __LINE__, "%s", string);

    free(string);
    errno = err;
}

int
stonith__lha_metadata(const char *agent, int timeout, char **output)
{
    int rc = 0;
    char *buffer = NULL;
    static const char *no_parameter_info = "<!-- no value -->";

    Stonith *stonith_obj = NULL;

    static gboolean need_init = TRUE;
    static Stonith *(*st_new_fn) (const char *) = NULL;
    static const char *(*st_info_fn) (Stonith *, int) = NULL;
    static void (*st_del_fn) (Stonith *) = NULL;
    static void (*st_log_fn) (Stonith *, PILLogFun) = NULL;

    if (need_init) {
        need_init = FALSE;
        st_new_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                          "stonith_new", FALSE);
        st_del_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                          "stonith_delete", FALSE);
        st_log_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                          "stonith_set_log", FALSE);
        st_info_fn = find_library_function(&lha_agents_lib, LHA_STONITH_LIBRARY,
                                           "stonith_get_info", FALSE);
    }

    if (lha_agents_lib && st_new_fn && st_del_fn && st_info_fn && st_log_fn) {
        char *xml_meta_longdesc = NULL;
        char *xml_meta_shortdesc = NULL;

        char *meta_param = NULL;
        char *meta_longdesc = NULL;
        char *meta_shortdesc = NULL;

        stonith_obj = (*st_new_fn) (agent);
        if (stonith_obj) {
            (*st_log_fn) (stonith_obj, (PILLogFun) & stonith_plugin);
            meta_longdesc = strdup_null((*st_info_fn) (stonith_obj, ST_DEVICEDESCR));
            if (meta_longdesc == NULL) {
                crm_warn("no long description in %s's metadata.", agent);
                meta_longdesc = strdup(no_parameter_info);
            }

            meta_shortdesc = strdup_null((*st_info_fn) (stonith_obj, ST_DEVICEID));
            if (meta_shortdesc == NULL) {
                crm_warn("no short description in %s's metadata.", agent);
                meta_shortdesc = strdup(no_parameter_info);
            }

            meta_param = strdup_null((*st_info_fn) (stonith_obj, ST_CONF_XML));
            if (meta_param == NULL) {
                crm_warn("no list of parameters in %s's metadata.", agent);
                meta_param = strdup(no_parameter_info);
            }
            (*st_del_fn) (stonith_obj);
        } else {
            errno = EINVAL;
            crm_perror(LOG_ERR, "Agent %s not found", agent);
            return -EINVAL;
        }

        xml_meta_longdesc =
            (char *)xmlEncodeEntitiesReentrant(NULL, (const unsigned char *)meta_longdesc);
        xml_meta_shortdesc =
            (char *)xmlEncodeEntitiesReentrant(NULL, (const unsigned char *)meta_shortdesc);

        buffer = crm_strdup_printf(META_TEMPLATE, agent, xml_meta_longdesc,
                                   xml_meta_shortdesc, meta_param);

        xmlFree(xml_meta_longdesc);
        xmlFree(xml_meta_shortdesc);

        free(meta_shortdesc);
        free(meta_longdesc);
        free(meta_param);
    }
    if (output) {
        *output = buffer;
    } else {
        free(buffer);
    }
    return rc;
}

/* Implement a dummy function that uses -lpils so that linkers don't drop the
 * reference.
 */

#include <pils/plugin.h>

const char *i_hate_pils(int rc);

const char *
i_hate_pils(int rc)
{
    return PIL_strerror(rc);
}

int
stonith__lha_validate(stonith_t *st, int call_options, const char *target,
                      const char *agent, GHashTable *params, int timeout,
                      char **output, char **error_output)
{
    errno = EOPNOTSUPP;
    crm_perror(LOG_ERR, "Cannot validate Linux-HA fence agents");
    return -EOPNOTSUPP;
}
