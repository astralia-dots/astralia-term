#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")"

cmd_setup() {
	sudo pacman -Syu --needed base-devel fcft libspng libxcb libxkbcommon libxkbcommon-x11 meson ninja pixman wayland wayland-protocols xcb-util-cursor
}

cmd_build() {
	if [ -d build ]; then
		meson setup --prefix=/usr --reconfigure build
	else
		meson setup --prefix=/usr build
	fi
	ninja -C build -j "${ASTRALIA_TERM_BUILD_JOBS:-4}"
}

cmd_check() { cmd_build; meson test -C build --print-errorlogs; }
cmd_install() { cmd_build; sudo ninja -C build install; update_desktop_db; }
cmd_run() { cmd_build; ./build/astralia-term "$@"; }
cmd_test() { cmd_check; ./build/astralia-term "$@"; }
cmd_uninstall() { sudo ninja -C build uninstall; update_desktop_db; }

update_desktop_db() {
	command -v update-desktop-database >/dev/null || return 0
	sudo update-desktop-database /usr/share/applications
}

main() {
	if [ $# -eq 0 ]; then
		cmd_check
		return
	fi
	local cmd="$1"
	shift
	case "$cmd" in
		setup|build|install|uninstall) "cmd_$cmd" ;;
		run|test) "cmd_$cmd" "$@" ;;
		*) echo "unknown command: $cmd" >&2; exit 2 ;;
	esac
}

main "$@"
