# -*- rpm-spec -*-

# Plugin isn't ready for real world use yet - it needs
# a security audit at very least
%define _with_plugin %{?with_plugin:1}%{!?with_plugin:0}

%define with_gtk3 0
%if 0%{?fedora} >= 15
%define with_gtk3 1
%endif

%define with_spice 0
%if 0%{?fedora} >= 16
%define with_spice 1
%endif

# spice-gtk is x86 x86_64 only currently:
%ifnarch %{ix86} x86_64
%define with_spice 0
%endif

Name: virt-viewer
Version: 0.4.1
Release: 1%{?dist}%{?extra_release}
Summary: Virtual Machine Viewer
Group: Applications/System
License: GPLv2+
URL: http://virt-manager.org/
Source0: http://virt-manager.org/download/sources/%{name}/%{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: openssh-clients

%if %{with_gtk3}
BuildRequires: gtk3-devel >= 3.0.0
%else
BuildRequires: gtk2-devel >= 2.12.0
%endif
BuildRequires: libvirt-devel >= 0.6.0
BuildRequires: libxml2-devel
%if %{with_gtk3}
BuildRequires: gtk-vnc2-devel >= 0.4.3
%else
BuildRequires: gtk-vnc-devel >= 0.3.8
%endif
%if %{with_spice}
%if %{with_gtk3}
BuildRequires: spice-gtk3-devel >= 0.6
%else
BuildRequires: spice-gtk-devel >= 0.6
%endif
%endif
BuildRequires: /usr/bin/pod2man
BuildRequires: intltool
%if %{_with_plugin}
%if 0%{?fedora} > 8
BuildRequires: xulrunner-devel
%else
BuildRequires: firefox-devel
%endif
%endif

%description
Virtual Machine Viewer provides a graphical console client for connecting
to virtual machines. It uses the GTK-VNC or SPICE-GTK widgets to provide
the display, and libvirt for looking up VNC/SPICE server details.

%if %{_with_plugin}
%package plugin
Summary: Mozilla plugin for the gtk-vnc library
Group: Development/Libraries
Requires: %{name} = %{version}

%description plugin
Virtual Machine Viewer provides a graphical console client for connecting
to virtual machines. It uses the GTK-VNC or SPICE-GTK widgets to provide
the display, and libvirt for looking up VNC/SPICE server details.

This package provides a web browser plugin for Mozilla compatible
browsers.
%endif

%prep
%setup -q

%build

%if %{_with_plugin}
%define plugin_arg --enable-plugin
%else
%define plugin_arg --disable-plugin
%endif

%if %{with_spice}
%define spice_arg --enable-spice
%else
%define spice_arg --disable-spice
%endif

%if %{with_gtk3}
%define gtk_arg --with-gtk=3.0
%else
%define gtk_arg --with-gtk=2.0
%endif

%configure %{spice_arg} %{plugin_arg} %{gtk_arg}
%__make %{?_smp_mflags}


%install
rm -rf $RPM_BUILD_ROOT
%__make install  DESTDIR=$RPM_BUILD_ROOT
%if %{_with_plugin}
rm -f %{buildroot}%{_libdir}/mozilla/plugins/%{name}-plugin.a
rm -f %{buildroot}%{_libdir}/mozilla/plugins/%{name}-plugin.la
%endif
%find_lang %{name}

%clean
rm -rf $RPM_BUILD_ROOT

%files -f %{name}.lang
%defattr(-,root,root,-)
%doc README COPYING AUTHORS ChangeLog NEWS
%{_bindir}/%{name}
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/ui/
%{_datadir}/%{name}/ui/virt-viewer.xml
%{_datadir}/%{name}/ui/virt-viewer-auth.xml
%{_datadir}/%{name}/ui/virt-viewer-about.xml
%{_mandir}/man1/%{name}*

%if %{_with_plugin}
%files plugin
%defattr(-, root, root)
%{_libdir}/mozilla/plugins/%{name}-plugin.so
%endif

%changelog
