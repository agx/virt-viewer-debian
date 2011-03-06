/*
 * Virt Viewer: A virtual machine console viewer
 *
 * Copyright (C) 2007-2009 Red Hat,
 * Copyright (C) 2009 Daniel P. Berrange
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
 */

#include <config.h>

#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include "viewer.h"
#include "viewer-priv.h"
#include "events.h"
#include "auth.h"
#include "display-vnc.h"

#ifdef HAVE_SPICE_GTK
#include "display-spice.h"
#endif

#define SCALE(x) do { x = viewer->fullscreen ? x : x * viewer->zoomlevel / 100; } while (0);

gboolean doDebug = FALSE;

static const char * const menuNames[LAST_MENU] = {
	"menu-file", "menu-view", "menu-send", "menu-help"
};


#define MAX_KEY_COMBO 3
struct	keyComboDef {
	guint keys[MAX_KEY_COMBO];
	guint nkeys;
	const char *label;
};

static const struct keyComboDef keyCombos[] = {
	{ { GDK_Control_L, GDK_Alt_L, GDK_Delete }, 3, "Ctrl+Alt+_Del"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_BackSpace }, 3, "Ctrl+Alt+_Backspace"},
	{ {}, 0, "" },
	{ { GDK_Control_L, GDK_Alt_L, GDK_F1 }, 3, "Ctrl+Alt+F_1"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F2 }, 3, "Ctrl+Alt+F_2"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F3 }, 3, "Ctrl+Alt+F_3"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F4 }, 3, "Ctrl+Alt+F_4"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F5 }, 3, "Ctrl+Alt+F_5"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F6 }, 3, "Ctrl+Alt+F_6"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F7 }, 3, "Ctrl+Alt+F_7"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F8 }, 3, "Ctrl+Alt+F_8"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F5 }, 3, "Ctrl+Alt+F_9"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F6 }, 3, "Ctrl+Alt+F1_0"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F7 }, 3, "Ctrl+Alt+F11"},
	{ { GDK_Control_L, GDK_Alt_L, GDK_F8 }, 3, "Ctrl+Alt+F12"},
	{ {}, 0, "" },
	{ { GDK_Print }, 1, "_PrintScreen"},
};


static gboolean viewer_connect_timer(void *opaque);
static int viewer_initial_connect(VirtViewer *viewer);


void viewer_simple_message_dialog(GtkWidget *window, const char *fmt, ...)
{
	GtkWidget *dialog;
	char *msg;
	va_list vargs;

	va_start(vargs, fmt);

	msg = g_strdup_vprintf(fmt, vargs);

	va_end(vargs);

	dialog = gtk_message_dialog_new(GTK_WINDOW(window),
					GTK_DIALOG_MODAL |
					GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_MESSAGE_ERROR,
					GTK_BUTTONS_OK,
					"%s",
					msg);

	gtk_dialog_run(GTK_DIALOG(dialog));

	gtk_widget_destroy(dialog);

	g_free(msg);
}


void viewer_add_display_and_realize(VirtViewer *viewer)
{
	GtkWidget *notebook;
	GtkWidget *align;

	g_return_if_fail(viewer != NULL);
	g_return_if_fail(viewer->display != NULL);
	g_return_if_fail(viewer->display->widget != NULL);

	notebook = glade_xml_get_widget(viewer->glade, "notebook");
	if (viewer->display->need_align) {
		align = glade_xml_get_widget(viewer->glade, "display-align");
		gtk_container_add(GTK_CONTAINER(align), viewer->display->widget);
	} else {
		gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), 2);
		if (gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), viewer->display->widget,
					     NULL, 2) == -1)
			g_warning("failed to insert a notebook page");
	}

	if (!viewer->window) {
		gtk_container_add(GTK_CONTAINER(viewer->container), GTK_WIDGET(notebook));
		gtk_widget_show_all(viewer->container);
	}

	gtk_widget_realize(viewer->display->widget);
}

/* Now that the size is set to our preferred sizing, this
 * triggers another resize calculation but without our
 * scrolled window callback active. This is the key that
 * allows us to set the fixed size, but then allow the user
 * to later resize it smaller again
 */
static gboolean
viewer_unset_widget_size_cb(gpointer data)
{
	GtkWidget *widget = data;
	DEBUG_LOG("Unset requisition on widget=%p", widget);

	gtk_widget_queue_resize_no_redraw (widget);

	return FALSE;
}

/*
 * This sets the actual size of the widget, and then
 * sets an idle callback to resize again, without constraints
 * activated
 */
static gboolean
viewer_set_widget_size_cb(GtkWidget *widget,
			  GtkRequisition *req,
			  gpointer data)
{
	VirtViewerSize *size = data;
	DEBUG_LOG("Set requisition on widget=%p to %dx%d", widget, size->width, size->height);

	req->width = size->width;
	req->height = size->height;

	g_signal_handler_disconnect (widget, size->sig_id);
	g_free (size);
	g_idle_add (viewer_unset_widget_size_cb, widget);

	return FALSE;
}


