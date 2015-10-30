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
 * Copyright (C) 2009 - 2014 Red Hat, Inc.
 * Copyright (C) 2009 Novell, Inc.
 * Copyright (C) 2009 - 2013 Canonical Ltd.
 */

#include "config.h"

#include <string.h>

#include <libmm-glib.h>

#include "nm-modem-manager.h"
#include "nm-logging.h"
#include "nm-modem.h"
#include "nm-modem-broadband.h"

#if WITH_OFONO
#include "nm-dbus-manager.h"
#include "nm-modem-ofono.h"
#endif

#define MODEM_POKE_INTERVAL 120

G_DEFINE_TYPE (NMModemManager, nm_modem_manager, G_TYPE_OBJECT)

struct _NMModemManagerPrivate {
	GDBusConnection *dbus_connection;
	MMManager *modem_manager;
	guint mm_launch_id;
	guint mm_name_owner_changed_id;
	guint mm_object_added_id;
	guint mm_object_removed_id;

#if WITH_OFONO
	GDBusProxy *ofono_proxy;

	guint ofono_name_owner_changed_id;
#endif

	/* Common */
	GHashTable *modems;
};

enum {
	MODEM_ADDED,
	LAST_SIGNAL,
};
static guint signals[LAST_SIGNAL] = { 0 };

/************************************************************************/

static void
handle_new_modem (NMModemManager *self, NMModem *modem)
{
	const char *path;

	path = nm_modem_get_path (modem);
	if (g_hash_table_lookup (self->priv->modems, path)) {
		g_warn_if_reached ();
		return;
	}

	/* Track the new modem */
	g_hash_table_insert (self->priv->modems, g_strdup (path), modem);
	g_signal_emit (self, signals[MODEM_ADDED], 0, modem);
}

static gboolean
remove_one_modem (gpointer key, gpointer value, gpointer user_data)
{
	nm_modem_emit_removed (NM_MODEM (value));
	return TRUE;
}

static void
modem_manager_clear_signals (NMModemManager *self)
{
	if (!self->priv->modem_manager)
		return;

	if (self->priv->mm_name_owner_changed_id) {
		if (g_signal_handler_is_connected (self->priv->modem_manager,
		                                   self->priv->mm_name_owner_changed_id))
			g_signal_handler_disconnect (self->priv->modem_manager,
			                             self->priv->mm_name_owner_changed_id);
		self->priv->mm_name_owner_changed_id = 0;
	}

	if (self->priv->mm_object_added_id) {
		if (g_signal_handler_is_connected (self->priv->modem_manager,
		                                   self->priv->mm_object_added_id))
			g_signal_handler_disconnect (self->priv->modem_manager,
			                             self->priv->mm_object_added_id);
		self->priv->mm_object_added_id = 0;
	}

	if (self->priv->mm_object_removed_id) {
		if (g_signal_handler_is_connected (self->priv->modem_manager,
		                                   self->priv->mm_object_removed_id))
			g_signal_handler_disconnect (self->priv->modem_manager,
			                             self->priv->mm_object_removed_id);
		self->priv->mm_object_removed_id = 0;
	}
}

static void
modem_object_added (MMManager *modem_manager,
                    MMObject  *modem_object,
                    NMModemManager *self)
{
	const gchar *path;
	MMModem *modem_iface;
	NMModem *modem;
	GError *error = NULL;

	/* Ensure we don't have the same modem already */
	path = mm_object_get_path (modem_object);
	if (g_hash_table_lookup (self->priv->modems, path)) {
		nm_log_warn (LOGD_MB, "modem with path %s already exists, ignoring", path);
		return;
	}

	/* Ensure we have the 'Modem' interface at least */
	modem_iface = mm_object_peek_modem (modem_object);
	if (!modem_iface) {
		nm_log_warn (LOGD_MB, "modem with path %s doesn't have the Modem interface, ignoring", path);
		return;
	}

	/* Ensure we have a primary port reported */
	if (!mm_modem_get_primary_port (modem_iface)) {
		nm_log_warn (LOGD_MB, "modem with path %s has unknown primary port, ignoring", path);
		return;
	}

	/* Create a new modem object */
	modem = nm_modem_broadband_new (G_OBJECT (modem_object), &error);
	if (modem)
		handle_new_modem (self, modem);
	else {
		nm_log_warn (LOGD_MB, "failed to create modem: %s",
		             error ? error->message : "(unknown)");
	}
	g_clear_error (&error);
}

