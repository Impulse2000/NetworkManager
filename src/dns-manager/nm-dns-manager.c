/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2004 - 2005 Colin Walters <walters@redhat.com>
 * Copyright (C) 2004 - 2013 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 *   and others
 */

#include "nm-default.h"

#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/fs.h>

#include "nm-utils.h"
#include "nm-core-internal.h"
#include "nm-dns-manager.h"
#include "nm-ip4-config.h"
#include "nm-ip6-config.h"
#include "NetworkManagerUtils.h"
#include "nm-config.h"

#include "nm-dns-plugin.h"
#include "nm-dns-dnsmasq.h"
#include "nm-dns-unbound.h"

#if WITH_LIBSOUP
#include <libsoup/soup.h>

#ifdef SOUP_CHECK_VERSION
#if SOUP_CHECK_VERSION (2, 40, 0)
#define DOMAIN_IS_VALID(domain) (*(domain) && !soup_tld_domain_is_public_suffix (domain))
#endif
#endif
#endif

#ifndef DOMAIN_IS_VALID
#define DOMAIN_IS_VALID(domain) (*(domain))
#endif

G_DEFINE_TYPE (NMDnsManager, nm_dns_manager, G_TYPE_OBJECT)

#define NM_DNS_MANAGER_GET_PRIVATE(o) ((o)->priv)

#define HASH_LEN 20

#ifdef RESOLVCONF_PATH
#define RESOLVCONF_SELECTED
#else
#define RESOLVCONF_PATH "/sbin/resolvconf"
#endif

#ifdef NETCONFIG_PATH
#define NETCONFIG_SELECTED
#else
#define NETCONFIG_PATH "/sbin/netconfig"
#endif

#define PLUGIN_RATELIMIT_INTERVAL    30
#define PLUGIN_RATELIMIT_BURST       5
#define PLUGIN_RATELIMIT_DELAY       300

NM_DEFINE_SINGLETON_INSTANCE (NMDnsManager);

/*********************************************************************************************/

#define _NMLOG_PREFIX_NAME                "dns-mgr"
#define _NMLOG_DOMAIN                     LOGD_DNS
#define _NMLOG(level, ...) \
    G_STMT_START { \
        const NMLogLevel __level = (level); \
        \
        if (nm_logging_enabled (__level, _NMLOG_DOMAIN)) { \
            char __prefix[20]; \
            const NMDnsManager *const __self = (self); \
            \
            _nm_log (__level, _NMLOG_DOMAIN, 0, \
                     "%s%s: " _NM_UTILS_MACRO_FIRST (__VA_ARGS__), \
                     _NMLOG_PREFIX_NAME, \
                     ((!__self || __self == singleton_instance) \
                        ? "" \
                        : nm_sprintf_buf (__prefix, "[%p]", __self)) \
                     _NM_UTILS_MACRO_REST (__VA_ARGS__)); \
        } \
    } G_STMT_END

/*********************************************************************************************/

typedef struct _NMDnsManagerPrivate {
	GSList *ip4_vpn_configs;
	NMIP4Config *ip4_device_config;
	GSList *ip6_vpn_configs;
	NMIP6Config *ip6_device_config;
	GSList *configs;
	char *hostname;
	guint updates_queue;

	guint8 hash[HASH_LEN];  /* SHA1 hash of current DNS config */
	guint8 prev_hash[HASH_LEN];  /* Hash when begin_updates() was called */

	NMDnsManagerResolvConfManager rc_manager;
	NMDnsPlugin *plugin;

	NMConfig *config;

	gboolean dns_touched;

	struct {
		guint64 ts;
		guint num_restarts;
		guint timer;
	} plugin_ratelimit;
} NMDnsManagerPrivate;

enum {
	CONFIG_CHANGED,

	LAST_SIGNAL
};

typedef enum {
	SR_SUCCESS,
	SR_NOTFOUND,
	SR_ERROR
} SpawnResult;

static guint signals[LAST_SIGNAL] = { 0 };


typedef struct {
	GPtrArray *nameservers;
	GPtrArray *searches;
	GPtrArray *options;
	const char *nis_domain;
	GPtrArray *nis_servers;
} NMResolvConfData;

NM_UTILS_LOOKUP_STR_DEFINE_STATIC (_rc_manager_to_string, NMDnsManagerResolvConfManager,
	NM_UTILS_LOOKUP_DEFAULT_WARN (NULL),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN,        "unknown"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,      "unmanaged"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE,      "immutable"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK,        "symlink"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE,           "file"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF,     "resolvconf"),
	NM_UTILS_LOOKUP_STR_ITEM (NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG,      "netconfig"),
);

static void
add_string_item (GPtrArray *array, const char *str)
{
	int i;

	g_return_if_fail (array != NULL);
	g_return_if_fail (str != NULL);

	/* Check for dupes before adding */
	for (i = 0; i < array->len; i++) {
		const char *candidate = g_ptr_array_index (array, i);

		if (candidate && !strcmp (candidate, str))
			return;
	}

	/* No dupes, add the new item */
	g_ptr_array_add (array, g_strdup (str));
}

static void
add_dns_option_item (GPtrArray *array, const char *str, gboolean ipv6)
{
	if (_nm_utils_dns_option_find_idx (array, str) < 0)
		g_ptr_array_add (array, g_strdup (str));
}

static void
merge_one_ip4_config (NMResolvConfData *rc, NMIP4Config *src)
{
	guint32 num, num_domains, num_searches, i;

	num = nm_ip4_config_get_num_nameservers (src);
	for (i = 0; i < num; i++) {
		add_string_item (rc->nameservers,
		                 nm_utils_inet4_ntop (nm_ip4_config_get_nameserver (src, i), NULL));
	}

	num_domains = nm_ip4_config_get_num_domains (src);
	num_searches = nm_ip4_config_get_num_searches (src);

	for (i = 0; i < num_searches; i++) {
		const char *search;

		search = nm_ip4_config_get_search (src, i);
		if (!DOMAIN_IS_VALID (search))
			continue;
		add_string_item (rc->searches, search);
	}

	if (num_domains > 1 || !num_searches) {
		for (i = 0; i < num_domains; i++) {
			const char *domain;

			domain = nm_ip4_config_get_domain (src, i);
			if (!DOMAIN_IS_VALID (domain))
				continue;
			add_string_item (rc->searches, domain);
		}
	}

	num = nm_ip4_config_get_num_dns_options (src);
	for (i = 0; i < num; i++) {
		const char *option;

		option = nm_ip4_config_get_dns_option (src, i);
		add_dns_option_item (rc->options, option, FALSE);
	}

	/* NIS stuff */
	num = nm_ip4_config_get_num_nis_servers (src);
	for (i = 0; i < num; i++) {
		add_string_item (rc->nis_servers,
		                 nm_utils_inet4_ntop (nm_ip4_config_get_nis_server (src, i), NULL));
	}

	if (nm_ip4_config_get_nis_domain (src)) {
		/* FIXME: handle multiple domains */
		if (!rc->nis_domain)
			rc->nis_domain = nm_ip4_config_get_nis_domain (src);
	}
}