/*
 * Registers a callback used to set the widget size once
 */
static void
viewer_set_widget_size(VirtViewer *viewer,
		       GtkWidget *widget,
		       int width,
		       int height)
{
	VirtViewerSize *size = g_new (VirtViewerSize, 1);
	DEBUG_LOG("Queue resize widget=%p width=%d height=%d", widget, width, height);
	size->viewer = viewer;
	size->width = width;
	size->height = height;
	size->sig_id = g_signal_connect
		(widget, "size-request",
		 G_CALLBACK (viewer_set_widget_size_cb),
		 size);

	gtk_widget_queue_resize (widget);
}


/*
 * This code attempts to resize the top level window to be large enough
 * to contain the entire display desktop at 1:1 ratio. If the local desktop
 * isn't large enough that it goes as large as possible and lets the display
 * scale down to fit, maintaining aspect ratio
 */
void viewer_resize_main_window(VirtViewer *viewer)
{
	GdkRectangle fullscreen;
	GdkScreen *screen;
	int width, height;
	double desktopAspect;
	double screenAspect;

	DEBUG_LOG("Preparing main window resize");
	if (!viewer->active) {
		DEBUG_LOG("Skipping inactive resize");
		return;
	}

	gtk_window_resize(GTK_WINDOW (viewer->window), 1, 1);

	screen = gdk_drawable_get_screen(gtk_widget_get_window(viewer->window));
	gdk_screen_get_monitor_geometry(screen,
					gdk_screen_get_monitor_at_window
					(screen, gtk_widget_get_window(viewer->window)),
					&fullscreen);

	desktopAspect = (double)viewer->desktopWidth / (double)viewer->desktopHeight;
	screenAspect = (double)(fullscreen.width - 128) / (double)(fullscreen.height - 128);

	if ((viewer->desktopWidth > (fullscreen.width - 128)) ||
	    (viewer->desktopHeight > (fullscreen.height - 128))) {
		/* Doesn't fit native res, so go as large as possible
		   maintaining aspect ratio */
		if (screenAspect > desktopAspect) {
			width = viewer->desktopHeight * desktopAspect;
			height = viewer->desktopHeight;
		} else {
			width = viewer->desktopWidth;
			height = viewer->desktopWidth / desktopAspect;
		}
	} else {
		width = viewer->desktopWidth;
		height = viewer->desktopHeight;
	}

	SCALE(width);
	SCALE(height);

	viewer_set_widget_size(viewer,
			       glade_xml_get_widget(viewer->glade, "display-align"),
			       width,
			       height);
}

static void viewer_menu_view_zoom_out(G_GNUC_UNUSED GtkWidget *menu, VirtViewer *viewer)
{
	viewer->zoomlevel -= 10;
	if (viewer->zoomlevel < 10)
		viewer->zoomlevel = 10;

	viewer_resize_main_window(viewer);
}

static void viewer_menu_view_zoom_in(G_GNUC_UNUSED GtkWidget *menu, VirtViewer *viewer)
{
	viewer->zoomlevel += 10;
	if (viewer->zoomlevel > 200)
		viewer->zoomlevel = 200;

	viewer_resize_main_window(viewer);
}

static void viewer_menu_view_zoom_reset(G_GNUC_UNUSED GtkWidget *menu, VirtViewer *viewer)
{
	viewer->zoomlevel = 100;

	viewer_resize_main_window(viewer);
}

void viewer_set_title(VirtViewer *viewer, gboolean grabbed)
{
	char *title;
	const char *subtitle;

	if (!viewer->window)
		return;

	if (grabbed)
		subtitle = "(Press Ctrl+Alt to release pointer) ";
	else
		subtitle = "";

	title = g_strdup_printf("%s%s - Virt Viewer",
				subtitle, viewer->domtitle);

	gtk_window_set_title(GTK_WINDOW(viewer->window), title);

	g_free(title);
}

static gboolean viewer_ignore_accel(GtkWidget *menu G_GNUC_UNUSED,
				    VirtViewer *viewer G_GNUC_UNUSED)
{
	/* ignore accelerator */
	return TRUE;
}


