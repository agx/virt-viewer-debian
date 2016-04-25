/*
 * Virt Viewer: A virtual machine console viewer
 *
 * Copyright (C) 2007-2014 Red Hat, Inc.
 * Copyright (C) 2009-2012 Daniel P. Berrange
 * Copyright (C) 2010 Marc-André Lureau
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 *         Christope Fergeau <cfergeau@redhat.com>
 */

#include <config.h>

#include <string.h>

#include "ovirt-foreign-menu.h"
#include "virt-glib-compat.h"
#include "virt-viewer-util.h"

typedef enum {
    STATE_0,
    STATE_API,
    STATE_VM,
    STATE_STORAGE_DOMAIN,
    STATE_VM_CDROM,
    STATE_CDROM_FILE,
    STATE_ISOS
} OvirtForeignMenuState;

static void ovirt_foreign_menu_next_async_step(OvirtForeignMenu *menu, OvirtForeignMenuState state);
static void ovirt_foreign_menu_fetch_api_async(OvirtForeignMenu *menu);
static void ovirt_foreign_menu_fetch_vm_async(OvirtForeignMenu *menu);
static void ovirt_foreign_menu_fetch_storage_domain_async(OvirtForeignMenu *menu);
static void ovirt_foreign_menu_fetch_vm_cdrom_async(OvirtForeignMenu *menu);
static void ovirt_foreign_menu_refresh_cdrom_file_async(OvirtForeignMenu *menu);
static gboolean ovirt_foreign_menu_refresh_iso_list(gpointer user_data);

G_DEFINE_TYPE (OvirtForeignMenu, ovirt_foreign_menu, G_TYPE_OBJECT)


struct _OvirtForeignMenuPrivate {
    OvirtProxy *proxy;
    OvirtApi *api;
    OvirtVm *vm;
    char *vm_guid;

    OvirtCollection *files;
    OvirtCdrom *cdrom;

    /* The next 2 members are used when changing the ISO image shown in
     * a VM */
    /* Name of the ISO which is currently used by the VM OvirtCdrom */
    char *current_iso_name;
    /* Name of the ISO we are trying to insert in the VM OvirtCdrom */
    char *next_iso_name;

    GList *iso_names;
};


#define OVIRT_FOREIGN_MENU_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), OVIRT_TYPE_FOREIGN_MENU, OvirtForeignMenuPrivate))


enum {
    PROP_0,
    PROP_PROXY,
    PROP_API,
    PROP_VM,
    PROP_FILE,
    PROP_FILES,
    PROP_VM_GUID,
};


static char *
ovirt_foreign_menu_get_current_iso_name(OvirtForeignMenu *foreign_menu)
{
    char *name;

    if (foreign_menu->priv->cdrom == NULL) {
        return NULL;
    }

    g_object_get(foreign_menu->priv->cdrom, "file", &name, NULL);

    return name;
}