static void
modem_object_removed (MMManager *manager,
                      MMObject  *modem_object,
                      NMModemManager *self)
{
	NMModem *modem;
	const gchar *path;

	path = mm_object_get_path (modem_object);
	modem = (NMModem *) g_hash_table_lookup (self->priv->modems, path);
	if (!modem)
		return;

	nm_modem_emit_removed (modem);
	g_hash_table_remove (self->priv->modems, path);
}

static void
modem_manager_available (NMModemManager *self)
{
	GList *modems, *l;

	nm_log_info (LOGD_MB, "ModemManager available in the bus");

	/* Update initial modems list */
	modems = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (self->priv->modem_manager));
	for (l = modems; l; l = g_list_next (l))
		modem_object_added (self->priv->modem_manager, MM_OBJECT (l->data), self);
	g_list_free_full (modems, (GDestroyNotify) g_object_unref);
}

static void schedule_modem_manager_relaunch (NMModemManager *self,
                                             guint n_seconds);
static void ensure_client                   (NMModemManager *self);

static void
modem_manager_name_owner_changed (MMManager *modem_manager,
                                  GParamSpec *pspec,
                                  NMModemManager *self)
{
	gchar *name_owner;

	/* Quit poking, if any */
	if (self->priv->mm_launch_id) {
		g_source_remove (self->priv->mm_launch_id);
		self->priv->mm_launch_id = 0;
	}

	name_owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (modem_manager));
	if (!name_owner) {
		nm_log_info (LOGD_MB, "ModemManager disappeared from bus");

#if !HAVE_SYSTEMD
		/* If not managed by systemd, schedule relaunch */
		schedule_modem_manager_relaunch (self, 0);
#endif

		return;
	}

	/* Available! */
	g_free (name_owner);

	/* Hack alert: GDBusObjectManagerClient won't signal neither 'object-added'
	 * nor 'object-removed' if it was created while there was no ModemManager in
	 * the bus. This hack avoids this issue until we get a GIO with the fix
	 * included... */
	modem_manager_clear_signals (self);
	g_clear_object (&self->priv->modem_manager);
	ensure_client (self);

	/* Whenever GDBusObjectManagerClient is fixed, we can just do the following:
	 * modem_manager_available (self);
	 */
}

#if WITH_OFONO
static void
ofono_clear_signals (NMModemManager *self)
{
	if (!self->priv->ofono_proxy)
		return;

	if (self->priv->ofono_name_owner_changed_id) {
		if (g_signal_handler_is_connected (self->priv->ofono_proxy,
		                                   self->priv->ofono_name_owner_changed_id))
			g_signal_handler_disconnect (self->priv->ofono_proxy,
			                             self->priv->ofono_name_owner_changed_id);
		self->priv->ofono_name_owner_changed_id = 0;
	}
}

static void
ofono_create_modem (NMModemManager *self, const char *path)
{
	NMModem *modem = NULL;

	if (g_hash_table_lookup (self->priv->modems, path)) {
		nm_log_warn (LOGD_MB, "modem with path %s already exists, ignoring", path);
		return;
	}

	/* Create modem instance */
	modem = nm_modem_ofono_new (path);
	if (modem)
		handle_new_modem (self, modem);
	else
		nm_log_warn (LOGD_MB, "Failed to create oFono modem for %s", path);
}