void viewer_disable_modifiers(VirtViewer *viewer)
{
	GtkSettings *settings = gtk_settings_get_default();
	GValue empty;
	GSList *accels;
	int i;

	if (!viewer->window)
		return;

	if (!viewer->accelEnabled)
		return;

	/* This stops F10 activating menu bar */
	memset(&empty, 0, sizeof empty);
	g_value_init(&empty, G_TYPE_STRING);
	g_object_get_property(G_OBJECT(settings), "gtk-menu-bar-accel", &viewer->accelSetting);
	g_object_set_property(G_OBJECT(settings), "gtk-menu-bar-accel", &empty);

	/* This stops global accelerators like Ctrl+Q == Quit */
	for (accels = viewer->accelList ; accels ; accels = accels->next) {
		gtk_window_remove_accel_group(GTK_WINDOW(viewer->window), accels->data);
	}

	/* This stops menu bar shortcuts like Alt+F == File */
	for (i = 0 ; i < LAST_MENU ; i++) {
		GtkWidget *menu = glade_xml_get_widget(viewer->glade, menuNames[i]);
		viewer->accelMenuSig[i] =
			g_signal_connect(menu, "mnemonic-activate",
					 G_CALLBACK(viewer_ignore_accel), viewer);
	}

	viewer->accelEnabled = FALSE;
}


void viewer_enable_modifiers(VirtViewer *viewer)
{
	GtkSettings *settings = gtk_settings_get_default();
	GSList *accels;
	int i;

	if (!viewer->window)
		return;

	if (viewer->accelEnabled)
		return;

	/* This allows F10 activating menu bar */
	g_object_set_property(G_OBJECT(settings), "gtk-menu-bar-accel", &viewer->accelSetting);

	/* This allows global accelerators like Ctrl+Q == Quit */
	for (accels = viewer->accelList ; accels ; accels = accels->next) {
		gtk_window_add_accel_group(GTK_WINDOW(viewer->window), accels->data);
	}

	/* This allows menu bar shortcuts like Alt+F == File */
	for (i = 0 ; i < LAST_MENU ; i++) {
		GtkWidget *menu = glade_xml_get_widget(viewer->glade, menuNames[i]);
		g_signal_handler_disconnect(menu, viewer->accelMenuSig[i]);
	}

	viewer->accelEnabled = TRUE;
}

void viewer_quit(VirtViewer *viewer)
{
	g_return_if_fail(viewer != NULL);

	if (viewer->display)
		virt_viewer_display_close(viewer->display);
	gtk_main_quit();
}

static void viewer_delete(GtkWidget *src G_GNUC_UNUSED, void *dummy G_GNUC_UNUSED, VirtViewer *viewer)
{
	viewer_quit(viewer);
}

static void viewer_menu_file_quit(GtkWidget *src G_GNUC_UNUSED, VirtViewer *viewer)
{
	viewer_quit(viewer);
}

static void viewer_menu_view_fullscreen(GtkWidget *menu, VirtViewer *viewer)
{
	if (!viewer->window)
		return;

	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu))) {
		viewer->fullscreen = TRUE;
		gtk_window_fullscreen(GTK_WINDOW(viewer->window));
	} else {
		viewer->fullscreen = FALSE;
		gtk_window_unfullscreen(GTK_WINDOW(viewer->window));
		if (viewer->autoResize)
			viewer_resize_main_window(viewer);
	}
}

static void viewer_menu_view_resize(GtkWidget *menu, VirtViewer *viewer)
{
	if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(menu))) {
		viewer->autoResize = TRUE;
		if (!viewer->fullscreen)
			viewer_resize_main_window(viewer);
	} else {
		viewer->autoResize = FALSE;
	}
}

static void viewer_menu_send(GtkWidget *menu G_GNUC_UNUSED, VirtViewer *viewer)
{
	int i;
	GtkWidget *label = gtk_bin_get_child(GTK_BIN(menu));
	const char *text = gtk_label_get_label(GTK_LABEL(label));

	for (i = 0 ; i < G_N_ELEMENTS(keyCombos) ; i++) {
		if (!strcmp(text, keyCombos[i].label)) {
			DEBUG_LOG("Sending key combo %s", gtk_label_get_text(GTK_LABEL(label)));
			virt_viewer_display_send_keys(viewer->display,
						      keyCombos[i].keys,
						      keyCombos[i].nkeys);
			return;
		}
	}
	DEBUG_LOG("Failed to find key combo %s", gtk_label_get_text(GTK_LABEL(label)));
}


static void viewer_save_screenshot(VirtViewer *viewer, const char *file)
{
	GdkPixbuf *pix = virt_viewer_display_get_pixbuf(viewer->display);
	gdk_pixbuf_save(pix, file, "png", NULL,
			"tEXt::Generator App", PACKAGE, NULL);
	gdk_pixbuf_unref(pix);
}

static void viewer_menu_file_screenshot(GtkWidget *menu G_GNUC_UNUSED, VirtViewer *viewer)
{
	GtkWidget *dialog;

	g_return_if_fail(viewer->display != NULL);

	dialog = gtk_file_chooser_dialog_new ("Save screenshot",
					      NULL,
					      GTK_FILE_CHOOSER_ACTION_SAVE,
					      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					      GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT,
					      NULL);
	gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);

	//gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), default_folder_for_saving);
	//gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), "Screenshot");

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
		char *filename;

		filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
		viewer_save_screenshot(viewer, filename);
		g_free (filename);
	}

	gtk_widget_destroy (dialog);
}

