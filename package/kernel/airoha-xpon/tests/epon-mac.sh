#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)"
TEST_DIR="$ROOT/package/kernel/airoha-xpon/tests"
DRIVER="$TEST_DIR/../src/airoha-epon.c"
DTS="$ROOT/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
OUT="$(mktemp)"
trap 'rm -f "$OUT" "$OUT".*' EXIT

cc -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_DIR/../src" "$TEST_DIR/epon-mac-test.c" -o "$OUT"
"$OUT"

SDK_ROOT="${AIROHA_SDK_ROOT:-$ROOT/../tmp/airoha-sdk/airoha_sdk}"
[ -f "$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c" ]
[ -f "$SDK_ROOT/private/xpon_10g/src/epon/epon_isr.c" ]
[ -f "$SDK_ROOT/private/xpon_10g/src/epon/epon_mpcp.c" ]
[ -f "$SDK_ROOT/private/xpon_10g/inc/epon/xepon_mac_c_header_en7581.h" ]
[ -f "$SDK_ROOT/private/xpon_phy_10g/inc/en7581_reg.h" ]
[ -f "$SDK_ROOT/private/xpon_phy_10g/src/en7581.c" ]
grep -q 'WRITE_REG_WORD(e_rpt_cfg, 0x1)' \
	"$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
grep -q 'D10G_RX_TSADJ' "$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
grep -q 'U10G_TX_STMADJ' "$SDK_ROOT/private/xpon_10g/src/ic/AN7581.c"
grep -q 'REGISTER_ACK_TX_IN_NORMAL_GATE' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_mpcp.c"
grep -q 'epon_mpcp_set_sync_time()' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_mpcp.c"
grep -q '0x002ffff1.*sync_time' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_mpcp.c"
grep -q 'epon_mpcp_local_deregister(llid_index)' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_isr.c"
grep -q 'REGISTER_REQ_TX_IN_NORMAL_GATE,REGISTER_REQ_FLAG_DEREGISTER' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_mpcp.c"
sed -n '/^int epon_isr_grant_buffer_overrun_handler/,/^}/p' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_isr.c" | \
	grep -q 'return EPON_SUCCESS'
sed -n '/^int epon_isr_tx_fifo_under_run_handler/,/^}/p' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_isr.c" | \
	grep -q 'return EPON_SUCCESS'
sed -n '/^int epon_isr_int2_handler/,/^}/p' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_isr.c" | \
	grep -q 'return EPON_SUCCESS'
grep -q 'IO_CBITS(PHY_CSR_DUMMY_REG_RX,(1<<21))' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_dev.c"
grep -q 'IO_SBITS(PHY_CSR_DUMMY_REG_RX,(1<<21))' \
	"$SDK_ROOT/private/xpon_10g/src/epon/epon_dev.c"
grep -q 'e_dyinggsp_cfg.*// 62AC' \
	"$SDK_ROOT/private/xpon_10g/inc/epon/xepon_mac_c_header_en7581.h"
grep -q '^#define EN7581_EPON_DYING_GASP[[:space:]]*0x2ac$' \
	"$DRIVER"

PHY_REGS="$SDK_ROOT/private/xpon_phy_10g/inc/en7581_reg.h"
PHY_SDK="$SDK_ROOT/private/xpon_phy_10g/src/en7581.c"
grep -q '^#define REG_BASE_XEPON_PCS[[:space:]]*0x1FAF1000[[:space:]]*$' \
	"$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_INT_STATUS.*REG_BASE_XEPON_PCS+0x020' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_INT_EN.*REG_BASE_XEPON_PCS+0x024' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_RX_SYNC_STATUS.*REG_BASE_XEPON_PCS+0x06C' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_SFP_STATUS.*REG_BASE_XEPON_PCS+0x224' "$PHY_REGS"
grep -q 'XEPON_PCS_RX_SYNC_STATUS_OK.*(1<<31)' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_SFP_STATUS_RX_LOSS.*(1<<28)' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_RX_ENABLE.*0x80810302' "$PHY_REGS"
grep -q 'EN7581_XEPON_PCS_RX_DISABLE.*0x80810300' "$PHY_REGS"
sed -n '/^int en7581_xepon_phy_int_config/,/^}/p' "$PHY_SDK" | \
	grep -q 'EN7581_XEPON_PCS_INT_NOT_LASER_RX_LOSS_EN'
sed -n '/^int en7581_xepon_phy_isr/,/^int en7581_xepon_phy_event_poll/p' \
	"$PHY_SDK" | grep -q 'phy_pma_reset_with_lock()'
sed -n '/^int en7581_xepon_phy_pma_reset/,/^}/p' "$PHY_SDK" > "$OUT.sdk-reset"
grep -q 'EN7581_XEPON_PCS_RX_CTRL_CFG, EN7581_XEPON_PCS_RX_DISABLE' \
	"$OUT.sdk-reset"
