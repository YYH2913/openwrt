#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
root="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
tests="$root/package/kernel/airoha-xpon/tests"
driver="$root/package/kernel/airoha-xpon/src/airoha-epon.c"
sdk="$root/../tmp/airoha-sdk/airoha_sdk/private/xpon_10g/src/ic/AN7581.c"
binary="${TMPDIR:-/tmp}/epon-security-test.$$"
trap 'status=$?; rm -f "$binary"; exit "$status"' EXIT

cc -std=c11 -Wall -Wextra -Werror -I"$root/package/kernel/airoha-xpon/src" \
	"$tests/epon-security-test.c" -o "$binary"
"$binary"

[ -f "$sdk" ]
grep -q 'for(i=0;i < EPON_10G_MAX_KEY_NUM; i++)' "$sdk"
grep -q 'an7581_epon_triple_churning_key_cfg(EPON_WRITE_KEY, llidIndex, keyIndex, i)' "$sdk"
grep -q 'for(i=0;i < DPOE_MAX_KEY_NUM; i++)' "$sdk"
grep -q 'an7581_epon_dpoe_decrypt_key_cfg(EPON_WRITE_KEY, llidIndex, keyIndex, (DPOE_MAX_KEY_NUM -1 -i))' "$sdk"
grep -q 'e_key_value_SET_key_value(e_key_value, keyValue)' "$sdk"
grep -q 'e_enckey_val_SET_enckey_value(e_enckey_val, keyValue)' "$sdk"
grep -q 'e_crpt_cfg_SET_decrpt_mode(e_crpt_cfg,2)' "$sdk"
grep -q 'e_crpt_cfg_SET_encrpt_mode(e_crpt_cfg,2)' "$sdk"

grep -q 'EN7581_EPON_SECURITY_KEY_CFG.*0x10c' "$driver"
grep -q 'EN7581_EPON_ENCRYPT_KEY_CFG.*0x114' "$driver"
grep -q 'EN7581_EPON_ENCRYPT_KEY_VALUE.*0x118' "$driver"
grep -q 'u32 value_reg = encrypt ? EN7581_EPON_ENCRYPT_KEY_VALUE :' "$driver"
grep -q 'en7581_epon_write(priv, value_reg, \*value)' "$driver"
grep -q '\*value = en7581_epon_read(priv, value_reg)' "$driver"
grep -q 'capable(CAP_NET_ADMIN)' "$driver"
grep -q 'memzero_explicit(&key, sizeof(key))' "$driver"
grep -q 'en7581_epon_clear_all_keys_locked(priv)' "$driver"
grep -q 'AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS' "$driver"
grep -q 'events |= EPOLLPRI' "$driver"
grep -q 'priv->pending_ds_key_events |= key_low' "$driver"
grep -q 'priv->pending_us_key_events |= key_low' "$driver"
grep -q 'A bad userspace pointer must not silently discard a key event' "$driver"
grep -q 'en7581_epon_snapshot_dba_locked' "$driver"
grep -q 'goto rollback' "$driver"
grep -q 'readback != dba->threshold' "$driver"
! grep -q 'DEVICE_ATTR.*key' "$driver"
! grep -q 'disable_irq' "$driver"

echo 'AN7581 CTC/DPoE key and transactional DBA programming match SDK evidence'
