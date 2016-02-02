/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * Søren Sandmann <sandmann@daimi.au.dk>
 * Dan Williams <dcbw@redhat.com>
 * Tambet Ingo <tambet@gmail.com>
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
 * (C) Copyright 2007 - 2011 Red Hat, Inc.
 * (C) Copyright 2008 Novell, Inc.
 */

#include "config.h"

#include <gmodule.h>
#include <nm-dbus-interface.h>

#include "nm-netns-controller.h"

G_DEFINE_TYPE (NMNetnsController, nm_netns_controller, NM_TYPE_EXPORTED_OBJECT)

typedef struct {
} NMNetnsControllerPrivate;

NMNetnsController *
nm_netns_controller_new (void)
{
	return NULL;
}

gboolean
nm_netns_controller_start (NMNetnsController *self, GError **error)
{
	return FALSE;
}

static void
nm_netns_controller_init (NMNetnsController *self)
{
}

static void
nm_netns_controller_class_init (NMNetnsControllerClass *class)
{
}

