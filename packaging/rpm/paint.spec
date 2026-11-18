Name:           paint
Version:        0.1.0
Release:        1%{?dist}
Summary:        Clean GNOME Paint application built with GTK 4 and Libadwaita

License:        GPL-3.0-or-later
URL:            https://github.com/Spondle/Draw
Source0:        %{url}/archive/v%{version}/Draw-%{version}.tar.gz

BuildRequires:  appstream
BuildRequires:  desktop-file-utils
BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(gtk4) >= 4.16
BuildRequires:  pkgconfig(libadwaita-1) >= 1.6

Requires:       hicolor-icon-theme

%description
Paint is a fast, distraction-free raster drawing and painting application
built with GTK 4, Libadwaita, and Cairo in the style of GNOME Text Editor
and Ptyxis.

%prep
%autosetup -n Draw-%{version}

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.spondle.Paint.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/io.github.spondle.Paint.metainfo.xml

%files
%license README.md
%{_bindir}/paint
%{_datadir}/applications/io.github.spondle.Paint.desktop
%{_datadir}/metainfo/io.github.spondle.Paint.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.spondle.Paint.svg
%{_datadir}/icons/hicolor/symbolic/apps/io.github.spondle.Paint-symbolic.svg

%changelog
* Tue Nov 17 2026 spondle <gordonumolu@gmail.com> - 0.1.0-1
- Initial release