static void
merge_one_ip6_config (NMResolvConfData *rc, NMIP6Config *src)
{
	guint32 num, num_domains, num_searches, i;
	const char *iface;

	iface = g_object_get_data (G_OBJECT (src), IP_CONFIG_IFACE_TAG);

	num = nm_ip6_config_get_num_nameservers (src);
	for (i = 0; i < num; i++) {
		const struct in6_addr *addr;
		char buf[NM_UTILS_INET_ADDRSTRLEN + 50];

		addr = nm_ip6_config_get_nameserver (src, i);

		/* inet_ntop is probably supposed to do this for us, but it doesn't */
		if (IN6_IS_ADDR_V4MAPPED (addr))
			nm_utils_inet4_ntop (addr->s6_addr32[3], buf);
		else {
			nm_utils_inet6_ntop (addr, buf);
			if (iface && IN6_IS_ADDR_LINKLOCAL (addr)) {
				g_strlcat (buf, "%", sizeof (buf));
				g_strlcat (buf, iface, sizeof (buf));
			}
		}
		add_string_item (rc->nameservers, buf);
	}

	num_domains = nm_ip6_config_get_num_domains (src);
	num_searches = nm_ip6_config_get_num_searches (src);

	for (i = 0; i < num_searches; i++) {
		const char *search;

		search = nm_ip6_config_get_search (src, i);
		if (!DOMAIN_IS_VALID (search))
			continue;
		add_string_item (rc->searches, search);
	}

	if (num_domains > 1 || !num_searches) {
		for (i = 0; i < num_domains; i++) {
			const char *domain;

			domain = nm_ip6_config_get_domain (src, i);
			if (!DOMAIN_IS_VALID (domain))
				continue;
			add_string_item (rc->searches, domain);
		}
	}

	num = nm_ip6_config_get_num_dns_options (src);
	for (i = 0; i < num; i++) {
		const char *option;

		option = nm_ip6_config_get_dns_option (src, i);
		add_dns_option_item (rc->options, option, TRUE);
	}
}

static GPid
run_netconfig (NMDnsManager *self, GError **error, gint *stdin_fd)
{
	char *argv[5];
	gs_free char *tmp = NULL;
	GPid pid = -1;

	argv[0] = NETCONFIG_PATH;
	argv[1] = "modify";
	argv[2] = "--service";
	argv[3] = "NetworkManager";
	argv[4] = NULL;

	_LOGD ("spawning '%s'",
	       (tmp = g_strjoinv (" ", argv)));

	if (!g_spawn_async_with_pipes (NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL,
	                               NULL, &pid, stdin_fd, NULL, NULL, error))
		return -1;

	return pid;
}

static void
write_to_netconfig (NMDnsManager *self, gint fd, const char *key, const char *value)
{
	char *str;
	int x;

	str = g_strdup_printf ("%s='%s'\n", key, value);
	_LOGD ("writing to netconfig: %s", str);
	x = write (fd, str, strlen (str));
	g_free (str);
}

static SpawnResult
dispatch_netconfig (NMDnsManager *self,
                    char **searches,
                    char **nameservers,
                    const char *nis_domain,
                    char **nis_servers,
                    GError **error)
{
	char *str;
	GPid pid;
	gint fd;
	int status;

	pid = run_netconfig (self, error, &fd);
	if (pid <= 0)
		return SR_NOTFOUND;

	/* NM is writing already-merged DNS information to netconfig, so it
	 * does not apply to a specific network interface.
	 */
	write_to_netconfig (self, fd, "INTERFACE", "NetworkManager");

	if (searches) {
		str = g_strjoinv (" ", searches);
		write_to_netconfig (self, fd, "DNSSEARCH", str);
		g_free (str);
	}

	if (nameservers) {
		str = g_strjoinv (" ", nameservers);
		write_to_netconfig (self, fd, "DNSSERVERS", str);
		g_free (str);
	}

	if (nis_domain)
		write_to_netconfig (self, fd, "NISDOMAIN", nis_domain);

	if (nis_servers) {
		str = g_strjoinv (" ", nis_servers);
		write_to_netconfig (self, fd, "NISSERVERS", str);
		g_free (str);
	}

	close (fd);

	/* Wait until the process exits */
	if (!nm_utils_kill_child_sync (pid, 0, LOGD_DNS, "netconfig", &status, 1000, 0)) {
		int errsv = errno;

		g_set_error (error, NM_MANAGER_ERROR, NM_MANAGER_ERROR_FAILED,
		             "Error waiting for netconfig to exit: %s",
		             strerror (errsv));
		return SR_ERROR;
	}
	if (!WIFEXITED (status) || WEXITSTATUS (status) != EXIT_SUCCESS) {
		g_set_error (error, NM_MANAGER_ERROR, NM_MANAGER_ERROR_FAILED,
		             "Error calling netconfig: %s %d",
		             WIFEXITED (status) ? "exited with status" : (WIFSIGNALED (status) ? "exited with signal" : "exited with unknown reason"),
		             WIFEXITED (status) ? WEXITSTATUS (status) : (WIFSIGNALED (status) ? WTERMSIG (status) : status));
		return SR_ERROR;
	}
	return SR_SUCCESS;
}

static char *
create_resolv_conf (char **searches,
                    char **nameservers,
                    char **options)
{
	gs_free char *searches_str = NULL;
	gs_free char *nameservers_str = NULL;
	gs_free char *options_str = NULL;
	char *tmp_str;
	GString *str;
	int i;

	if (searches) {
		tmp_str = g_strjoinv (" ", searches);
		searches_str = g_strconcat ("search ", tmp_str, "\n", NULL);
		g_free (tmp_str);
	}

	if (options) {
		tmp_str = g_strjoinv (" ", options);
		options_str = g_strconcat ("options ", tmp_str, "\n", NULL);
		g_free (tmp_str);
	}

	if (nameservers) {
		int num = g_strv_length (nameservers);

		str = g_string_new ("");
		for (i = 0; i < num; i++) {
			if (i == 3) {
				g_string_append (str, "# ");
				g_string_append (str, "NOTE: the libc resolver may not support more than 3 nameservers.");
				g_string_append (str, "\n# ");
				g_string_append (str, "The nameservers listed below may not be recognized.");
				g_string_append_c (str, '\n');
			}

			g_string_append (str, "nameserver ");
			g_string_append (str, nameservers[i]);
			g_string_append_c (str, '\n');
		}
		nameservers_str = g_string_free (str, FALSE);
	}

	return g_strdup_printf ("# Generated by NetworkManager\n%s%s%s",
	                        searches_str ?: "",
	                        nameservers_str ?: "",
	                        options_str ?: "");
}

