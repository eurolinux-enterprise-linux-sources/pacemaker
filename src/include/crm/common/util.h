/* 
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#ifndef CRM_COMMON_UTIL__H
#  define CRM_COMMON_UTIL__H

/**
 * \file
 * \brief Utility functions
 * \ingroup core
 */

#  include <sys/types.h>
#  include <stdlib.h>
#  include <stdbool.h>
#  include <limits.h>
#  include <signal.h>
#  include <sysexits.h>
#  include <glib.h>

#  include <libxml/tree.h>

#  include <crm/lrmd.h>

#  if SUPPORT_HEARTBEAT
#    include <heartbeat.h>
#  else
#    define	NORMALNODE	"normal"
#    define	ACTIVESTATUS	"active"/* fully functional, and all links are up */
#    define	DEADSTATUS	"dead"
                                /* Status of non-working link or machine */
#    define	PINGSTATUS	"ping"
                                /* Status of a working ping node */
#    define	JOINSTATUS	"join"
                                /* Status when an api client joins */
#    define	LEAVESTATUS	"leave"
                                /* Status when an api client leaves */
#    define	ONLINESTATUS	"online"/* Status of an online client */
#    define	OFFLINESTATUS	"offline"
                                        /* Status of an offline client */
#  endif

/* public Pacemaker Remote functions (from remote.c) */
int crm_default_remote_port(void);

/* public string functions (from strings.c) */
char *crm_itoa_stack(int an_int, char *buf, size_t len);
char *crm_itoa(int an_int);
gboolean crm_is_true(const char *s);
int crm_str_to_boolean(const char *s, int *ret);
int crm_parse_int(const char *text, const char *default_text);
char * crm_strip_trailing_newline(char *str);
gboolean crm_str_eq(const char *a, const char *b, gboolean use_case);
gboolean safe_str_neq(const char *a, const char *b);
guint crm_strcase_hash(gconstpointer v);
guint g_str_hash_traditional(gconstpointer v);

#  define safe_str_eq(a, b) crm_str_eq(a, b, FALSE)
#  define crm_str_hash g_str_hash_traditional

/* used with hash tables where case does not matter */
static inline gboolean
crm_strcase_equal(gconstpointer a, gconstpointer b)
{
    return crm_str_eq((const char *) a, (const char *) b, FALSE);
}

/*!
 * \brief Create hash table with dynamically allocated string keys/values
 *
 * \return Newly hash table
 * \note It is the caller's responsibility to free the result, using
 *       g_hash_table_destroy().
 */
static inline GHashTable *
crm_str_table_new()
{
    return g_hash_table_new_full(crm_str_hash, g_str_equal, free, free);
}

/*!
 * \brief Create hash table with case-insensitive dynamically allocated string keys/values
 *
 * \return Newly hash table
 * \note It is the caller's responsibility to free the result, using
 *       g_hash_table_destroy().
 */
static inline GHashTable *
crm_strcase_table_new()
{
    return g_hash_table_new_full(crm_strcase_hash, crm_strcase_equal, free, free);
}

GHashTable *crm_str_table_dup(GHashTable *old_table);

#  define crm_atoi(text, default_text) crm_parse_int(text, default_text)

/* public I/O functions (from io.c) */
void crm_build_path(const char *path_c, mode_t mode);

long long crm_get_msec(const char *input);
unsigned long long crm_get_interval(const char *input);
int char2score(const char *score);
char *score2char(int score);
char *score2char_stack(int score, char *buf, size_t len);

/* public operation functions (from operations.c) */
gboolean parse_op_key(const char *key, char **rsc_id, char **op_type,
                      int *interval);
gboolean decode_transition_key(const char *key, char **uuid, int *action,
                               int *transition_id, int *target_rc);
gboolean decode_transition_magic(const char *magic, char **uuid,
                                 int *transition_id, int *action_id,
                                 int *op_status, int *op_rc, int *target_rc);
int rsc_op_expected_rc(lrmd_event_data_t *event);
gboolean did_rsc_op_fail(lrmd_event_data_t *event, int target_rc);
bool crm_op_needs_metadata(const char *rsc_class, const char *op);
xmlNode *crm_create_op_xml(xmlNode *parent, const char *prefix,
                           const char *task, const char *interval,
                           const char *timeout);
#define CRM_DEFAULT_OP_TIMEOUT_S "20s"

int compare_version(const char *version1, const char *version2);

/* coverity[+kill] */
void crm_abort(const char *file, const char *function, int line,
               const char *condition, gboolean do_core, gboolean do_fork);

static inline gboolean
is_not_set(long long word, long long bit)
{
    return ((word & bit) == 0);
}

static inline gboolean
is_set(long long word, long long bit)
{
    return ((word & bit) == bit);
}

static inline gboolean
is_set_any(long long word, long long bit)
{
    return ((word & bit) != 0);
}

static inline guint
crm_hash_table_size(GHashTable * hashtable)
{
    if (hashtable == NULL) {
        return 0;
    }
    return g_hash_table_size(hashtable);
}

char *crm_meta_name(const char *field);
const char *crm_meta_value(GHashTable * hash, const char *field);

char *crm_md5sum(const char *buffer);

char *crm_generate_uuid(void);
bool crm_is_daemon_name(const char *name);

int crm_user_lookup(const char *name, uid_t * uid, gid_t * gid);

#ifdef HAVE_GNUTLS_GNUTLS_H
void crm_gnutls_global_init(void);
#endif

int crm_exit(int rc);
bool pcmk_acl_required(const char *user);

char *crm_generate_ra_key(const char *class, const char *provider, const char *type);
bool crm_provider_required(const char *standard);
int crm_parse_agent_spec(const char *spec, char **standard, char **provider,
                         char **type);

#endif