static void viewer_about_close(GtkWidget *dialog, VirtViewer *viewer G_GNUC_UNUSED)
{
	gtk_widget_hide(dialog);
	gtk_widget_destroy(dialog);
}

static void viewer_about_delete(GtkWidget *dialog, void *dummy G_GNUC_UNUSED, VirtViewer *viewer G_GNUC_UNUSED)
{
	gtk_widget_hide(dialog);
	gtk_widget_destroy(dialog);
}

static void viewer_menu_help_about(GtkWidget *menu G_GNUC_UNUSED, VirtViewer *viewer)
{
	GladeXML *about;
	GtkWidget *dialog;

	about = viewer_load_glade("about.glade", "about");

	dialog = glade_xml_get_widget(about, "about");
	gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), VERSION);
	glade_xml_signal_connect_data(about, "about_delete",
				      G_CALLBACK(viewer_about_delete), viewer);
	glade_xml_signal_connect_data(about, "about_close",
				      G_CALLBACK(viewer_about_close), viewer);

	gtk_widget_show_all(dialog);

	g_object_unref(G_OBJECT(about));
}



static int viewer_parse_uuid(const char *name, unsigned char *uuid)
{
	int i;

	const char *cur = name;
	for (i = 0;i < 16;) {
		uuid[i] = 0;
		if (*cur == 0)
			return -1;
		if ((*cur == '-') || (*cur == ' ')) {
			cur++;
			continue;
		}
		if ((*cur >= '0') && (*cur <= '9'))
			uuid[i] = *cur - '0';
		else if ((*cur >= 'a') && (*cur <= 'f'))
			uuid[i] = *cur - 'a' + 10;
		else if ((*cur >= 'A') && (*cur <= 'F'))
			uuid[i] = *cur - 'A' + 10;
		else
			return -1;
		uuid[i] *= 16;
		cur++;
		if (*cur == 0)
			return -1;
		if ((*cur >= '0') && (*cur <= '9'))
			uuid[i] += *cur - '0';
		else if ((*cur >= 'a') && (*cur <= 'f'))
			uuid[i] += *cur - 'a' + 10;
		else if ((*cur >= 'A') && (*cur <= 'F'))
			uuid[i] += *cur - 'A' + 10;
		else
			return -1;
		i++;
		cur++;
	}

	return 0;
}


static virDomainPtr viewer_lookup_domain(VirtViewer *viewer)
{
	char *end;
	int id = strtol(viewer->domkey, &end, 10);
	virDomainPtr dom = NULL;
	unsigned char uuid[16];

	if (id >= 0 && end && !*end) {
		dom = virDomainLookupByID(viewer->conn, id);
	}
	if (!dom && viewer_parse_uuid(viewer->domkey, uuid) == 0) {
		dom = virDomainLookupByUUID(viewer->conn, uuid);
	}
	if (!dom) {
		dom = virDomainLookupByName(viewer->conn, viewer->domkey);
	}
	return dom;
}

static int viewer_matches_domain(VirtViewer *viewer,
				 virDomainPtr dom)
{
	char *end;
	const char *name;
	int id = strtol(viewer->domkey, &end, 10);
	unsigned char wantuuid[16];
	unsigned char domuuid[16];

	if (id >= 0 && end && !*end) {
		if (virDomainGetID(dom) == id)
			return 1;
	}
	if (viewer_parse_uuid(viewer->domkey, wantuuid) == 0) {
		virDomainGetUUID(dom, domuuid);
		if (memcmp(wantuuid, domuuid, VIR_UUID_BUFLEN) == 0)
			return 1;
	}

	name = virDomainGetName(dom);
	if (strcmp(name, viewer->domkey) == 0)
		return 1;

	return 0;
}

static char * viewer_extract_xpath_string(const gchar *xmldesc, const gchar *xpath)
{
	xmlDocPtr xml = NULL;
	xmlParserCtxtPtr pctxt = NULL;
	xmlXPathContextPtr ctxt = NULL;
	xmlXPathObjectPtr obj = NULL;
	char *port = NULL;

	pctxt = xmlNewParserCtxt();
	if (!pctxt || !pctxt->sax)
		goto error;

	xml = xmlCtxtReadDoc(pctxt, (const xmlChar *)xmldesc, "domain.xml", NULL,
			     XML_PARSE_NOENT | XML_PARSE_NONET |
			     XML_PARSE_NOWARNING);
	if (!xml)
		goto error;

	ctxt = xmlXPathNewContext(xml);
	if (!ctxt)
		goto error;

	obj = xmlXPathEval((const xmlChar *)xpath, ctxt);
	if (!obj || obj->type != XPATH_STRING || !obj->stringval || !obj->stringval[0])
		goto error;
	if (!strcmp((const char*)obj->stringval, "-1"))
		goto error;

	port = g_strdup((const char*)obj->stringval);
	xmlXPathFreeObject(obj);
	obj = NULL;

 error:
	if (obj)
		xmlXPathFreeObject(obj);
	if (ctxt)
		xmlXPathFreeContext(ctxt);
	if (xml)
		xmlFreeDoc(xml);
	if (pctxt)
		xmlFreeParserCtxt(pctxt);
	return port;
}