static void
ofono_signal_cb (GDBusProxy *proxy,
                 gchar *sender_name,
                 gchar *signal_name,
                 GVariant *parameters,
                 gpointer user_data)
{
	NMModemManager *self = NM_MODEM_MANAGER (user_data);
	gchar *object_path;
	NMModem *modem;

	if (g_strcmp0 (signal_name, "ModemAdded") == 0) {
		g_variant_get (parameters, "(oa{sv})", &object_path, NULL);
		nm_log_info (LOGD_MB, "oFono modem appeared: %s", object_path);

		ofono_create_modem (NM_MODEM_MANAGER (user_data), object_path);
		g_free (object_path);
	} else if (g_strcmp0 (signal_name, "ModemRemoved") == 0) {
		g_variant_get (parameters, "(o)", &object_path);
		nm_log_info (LOGD_MB, "oFono modem removed: %s", object_path);

		modem = (NMModem *) g_hash_table_lookup (self->priv->modems, object_path);
		if (modem) {
			nm_modem_emit_removed (modem);
			g_hash_table_remove (self->priv->modems, object_path);
		} else {
			nm_log_warn (LOGD_MB, "could not remove modem %s, not found in table",
			             object_path);
		}
		g_free (object_path);
	}
}

#define OFONO_DBUS_MODEM_ENTRY (dbus_g_type_get_struct ("GValueArray", DBUS_TYPE_G_OBJECT_PATH, DBUS_TYPE_G_MAP_OF_VARIANT, G_TYPE_INVALID))
#define OFONO_DBUS_MODEM_ENTRIES (dbus_g_type_get_collection ("GPtrArray", OFONO_DBUS_MODEM_ENTRY))

static void
ofono_enumerate_devices_done (GDBusProxy *proxy, GAsyncResult *res, gpointer user_data)
{
	NMModemManager *manager = NM_MODEM_MANAGER (user_data);
	GPtrArray *modems;
	GError *error = NULL;
	GVariant *results;
	GVariantIter *iter;
	const char *path;

	results = g_dbus_proxy_call_finish (proxy, res, &error);
	if (results) {
		g_variant_get (results, "(a(oa{sv}))", &iter);
		while (g_variant_iter_loop (iter, "(&oa{sv})", &path, NULL)) {
			ofono_create_modem (manager, path);
		}
		g_variant_iter_free (iter);
		g_variant_unref (results);
	}

	if (error)
		nm_log_warn (LOGD_MB, "failed to enumerate oFono devices: %s",
		             error->message ? error->message : "(unknown)");
}

static void ofono_appeared (NMModemManager *self);

static void
ofono_check_name_owner (NMModemManager *self)
{
	gchar *name_owner;

	name_owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (self->priv->ofono_proxy));
	if (name_owner) {
		/* Available! */
		ofono_appeared (self);
		goto free;
	}

	nm_log_info (LOGD_MB, "oFono disappeared from bus");

	ofono_clear_signals (self);
	g_clear_object (&self->priv->ofono_proxy);
	ensure_client (self);

free:
	g_free (name_owner);
	return;
}

static void
ofono_name_owner_changed (GDBusProxy *ofono_proxy,
                          GParamSpec *pspec,
                          NMModemManager *self)
{
	ofono_check_name_owner (self);
}

static void
ofono_appeared (NMModemManager *self)
{
	nm_log_info (LOGD_MB, "ofono is now available");

	self->priv->ofono_name_owner_changed_id =
		g_signal_connect (self->priv->ofono_proxy,
		                  "notify::name-owner",
		                  G_CALLBACK (ofono_name_owner_changed),
		                  self);
	g_dbus_proxy_call (self->priv->ofono_proxy,
	                   "GetModems",
	                   NULL,
	                   G_DBUS_CALL_FLAGS_NONE,
	                   -1,
	                   NULL,
	                   (GAsyncReadyCallback) ofono_enumerate_devices_done,
	                   g_object_ref (self));

	g_signal_connect (self->priv->ofono_proxy,
	                  "g-signal",
	                  G_CALLBACK (ofono_signal_cb),
	                  self);
}

static void
ofono_proxy_new_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	NMModemManager *self = NM_MODEM_MANAGER (user_data);
	GError *error = NULL;

	self->priv->ofono_proxy = g_dbus_proxy_new_finish (res, &error);

	if (error) {
		//FIXME: do stuff if there's an error.
		return;
	}

	ofono_appeared (self);

	/* Balance refcount */
	g_object_unref (self);
}
#endif

