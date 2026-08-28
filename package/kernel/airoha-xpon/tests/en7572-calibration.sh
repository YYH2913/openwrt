#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
source_dir="$(dirname -- "$script_dir")"
binary="${TMPDIR:-/tmp}/en7572-calibration-test.$$"
experimental="${TMPDIR:-/tmp}/en7572-bob-a2-as-a0.$$"
experimental_gpon="${TMPDIR:-/tmp}/en7572-bob-gpon-a2-as-a0.$$"
trap 'rm -f "$binary" "$experimental" "$experimental_gpon"' EXIT INT TERM
gpon="$source_dir/firmware/airoha/xg2010g/en7572-bob.bin"
xgspon="$source_dir/firmware/airoha/xg2010g/en7572-bob-xgspon.bin"
generator="$source_dir/scripts/en7572-a2-as-a0.sh"
driver="$source_dir/src/airoha-en7572.c"
board_dts="$source_dir/../../../target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
xgspon_board_dts="$source_dir/../../../target/linux/airoha/dts/an7581-axon-xg2010g-xgspon-ubi.dts"
package_makefile="$source_dir/Makefile"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
	-o "$binary" "$script_dir/en7572-calibration-test.c"
"$binary" "$gpon" a0
"$binary" "$xgspon" a2
sh "$generator" "$xgspon" "$experimental"
sh "$generator" "$xgspon" "$experimental_gpon"
"$binary" "$experimental" both
"$binary" "$experimental_gpon" both
cmp "$experimental" "$experimental_gpon"

# Everything except the table-wide identity fields is an exact A2 copy.
cmp -n 20 "$experimental" "$xgspon" 0 256
cmp -n 4 "$experimental" "$xgspon" 36 292
cmp -n 200 "$experimental" "$xgspon" 56 312
cmp -n 16 "$experimental" "$xgspon" 20 20
cmp -n 16 "$experimental" "$xgspon" 40 40

grep -F 'airoha,calibration-10g-a2-as-a0' "$driver" >/dev/null
grep -F '"experimental-a2-copy:a0"' "$driver" >/dev/null
grep -F 'airoha,calibration-gpon-a2-as-a0' "$driver" >/dev/null
grep -F '"experimental-a2-copy:gpon-a0"' "$driver" >/dev/null
grep -F 'airoha,calibration-10g-a2-as-a0;' "$board_dts" >/dev/null
grep -F 'airoha,calibration-gpon-firmware' "$board_dts" >/dev/null
grep -F 'en7572-bob-gpon.bin' "$board_dts" >/dev/null
if grep -Fq 'airoha,calibration-gpon-a2-as-a0;' "$board_dts"; then
	echo 'the base board must not select an experimental GPON calibration' >&2
	exit 1
fi
grep -F 'airoha,calibration-gpon-a2-as-a0;' "$xgspon_board_dts" >/dev/null
grep -F 'en7572-bob-a2-as-a0.bin' "$package_makefile" >/dev/null
grep -F 'en7572-bob-gpon-a2-as-a0.bin' "$package_makefile" >/dev/null
grep -F 'en7572_load_calibration_from_mtd' "$driver" >/dev/null
grep -F 'EN7572_ART_A0_CALIBRATION_OFFSET' "$driver" >/dev/null
grep -F 'en7572_calibration_factory_record_bank_valid' "$driver" >/dev/null
grep -F 'priv->pon_mode != AIROHA_XPON_MODE_GPON ||' "$driver" >/dev/null
grep -F '!priv->calibration_gpon_a2_as_a0)' "$driver" >/dev/null
[ "$(grep -Fc "reg = <0x51000 0x201>;" "$board_dts")" -eq 1 ]
gpon_mapping="$(sed -n '/case AIROHA_XPON_MODE_GPON:/,/break;/p' "$driver")"
printf '%s\n' "$gpon_mapping" | grep -Fq 'required_bank = EN7572_BOB_BANK_A0;'
printf '%s\n' "$gpon_mapping" | grep -Fq 'EN7572_DEFAULT_GPON_A2_AS_A0_FW'
printf '%s\n' "$gpon_mapping" | grep -Fq 'EN7572_DEFAULT_GPON_BOB_FW'
printf '%s\n' "$gpon_mapping" | grep -Fq 'property = "airoha,calibration-gpon-firmware";'

[ "$(sha256sum "$gpon" | cut -d' ' -f1)" = \
	508cb5d29925487eaf4ba9169e566107bf91533c4c6629571d549b6819f78233 ]
[ "$(sha256sum "$xgspon" | cut -d' ' -f1)" = \
	17f7c3facb104f8df58940f50b63c6ffedfda832b03aa15e44322dfeff9ea798 ]