static int viewer_extract_host(const char *uristr, char **host, char **transport, char **user, int *port)
{
	xmlURIPtr uri;
	char *offset;

	*host = NULL;
	*transport = NULL;
	*user = NULL;

	if (uristr == NULL ||
	    !g_strcasecmp(uristr, "xen"))
		uristr = "xen:///";

	uri = xmlParseURI(uristr);
	if (!uri || !uri->server) {
		*host = g_strdup("localhost");
	} else {
		*host = g_strdup(uri->server);
	}

	if (uri->user)
		*user = g_strdup(uri->user);
	*port = uri->port;

	offset = strchr(uri->scheme, '+');
	if (offset)
		*transport = g_strdup(offset+1);

	xmlFreeURI(uri);
	return 0;
}

#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)

static int viewer_open_tunnel(const char **cmd)
{
	int fd[2];
	pid_t pid;

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, fd) < 0)
		return -1;

	pid = fork();
	if (pid == -1) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}

	if (pid == 0) { /* child */
		close(fd[0]);
		close(0);
		close(1);
		if (dup(fd[1]) < 0)
			_exit(1);
		if (dup(fd[1]) < 0)
			_exit(1);
		close(fd[1]);
		execvp("ssh", (char *const*)cmd);
		_exit(1);
	}
	close(fd[1]);
	return fd[0];
}


static int viewer_open_tunnel_ssh(const char *sshhost, int sshport, const char *sshuser,
				  const char *port, const char *unixsock)
{
	const char *cmd[10];
	char portstr[50];
	int n = 0;

	if (!sshport)
		sshport = 22;

	sprintf(portstr, "%d", sshport);

	cmd[n++] = "ssh";
	cmd[n++] = "-p";
	cmd[n++] = portstr;
	if (sshuser) {
		cmd[n++] = "-l";
		cmd[n++] = sshuser;
	}
	cmd[n++] = sshhost;
	cmd[n++] = "nc";
	if (port) {
		cmd[n++] = "localhost";
		cmd[n++] = port;
	} else {
		cmd[n++] = "-U";
		cmd[n++] = unixsock;
	}
	cmd[n++] = NULL;

	return viewer_open_tunnel(cmd);
}

static int viewer_open_unix_sock(const char *unixsock)
{
	struct sockaddr_un addr;
	int fd;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, unixsock);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

#endif /* defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK) */

void viewer_set_status(VirtViewer *viewer, const char *text)
{
	GtkWidget *status, *notebook;

	notebook = glade_xml_get_widget(viewer->glade, "notebook");
	status = glade_xml_get_widget(viewer->glade, "status");

	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 0);
	gtk_label_set_text(GTK_LABEL(status), text);
}


static void viewer_show_display(VirtViewer *viewer)
{
	GtkWidget *notebook;

	g_return_if_fail(viewer != NULL);
	g_return_if_fail(viewer->display != NULL);
	g_return_if_fail(viewer->display->widget != NULL);

	notebook = glade_xml_get_widget(viewer->glade, "notebook");
	gtk_widget_show(viewer->display->widget);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook),
				      viewer->display->need_align ? 1 : 2);
}

static void viewer_connect_info_free(VirtViewer *viewer)
{
	free(viewer->host);
	free(viewer->gport);
	free(viewer->transport);
	free(viewer->user);

	viewer->host = NULL;
	viewer->gport = NULL;
	viewer->transport = NULL;
	viewer->user = NULL;
	viewer->port = 0;
}

