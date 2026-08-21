#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$TEST_DIR/../src/airoha-epon.c"
OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT".init "$OUT".rtt' EXIT

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-mac-init-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
SDK_IC="$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
SDK_EPON="$SDK_ROOT/private/xpon_10g/inc/epon/epon.h"
SDK_REG="$SDK_ROOT/private/xpon_10g/inc/epon/epon_reg.h"
SDK_ACT="$SDK_ROOT/private/xpon_10g/src/epon/epon_act.c"
SDK_XMCS="$SDK_ROOT/private/xpon_10g/src/xmcs/xmcs_if.c"
SDK_REGS="$SDK_ROOT/private/xpon_10g/inc/epon/xepon_mac_c_header_en7581.h"
[ -f "$SDK_IC" ] && [ -f "$SDK_EPON" ] && [ -f "$SDK_REG" ] &&
	[ -f "$SDK_ACT" ] && [ -f "$SDK_XMCS" ] && [ -f "$SDK_REGS" ]

grep -q 'WRITE_REG_WORD(e_rpt_cfg, 0x1)' "$SDK_IC"
grep -q 'u1g_fecon_rpt_qsizeadj(e_u1g_rpt_qsizeadj, 0xf1)' "$SDK_IC"
grep -q 'u1g_fecoff_rpt_qsizeadj(e_u1g_rpt_qsizeadj, 0xf1)' "$SDK_IC"
grep -q 'u10g_rpt_qsizeadj(e_u10g_rpt_qsizeadj, 0x19)' "$SDK_IC"
grep -q 'D10G_RX_TSADJ.*(0xff90)' "$SDK_EPON"
grep -q 'U10G_TX_STMADJ.*(0x8)' "$SDK_EPON"
grep -q 'EPON_DEFAULT_LASER_ON.*(0x20)' "$SDK_EPON"
grep -q 'EPON_DEFAULT_LASER_OFF.*(0x20)' "$SDK_EPON"
grep -q 'EPON_MPCP_TMOUT_INTVL.*(0x1f4)' "$SDK_EPON"
grep -q 'EPON_REG_ADJUST_TIME2_DEFAULT.*(6)' "$SDK_REG"
sed -n '/int epon_mac_reset(void)/,/^}/p' "$SDK_ACT" |
	grep -q 'WRITE_REG_WORD(e_trx_adjust_time2,EPON_REG_ADJUST_TIME2_DEFAULT)'
sed -n '/void prepare_epon(void)/,/^}/p' "$SDK_XMCS" |
	grep -q 'epon_reset(NULL)'
sed -n '/int an7581_epon_rtt_adjust(/,/^}/p' "$SDK_IC" > "$OUT.rtt"
grep -q 'XMCS_IF_WAN_DETECT_MODE_10G_1G_EPON' "$OUT.rtt"
grep -q 'XMCS_IF_WAN_DETECT_MODE_10G_10G_EPON' "$OUT.rtt"
! sed -n '/XMCS_IF_WAN_DETECT_MODE_10G_1G_EPON/,/^    }/p' "$OUT.rtt" |
	grep -q 'e_trx_adjust_time2'
grep -q 'e_txfetch_cfg.*0x6200, 0x002a03e8, 0x00ffffff' "$SDK_IC"
grep -q 'e_tx_cal_cnst.*0x6204, 0x2a120408, 0xffffff3f' "$SDK_IC"
grep -q 'e_dyinggsp_cfg.*0x62ac, 0x00000100, 0x8000ff00' "$SDK_IC"
grep -q 'e_dyinggsp_w1.*0x62b0, 0x88090300, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w2.*0x62b4, 0x52000110, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w3.*0x62b8, 0x01000000, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w4.*0x62bc, 0x0f05ee00, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w5.*0x62c0, 0x13250022, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w6.*0x62c4, 0x01000210, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w7.*0x62c8, 0x01000000, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w8.*0x62cc, 0x0f05ee00, 0xffffffff' "$SDK_IC"
grep -q 'e_dyinggsp_w9.*0x62d0, 0x13250000, 0xffffffff' "$SDK_IC"
grep -q 'e_trx_adjtime4_FLD_d10g_rx_tsadj.*REG_FLD(16, 16)' "$SDK_REGS"

sed -n '/static int en7581_epon_hw_initialize(/,/^}/p' "$DRIVER" > "$OUT.init"
grep -q 'airoha_epon_mac_init_transaction' "$OUT.init"
! grep -Eq 'en7581_epon_write\(priv, EN7581_EPON_(REPORT_BITMAP|REPORT_CFG|REPORT_CFG2|1G_REPORT_QSIZE_ADJUST|10G_REPORT_QSIZE_ADJUST|LASER_TIME|SYNC_TIME|MPCP_TIMEOUT|TRX_ADJUST[1-4]|TX_FETCH|TX_CAL|DYING_GASP)' "$OUT.init"
grep -q 'airoha_en7572_emergency_disable(priv->bosa)' "$DRIVER"
grep -q 'AIROHA_EPON_MAC_INIT_DYING_GASP, 0x8000ff00U, 0x00000100U' \
	"$TEST_DIR/../src/airoha-epon-mac-init.h"
echo "EPON MAC initialization SDK correspondence checks passed"
