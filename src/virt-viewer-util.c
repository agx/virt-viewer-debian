/*
 * Virt Viewer: A virtual machine console viewer
 *
 * Copyright (C) 2007-2009 Red Hat,
 * Copyright (C) 2009 Daniel P. Berrange
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
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>

#include "virt-viewer-util.h"

GtkBuilder *virt_viewer_util_load_ui(const char *name)
{
	struct stat sb;
	GtkBuilder *builder;
	GError *error = NULL;

	builder = gtk_builder_new();
	if (stat(name, &sb) >= 0) {
		gtk_builder_add_from_file(builder, name, &error);
	} else {
		gchar *path = g_strdup_printf("%s/%s", BUILDER_XML_DIR, name);
		gtk_builder_add_from_file(builder, path, &error);
		g_free(path);
	}

	if (error)
		g_error("Cannot load UI description %s: %s", name,
			error->message);

	return builder;
}

int
virt_viewer_util_extract_host(const char *uristr,
                              char **scheme,
                              char **host,
                              char **transport,
                              char **user,
                              int *port)
{
	xmlURIPtr uri;
	char *offset;

	if (uristr == NULL ||
	    !g_strcasecmp(uristr, "xen"))
		uristr = "xen:///";

	uri = xmlParseURI(uristr);
        if (host) {
                if (!uri || !uri->server)
                        *host = g_strdup("localhost");
                else
                        *host = g_strdup(uri->server);
        }

        if (user) {
                if (uri->user)
                        *user = g_strdup(uri->user);
                else
                        *user = NULL;
        }

	if (port)
		*port = uri->port;

	offset = strchr(uri->scheme, '+');

        if (transport) {
                if (offset)
                        *transport = g_strdup(offset+1);
                else
                        *transport = NULL;
        }

	if (scheme) {
		if (offset)
			*scheme = g_strndup(uri->scheme, offset - uri->scheme);
		else
			*scheme = g_strdup(uri->scheme);
	}

	xmlFreeURI(uri);
	return 0;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