static gboolean viewer_extract_connect_info(VirtViewer *viewer,
					    virDomainPtr dom)
{
	char *type = NULL;
	char *xpath = NULL;
	gboolean retval = FALSE;
	char *xmldesc = virDomainGetXMLDesc(dom, 0);

	viewer_connect_info_free(viewer);

	if ((type = viewer_extract_xpath_string(xmldesc, "string(/domain/devices/graphics/@type)")) == NULL) {
		viewer_simple_message_dialog(viewer->window, _("Cannot determine the graphic type for the guest %s"),
					     viewer->domkey);
		goto cleanup;
	}

	if (g_strcasecmp(type, "vnc") == 0)
		viewer->display = VIRT_VIEWER_DISPLAY(virt_viewer_display_vnc_new(viewer));
#ifdef HAVE_SPICE_GTK
	else if (g_strcasecmp(type, "spice") == 0)
		viewer->display = VIRT_VIEWER_DISPLAY(virt_viewer_display_spice_new(viewer));
#endif
	else {
		viewer_simple_message_dialog(viewer->window, _("Unknown graphic type for the guest %s"),
					     viewer->domkey);
		goto cleanup;
	}

	xpath = g_strdup_printf("string(/domain/devices/graphics[@type='%s']/@port)", type);
	if ((viewer->gport = viewer_extract_xpath_string(xmldesc, xpath)) == NULL) {
		free(xpath);
		xpath = g_strdup_printf("string(/domain/devices/graphics[@type='%s']/@socket)", type);
		if ((viewer->unixsock = viewer_extract_xpath_string(xmldesc, xpath)) == NULL) {
			viewer_simple_message_dialog(viewer->window, _("Cannot determine the graphic address for the guest %s"),
						     viewer->domkey);
			goto cleanup;
		}
	}

	DEBUG_LOG("Guest graphics address is %s", viewer->gport ? viewer->gport : viewer->unixsock);

	if (viewer_extract_host(viewer->uri, &viewer->host, &viewer->transport, &viewer->user, &viewer->port) < 0) {
		viewer_simple_message_dialog(viewer->window, _("Cannot determine the host for the guest %s"),
					     viewer->domkey);
		goto cleanup;
	}

	retval = TRUE;

cleanup:
	free(xpath);
	free(xmldesc);
	return retval;
}

void viewer_channel_open_fd(VirtViewer *viewer, VirtViewerDisplayChannel *channel)
{
#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)
	int fd = -1;

	g_return_if_fail(viewer != NULL);
	g_return_if_fail(viewer->display != NULL);

	if (viewer->transport && g_strcasecmp(viewer->transport, "ssh") == 0 &&
	    !viewer->direct) {
		if ((fd = viewer_open_tunnel_ssh(viewer->host, viewer->port, viewer->user, viewer->gport, NULL)) < 0)
			viewer_simple_message_dialog(viewer->window, _("Connect to ssh failed."));
	} else
		viewer_simple_message_dialog(viewer->window, _("Can't connect to channel, SSH only supported."));

	if (fd >= 0)
		virt_viewer_display_channel_open_fd(viewer->display, channel, fd);
#else
	viewer_simple_message_dialog(viewer->window, _("Connect to channel unsupported."));
#endif
}

static int viewer_activate(VirtViewer *viewer,
			   virDomainPtr dom)
{
	int fd = -1;
	int ret = -1;

	g_return_val_if_fail(viewer->display == NULL, -1);

	if (viewer->active)
		goto cleanup;

	if (!viewer_extract_connect_info(viewer, dom))
		goto cleanup;

	if (viewer->gport)
		viewer->pretty_address = g_strdup_printf("%s:%s", viewer->host, viewer->gport);
	else
		viewer->pretty_address = g_strdup_printf("%s:%s", viewer->host, viewer->unixsock);

#if defined(HAVE_SOCKETPAIR) && defined(HAVE_FORK)
	if (viewer->transport &&
	    g_strcasecmp(viewer->transport, "ssh") == 0 &&
	    !viewer->direct) {
		DEBUG_LOG("Opening SSH tunnel to %s@%s:%d (%s)",
			  viewer->user, viewer->host,
			  viewer->port, viewer->gport ? viewer->gport : viewer->unixsock);
		if ((fd = viewer_open_tunnel_ssh(viewer->host, viewer->port,
						 viewer->user, viewer->gport, viewer->unixsock)) < 0)
			return -1;
	} else if (viewer->unixsock) {
		DEBUG_LOG("Connecting to UNIX socket %s", viewer->unixsock);
		if ((fd = viewer_open_unix_sock(viewer->unixsock)) < 0)
			return -1;
	}
#endif

	if (fd >= 0) {
		DEBUG_LOG("Connecting to tunnel %d", fd);
		ret = virt_viewer_display_open_fd(viewer->display, fd);
	} else {
		DEBUG_LOG("Connecting to TCP socket %s:%s", viewer->host, viewer->gport);
		ret = virt_viewer_display_open_host(viewer->display,
						    viewer->host, viewer->gport);
	}

	viewer_set_status(viewer, "Connecting to graphic server");

	free(viewer->domtitle);
	viewer->domtitle = g_strdup(virDomainGetName(dom));

	viewer->connected = FALSE;
	viewer->active = TRUE;
	viewer_set_title(viewer, FALSE);

cleanup:
	return ret;
}