static void
ovirt_foreign_menu_get_property(GObject *object, guint property_id,
                                       GValue *value, GParamSpec *pspec)
{
    OvirtForeignMenu *self = OVIRT_FOREIGN_MENU(object);
    OvirtForeignMenuPrivate *priv = self->priv;

    switch (property_id) {
    case PROP_PROXY:
        g_value_set_object(value, priv->proxy);
        break;
    case PROP_API:
        g_value_set_object(value, priv->api);
        break;
    case PROP_VM:
        g_value_set_object(value, priv->vm);
        break;
    case PROP_FILE:
        g_value_take_string(value,
                            ovirt_foreign_menu_get_current_iso_name(self));
        break;
    case PROP_FILES:
        g_value_set_pointer(value, priv->iso_names);
        break;
    case PROP_VM_GUID:
        g_value_set_string(value, priv->vm_guid);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
ovirt_foreign_menu_set_property(GObject *object, guint property_id,
                                       const GValue *value G_GNUC_UNUSED, GParamSpec *pspec)
{
    OvirtForeignMenu *self = OVIRT_FOREIGN_MENU(object);
    OvirtForeignMenuPrivate *priv = self->priv;

    switch (property_id) {
    case PROP_PROXY:
        if (priv->proxy != NULL) {
            g_object_unref(priv->proxy);
        }
        priv->proxy = g_value_dup_object(value);
        break;
    case PROP_API:
        if (priv->api != NULL) {
            g_object_unref(priv->api);
        }
        priv->api = g_value_dup_object(value);
        break;
    case PROP_VM:
        if (priv->vm != NULL) {
            g_object_unref(priv->vm);
        }
        priv->vm = g_value_dup_object(value);
        g_free(priv->vm_guid);
        priv->vm_guid = NULL;
        if (priv->vm != NULL) {
            g_object_get(G_OBJECT(priv->vm), "guid", &priv->vm_guid, NULL);
        }
        break;
    case PROP_VM_GUID:
        if (priv->vm != NULL) {
            g_object_unref(priv->vm);
            priv->vm = NULL;
        }
        g_free(priv->vm_guid);
        priv->vm_guid = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
ovirt_foreign_menu_dispose(GObject *obj)
{
    OvirtForeignMenu *self = OVIRT_FOREIGN_MENU(obj);

    if (self->priv->proxy) {
        g_object_unref(self->priv->proxy);
        self->priv->proxy = NULL;
    }

    if (self->priv->api != NULL) {
        g_object_unref(self->priv->api);
        self->priv->api = NULL;
    }

    if (self->priv->vm) {
        g_object_unref(self->priv->vm);
        self->priv->vm = NULL;
    }

    g_free(self->priv->vm_guid);
    self->priv->vm_guid = NULL;

    if (self->priv->files) {
        g_object_unref(self->priv->files);
        self->priv->files = NULL;
    }

    if (self->priv->cdrom) {
        g_object_unref(self->priv->cdrom);
        self->priv->cdrom = NULL;
    }

    if (self->priv->iso_names) {
        g_list_free_full(self->priv->iso_names, (GDestroyNotify)g_free);
        self->priv->iso_names = NULL;
    }

    g_free(self->priv->current_iso_name);
    self->priv->current_iso_name = NULL;

    g_free(self->priv->next_iso_name);
    self->priv->next_iso_name = NULL;

    G_OBJECT_CLASS(ovirt_foreign_menu_parent_class)->dispose(obj);
}


static void
ovirt_foreign_menu_class_init(OvirtForeignMenuClass *klass)
{
    GObjectClass *oclass = G_OBJECT_CLASS(klass);

    oclass->get_property = ovirt_foreign_menu_get_property;
    oclass->set_property = ovirt_foreign_menu_set_property;
    oclass->dispose = ovirt_foreign_menu_dispose;

    g_type_class_add_private(klass, sizeof(OvirtForeignMenuPrivate));

    g_object_class_install_property(oclass,
                                    PROP_PROXY,
                                    g_param_spec_object("proxy",
                                                        "OvirtProxy instance",
                                                        "OvirtProxy instance",
                                                        OVIRT_TYPE_PROXY,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oclass,
                                    PROP_API,
                                    g_param_spec_object("api",
                                                        "OvirtApi instance",
                                                        "Ovirt api root",
                                                        OVIRT_TYPE_API,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oclass,
                                    PROP_VM,
                                    g_param_spec_object("vm",
                                                        "OvirtVm instance",
                                                        "OvirtVm being handled",
                                                        OVIRT_TYPE_VM,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oclass,
                                    PROP_FILE,
                                    g_param_spec_string("file",
                                                         "File",
                                                         "Name of the image currently inserted in the virtual CDROM",
                                                         NULL,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oclass,
                                    PROP_FILES,
                                    g_param_spec_pointer("files",
                                                         "ISO names",
                                                         "GSList of ISO names for this oVirt VM",
                                                         G_PARAM_READABLE |
                                                         G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(oclass,
                                    PROP_VM_GUID,
                                    g_param_spec_string("vm-guid",
                                                         "VM GUID",
                                                         "GUID of the virtual machine to provide a foreign menu for",
                                                         NULL,
                                                         G_PARAM_READWRITE |
                                                         G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_STRINGS));
}


static void
ovirt_foreign_menu_init(OvirtForeignMenu *self)
{
    self->priv = OVIRT_FOREIGN_MENU_GET_PRIVATE(self);
}


OvirtForeignMenu* ovirt_foreign_menu_new(OvirtProxy *proxy)
{
    return g_object_new(OVIRT_TYPE_FOREIGN_MENU,
                        "proxy", proxy,
                        NULL);
}


static void
ovirt_foreign_menu_next_async_step(OvirtForeignMenu *menu,
                                   OvirtForeignMenuState current_state)
{
    g_return_if_fail(current_state >= STATE_0);
    g_return_if_fail(current_state < STATE_ISOS);

    current_state++;

    if (current_state == STATE_API) {
        if (menu->priv->api == NULL) {
            ovirt_foreign_menu_fetch_api_async(menu);
        } else {
            current_state++;
        }
    }

    if (current_state == STATE_VM) {
        if (menu->priv->vm == NULL) {
            ovirt_foreign_menu_fetch_vm_async(menu);
        } else {
            current_state++;
        }
    }

    if (current_state == STATE_STORAGE_DOMAIN) {
        if (menu->priv->files == NULL) {
            ovirt_foreign_menu_fetch_storage_domain_async(menu);
        } else {
            current_state++;
        }
    }

    if (current_state == STATE_VM_CDROM) {
        if (menu->priv->cdrom == NULL) {
            ovirt_foreign_menu_fetch_vm_cdrom_async(menu);
        } else {
            current_state++;
        }
    }

    if (current_state == STATE_CDROM_FILE) {
        ovirt_foreign_menu_refresh_cdrom_file_async(menu);
    }

    if (current_state == STATE_ISOS) {
        g_warn_if_fail(menu->priv->api != NULL);
        g_warn_if_fail(menu->priv->vm != NULL);
        g_warn_if_fail(menu->priv->files != NULL);
        g_warn_if_fail(menu->priv->cdrom != NULL);

        ovirt_foreign_menu_refresh_iso_list(menu);
    }
}


void
ovirt_foreign_menu_start(OvirtForeignMenu *menu)
{
    ovirt_foreign_menu_next_async_step(menu, STATE_0);
}


static void
ovirt_foreign_menu_activate_item_cb(GtkMenuItem *menuitem, gpointer user_data);


static void
menu_item_set_active_no_signal(GtkMenuItem *menuitem,
                               gboolean active,
                               GCallback callback,
                               gpointer user_data)
{
    g_signal_handlers_block_by_func(menuitem, callback, user_data);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menuitem), active);
    g_signal_handlers_unblock_by_func(menuitem, callback, user_data);
}


static void updated_cdrom_cb(GObject *source_object,
                             GAsyncResult *result,
                             gpointer user_data)
{
    GError *error = NULL;
    OvirtForeignMenu *foreign_menu;
    gboolean updated;

    foreign_menu = OVIRT_FOREIGN_MENU(user_data);
    updated = ovirt_cdrom_update_finish(OVIRT_CDROM(source_object),
                                        result, &error);
    g_debug("Finished updating cdrom content");
    if (updated) {
        g_free(foreign_menu->priv->current_iso_name);
        foreign_menu->priv->current_iso_name = foreign_menu->priv->next_iso_name;
        foreign_menu->priv->next_iso_name = NULL;
        g_object_notify(G_OBJECT(foreign_menu), "file");
    } else {
        /* Reset old state back as we were not successful in switching to
         * the new ISO */
        const char *current_file = foreign_menu->priv->current_iso_name;

        if (error != NULL) {
            g_warning("failed to update cdrom resource: %s", error->message);
            g_clear_error(&error);
        }
        g_debug("setting OvirtCdrom:file back to '%s'",
                current_file?current_file:NULL);
        g_object_set(foreign_menu->priv->cdrom, "file", current_file, NULL);
    }
    g_free(foreign_menu->priv->next_iso_name);
    foreign_menu->priv->next_iso_name = NULL;
}


static void
ovirt_foreign_menu_activate_item_cb(GtkMenuItem *menuitem, gpointer user_data)
{
    OvirtForeignMenu *foreign_menu;
    const char *iso_name;
    gboolean checked;

    checked = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menuitem));
    foreign_menu = OVIRT_FOREIGN_MENU(user_data);
    g_return_if_fail(foreign_menu->priv->cdrom != NULL);
    g_return_if_fail(foreign_menu->priv->next_iso_name == NULL);

    g_debug("'%s' clicked", gtk_menu_item_get_label(menuitem));

    /* We only want to move the check mark for the currently selected ISO
     * when ovirt_cdrom_update_async() is successful, so for now we move
     * the check mark back to where it was before
     */
    menu_item_set_active_no_signal(menuitem, !checked,
                                   (GCallback)ovirt_foreign_menu_activate_item_cb,
                                   foreign_menu);

    if (checked) {
        iso_name = gtk_menu_item_get_label(menuitem);
        g_debug("Updating VM cdrom image to '%s'", iso_name);
        foreign_menu->priv->next_iso_name = g_strdup(iso_name);
    } else {
        g_debug("Removing current cdrom image");
        iso_name = NULL;
        foreign_menu->priv->next_iso_name = NULL;
    }
    g_object_set(foreign_menu->priv->cdrom,
                 "file", iso_name,
                 NULL);
    ovirt_cdrom_update_async(foreign_menu->priv->cdrom, TRUE,
                             foreign_menu->priv->proxy, NULL,
                             updated_cdrom_cb, foreign_menu);
}


GtkWidget *ovirt_foreign_menu_get_gtk_menu(OvirtForeignMenu *foreign_menu)
{
    GtkWidget *gtk_menu;
    GList *it;
    char *current_iso;

    g_debug("Creating GtkMenu for foreign menu");
    if (foreign_menu->priv->iso_names == NULL) {
        return NULL;
    }
    current_iso = ovirt_foreign_menu_get_current_iso_name(foreign_menu);
    gtk_menu = gtk_menu_new();
    for (it = foreign_menu->priv->iso_names; it != NULL; it = it->next) {
        GtkWidget *menuitem;

        menuitem = gtk_check_menu_item_new_with_label((char *)it->data);
        if (g_strcmp0((char *)it->data, current_iso) == 0) {
            g_warn_if_fail(g_strcmp0(current_iso, foreign_menu->priv->current_iso_name) == 0);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menuitem),
                                           TRUE);
        }
        g_signal_connect(menuitem, "activate",
                         G_CALLBACK(ovirt_foreign_menu_activate_item_cb),
                         foreign_menu);
        gtk_menu_shell_append(GTK_MENU_SHELL(gtk_menu), menuitem);
    }
    g_free(current_iso);

    return gtk_menu;
}


static void ovirt_foreign_menu_set_files(OvirtForeignMenu *menu,
                                         const GList *files)
{
    GList *sorted_files = NULL;
    const GList *it;
    GList *it2;

    for (it = files; it != NULL; it = it->next) {
        char *name;
        g_object_get(it->data, "name", &name, NULL);
        /* The oVirt REST API is supposed to have a 'type' node
         * associated with file resources , but as of 3.2, this node
         * is not present, so we do an extension check instead
         * to differentiate between ISOs and floppy images */
        if (g_str_has_suffix(name, ".vfd")) {
            g_free(name);
            continue;
        }
        sorted_files = g_list_insert_sorted(sorted_files, name,
                                            (GCompareFunc)g_strcmp0);
    }

    for (it = sorted_files, it2 = menu->priv->iso_names;
         (it != NULL) && (it2 != NULL);
         it = it->next, it2 = it2->next) {
        if (g_strcmp0(it->data, it2->data) != 0) {
            break;
        }
    }

    if ((it == NULL) && (it2 == NULL)) {
        /* sorted_files and menu->priv->files content was the same */
        g_list_free_full(sorted_files, (GDestroyNotify)g_free);
        return;
    }

    g_list_free_full(menu->priv->iso_names, (GDestroyNotify)g_free);
    menu->priv->iso_names = sorted_files;
    g_object_notify(G_OBJECT(menu), "files");
}


static void cdrom_file_refreshed_cb(GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data)
{
    OvirtResource *cdrom  = OVIRT_RESOURCE(source_object);
    OvirtForeignMenu *menu = OVIRT_FOREIGN_MENU(user_data);
    GError *error = NULL;

    ovirt_resource_refresh_finish(cdrom, result, &error);
    if (error != NULL) {
        g_warning("failed to refresh cdrom content: %s", error->message);
        g_clear_error(&error);
        return;
    }

    /* Content of OvirtCdrom is now current */
    g_free(menu->priv->current_iso_name);
    if (menu->priv->cdrom != NULL) {
        g_object_get(G_OBJECT(menu->priv->cdrom),
                     "file", &menu->priv->current_iso_name,
                     NULL);
    } else {
        menu->priv->current_iso_name = NULL;
    }
    g_object_notify(G_OBJECT(menu), "file");
    if (menu->priv->cdrom != NULL) {
        ovirt_foreign_menu_next_async_step(menu, STATE_CDROM_FILE);
    } else {
        g_debug("Could not find VM cdrom through oVirt REST API");
    }
}


static void ovirt_foreign_menu_refresh_cdrom_file_async(OvirtForeignMenu *menu)
{
    g_return_if_fail(OVIRT_IS_RESOURCE(menu->priv->cdrom));

    ovirt_resource_refresh_async(OVIRT_RESOURCE(menu->priv->cdrom),
                                 menu->priv->proxy, NULL,
                                 cdrom_file_refreshed_cb, menu);
}


static void cdroms_fetched_cb(GObject *source_object,
                              GAsyncResult *result,
                              gpointer user_data)
{
    GHashTable *cdroms;
    OvirtCollection *cdrom_collection = OVIRT_COLLECTION(source_object);
    OvirtForeignMenu *menu = OVIRT_FOREIGN_MENU(user_data);
    GHashTableIter iter;
    OvirtCdrom *cdrom;
    GError *error = NULL;

    ovirt_collection_fetch_finish(cdrom_collection, result, &error);
    if (error != NULL) {
        g_warning("failed to fetch cdrom collection: %s", error->message);
        g_clear_error(&error);
        return;
    }

    cdroms = ovirt_collection_get_resources(cdrom_collection);

    g_warn_if_fail(g_hash_table_size(cdroms) <= 1);

    g_hash_table_iter_init(&iter, cdroms);
    /* Set CDROM drive. If we have multiple ones, only the first
     * one will be kept, but currently oVirt only adds one CDROM
     * device per-VM
     */
    if (g_hash_table_iter_next(&iter, NULL, (gpointer *)&cdrom)) {
        if (menu->priv->cdrom != NULL) {
            g_object_unref(G_OBJECT(menu->priv->cdrom));
        }
        menu->priv->cdrom = g_object_ref(G_OBJECT(cdrom));
        g_debug("Set VM cdrom to %p", menu->priv->cdrom);
    }

    if (menu->priv->cdrom != NULL) {
        ovirt_foreign_menu_next_async_step(menu, STATE_VM_CDROM);
    } else {
        g_debug("Could not find VM cdrom through oVirt REST API");
    }
}


static void ovirt_foreign_menu_fetch_vm_cdrom_async(OvirtForeignMenu *menu)
{
    OvirtCollection *cdrom_collection;

    cdrom_collection = ovirt_vm_get_cdroms(menu->priv->vm);
    ovirt_collection_fetch_async(cdrom_collection, menu->priv->proxy, NULL,
                                 cdroms_fetched_cb, menu);
}


static void storage_domains_fetched_cb(GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data)
{
    GError *error = NULL;
    OvirtForeignMenu *menu = OVIRT_FOREIGN_MENU(user_data);
    OvirtCollection *collection = OVIRT_COLLECTION(source_object);
    GHashTableIter iter;
    OvirtStorageDomain *domain;

    ovirt_collection_fetch_finish(collection, result, &error);
    if (error != NULL) {
        g_warning("failed to fetch storage domains: %s", error->message);
        g_clear_error(&error);
        return;
    }

    g_hash_table_iter_init(&iter, ovirt_collection_get_resources(collection));
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&domain)) {
        OvirtCollection *file_collection;
        int type;

        g_object_get(domain, "type", &type, NULL);
        if (type != OVIRT_STORAGE_DOMAIN_TYPE_ISO) {
            continue;
        }

        file_collection = ovirt_storage_domain_get_files(domain);
        if (file_collection != NULL) {
            if (menu->priv->files) {
                g_object_unref(G_OBJECT(menu->priv->files));
            }
            menu->priv->files = g_object_ref(G_OBJECT(file_collection));
            g_debug("Set VM files to %p", menu->priv->files);
            break;
        }
    }

    if (menu->priv->files != NULL) {
        ovirt_foreign_menu_next_async_step(menu, STATE_STORAGE_DOMAIN);
    } else {
        g_debug("Could not find iso file collection");
    }
}


static void ovirt_foreign_menu_fetch_storage_domain_async(OvirtForeignMenu *menu)
{
    OvirtCollection *collection;

    g_debug("Start fetching oVirt REST collection");
    collection = ovirt_api_get_storage_domains(menu->priv->api);
    ovirt_collection_fetch_async(collection, menu->priv->proxy, NULL,
                                 storage_domains_fetched_cb, menu);
}


static void vms_fetched_cb(GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data)
{
    GError *error = NULL;
    OvirtForeignMenu *menu = OVIRT_FOREIGN_MENU(user_data);
    OvirtCollection *collection;
    GHashTableIter iter;
    OvirtVm *vm;

    collection = OVIRT_COLLECTION(source_object);
    ovirt_collection_fetch_finish(collection, result, &error);
    if (error != NULL) {
        g_debug("failed to fetch VM list: %s", error->message);
        g_clear_error(&error);
        return;
    }

    g_hash_table_iter_init(&iter, ovirt_collection_get_resources(collection));
    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&vm)) {
        char *guid;

        g_object_get(G_OBJECT(vm), "guid", &guid, NULL);
        if (g_strcmp0(guid, menu->priv->vm_guid) == 0) {
            menu->priv->vm = g_object_ref(vm);
            g_free(guid);
            break;
        }
        g_free(guid);
    }
    if (menu->priv->vm != NULL) {
        ovirt_foreign_menu_next_async_step(menu, STATE_VM);
    } else {
        g_warning("failed to find a VM with guid \"%s\"", menu->priv->vm_guid);
    }
}