static gboolean
write_resolv_conf_contents (FILE *f,
                            const char *content,
                            GError **error)
{
	int errsv;

	if (fprintf (f, "%s", content) < 0) {
		errsv = errno;
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not write " _PATH_RESCONF ": %s",
		             g_strerror (errsv));
		errno = errsv;
		return FALSE;
	}

	return TRUE;
}

static gboolean
write_resolv_conf (FILE *f,
                   char **searches,
                   char **nameservers,
                   char **options,
                   GError **error)
{
	gs_free char *content = NULL;

	content = create_resolv_conf (searches, nameservers, options);
	return write_resolv_conf_contents (f, content, error);
}

static SpawnResult
dispatch_resolvconf (NMDnsManager *self,
                     char **searches,
                     char **nameservers,
                     char **options,
                     GError **error)
{
	gs_free char *cmd = NULL;
	FILE *f;
	gboolean success = FALSE;
	int errnosv, err;

	if (!g_file_test (RESOLVCONF_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
		g_set_error_literal (error,
		                     NM_MANAGER_ERROR,
		                     NM_MANAGER_ERROR_FAILED,
		                     RESOLVCONF_PATH " is not executable");
		return SR_NOTFOUND;
	}

	if (!searches && !nameservers) {
		_LOGI ("Removing DNS information from %s", RESOLVCONF_PATH);

		cmd = g_strconcat (RESOLVCONF_PATH, " -d ", "NetworkManager", NULL);
		if (nm_spawn_process (cmd, error) != 0)
			return SR_ERROR;

		return SR_SUCCESS;
	}

	_LOGI ("Writing DNS information to %s", RESOLVCONF_PATH);

	cmd = g_strconcat (RESOLVCONF_PATH, " -a ", "NetworkManager", NULL);
	if ((f = popen (cmd, "w")) == NULL) {
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not write to %s: %s",
		             RESOLVCONF_PATH,
		             g_strerror (errno));
		return SR_ERROR;
	}

	success = write_resolv_conf (f, searches, nameservers, options, error);
	err = pclose (f);
	if (err < 0) {
		errnosv = errno;
		g_clear_error (error);
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errnosv),
		             "Failed to close pipe to resolvconf: %d", errnosv);
		return SR_ERROR;
	} else if (err > 0) {
		_LOGW ("resolvconf failed with status %d", err);
		g_clear_error (error);
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "resolvconf failed with status %d", err);
		return SR_ERROR;
	}

	return success ? SR_SUCCESS : SR_ERROR;
}

#define MY_RESOLV_CONF NMRUNDIR "/resolv.conf"
#define MY_RESOLV_CONF_TMP MY_RESOLV_CONF ".tmp"
#define RESOLV_CONF_TMP "/etc/.resolv.conf.NetworkManager"

