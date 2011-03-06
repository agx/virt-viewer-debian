# -*- rpm-spec -*-

# Plugin isn't ready for real world use yet - it needs
# a security audit at very least
%define _with_plugin %{?with_plugin:1}%{!?with_plugin:0}

%define with_spice 0
%if 0%{?fedora} >= 14
%define with_spice 1
%endif

Name: virt-viewer
Version: 0.3.1
Release: 1%{?dist}%{?extra_release}
Summary: Virtual Machine Viewer
Group: Applications/System
License: GPLv2+
URL: http://virt-manager.org/
Source0: http://virt-manager.org/download/sources/%{name}/%{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: openssh-clients

BuildRequires: gtk2-devel
BuildRequires: libvirt-devel >= 0.6.0
BuildRequires: libxml2-devel
BuildRequires: libglade2-devel
BuildRequires: gtk-vnc-devel >= 0.3.8
%if %{with_spice}
BuildRequires: spice-gtk-devel >= 0.5
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
to virtual machines. It uses the GTK-VNC widget to provide the display,
and libvirt for looking up VNC server details.

%if %{_with_plugin}
%package plugin
Summary: Mozilla plugin for the gtk-vnc library
Group: Development/Libraries
Requires: %{name} = %{version}

%description plugin
gtk-vnc is a VNC viewer widget for GTK. It is built using coroutines
allowing it to be completely asynchronous while remaining single threaded.

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

%configure %{spice_arg} %{plugin_arg}
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
%{_datadir}/%{name}/ui/auth.glade
%{_datadir}/%{name}/ui/about.glade
%{_datadir}/%{name}/ui/viewer.glade
%{_mandir}/man1/%{name}*

%if %{_with_plugin}
%files plugin
%defattr(-, root, root)
%{_libdir}/mozilla/plugins/%{name}-plugin.so
%endif

%changelog
