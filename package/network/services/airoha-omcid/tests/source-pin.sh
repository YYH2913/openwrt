#!/bin/sh

set -eu

openwrt="${1:?OpenWrt tree path is required}"
package_makefile="$openwrt/package/network/services/airoha-omcid/Makefile"
omci="${AIROHA_OMCI_SOURCE:-$openwrt/../airoha-omci}"

[ -f "$package_makefile" ] && [ -d "$omci/.git" ] || {
	echo 'OpenWrt package or standalone OMCI repository is missing' >&2
	exit 1
}

pin="$(sed -n 's/^PKG_SOURCE_VERSION:=//p' "$package_makefile")"
release="$(sed -n 's/^PKG_RELEASE:=//p' "$package_makefile")"
source_archive="$(sed -n 's/^PKG_SOURCE:=//p' "$package_makefile")"
case "$pin" in
	????????????????????????????????????????) ;;
	*)
		echo "invalid airoha-omcid source pin: $pin" >&2
		exit 1
		;;
esac
case "$pin" in *[!0-9a-f]*)
	echo "non-hexadecimal airoha-omcid source pin: $pin" >&2
	exit 1
;; esac

head="$(git -C "$omci" rev-parse HEAD)"
[ "$pin" = "$head" ] || {
	echo "airoha-omcid pins $pin but standalone OMCI HEAD is $head" >&2
	exit 1
}
git -C "$omci" diff --quiet -- || {
	echo 'standalone OMCI tracked worktree changes are absent from the package pin' >&2
	exit 1
}
git -C "$omci" diff --cached --quiet -- || {
	echo 'standalone OMCI staged changes are absent from the package pin' >&2
	exit 1
}
untracked="$(git -C "$omci" ls-files --others --exclude-standard)"
[ -z "$untracked" ] || {
	echo 'standalone OMCI untracked files are absent from the package pin:' >&2
	printf '%s\n' "$untracked" >&2
	exit 1
}

[ "$release" -ge 15 ] 2>/dev/null || {
	echo "airoha-omcid release $release predates the integrated XGS session ABI" >&2
	exit 1
}
[ "$source_archive" = 'airoha-omcid-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.zst' ] || {
	echo 'airoha-omcid source archive name does not include its pinned commit' >&2
	exit 1
}
git -C "$omci" show "$pin:internal/transport/device.go" |
	grep -Fq 'deviceABIVersion = 3'
git -C "$omci" show "$pin:cmd/airoha-omcid/main.go" |
	grep -Fq 'xgs-omci-evidence'
git -C "$omci" show "$pin:internal/platform/graph.go" |
	grep -Fq 'BuildServiceGraphForMode'

echo "airoha-omcid release $release pins complete standalone OMCI source $pin"