static SpawnResult
update_resolv_conf (NMDnsManager *self,
                    char **searches,
                    char **nameservers,
                    char **options,
                    GError **error,
                    NMDnsManagerResolvConfManager rc_manager)
{
	FILE *f;
	struct stat st;
	gboolean success;
	gs_free char *content = NULL;
	SpawnResult write_file_result = SR_SUCCESS;
	int errsv;

	/* If we are not managing /etc/resolv.conf and it points to
	 * MY_RESOLV_CONF, don't write the private DNS configuration to
	 * MY_RESOLV_CONF otherwise we would overwrite the changes done by
	 * some external application.
	 *
	 * This is the only situation, where we don't try to update our
	 * internal resolv.conf file. */
	if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED) {
		gs_free char *path = g_file_read_link (_PATH_RESCONF, NULL);

		if (g_strcmp0 (path, MY_RESOLV_CONF) == 0) {
			_LOGD ("update-resolv-conf: not updating " _PATH_RESCONF
			       " since it points to " MY_RESOLV_CONF);
			return SR_SUCCESS;
		}
	}

	content = create_resolv_conf (searches, nameservers, options);

	if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE) {
		GError *local = NULL;

		/* we first write to /etc/resolv.conf directly. If that fails,
		 * we still continue to write to runstatedir but remember the
		 * error. */
		if (!g_file_set_contents (_PATH_RESCONF, content, -1, &local)) {
			_LOGT ("update-resolv-conf: write to %s failed (rc-managed=file, %s)",
			       _PATH_RESCONF, local->message);
			write_file_result = SR_ERROR;
			g_propagate_error (error, local);
			error = NULL;
		} else {
			_LOGT ("update-resolv-conf: write to %s succeeded (rc-managed=file)",
			       _PATH_RESCONF);
		}
	}

	if ((f = fopen (MY_RESOLV_CONF_TMP, "w")) == NULL) {
		errsv = errno;
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not open %s: %s",
		             MY_RESOLV_CONF_TMP,
		             g_strerror (errsv));
		_LOGT ("update-resolv-conf: open temporary file %s failed (%s)",
		       MY_RESOLV_CONF_TMP, g_strerror (errsv));
		return SR_ERROR;
	}

	success = write_resolv_conf_contents (f, content, error);
	if (!success) {
		errsv = errno;
		_LOGT ("update-resolv-conf: write temporary file %s failed (%s)",
		       MY_RESOLV_CONF_TMP, g_strerror (errsv));
	}

	if (fclose (f) < 0) {
		if (success) {
			errsv = errno;
			/* only set an error here if write_resolv_conf() was successful,
			 * since its error is more important.
			 */
			g_set_error (error,
			             NM_MANAGER_ERROR,
			             NM_MANAGER_ERROR_FAILED,
			             "Could not close %s: %s",
			             MY_RESOLV_CONF_TMP,
			             g_strerror (errsv));
			_LOGT ("update-resolv-conf: close temporary file %s failed (%s)",
			       MY_RESOLV_CONF_TMP, g_strerror (errsv));
		}
		return SR_ERROR;
	} else if (!success)
		return SR_ERROR;

	if (rename (MY_RESOLV_CONF_TMP, MY_RESOLV_CONF) < 0) {
		errsv = errno;
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not replace %s: %s",
		             MY_RESOLV_CONF,
		             g_strerror (errno));
		_LOGT ("update-resolv-conf: failed to rename temporary file %s to %s (%s)",
		       MY_RESOLV_CONF_TMP, MY_RESOLV_CONF, g_strerror (errsv));
		return SR_ERROR;
	}

	if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE) {
		_LOGT ("update-resolv-conf: write internal file %s succeeded (rc-manager=file)",
		       MY_RESOLV_CONF);
		return write_file_result;
	}

	if (rc_manager != NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK) {
		_LOGT ("update-resolv-conf: write internal file %s succeeded", MY_RESOLV_CONF);
		return SR_SUCCESS;
	}

	/* A symlink pointing to NM's own resolv.conf (MY_RESOLV_CONF) is always
	 * overwritten to ensure that changes are indicated with inotify.  Symlinks
	 * pointing to any other file are never overwritten.
	 */
	if (lstat (_PATH_RESCONF, &st) != 0) {
		errsv = errno;
		if (errsv != ENOENT) {
			/* NM cannot read /etc/resolv.conf */
			_LOGT ("update-resolv-conf: write internal file %s succeeded but lstat(%s) failed (%s)",
			       MY_RESOLV_CONF, _PATH_RESCONF, g_strerror (errsv));
			g_set_error (error,
			             NM_MANAGER_ERROR,
			             NM_MANAGER_ERROR_FAILED,
			             "Could not lstat %s: %s",
			             _PATH_RESCONF,
			             g_strerror (errsv));
			return SR_ERROR;
		}
	} else {
		if (S_ISLNK (st.st_mode)) {
			if (stat (_PATH_RESCONF, &st) != -1) {
				gs_free char *path = g_file_read_link (_PATH_RESCONF, NULL);

				if (!path || !nm_streq (path, MY_RESOLV_CONF)) {
					/* It's not NM's symlink; do nothing */
					_LOGT ("update-resolv-conf: write internal file %s succeeded "
					       "but don't update %s as it points to %s",
					       MY_RESOLV_CONF, _PATH_RESCONF, path ?: "");
					return SR_SUCCESS;
				}

				/* resolv.conf is a symlink owned by NM and the target is accessible
				 */
			} else {
				/* resolv.conf is a symlink but the target is not accessible;
				 * some other program is probably managing resolv.conf and
				 * NM should not touch it.
				 */
				_LOGT ("update-resolv-conf: write internal file %s succeeded "
				       "but don't update %s as the symlinks points somewhere else",
				       MY_RESOLV_CONF, _PATH_RESCONF);
				return SR_SUCCESS;
			}
		}
	}

	/* By this point, either /etc/resolv.conf does not exist, is a regular
	 * file, or is a symlink already owned by NM.  In all cases /etc/resolv.conf
	 * is replaced with a symlink pointing to NM's resolv.conf in /var/run/.
	 */
	if (   unlink (RESOLV_CONF_TMP) != 0
	    && ((errsv = errno) != ENOENT)) {
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not unlink %s: %s",
		             RESOLV_CONF_TMP,
		             g_strerror (errsv));
		_LOGT ("update-resolv-conf: write internal file %s succeeded "
		       "but canot delete temporary file %s: %s",
		       MY_RESOLV_CONF, RESOLV_CONF_TMP, g_strerror (errsv));
		return SR_ERROR;
	}

	if (symlink (MY_RESOLV_CONF, RESOLV_CONF_TMP) == -1) {
		errsv = errno;
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not create symlink %s pointing to %s: %s",
		             RESOLV_CONF_TMP,
		             MY_RESOLV_CONF,
		             g_strerror (errsv));
		_LOGT ("update-resolv-conf: write internal file %s succeeded "
		       "but failed to symlink %s: %s",
		       MY_RESOLV_CONF, RESOLV_CONF_TMP, g_strerror (errsv));
		return SR_ERROR;
	}

	if (rename (RESOLV_CONF_TMP, _PATH_RESCONF) == -1) {
		errsv = errno;
		g_set_error (error,
		             NM_MANAGER_ERROR,
		             NM_MANAGER_ERROR_FAILED,
		             "Could not rename %s to %s: %s",
		             RESOLV_CONF_TMP,
		             _PATH_RESCONF,
		             g_strerror (errsv));
		_LOGT ("update-resolv-conf: write internal file %s succeeded "
		       "but failed to rename temporary symlink %s to %s: %s",
		       MY_RESOLV_CONF, RESOLV_CONF_TMP, _PATH_RESCONF, g_strerror (errsv));
		return SR_ERROR;
	}

	_LOGT ("update-resolv-conf: write internal file %s succeeded and update symlink %s",
	       MY_RESOLV_CONF, _PATH_RESCONF);
	return SR_SUCCESS;
}

static void
compute_hash (NMDnsManager *self, const NMGlobalDnsConfig *global, guint8 buffer[HASH_LEN])
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	GChecksum *sum;
	GSList *iter;
	gsize len = HASH_LEN;

	sum = g_checksum_new (G_CHECKSUM_SHA1);
	g_assert (len == g_checksum_type_get_length (G_CHECKSUM_SHA1));

	if (global)
		nm_global_dns_config_update_checksum (global, sum);

	for (iter = priv->ip4_vpn_configs; iter; iter = g_slist_next (iter))
		nm_ip4_config_hash (iter->data, sum, TRUE);
	if (priv->ip4_device_config)
		nm_ip4_config_hash (priv->ip4_device_config, sum, TRUE);

	for (iter = priv->ip6_vpn_configs; iter; iter = g_slist_next (iter))
		nm_ip6_config_hash (iter->data, sum, TRUE);
	if (priv->ip6_device_config)
		nm_ip6_config_hash (priv->ip6_device_config, sum, TRUE);

	/* add any other configs we know about */
	for (iter = priv->configs; iter; iter = g_slist_next (iter)) {
		if (NM_IN_SET (iter->data, priv->ip4_device_config,
		                           priv->ip6_device_config))
			continue;

		if (NM_IS_IP4_CONFIG (iter->data))
			nm_ip4_config_hash (NM_IP4_CONFIG (iter->data), sum, TRUE);
		else if (NM_IS_IP6_CONFIG (iter->data))
			nm_ip6_config_hash (NM_IP6_CONFIG (iter->data), sum, TRUE);
	}

	g_checksum_get_digest (sum, buffer, &len);
	g_checksum_free (sum);
}

static void
build_plugin_config_lists (NMDnsManager *self,
                           GSList **out_vpn_configs,
                           GSList **out_dev_configs,
                           GSList **out_other_configs)
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	GSList *iter;

	g_return_if_fail (out_vpn_configs && !*out_vpn_configs);
	g_return_if_fail (out_dev_configs && !*out_dev_configs);
	g_return_if_fail (out_other_configs && !*out_other_configs);

	/* Build up config lists for plugins; we use the raw configs here, not the
	 * merged information that we write to resolv.conf so that the plugins can
	 * still use the domain information in each config to provide split DNS if
	 * they want to.
	 */
	for (iter = priv->ip4_vpn_configs; iter; iter = g_slist_next (iter))
		*out_vpn_configs = g_slist_append (*out_vpn_configs, iter->data);
	for (iter = priv->ip6_vpn_configs; iter; iter = g_slist_next (iter))
		*out_vpn_configs = g_slist_append (*out_vpn_configs, iter->data);
	if (priv->ip4_device_config)
		*out_dev_configs = g_slist_append (*out_dev_configs, priv->ip4_device_config);
	if (priv->ip6_device_config)
		*out_dev_configs = g_slist_append (*out_dev_configs, priv->ip6_device_config);

	for (iter = priv->configs; iter; iter = g_slist_next (iter)) {
		if (!NM_IN_SET (iter->data, priv->ip4_device_config,
		                            priv->ip6_device_config))
			*out_other_configs = g_slist_append (*out_other_configs, iter->data);
	}
}