static void ovirt_foreign_menu_fetch_vm_async(OvirtForeignMenu *menu)
{
    OvirtCollection *vms;

    g_return_if_fail(OVIRT_IS_FOREIGN_MENU(menu));
    g_return_if_fail(OVIRT_IS_PROXY(menu->priv->proxy));
    g_return_if_fail(OVIRT_IS_API(menu->priv->api));

    vms = ovirt_api_get_vms(menu->priv->api);
    ovirt_collection_fetch_async(vms, menu->priv->proxy,
                                 NULL, vms_fetched_cb, menu);
}


static void api_fetched_cb(GObject *source_object,
                           GAsyncResult *result,
                           gpointer user_data)
{
    GError *error = NULL;
    OvirtProxy *proxy;
    OvirtForeignMenu *menu = OVIRT_FOREIGN_MENU(user_data);

    proxy = OVIRT_PROXY(source_object);
    menu->priv->api = ovirt_proxy_fetch_api_finish(proxy, result, &error);
    if (error != NULL) {
        g_debug("failed to fetch toplevel API object: %s", error->message);
        g_clear_error(&error);
        return;
    }
    g_return_if_fail(OVIRT_IS_API(menu->priv->api));

    ovirt_foreign_menu_next_async_step(menu, STATE_API);
}