#if !HAVE_SYSTEMD

static void
modem_manager_poke_cb (GDBusConnection *connection,
                       GAsyncResult *res,
                       NMModemManager *self)
{
	GError *error = NULL;
	GVariant *result;

	result = g_dbus_connection_call_finish (connection, res, &error);
	if (error) {
		/* Ignore common errors when MM is not installed and such */
		if (   !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
		    && !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_EXEC_FAILED)
		    && !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FORK_FAILED)
		    && !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FAILED)
		    && !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMEOUT)
		    && !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND)) {
			nm_log_dbg (LOGD_MB, "error poking ModemManager: %s", error->message);
		}
		g_error_free (error);

		/* Setup timeout to relaunch */
		schedule_modem_manager_relaunch (self, MODEM_POKE_INTERVAL);
	} else
		g_variant_unref (result);

	/* Balance refcount */
	g_object_unref (self);
}

static void
modem_manager_poke (NMModemManager *self)
{
	/* If there is no current owner right away, ensure we poke to get one */
	g_dbus_connection_call (self->priv->dbus_connection,
	                        "org.freedesktop.ModemManager1",
	                        "/org/freedesktop/ModemManager1",
	                        "org.freedesktop.DBus.Peer",
	                        "Ping",
	                        NULL, /* inputs */
	                        NULL, /* outputs */
	                        G_DBUS_CALL_FLAGS_NONE,
	                        -1,
	                        NULL, /* cancellable */
	                        (GAsyncReadyCallback)modem_manager_poke_cb, /* callback */
	                        g_object_ref (self)); /* user_data */
}

#endif /* HAVE_SYSTEMD */

static void
modem_manager_check_name_owner (NMModemManager *self)
{
	gchar *name_owner;

	name_owner = g_dbus_object_manager_client_get_name_owner (G_DBUS_OBJECT_MANAGER_CLIENT (self->priv->modem_manager));
	if (name_owner) {
		/* Available! */
		modem_manager_available (self);
		g_free (name_owner);
		return;
	}

#if !HAVE_SYSTEMD
	/* If the lifecycle is not managed by systemd, poke */
	modem_manager_poke (self);
#endif
}

static void
manager_new_ready (GObject *source,
                   GAsyncResult *res,
                   NMModemManager *self)
{
	/* Note we always get an extra reference to self here */

	GError *error = NULL;

	g_assert (!self->priv->modem_manager);
	self->priv->modem_manager = mm_manager_new_finish (res, &error);
	if (!self->priv->modem_manager) {
		/* We're not really supposed to get any error here. If we do get one,
		 * though, just re-schedule the MMManager creation after some time.
		 * During this period, name-owner changes won't be followed. */
		nm_log_warn (LOGD_MB, "error creating ModemManager client: %s", error->message);
		g_error_free (error);
		/* Setup timeout to relaunch */
		schedule_modem_manager_relaunch (self, MODEM_POKE_INTERVAL);
	} else {
		/* Setup signals in the GDBusObjectManagerClient */
		self->priv->mm_name_owner_changed_id =
			g_signal_connect (self->priv->modem_manager,
			                  "notify::name-owner",
			                  G_CALLBACK (modem_manager_name_owner_changed),
			                  self);
		self->priv->mm_object_added_id =
			g_signal_connect (self->priv->modem_manager,
			                  "object-added",
			                  G_CALLBACK (modem_object_added),
			                  self);
		self->priv->mm_object_removed_id =
			g_signal_connect (self->priv->modem_manager,
			                  "object-removed",
			                  G_CALLBACK (modem_object_removed),
			                  self);

		modem_manager_check_name_owner (self);
	}

	/* Balance refcount */
	g_object_unref (self);
}

