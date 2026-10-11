Name:           shaker
Version:        0.1.0
Release:        1%{?dist}
Summary:        Shake your pointer to run a command — libinput seat daemon

License:        MIT
URL:            https://github.com/pb-kh/shaker
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson >= 1.1
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  libinput-devel >= 1.24
BuildRequires:  systemd-devel

Requires:       libinput >= 1.24

%description
shaker watches pointer devices on the seat through libinput's udev backend
and executes a user-configured command when the device is shaken
(path-to-extent ratio detection, as in KDE Plasma's shake cursor and
PowerToys' Find My Mouse). Configuration is a single TOML file at
~/.config/shaker/config.toml, auto-created with defaults on first run.
Runtime dependencies: libinput + libudev only.

%prep
%autosetup

%build
%meson -Dudev_rules_dir=%{_udevrulesdir}
%meson_build

%install
%meson_install

%check
%meson_test

%files
%doc README.md
%{_bindir}/shaker
%{_udevrulesdir}/99-shaker-mouse-access.rules

%post
# Fold the new udev rule into the running udev database and reprocess the
# input devices. Errors are non-fatal — udev may not be running (containers).
/usr/bin/udevadm control --reload || :
/usr/bin/udevadm trigger --subsystem-match=input --action=change || :