grep -q 'EN7581_XEPON_PCS_LOGIC_RST, EN7581_XEPON_PCS_LOGIC_RST_ON' \
	"$OUT.sdk-reset"
grep -q 'EN7581_XEPON_PCS_LOGIC_RST, EN7581_XEPON_PCS_LOGIC_RST_OFF' \
	"$OUT.sdk-reset"
grep -q 'EN7581_XEPON_PCS_RX_CTRL_CFG, EN7581_XEPON_PCS_RX_ENABLE' \
	"$OUT.sdk-reset"

grep -q '^#define EN7581_XEPON_PCS_INT_STATUS[[:space:]]*0x020$' "$DRIVER"
grep -q '^#define EN7581_XEPON_PCS_INT_ENABLE[[:space:]]*0x024$' "$DRIVER"
grep -q '^#define EN7581_XEPON_PCS_RX_SYNC_STATUS[[:space:]]*0x06c$' "$DRIVER"
grep -q '^#define EN7581_XEPON_PCS_SFP_STATUS[[:space:]]*0x224$' "$DRIVER"
grep -q 'EN7581_XEPON_PCS_RX_SYNC_OK.*BIT(31)' "$DRIVER"
grep -q 'EN7581_XEPON_PCS_SFP_RX_LOS.*BIT(28)' "$DRIVER"
grep -q '^#define  EN7581_XEPON_PCS_RX_ENABLE[[:space:]]*0x80810302$' "$DRIVER"
grep -q '^#define  EN7581_XEPON_PCS_RX_DISABLE[[:space:]]*0x80810300$' "$DRIVER"
grep -q '^#define EN7581_XEPON_PCS_LOGIC_RST[[:space:]]*0x034$' "$DRIVER"
! grep -q 'EN7581_EPON_PHY_INT_STATUS.*0xa10' "$DRIVER"
! grep -q 'EN7581_EPON_PHY_SFP_STATUS.*0xb4c' "$DRIVER"
grep -q 'airoha_pcs_xpon_quiesce(priv->pcs)' "$DRIVER"
grep -q 'airoha_pcs_xpon_select_wan(priv->pcs, mode)' "$DRIVER"
grep -q 'airoha_pcs_xpon_set_mode(priv->pcs, mode)' "$DRIVER"
grep -q 'airoha_pcs_xpon_recover(priv->pcs)' "$DRIVER"

sed -n '/static void en7581_epon_phy_recovery_work(/,/^}/p' "$DRIVER" \
	> "$OUT.recovery"
disable_line="$(grep -n 'en7581_epon_phy_set_rx(priv, false)' "$OUT.recovery" | tail -1 | cut -d: -f1)"
hold_line="$(grep -n 'en7581_epon_phy_set_logic_reset(priv, true)' "$OUT.recovery" | tail -1 | cut -d: -f1)"
quiesce_line="$(grep -n 'airoha_pcs_xpon_quiesce(priv->pcs)' "$OUT.recovery" | cut -d: -f1)"
select_line="$(grep -n 'airoha_pcs_xpon_select_wan(priv->pcs, mode)' "$OUT.recovery" | cut -d: -f1)"
set_mode_line="$(grep -n 'airoha_pcs_xpon_set_mode(priv->pcs, mode)' "$OUT.recovery" | cut -d: -f1)"
recover_line="$(grep -n 'airoha_pcs_xpon_recover(priv->pcs)' "$OUT.recovery" | cut -d: -f1)"
release_line="$(grep -n 'en7581_epon_phy_set_logic_reset(priv, false)' "$OUT.recovery" | cut -d: -f1)"
enable_line="$(grep -n 'en7581_epon_phy_set_rx(priv, true)' "$OUT.recovery" | cut -d: -f1)"
[ "$disable_line" -lt "$hold_line" ] && [ "$hold_line" -lt "$quiesce_line" ] &&
	[ "$quiesce_line" -lt "$set_mode_line" ] &&
	[ "$quiesce_line" -lt "$select_line" ] &&
	[ "$select_line" -lt "$set_mode_line" ] &&
	[ "$set_mode_line" -lt "$recover_line" ] &&
	[ "$recover_line" -lt "$release_line" ] &&
	[ "$release_line" -lt "$enable_line" ]

sed -n '/static int en7581_epon_hw_initialize(/,/^}/p' "$DRIVER" > "$OUT.init"
sed -n '/static int en7581_epon_start_datapath(/,/^}/p' "$DRIVER" > "$OUT.start"
grep -q 'airoha_epon_mac_init_transaction' "$OUT.init"
grep -q 'value | EN7581_EPON_PATH_STOP' "$OUT.init"
! grep -q 'value & ~EN7581_EPON_PATH_STOP' "$OUT.init"
grep -q 'value & ~EN7581_EPON_PATH_STOP' "$OUT.start"
grep -q 'reg-names = "mac", "phy-csr", "xepon-pcs"' "$DTS"
grep -q 'pcs-handle = <&pon_pcs>;' "$DTS"
echo "EPON SDK correspondence checks passed"