/* text was actually requested */
static void viewer_vnc_clipboard_copy(GtkClipboard *clipboard G_GNUC_UNUSED,
				      GtkSelectionData *data,
				      guint info G_GNUC_UNUSED,
				      VirtViewer *viewer)
{
	gtk_selection_data_set_text(data, viewer->clipboard, -1);
}

void viewer_server_cut_text(VirtViewer *viewer, const gchar *text)
{
	GtkClipboard *cb;
	gsize a, b;
	GtkTargetEntry targets[] = {
		{g_strdup("UTF8_STRING"), 0, 0},
		{g_strdup("COMPOUND_TEXT"), 0, 0},
		{g_strdup("TEXT"), 0, 0},
		{g_strdup("STRING"), 0, 0},
	};

	if (!text)
		return;

	g_free (viewer->clipboard);
	viewer->clipboard = g_convert (text, -1, "utf-8", "iso8859-1", &a, &b, NULL);

	if (viewer->clipboard) {
		cb = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

		gtk_clipboard_set_with_owner (cb,
					      targets,
					      G_N_ELEMENTS(targets),
					      (GtkClipboardGetFunc)viewer_vnc_clipboard_copy,
					      NULL,
					      G_OBJECT (viewer));
	}
}

static gboolean viewer_retryauth(gpointer opaque)
{
	VirtViewer *viewer = opaque;
	viewer_initial_connect(viewer);

	return FALSE;
}

static void viewer_deactivate(VirtViewer *viewer)
{
	if (!viewer->active)
		return;

	if (viewer->display)
		virt_viewer_display_close(viewer->display);
	free(viewer->domtitle);
	viewer->domtitle = NULL;

	viewer->connected = FALSE;
	viewer->active = FALSE;
	g_free(viewer->pretty_address);
	viewer->pretty_address = NULL;
	viewer_set_title(viewer, FALSE);

	if (viewer->authretry) {
		viewer->authretry = FALSE;
		g_idle_add(viewer_retryauth, viewer);
	} else if (viewer->reconnect) {
		if (!viewer->withEvents) {
			DEBUG_LOG("No domain events, falling back to polling");
			g_timeout_add(500,
				      viewer_connect_timer,
				      viewer);
		}

		viewer_set_status(viewer, "Waiting for guest domain to re-start");
	} else {
		viewer_set_status(viewer, "Guest domain has shutdown");
		gtk_main_quit();
	}
}

void viewer_connected(VirtViewer *viewer)
{
	viewer->connected = TRUE;
	viewer_set_status(viewer, "Connected to graphic server");
}

void viewer_initialized(VirtViewer *viewer)
{
	viewer_show_display(viewer);
	viewer_set_title(viewer, FALSE);
}

void viewer_disconnected(VirtViewer *viewer)
{
	if (!viewer->connected) {
		viewer_simple_message_dialog(viewer->window, _("Unable to connect to the graphic server %s"),
					     viewer->pretty_address);
	}
	viewer_deactivate(viewer);
}


static int viewer_domain_event(virConnectPtr conn G_GNUC_UNUSED,
			       virDomainPtr dom,
			       int event,
			       int detail G_GNUC_UNUSED,
			       void *opaque)
{
	VirtViewer *viewer = opaque;

	DEBUG_LOG("Got domain event %d %d", event, detail);

	if (!viewer_matches_domain(viewer, dom))
		return 0;

	switch (event) {
	case VIR_DOMAIN_EVENT_STOPPED:
		viewer_deactivate(viewer);
		break;

	case VIR_DOMAIN_EVENT_STARTED:
		viewer_activate(viewer, dom);
		break;
	}

	return 0;
}


static int viewer_initial_connect(VirtViewer *viewer)
{
	virDomainPtr dom = NULL;
	virDomainInfo info;
	int ret = -1;

	viewer_set_status(viewer, "Finding guest domain");
	dom = viewer_lookup_domain(viewer);
	if (!dom) {
		if (viewer->waitvm) {
			viewer_set_status(viewer, "Waiting for guest domain to be created");
			goto done;
		} else {
			viewer_simple_message_dialog(viewer->window, _("Cannot find guest domain %s"),
						     viewer->domkey);
			DEBUG_LOG("Cannot find guest %s", viewer->domkey);
			goto cleanup;
		}
	}

	viewer_set_status(viewer, "Checking guest domain status");
	if (virDomainGetInfo(dom, &info) < 0) {
		DEBUG_LOG("Cannot get guest state");
		goto cleanup;
	}

	if (info.state == VIR_DOMAIN_SHUTOFF) {
		viewer_set_status(viewer, "Waiting for guest domain to start");
	} else {
		ret = viewer_activate(viewer, dom);
		if (ret < 0) {
			if (viewer->waitvm) {
				viewer_set_status(viewer, "Waiting for guest domain to start server");
			} else {
				DEBUG_LOG("Failed to activate viewer");
				goto cleanup;
			}
		} else if (ret == 0) {
			DEBUG_LOG("Failed to activate viewer");
			ret = -1;
			goto cleanup;
		}
	}

 done:
	ret = 0;
 cleanup:
	if (dom)
		virDomainFree(dom);
	return ret;
}