static void
ensure_client (NMModemManager *self)
{
	NMModemManagerPrivate *priv = self->priv;
	g_assert (priv->dbus_connection);
	gboolean created = FALSE;

	/* Create the GDBusObjectManagerClient. We do not request to autostart, as
	 * we don't really want the MMManager creation to fail. We can always poke
	 * later on if we want to request the autostart */
	if (!priv->modem_manager) {
		mm_manager_new (priv->dbus_connection,
		                G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
		                NULL,
		                (GAsyncReadyCallback)manager_new_ready,
		                g_object_ref (self));
		created = TRUE;
	}

#if WITH_OFONO
	if (!priv->ofono_proxy) {
		g_dbus_proxy_new (priv->dbus_connection,
		                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
		                  NULL,
		                  OFONO_DBUS_SERVICE,
		                  OFONO_DBUS_PATH,
		                  OFONO_DBUS_INTERFACE,
		                  NULL,
		                  (GAsyncReadyCallback)ofono_proxy_new_cb,
		                  g_object_ref (self));
		created = TRUE;
	}
#endif /* WITH_OFONO */

	if (created)
		return;

	/* If already available, recheck name owner! */
	modem_manager_check_name_owner (self);
	ofono_check_name_owner (self);
}

static void
bus_get_ready (GObject *source,
               GAsyncResult *res,
               NMModemManager *self)
{
	/* Note we always get an extra reference to self here */

	GError *error = NULL;

	self->priv->dbus_connection = g_bus_get_finish (res, &error);
	if (!self->priv->dbus_connection) {
		nm_log_warn (LOGD_CORE, "error getting bus connection: %s", error->message);
		g_error_free (error);
		/* Setup timeout to relaunch */
		schedule_modem_manager_relaunch (self, MODEM_POKE_INTERVAL);
	} else {
		/* Got the bus, ensure client */
		ensure_client (self);
	}

	/* Balance refcount */
	g_object_unref (self);
}

static gboolean
ensure_bus (NMModemManager *self)
{
	/* Clear launch ID */
	self->priv->mm_launch_id = 0;

	if (!self->priv->dbus_connection)
		g_bus_get (G_BUS_TYPE_SYSTEM,
		           NULL,
		           (GAsyncReadyCallback)bus_get_ready,
		           g_object_ref (self));
	else
		/* If bus is already available, ensure client */
		ensure_client (self);

	return FALSE;
}

static void
schedule_modem_manager_relaunch (NMModemManager *self,
                                 guint n_seconds)
{
	/* No need to pass an extra reference to self; timeout/idle will be
	 * cancelled if the object gets disposed. */

	if (n_seconds)
		self->priv->mm_launch_id = g_timeout_add_seconds (n_seconds, (GSourceFunc)ensure_bus, self);
	else
		self->priv->mm_launch_id = g_idle_add ((GSourceFunc)ensure_bus, self);
}

/************************************************************************/

static void
nm_modem_manager_init (NMModemManager *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NM_TYPE_MODEM_MANAGER, NMModemManagerPrivate);

	self->priv->modems = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

	schedule_modem_manager_relaunch (self, 0);
}

static void
dispose (GObject *object)
{
	NMModemManager *self = NM_MODEM_MANAGER (object);

	if (self->priv->mm_launch_id) {
		g_source_remove (self->priv->mm_launch_id);
		self->priv->mm_launch_id = 0;
	}

	modem_manager_clear_signals (self);
	g_clear_object (&self->priv->modem_manager);
#if WITH_OFONO
	ofono_clear_signals (self);
	g_clear_object (&self->priv->ofono_proxy);
#endif
	g_clear_object (&self->priv->dbus_connection);

	if (self->priv->modems) {
		g_hash_table_foreach_remove (self->priv->modems, remove_one_modem, object);
		g_hash_table_destroy (self->priv->modems);
	}

	/* Chain up to the parent class */
	G_OBJECT_CLASS (nm_modem_manager_parent_class)->dispose (object);
}

static void
nm_modem_manager_class_init (NMModemManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMModemManagerPrivate));

	object_class->dispose = dispose;

	signals[MODEM_ADDED] =
		g_signal_new (NM_MODEM_MANAGER_MODEM_ADDED,
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (NMModemManagerClass, modem_added),
		              NULL, NULL, NULL,
		              G_TYPE_NONE, 1, NM_TYPE_MODEM);
}