static gboolean
merge_global_dns_config (NMResolvConfData *rc, NMGlobalDnsConfig *global_conf)
{
	NMGlobalDnsDomain *default_domain;
	const char *const *searches;
	const char *const *options;
	const char *const *servers;
	gint i;

	if (!global_conf)
		return FALSE;

	searches = nm_global_dns_config_get_searches (global_conf);
	options = nm_global_dns_config_get_options (global_conf);

	for (i = 0; searches && searches[i]; i++) {
		if (DOMAIN_IS_VALID (searches[i]))
			add_string_item (rc->searches, searches[i]);
	}

	for (i = 0; options && options[i]; i++)
		add_string_item (rc->options, options[i]);

	default_domain = nm_global_dns_config_lookup_domain (global_conf, "*");
	g_assert (default_domain);
	servers = nm_global_dns_domain_get_servers (default_domain);
	for (i = 0; servers && servers[i]; i++)
		add_string_item (rc->nameservers, servers[i]);

	return TRUE;
}

static gboolean
update_dns (NMDnsManager *self,
            gboolean no_caching,
            GError **error)
{
	NMDnsManagerPrivate *priv;
	NMResolvConfData rc;
	GSList *iter;
	const char *nis_domain = NULL;
	char **searches = NULL;
	char **options = NULL;
	char **nameservers = NULL;
	char **nis_servers = NULL;
	int num, i, len;
	gboolean caching = FALSE, update = TRUE;
	gboolean resolv_conf_updated = FALSE;
	SpawnResult result = SR_ERROR;
	NMConfigData *data;
	NMGlobalDnsConfig *global_config;

	g_return_val_if_fail (!error || !*error, FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	nm_clear_g_source (&priv->plugin_ratelimit.timer);

	if (NM_IN_SET (priv->rc_manager, NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,
	                                 NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE)) {
		update = FALSE;
		_LOGD ("update-dns: not updating resolv.conf");
	} else {
		priv->dns_touched = TRUE;
		_LOGD ("update-dns: updating resolv.conf");
	}

	data = nm_config_get_data (priv->config);
	global_config = nm_config_data_get_global_dns_config (data);

	/* Update hash with config we're applying */
	compute_hash (self, global_config, priv->hash);

	rc.nameservers = g_ptr_array_new ();
	rc.searches = g_ptr_array_new ();
	rc.options = g_ptr_array_new ();
	rc.nis_domain = NULL;
	rc.nis_servers = g_ptr_array_new ();

	if (global_config)
		merge_global_dns_config (&rc, global_config);
	else {
		for (iter = priv->ip4_vpn_configs; iter; iter = g_slist_next (iter))
			merge_one_ip4_config (&rc, iter->data);
		if (priv->ip4_device_config)
			merge_one_ip4_config (&rc, priv->ip4_device_config);

		for (iter = priv->ip6_vpn_configs; iter; iter = g_slist_next (iter))
			merge_one_ip6_config (&rc, iter->data);
		if (priv->ip6_device_config)
			merge_one_ip6_config (&rc, priv->ip6_device_config);

		for (iter = priv->configs; iter; iter = g_slist_next (iter)) {
			if (NM_IN_SET (iter->data, priv->ip4_device_config,
			                           priv->ip6_device_config))
				continue;

			if (NM_IS_IP4_CONFIG (iter->data)) {
				NMIP4Config *config = NM_IP4_CONFIG (iter->data);

				merge_one_ip4_config (&rc, config);
			} else if (NM_IS_IP6_CONFIG (iter->data)) {
				NMIP6Config *config = NM_IP6_CONFIG (iter->data);

				merge_one_ip6_config (&rc, config);
			} else
				g_assert_not_reached ();
		}
	}

	/* If the hostname is a FQDN ("dcbw.example.com"), then add the domain part of it
	 * ("example.com") to the searches list, to ensure that we can still resolve its
	 * non-FQ form ("dcbw") too. (Also, if there are no other search domains specified,
	 * this makes a good default.) However, if the hostname is the top level of a domain
	 * (eg, "example.com"), then use the hostname itself as the search (since the user is
	 * unlikely to want "com" as a search domain).
	 */
	if (priv->hostname) {
		const char *hostdomain = strchr (priv->hostname, '.');

		if (   hostdomain
		    && !nm_utils_ipaddr_valid (AF_UNSPEC, priv->hostname)) {
			hostdomain++;
			if (DOMAIN_IS_VALID (hostdomain))
				add_string_item (rc.searches, hostdomain);
			else if (DOMAIN_IS_VALID (priv->hostname))
				add_string_item (rc.searches, priv->hostname);
		}
	}

	/* Per 'man resolv.conf', the search list is limited to 6 domains
	 * totalling 256 characters.
	 */
	num = MIN (rc.searches->len, 6);
	for (i = 0, len = 0; i < num; i++) {
		len += strlen (rc.searches->pdata[i]) + 1; /* +1 for spaces */
		if (len > 256)
			break;
	}
	g_ptr_array_set_size (rc.searches, i);
	if (rc.searches->len) {
		g_ptr_array_add (rc.searches, NULL);
		searches = (char **) g_ptr_array_free (rc.searches, FALSE);
	} else
		g_ptr_array_free (rc.searches, TRUE);

	if (rc.options->len) {
		g_ptr_array_add (rc.options, NULL);
		options = (char **) g_ptr_array_free (rc.options, FALSE);
	} else
		g_ptr_array_free (rc.options, TRUE);

	if (rc.nameservers->len) {
		g_ptr_array_add (rc.nameservers, NULL);
		nameservers = (char **) g_ptr_array_free (rc.nameservers, FALSE);
	} else
		g_ptr_array_free (rc.nameservers, TRUE);

	if (rc.nis_servers->len) {
		g_ptr_array_add (rc.nis_servers, NULL);
		nis_servers = (char **) g_ptr_array_free (rc.nis_servers, FALSE);
	} else
		g_ptr_array_free (rc.nis_servers, TRUE);

	nis_domain = rc.nis_domain;

	/* Let any plugins do their thing first */
	if (priv->plugin) {
		NMDnsPlugin *plugin = priv->plugin;
		const char *plugin_name = nm_dns_plugin_get_name (plugin);
		GSList *vpn_configs = NULL, *dev_configs = NULL, *other_configs = NULL;

		if (nm_dns_plugin_is_caching (plugin)) {
			if (no_caching) {
				_LOGD ("update-dns: plugin %s ignored (caching disabled)",
				       plugin_name);
				goto skip;
			}
			caching = TRUE;
		}

		if (!global_config)
			build_plugin_config_lists (self, &vpn_configs, &dev_configs, &other_configs);

		_LOGD ("update-dns: updating plugin %s", plugin_name);
		if (!nm_dns_plugin_update (plugin,
		                           vpn_configs,
		                           dev_configs,
		                           other_configs,
		                           global_config,
		                           priv->hostname)) {
			_LOGW ("update-dns: plugin %s update failed", plugin_name);

			/* If the plugin failed to update, we shouldn't write out a local
			 * caching DNS configuration to resolv.conf.
			 */
			caching = FALSE;
		}
		g_slist_free (vpn_configs);
		g_slist_free (dev_configs);
		g_slist_free (other_configs);

	skip:
		;
	}

	/* If caching was successful, we only send 127.0.0.1 to /etc/resolv.conf
	 * to ensure that the glibc resolver doesn't try to round-robin nameservers,
	 * but only uses the local caching nameserver.
	 */
	if (caching) {
		if (nameservers)
			g_strfreev (nameservers);
		nameservers = g_new0 (char*, 2);
		nameservers[0] = g_strdup ("127.0.0.1");
	}

	if (update) {
		switch (priv->rc_manager) {
		case NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK:
		case NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE:
			result = update_resolv_conf (self, searches, nameservers, options, error, priv->rc_manager);
			resolv_conf_updated = TRUE;
			break;
		case NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF:
			result = dispatch_resolvconf (self, searches, nameservers, options, error);
			break;
		case NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG:
			result = dispatch_netconfig (self, searches, nameservers, nis_domain,
			                             nis_servers, error);
			break;
		default:
			g_assert_not_reached ();
		}

		if (result == SR_NOTFOUND) {
			_LOGD ("update-dns: program not available, writing to resolv.conf");
			g_clear_error (error);
			result = update_resolv_conf (self, searches, nameservers, options, error, NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK);
			resolv_conf_updated = TRUE;
		}
	}

	/* Unless we've already done it, update private resolv.conf in NMRUNDIR
	   ignoring any errors */
	if (!resolv_conf_updated)
		update_resolv_conf (self, searches, nameservers, options, NULL, NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED);

	/* signal that resolv.conf was changed */
	if (update && result == SR_SUCCESS)
		g_signal_emit (self, signals[CONFIG_CHANGED], 0);

	if (searches)
		g_strfreev (searches);
	if (options)
		g_strfreev (options);
	if (nameservers)
		g_strfreev (nameservers);
	if (nis_servers)
		g_strfreev (nis_servers);

	return !update || result == SR_SUCCESS;
}

static void
plugin_failed (NMDnsPlugin *plugin, gpointer user_data)
{
	NMDnsManager *self = NM_DNS_MANAGER (user_data);
	GError *error = NULL;

	/* Errors with non-caching plugins aren't fatal */
	if (!nm_dns_plugin_is_caching (plugin))
		return;

	/* Disable caching until the next DNS update */
	if (!update_dns (self, TRUE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}
}

static gboolean
plugin_child_quit_update_dns (gpointer user_data)
{
	GError *error = NULL;
	NMDnsManager *self = NM_DNS_MANAGER (user_data);

	/* Let the plugin try to spawn the child again */
	if (!update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	return G_SOURCE_REMOVE;
}

static void
plugin_child_quit (NMDnsPlugin *plugin, int exit_status, gpointer user_data)
{
	NMDnsManager *self = NM_DNS_MANAGER (user_data);
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	gint64 ts = nm_utils_get_monotonic_timestamp_ms ();

	_LOGW ("plugin %s child quit unexpectedly", nm_dns_plugin_get_name (plugin));

	if (   !priv->plugin_ratelimit.ts
	    || (ts - priv->plugin_ratelimit.ts) / 1000 > PLUGIN_RATELIMIT_INTERVAL) {
		priv->plugin_ratelimit.ts = ts;
		priv->plugin_ratelimit.num_restarts = 0;
	} else {
		priv->plugin_ratelimit.num_restarts++;
		if (priv->plugin_ratelimit.num_restarts > PLUGIN_RATELIMIT_BURST) {
			_LOGW ("plugin %s child respawning too fast, delaying update for %u seconds",
			        nm_dns_plugin_get_name (plugin), PLUGIN_RATELIMIT_DELAY);
			priv->plugin_ratelimit.timer = g_timeout_add_seconds (PLUGIN_RATELIMIT_DELAY,
			                                                      plugin_child_quit_update_dns,
			                                                      self);
			return;
		}
	}

	plugin_child_quit_update_dns (self);
}

gboolean
nm_dns_manager_add_ip4_config (NMDnsManager *self,
                               const char *iface,
                               NMIP4Config *config,
                               NMDnsIPConfigType cfg_type)
{
	NMDnsManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	g_object_set_data_full (G_OBJECT (config), IP_CONFIG_IFACE_TAG, g_strdup (iface), g_free);

	switch (cfg_type) {
	case NM_DNS_IP_CONFIG_TYPE_VPN:
		if (!g_slist_find (priv->ip4_vpn_configs, config)) {
			priv->ip4_vpn_configs = g_slist_append (priv->ip4_vpn_configs,
			                                        g_object_ref (config));
		}
		break;
	case NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE:
		priv->ip4_device_config = config;
		/* Fall through */
	case NM_DNS_IP_CONFIG_TYPE_DEFAULT:
		if (!g_slist_find (priv->configs, config))
			priv->configs = g_slist_append (priv->configs, g_object_ref (config));
		break;
	default:
		g_return_val_if_reached (FALSE);
	}

	if (!priv->updates_queue && !update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	return TRUE;
}

gboolean
nm_dns_manager_remove_ip4_config (NMDnsManager *self, NMIP4Config *config)
{
	NMDnsManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	if (g_slist_find (priv->configs, config)) {
		priv->configs = g_slist_remove (priv->configs, config);
		if (config == priv->ip4_device_config)
			priv->ip4_device_config = NULL;
	} else if (g_slist_find (priv->ip4_vpn_configs, config))
		priv->ip4_vpn_configs = g_slist_remove (priv->ip4_vpn_configs, config);
	else
		return FALSE;

	g_object_unref (config);

	if (!priv->updates_queue && !update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	g_object_set_data (G_OBJECT (config), IP_CONFIG_IFACE_TAG, NULL);

	return TRUE;
}

gboolean
nm_dns_manager_add_ip6_config (NMDnsManager *self,
                               const char *iface,
                               NMIP6Config *config,
                               NMDnsIPConfigType cfg_type)
{
	NMDnsManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	g_object_set_data_full (G_OBJECT (config), IP_CONFIG_IFACE_TAG, g_strdup (iface), g_free);

	switch (cfg_type) {
	case NM_DNS_IP_CONFIG_TYPE_VPN:
		if (!g_slist_find (priv->ip6_vpn_configs, config)) {
			priv->ip6_vpn_configs = g_slist_append (priv->ip6_vpn_configs,
			                                        g_object_ref (config));
		}
		break;
	case NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE:
		priv->ip6_device_config = config;
		/* Fall through */
	case NM_DNS_IP_CONFIG_TYPE_DEFAULT:
		if (!g_slist_find (priv->configs, config))
			priv->configs = g_slist_append (priv->configs, g_object_ref (config));
		break;
	default:
		g_return_val_if_reached (FALSE);
	}

	if (!priv->updates_queue && !update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	return TRUE;
}

gboolean
nm_dns_manager_remove_ip6_config (NMDnsManager *self, NMIP6Config *config)
{
	NMDnsManagerPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (config != NULL, FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	if (g_slist_find (priv->configs, config)) {
		priv->configs = g_slist_remove (priv->configs, config);
		if (config == priv->ip6_device_config)
			priv->ip6_device_config = NULL;
	} else if (g_slist_find (priv->ip6_vpn_configs, config))
		priv->ip6_vpn_configs = g_slist_remove (priv->ip6_vpn_configs, config);
	else
		return FALSE;

	g_object_unref (config);

	if (!priv->updates_queue && !update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	g_object_set_data (G_OBJECT (config), IP_CONFIG_IFACE_TAG, NULL);

	return TRUE;
}

void
nm_dns_manager_set_initial_hostname (NMDnsManager *self,
                                     const char *hostname)
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	priv->hostname = g_strdup (hostname);
}

void
nm_dns_manager_set_hostname (NMDnsManager *self,
                             const char *hostname)
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	GError *error = NULL;
	const char *filtered = NULL;

	/* Certain hostnames we don't want to include in resolv.conf 'searches' */
	if (   hostname
	    && nm_utils_is_specific_hostname (hostname)
	    && !strstr (hostname, ".in-addr.arpa")
	    && strchr (hostname, '.')) {
		filtered = hostname;
	}

	if (   (!priv->hostname && !filtered)
	    || (priv->hostname && filtered && !strcmp (priv->hostname, filtered)))
		return;

	g_free (priv->hostname);
	priv->hostname = g_strdup (filtered);

	if (!priv->updates_queue && !update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}
}

gboolean
nm_dns_manager_get_resolv_conf_explicit (NMDnsManager *self)
{
	NMDnsManagerPrivate *priv;

	g_return_val_if_fail (NM_IS_DNS_MANAGER (self), FALSE);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	if (   NM_IN_SET (priv->rc_manager, NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED,
	                                    NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE)
	    || priv->plugin)
		return FALSE;

	return TRUE;
}

void
nm_dns_manager_begin_updates (NMDnsManager *self, const char *func)
{
	NMDnsManagerPrivate *priv;

	g_return_if_fail (self != NULL);
	priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	/* Save current hash when starting a new batch */
	if (priv->updates_queue == 0)
		memcpy (priv->prev_hash, priv->hash, sizeof (priv->hash));

	priv->updates_queue++;

	_LOGD ("(%s): queueing DNS updates (%d)", func, priv->updates_queue);
}

void
nm_dns_manager_end_updates (NMDnsManager *self, const char *func)
{
	NMDnsManagerPrivate *priv;
	GError *error = NULL;
	gboolean changed;
	guint8 new[HASH_LEN];

	g_return_if_fail (self != NULL);

	priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	g_return_if_fail (priv->updates_queue > 0);

	compute_hash (self, nm_config_data_get_global_dns_config (nm_config_get_data (priv->config)), new);
	changed = (memcmp (new, priv->prev_hash, sizeof (new)) != 0) ? TRUE : FALSE;
	_LOGD ("(%s): DNS configuration %s", func, changed ? "changed" : "did not change");

	priv->updates_queue--;
	if ((priv->updates_queue > 0) || (changed == FALSE)) {
		_LOGD ("(%s): no DNS changes to commit (%d)", func, priv->updates_queue);
		return;
	}

	/* Commit all the outstanding changes */
	_LOGD ("(%s): committing DNS changes (%d)", func, priv->updates_queue);
	if (!update_dns (self, FALSE, &error)) {
		_LOGW ("could not commit DNS changes: %s", error->message);
		g_clear_error (&error);
	}

	memset (priv->prev_hash, 0, sizeof (priv->prev_hash));
}

/******************************************************************/

static gboolean
_clear_plugin (NMDnsManager *self)
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	if (priv->plugin) {
		g_signal_handlers_disconnect_by_func (priv->plugin, plugin_failed, self);
		g_signal_handlers_disconnect_by_func (priv->plugin, plugin_child_quit, self);
		g_clear_object (&priv->plugin);
		return TRUE;
	}
	return FALSE;
}

static NMDnsManagerResolvConfManager
_get_resolv_conf_manager_default (void)
{
#if defined(RESOLVCONF_SELECTED)
	return NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF;
#elif defined(NETCONFIG_SELECTED)
	return NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG;
#else
	return NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK;
#endif
}

static NMDnsManagerResolvConfManager
_get_resolv_conf_manager (NMConfig *config)
{
	const char *man;

	man = nm_config_data_get_rc_manager (nm_config_get_data (config));
	if (!man)
		return _get_resolv_conf_manager_default ();

	if (NM_IN_STRSET (man, "symlink", "none"))
		return NM_DNS_MANAGER_RESOLV_CONF_MAN_SYMLINK;
	if (nm_streq (man, "file"))
		return NM_DNS_MANAGER_RESOLV_CONF_MAN_FILE;
	if (nm_streq (man, "resolvconf"))
		return NM_DNS_MANAGER_RESOLV_CONF_MAN_RESOLVCONF;
	if (nm_streq (man, "netconfig"))
		return NM_DNS_MANAGER_RESOLV_CONF_MAN_NETCONFIG;
	if (nm_streq (man, "unmanaged"))
		return NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;

	return NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN;
}

static bool
_get_resconf_immutable (void)
{
	int fd, flags;
	bool immutable = FALSE;

	fd = open (_PATH_RESCONF, O_RDONLY);
	if (fd != -1) {
		if (ioctl (fd, FS_IOC_GETFLAGS, &flags) != -1)
			immutable = NM_FLAGS_HAS (flags, FS_IMMUTABLE_FL);
		close (fd);
	}
	return immutable;
}

NM_DEFINE_SINGLETON_GETTER (NMDnsManager, nm_dns_manager_get, NM_TYPE_DNS_MANAGER);

static void
init_resolv_conf_mode (NMDnsManager *self)
{
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	NMDnsManagerResolvConfManager rc_manager;
	const char *mode;
	gboolean plugin_changed = FALSE;

	mode = nm_config_data_get_dns_mode (nm_config_get_data (priv->config));

	if (nm_streq0 (mode, "none"))
		rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_UNMANAGED;
	else if (_get_resconf_immutable ())
		rc_manager = NM_DNS_MANAGER_RESOLV_CONF_MAN_IMMUTABLE;
	else {
		rc_manager = _get_resolv_conf_manager (priv->config);
		if (rc_manager == NM_DNS_MANAGER_RESOLV_CONF_MAN_UNKNOWN) {
			_LOGW ("init: unknown resolv.conf manager '%s'",
			       nm_config_data_get_rc_manager (nm_config_get_data (priv->config)));
			rc_manager = _get_resolv_conf_manager_default ();
		}
	}

	if (nm_streq0 (mode, "dnsmasq")) {
		if (!NM_IS_DNS_DNSMASQ (priv->plugin)) {
			_clear_plugin (self);
			priv->plugin = nm_dns_dnsmasq_new ();
			plugin_changed = TRUE;
		}
	} else if (nm_streq0 (mode, "unbound")) {
		if (!NM_IS_DNS_UNBOUND (priv->plugin)) {
			_clear_plugin (self);
			priv->plugin = nm_dns_unbound_new ();
			plugin_changed = TRUE;
		}
	} else {
		if (!NM_IN_STRSET (mode, NULL, "none", "default")) {
			_LOGW ("init: unknown dns mode '%s'", mode);
			mode = "default";
		}
		if (_clear_plugin (self))
			plugin_changed = TRUE;
	}

	if (plugin_changed && priv->plugin) {
		g_signal_connect (priv->plugin, NM_DNS_PLUGIN_FAILED, G_CALLBACK (plugin_failed), self);
		g_signal_connect (priv->plugin, NM_DNS_PLUGIN_CHILD_QUIT, G_CALLBACK (plugin_child_quit), self);
	}

	if (   plugin_changed
	    || priv->rc_manager != rc_manager) {
		priv->rc_manager = rc_manager;
		_LOGI ("init: dns=%s, rc-manager=%s%s%s%s",
		       mode, _rc_manager_to_string (rc_manager),
		       NM_PRINT_FMT_QUOTED (priv->plugin, ", plugin=", nm_dns_plugin_get_name (priv->plugin), "", ""));
	}
}

static void
config_changed_cb (NMConfig *config,
                   NMConfigData *config_data,
                   NMConfigChangeFlags changes,
                   NMConfigData *old_data,
                   NMDnsManager *self)
{
	GError *error = NULL;

	if (NM_FLAGS_ANY (changes, NM_CONFIG_CHANGE_DNS_MODE |
	                           NM_CONFIG_CHANGE_RC_MANAGER |
	                           NM_CONFIG_CHANGE_SIGHUP)) {
		/* reload the resolv-conf mode also on SIGHUP (when DNS_MODE didn't change).
		 * The reason is, that the configuration also depends on whether resolv.conf
		 * is immutable, thus, without the configuration changing, we always want to
		 * re-configure the mode. */
		init_resolv_conf_mode (self);
	}

	if (NM_FLAGS_ANY (changes, NM_CONFIG_CHANGE_SIGHUP |
	                           NM_CONFIG_CHANGE_SIGUSR1 |
	                           NM_CONFIG_CHANGE_DNS_MODE |
	                           NM_CONFIG_CHANGE_RC_MANAGER |
	                           NM_CONFIG_CHANGE_GLOBAL_DNS_CONFIG)) {
		if (!update_dns (self, FALSE, &error)) {
			_LOGW ("could not commit DNS changes: %s", error->message);
			g_clear_error (&error);
		}
	}
}

static void
nm_dns_manager_init (NMDnsManager *self)
{
	NMDnsManagerPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NM_TYPE_DNS_MANAGER, NMDnsManagerPrivate);

	self->priv = priv;

	_LOGT ("creating...");

	priv->config = g_object_ref (nm_config_get ());
	/* Set the initial hash */
	compute_hash (self, NULL, NM_DNS_MANAGER_GET_PRIVATE (self)->hash);

	g_signal_connect (G_OBJECT (priv->config),
	                  NM_CONFIG_SIGNAL_CONFIG_CHANGED,
	                  G_CALLBACK (config_changed_cb),
	                  self);
	init_resolv_conf_mode (self);
}

static void
dispose (GObject *object)
{
	NMDnsManager *self = NM_DNS_MANAGER (object);
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);
	GError *error = NULL;

	_LOGT ("disposing");

	_clear_plugin (self);

	/* If we're quitting, leave a valid resolv.conf in place, not one
	 * pointing to 127.0.0.1 if any plugins were active.  Thus update
	 * DNS after disposing of all plugins.  But if we haven't done any
	 * DNS updates yet, there's no reason to touch resolv.conf on shutdown.
	 */
	if (priv->dns_touched && !update_dns (self, TRUE, &error)) {
		_LOGW ("could not commit DNS changes on shutdown: %s", error->message);
		g_clear_error (&error);
		priv->dns_touched = FALSE;
	}

	if (priv->config) {
		g_signal_handlers_disconnect_by_func (priv->config, config_changed_cb, self);
		g_clear_object (&priv->config);
	}

	g_slist_free_full (priv->configs, g_object_unref);
	priv->configs = NULL;
	g_slist_free_full (priv->ip4_vpn_configs, g_object_unref);
	priv->ip4_vpn_configs = NULL;
	g_slist_free_full (priv->ip6_vpn_configs, g_object_unref);
	priv->ip6_vpn_configs = NULL;

	G_OBJECT_CLASS (nm_dns_manager_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMDnsManager *self = NM_DNS_MANAGER (object);
	NMDnsManagerPrivate *priv = NM_DNS_MANAGER_GET_PRIVATE (self);

	g_free (priv->hostname);

	G_OBJECT_CLASS (nm_dns_manager_parent_class)->finalize (object);
}

static void
nm_dns_manager_class_init (NMDnsManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDnsManagerPrivate));

	/* virtual methods */
	object_class->dispose = dispose;
	object_class->finalize = finalize;

	/* signals */
	signals[CONFIG_CHANGED] =
	    g_signal_new (NM_DNS_MANAGER_CONFIG_CHANGED,
	                  G_OBJECT_CLASS_TYPE (object_class),
	                  G_SIGNAL_RUN_FIRST,
	                  G_STRUCT_OFFSET (NMDnsManagerClass, config_changed),
	                  NULL, NULL,
	                  g_cclosure_marshal_VOID__VOID,
	                  G_TYPE_NONE, 0);
}