static void ovirt_foreign_menu_fetch_api_async(OvirtForeignMenu *menu)
{
    g_debug("Start fetching oVirt main entry point");

    g_return_if_fail(OVIRT_IS_FOREIGN_MENU(menu));
    g_return_if_fail(OVIRT_IS_PROXY(menu->priv->proxy));

    ovirt_proxy_fetch_api_async(menu->priv->proxy, NULL, api_fetched_cb, menu);
}


static void iso_list_fetched_cb(GObject *source_object,
                                GAsyncResult *result,
                                gpointer user_data)
{
    OvirtCollection *collection = OVIRT_COLLECTION(source_object);
    GError *error = NULL;
    GList *files;

    ovirt_collection_fetch_finish(collection, result, &error);
    if (error != NULL) {
        g_warning("failed to fetch files for ISO storage domain: %s",
                   error->message);
        g_clear_error(&error);
        return;
    }

    files = g_hash_table_get_values(ovirt_collection_get_resources(collection));
    ovirt_foreign_menu_set_files(OVIRT_FOREIGN_MENU(user_data), files);
    g_list_free(files);

    g_timeout_add_seconds(15, ovirt_foreign_menu_refresh_iso_list, user_data);
}


static void ovirt_foreign_menu_fetch_iso_list_async(OvirtForeignMenu *menu)
{
    if (menu->priv->files == NULL) {
        return;
    }

    ovirt_collection_fetch_async(menu->priv->files, menu->priv->proxy,
                                 NULL, iso_list_fetched_cb, menu);
}


