# -*- rpm-spec -*-

Name: virt-viewer
Version: 0.0.2
Release: 1%{?dist}%{?extra_release}
Summary: Virtual Machine Viewer

Group: Applications/System
License: GPL
URL: http://virt-manager.org/
Source0: http://virt-manager.org/download/sources/%{name}/%{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires: gtk2-devel
BuildRequires: libvirt-devel
BuildRequires: libxml2-devel
BuildRequires: gtk-vnc-devel >= 0.1.0
BuildRequires: /usr/bin/pod2man

%description
Virtual Machine Viewer provides a graphical console client for connecting
to virtual machines. It uses the GTK-VNC widget to provide the display,
and libvirt for looking up VNC server details.

%prep
%setup -q

%build
%configure
make %{?_smp_mflags}


%install
rm -rf $RPM_BUILD_ROOT
make install  DESTDIR=$RPM_BUILD_ROOT

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%doc README COPYING AUTHORS ChangeLog NEWS
%{_bindir}/%{name}
%{_mandir}/man1/%{name}*

%changelog
* Tue Aug 28 2007 Daniel P. Berrange <berrange@redhat.com> - 0.0.2-1
- Added support for remote console access

* Wed Aug 15 2007 Daniel P. Berrange <berrange@redhat.com> - 0.0.1-1
- First release