static gboolean viewer_connect_timer(void *opaque)
{
	VirtViewer *viewer = opaque;

	DEBUG_LOG("Connect timer fired");

	if (!viewer->active &&
	    viewer_initial_connect(viewer) < 0)
		gtk_main_quit();

	if (viewer->active)
		return FALSE;

	return TRUE;
}

static void viewer_error_func (void *data G_GNUC_UNUSED, virErrorPtr error G_GNUC_UNUSED)
{
	/* nada */
}

int
viewer_start (const char *uri,
	      const char *name,
	      gint zoom,
	      gboolean direct,
	      gboolean waitvm,
	      gboolean reconnect,
	      gboolean verbose,
	      gboolean debug,
	      GtkWidget *container)
{
	VirtViewer *viewer;
	GtkWidget *notebook;
	GtkWidget *menu;
	int cred_types[] =
		{ VIR_CRED_AUTHNAME, VIR_CRED_PASSPHRASE };
	virConnectAuth auth_libvirt = {
		.credtype = cred_types,
		.ncredtype = ARRAY_CARDINALITY(cred_types),
		.cb = viewer_auth_libvirt_credentials,
		.cbdata = (void *)uri,
	};

	doDebug = debug;

	viewer = g_new0(VirtViewer, 1);

	viewer->active = FALSE;
	viewer->autoResize = TRUE;
	viewer->direct = direct;
	viewer->waitvm = waitvm;
	viewer->reconnect = reconnect;
	viewer->verbose = verbose;
	viewer->domkey = g_strdup(name);
	viewer->uri = g_strdup(uri);
	viewer->zoomlevel = zoom;

	g_value_init(&viewer->accelSetting, G_TYPE_STRING);

	viewer_event_register();

	virSetErrorFunc(NULL, viewer_error_func);

	viewer->conn = virConnectOpenAuth(uri,
					  //virConnectAuthPtrDefault,
					  &auth_libvirt,
					  VIR_CONNECT_RO);
	if (!viewer->conn) {
		viewer_simple_message_dialog(NULL, _("Unable to connect to libvirt with URI %s"),
					     uri ? uri : _("[none]"));
		return -1;
	}

	if (!(viewer->glade = viewer_load_glade("viewer.glade",
						container ? "notebook" : "viewer")))
		return -1;

	menu = glade_xml_get_widget(viewer->glade, "menu-view-resize");
	if (!container)
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu), TRUE);

	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_file_quit",
				      G_CALLBACK(viewer_menu_file_quit), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_file_screenshot",
				      G_CALLBACK(viewer_menu_file_screenshot), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_view_fullscreen",
				      G_CALLBACK(viewer_menu_view_fullscreen), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_view_zoom_in",
				      G_CALLBACK(viewer_menu_view_zoom_in), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_view_zoom_out",
				      G_CALLBACK(viewer_menu_view_zoom_out), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_view_zoom_reset",
				      G_CALLBACK(viewer_menu_view_zoom_reset), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_view_resize",
				      G_CALLBACK(viewer_menu_view_resize), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_send",
				      G_CALLBACK(viewer_menu_send), viewer);
	glade_xml_signal_connect_data(viewer->glade, "viewer_menu_help_about",
				      G_CALLBACK(viewer_menu_help_about), viewer);


	notebook = glade_xml_get_widget(viewer->glade, "notebook");
	gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);

	if (container) {
		viewer->container = container;
	} else {
		GtkWidget *window = glade_xml_get_widget(viewer->glade, "viewer");
		GSList *accels;
		viewer->container = window;
		viewer->window = window;
		g_signal_connect(window, "delete-event",
				 G_CALLBACK(viewer_delete), viewer);
		gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
		viewer->accelEnabled = TRUE;
		accels = gtk_accel_groups_from_object(G_OBJECT(window));
		for ( ; accels ; accels = accels->next) {
			viewer->accelList = g_slist_append(viewer->accelList, accels->data);
			g_object_ref(G_OBJECT(accels->data));
		}
		gtk_widget_show_all(viewer->window);
	}

	if (viewer_initial_connect(viewer) < 0)
		return -1;

	if (virConnectDomainEventRegister(viewer->conn,
					  viewer_domain_event,
					  viewer,
					  NULL) < 0)
		viewer->withEvents = FALSE;
	else
		viewer->withEvents = TRUE;

	if (!viewer->withEvents &&
	    !viewer->active) {
		DEBUG_LOG("No domain events, falling back to polling");
		g_timeout_add(500,
			      viewer_connect_timer,
			      viewer);
	}

	return 0;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