static gboolean ovirt_foreign_menu_refresh_iso_list(gpointer user_data)
{
    OvirtForeignMenu *menu;

    g_debug("Refreshing foreign menu iso list");
    menu = OVIRT_FOREIGN_MENU(user_data);
    ovirt_foreign_menu_fetch_iso_list_async(menu);

    /* ovirt_foreign_menu_fetch_iso_list_async() will schedule a new call to
     * that function through iso_list_fetched_cb() when it has finished
     * fetching the iso list
     */
    return G_SOURCE_REMOVE;
}


OvirtForeignMenu *ovirt_foreign_menu_new_from_file(VirtViewerFile *file)
{
    OvirtProxy *proxy = NULL;
    OvirtForeignMenu *menu = NULL;
    gboolean admin;
    char *ca_str = NULL;
    char *jsessionid = NULL;
    char *url = NULL;
    char *vm_guid = NULL;
    GByteArray *ca = NULL;

    url = virt_viewer_file_get_ovirt_host(file);
    vm_guid = virt_viewer_file_get_ovirt_vm_guid(file);
    jsessionid = virt_viewer_file_get_ovirt_jsessionid(file);
    ca_str = virt_viewer_file_get_ovirt_ca(file);
    admin = virt_viewer_file_get_ovirt_admin(file);

    if ((url == NULL) || (vm_guid == NULL))
        goto end;

    proxy = ovirt_proxy_new(url);
    if (proxy == NULL)
        goto end;

    if (ca_str != NULL) {
        ca = g_byte_array_new_take((guint8 *)ca_str, strlen(ca_str) + 1);
        ca_str = NULL;
    }

    g_object_set(G_OBJECT(proxy),
                 "admin", admin,
                 "session-id", jsessionid,
                 "ca-cert", ca,
                 NULL);
    menu = g_object_new(OVIRT_TYPE_FOREIGN_MENU,
                        "proxy", proxy,
                        "vm-guid", vm_guid,
                        NULL);

end:
    g_free(url);
    g_free(vm_guid);
    g_free(jsessionid);
    g_free(ca_str);
    if (ca != NULL) {
        g_byte_array_unref(ca);
    }

    return menu;
}
