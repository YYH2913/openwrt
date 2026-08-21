#!/bin/sh
set -eu

base="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
makefile="$base/Makefile"
coordinator="$base/files/airoha-gpon-config.init"

sed -n '/^define Package\/airoha-omcid\/conffiles$/,/^endef$/p' "$makefile" |
	grep -Fxq '/etc/airoha-omci/' || {
	echo 'persistent OMCI runtime state is not preserved by sysupgrade' >&2
	exit 1
}

sed -n '/^[[:space:]]*procd_open_instance$/,/^[[:space:]]*procd_close_instance$/p' \
	"$coordinator" | grep -Eq \
	'^[[:space:]]*procd_set_param[[:space:]]+respawn[[:space:]]+[0-9]+[[:space:]]+[0-9]+[[:space:]]+[0-9]+$' || {
	echo 'PON configuration coordinator is not supervised by procd' >&2
	exit 1
}

echo 'OMCI system lifecycle integration passed'
