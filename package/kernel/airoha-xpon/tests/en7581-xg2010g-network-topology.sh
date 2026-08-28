#!/bin/sh
set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
openwrt_dir="${1:-$(CDPATH= cd -- "$script_dir/../../../.." && pwd)}"
board_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-ubi.dts"
xgspon_dts="$openwrt_dir/target/linux/airoha/dts/an7581-axon-xg2010g-xgspon-ubi.dts"
image_makefile="$openwrt_dir/target/linux/airoha/image/an7581.mk"
omcid_makefile="$openwrt_dir/package/network/services/airoha-omcid/Makefile"
pon_mode_defaults="$openwrt_dir/package/network/services/airoha-omcid/files/99-airoha-pon-mode"
board_network="$openwrt_dir/target/linux/airoha/an7581/base-files/etc/board.d/02_network"
board_leds="$openwrt_dir/target/linux/airoha/an7581/base-files/etc/board.d/01_leds"
platform_upgrade="$openwrt_dir/target/linux/airoha/an7581/base-files/lib/upgrade/platform.sh"
uboot_envtools="$openwrt_dir/package/boot/uboot-tools/uboot-envtools/files/airoha_an7581"
multi_serdes_patch="$openwrt_dir/target/linux/airoha/patches-6.18/165-02-v7.2-net-airoha-Support-multiple-net_devices-for-a-single.patch"
random_mac_patch="$openwrt_dir/target/linux/airoha/patches-6.18/165-06-v7.2-net-airoha-ignore-random-peer-macs.patch"
pcie_reset_patch="$openwrt_dir/target/linux/airoha/patches-6.18/609-02-clk-en7523-add-support-for-dedicated-PCIe-PERSTOUT-r.patch"
usb_hsgmii_patch="$openwrt_dir/target/linux/airoha/patches-6.18/616-net-pcs-airoha-bypass-usb-fixed-hsgmii-rate-adapter.patch"
realtek_host_serdes_patch="$openwrt_dir/target/linux/airoha/patches-6.18/618-net-phy-realtek-configure-xg2010g-host-serdes.patch"
workspace_dir="$(CDPATH= cd -- "$openwrt_dir/.." && pwd)"
uboot_dtsi="$workspace_dir/u-boot/arch/arm/dts/xg2010g-u-boot.dtsi"
uboot_driver="$workspace_dir/u-boot/drivers/net/airoha_eth.c"

require_fixed() {
	pattern="$1"
	file="$2"
	description="$3"

	if ! grep -Fq -- "$pattern" "$file"; then
		echo "missing $description: $pattern" >&2
		exit 1
	fi
}

extract_block() {
	start="$1"
	file="$2"

	awk -v start="$start" '
		!found && index($0, start) { found = 1 }
		found {
			print
			opens = gsub(/\{/, "{")
			closes = gsub(/\}/, "}")
			depth += opens - closes
			if (seen && depth == 0)
				exit
			if (opens)
				seen = 1
		}
		END { if (!found || depth != 0) exit 1 }
	' "$file"
}

[ -f "$board_dts" ] || {
	echo "XG2010G board DTS not found at $board_dts" >&2
	exit 2
}
[ -f "$xgspon_dts" ] || {
	echo "XG2010G XGS-PON board DTS not found at $xgspon_dts" >&2
	exit 2
}
[ -f "$multi_serdes_patch" ] || {
	echo "Airoha multi-SerDes patch not found at $multi_serdes_patch" >&2
	exit 2
}
[ -f "$random_mac_patch" ] || {
	echo "Airoha random-MAC convergence patch not found at $random_mac_patch" >&2
	exit 2
}
[ -f "$pcie_reset_patch" ] || {
	echo "Airoha PCIe reset patch not found at $pcie_reset_patch" >&2
	exit 2
}
[ -f "$usb_hsgmii_patch" ] || {
	echo "Airoha USB HSGMII patch not found at $usb_hsgmii_patch" >&2
	exit 2
}
[ -f "$realtek_host_serdes_patch" ] || {
	echo "RTL8261 host-SerDes patch not found at $realtek_host_serdes_patch" >&2
	exit 2
}
[ -f "$uboot_dtsi" ] && [ -f "$uboot_driver" ] || {
	echo "XG2010G U-Boot network sources not found under $workspace_dir/u-boot" >&2
	exit 2
}

# The board declares four LAN endpoints and PON.  This proves only the
# configured topology, not link training or concurrent throughput.
linux_gdm3="$(extract_block 'gdm3: ethernet@3 {' "$board_dts")"
linux_gdm4="$(extract_block '&gdm4 {' "$board_dts")"
linux_gdm1="$(extract_block '&gdm1 {' "$board_dts")"
linux_lan4="$(extract_block '&gsw_port4 {' "$board_dts")"
linux_lan4_phy="$(extract_block '&gsw_phy4 {' "$board_dts")"
linux_phy_lan1="$(extract_block 'rtl8261_lan1: ethernet-phy@5 {' "$board_dts")"
linux_phy_lan2="$(extract_block 'rtl8261_lan2: ethernet-phy@8 {' "$board_dts")"
linux_phy_lan3="$(extract_block 'en8811h: ethernet-phy@f {' "$board_dts")"
printf '%s\n' "$linux_gdm3" | grep -Fq 'openwrt,netdev-name = "lan2";'
printf '%s\n' "$linux_gdm3" | grep -Fq 'pcs-handle = <&pcie_pcs 1>;'
printf '%s\n' "$linux_gdm3" | grep -Fq 'phy-handle = <&rtl8261_lan2>;'
lan2_block="$(extract_block 'lan2_port: ethernet-port@5 {' "$board_dts")"
printf '%s\n' "$lan2_block" | grep -Fq 'managed = "in-band-status";'
printf '%s\n' "$linux_gdm4" | grep -Fq 'openwrt,netdev-name = "lan1";'
printf '%s\n' "$linux_gdm4" | grep -Fq 'pcs-handle = <&eth_pcs>;'
printf '%s\n' "$linux_gdm4" | grep -Fq 'openwrt,netdev-name = "lan3";'
printf '%s\n' "$linux_gdm4" | grep -Fq 'pcs-handle = <&usb_pcs>;'
lan1_block="$(extract_block 'lan1_port: ethernet-port@0 {' "$board_dts")"
printf '%s\n' "$lan1_block" | grep -Fq 'managed = "in-band-status";'
lan3_block="$(extract_block 'lan3_port: ethernet-port@1 {' "$board_dts")"
printf '%s\n' "$lan3_block" | grep -Fq 'phy-handle = <&en8811h>;'
if printf '%s\n' "$lan3_block" | grep -Fq 'managed = "in-band-status";'; then
	echo 'LAN3 must use the EN8811H copper PHY for link state' >&2
	exit 1
fi
require_fixed 'data->port_type == AIROHA_PCS_USB' "$usb_hsgmii_patch" \
	'USB-only fixed HSGMII rate-adapter bypass'
require_fixed 'interface == PHY_INTERFACE_MODE_2500BASEX' "$usb_hsgmii_patch" \
	'2.5G-only fixed HSGMII rate-adapter bypass'
printf '%s\n' "$linux_gdm1" | grep -Fq 'status = "okay";'
printf '%s\n' "$linux_lan4" | grep -Fq 'status = "okay";'
printf '%s\n' "$linux_lan4" | grep -Fq 'label = "lan4";'
printf '%s\n' "$linux_lan4_phy" | grep -Fq 'interrupts = <4>;'
require_fixed 'gsw_phy4: ethernet-phy@c' "$openwrt_dir/target/linux/airoha/dts/an7581.dtsi" \
	'LAN4 internal PHY12 declaration'
printf '%s\n' "$linux_phy_lan1" | grep -Fq 'reg = <5>;'
printf '%s\n' "$linux_phy_lan1" | grep -Fq 'reset-gpios = <&en7581_pinctrl 29 GPIO_ACTIVE_LOW>;'
printf '%s\n' "$linux_phy_lan1" | grep -Fq 'realtek,host-serdes-usxgmii;'
printf '%s\n' "$linux_phy_lan2" | grep -Fq 'reg = <8>;'
printf '%s\n' "$linux_phy_lan2" | grep -Fq 'reset-gpios = <&en7581_pinctrl 27 GPIO_ACTIVE_LOW>;'
printf '%s\n' "$linux_phy_lan2" | grep -Fq 'realtek,host-serdes-usxgmii;'
printf '%s\n' "$linux_phy_lan3" | grep -Fq 'compatible = "ethernet-phy-ieee802.3-c22";'
printf '%s\n' "$linux_phy_lan3" | grep -Fq 'reg = <15>;'
require_fixed 'phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x19, 0x0154)' \
	"$realtek_host_serdes_patch" 'RTL8261 VEND1 register/value order'
if grep -Fq 'phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x154, 0x0019)' \
	"$realtek_host_serdes_patch"; then
	echo 'RTL8261 host-SerDes register/value order is reversed' >&2
	exit 1
fi
require_fixed '&gdm2 {' "$board_dts" 'PON GDM2 enablement'
require_fixed 'openwrt,netdev-name = "pon";' "$board_dts" 'PON netdev'
require_fixed 'val = assert ? 0 : BIT(id % RST_NR_PER_BANK);' \
	"$pcie_reset_patch" 'deterministic inverted PCIe reset deassert value'
require_fixed 'val = assert ? BIT(id % RST_NR_PER_BANK) : 0;' \
	"$pcie_reset_patch" 'deterministic ordinary PCIe reset value'
if grep -Fq 'val |= assert ?' "$pcie_reset_patch"; then
	echo 'PCIe reset update still uses an uninitialized value' >&2
	exit 1
fi

# Hardware mode is selected at boot. The legacy compatible defaults to GPON,
# while the distinct XGS image defaults to XGS-PON and keeps its board identity.
require_fixed '#include "an7581-axon-xg2010g-ubi.dts"' "$xgspon_dts" \
	'XGS-PON board inheritance'
require_fixed 'compatible = "axon,xg2010g-xgspon"' "$xgspon_dts" \
	'XGS-PON board identity'
[ "$(grep -Fc 'airoha,pon-mode = "xgspon";' "$xgspon_dts")" -eq 2 ]
require_fixed 'define Device/axon_xg2010g-xgspon-ubi' "$image_makefile" \
	'XGS-PON image definition'
require_fixed 'DEVICE_DTS := an7581-axon-xg2010g-xgspon-ubi' "$image_makefile" \
	'XGS-PON image DT selection'
require_fixed 'SUPPORTED_DEVICES := axon,xg2010g axon,xg2010g-xgspon econet,xg2010g' \
	"$image_makefile" 'generic image cross-mode sysupgrade compatibility'
require_fixed 'SUPPORTED_DEVICES := axon,xg2010g-xgspon axon,xg2010g econet,xg2010g' \
	"$image_makefile" 'XGS-PON image cross-mode sysupgrade compatibility'
require_fixed 'axon,xg2010g-xgspon)' "$pon_mode_defaults" \
	'XGS-PON UCI board match'
require_fixed 'default_mode=xgspon' "$pon_mode_defaults" \
	'XGS-PON board default UCI mode'
require_fixed 'axon,xg2010g|econet,xg2010g)' "$pon_mode_defaults" \
	'legacy XG2010G UCI board matches'
require_fixed 'default_mode=gpon' "$pon_mode_defaults" \
	'legacy XG2010G board default UCI mode'
require_fixed 'pon_mode="${current_mode:-$default_mode}"' "$pon_mode_defaults" \
	'existing UCI PON mode preservation'
require_fixed './files/99-airoha-pon-mode $(1)/etc/uci-defaults/' "$omcid_makefile" \
	'PON mode defaults package installation'

# The XGS-specific compatible is the first root compatible and therefore the
# runtime board name. It must follow every XG2010G base-files path rather than
# falling through to an unconfigured network or the generic NAND upgrader.
for board_file in "$board_network" "$board_leds" "$platform_upgrade" \
		"$uboot_envtools"; do
	require_fixed 'axon,xg2010g-xgspon' "$board_file" \
		'XGS-PON runtime board match'
done
[ "$(grep -Fc 'axon,xg2010g-xgspon' "$board_network")" -eq 1 ]
[ "$(grep -Fc 'axon,xg2010g-xgspon' "$board_leds")" -eq 1 ]
[ "$(grep -Fc 'axon,xg2010g-xgspon' "$platform_upgrade")" -eq 2 ]
[ "$(grep -Fc 'axon,xg2010g-xgspon' "$uboot_envtools")" -eq 2 ]
require_fixed 'ucidef_set_interfaces_lan_wan "lan1 lan2 lan3 lan4" "pon"' \
	"$board_network" 'four-LAN plus PON network defaults'
require_fixed 'fit_check_image "$1"' "$platform_upgrade" \
	'XG2010G FIT image validation'
require_fixed 'fit_do_upgrade "$1"' "$platform_upgrade" \
	'XG2010G FIT upgrade path'
require_fixed 'ubootenv_add_ubi_default' "$uboot_envtools" \
	'XG2010G redundant UBI environment setup'

# GDM3/GDM4, not GDM1/GDM2, support the external hardware arbiter.  Its TDM
# behavior makes LAN1/LAN3 a shared resource; it does not imply CPU forwarding
# or a shared GDM across all four LAN ports.
require_fixed 'GDM3 or GDM4 ports via a hw arbiter that' "$multi_serdes_patch" \
	'external-arbiter scope'
require_fixed 'manages the traffic in a TDM manner' "$multi_serdes_patch" \
	'external-arbiter TDM behavior'
require_fixed 'Please note GDM1 or GDM2 does not support the connection with the external' \
	"$multi_serdes_patch" 'GDM1/GDM2 arbiter exclusion'
require_fixed 'Allowed nbq for EN7581 on GDM3 port are 4 and 5 for PCIE0' \
	"$multi_serdes_patch" 'GDM3 endpoint mapping'
require_fixed 'netdev->addr_assign_type == NET_ADDR_RANDOM' \
	"$random_mac_patch" 'factory MAC convergence from random probe addresses'
require_fixed 'ether_addr_equal(addr, netdev_from_priv(dev)->dev_addr)' \
	"$random_mac_patch" 'probe-time random MAC hardware-programming deferral'
[ "$(grep -Fc 'addr_assign_type == NET_ADDR_RANDOM' "$random_mac_patch")" -eq 2 ] || {
	echo 'random MAC handling must cover both the current device and its peers' >&2
	exit 1
}

# Recovery U-Boot must retain all three external SerDes endpoints. LAN1 is the
# GDM4 primary endpoint, LAN3 is its USB secondary, and LAN2 remains on GDM3.
uboot_gdm3="$(extract_block 'gdm3: ethernet@3 {' "$uboot_dtsi")"
uboot_gdm4="$(extract_block '&gdm4 {' "$uboot_dtsi")"
uboot_gdm1="$(extract_block '&gdm1 {' "$uboot_dtsi")"
uboot_eth_pcs="$(extract_block '&eth_pcs {' "$uboot_dtsi")"
uboot_pcie_pcs="$(extract_block 'pcie_pcs: pcs@1fa04000 {' "$uboot_dtsi")"
uboot_switch="$(extract_block '&switch {' "$uboot_dtsi")"
printf '%s\n' "$uboot_gdm3" | grep -Fq 'phy-handle = <&phy8>;'
printf '%s\n' "$uboot_gdm3" | grep -Fq 'pcs = <&pcie_pcs 1>;'
printf '%s\n' "$uboot_gdm3" | grep -Fq 'airoha,source-port = <0x17>;'
printf '%s\n' "$uboot_gdm3" | grep -Fq 'airoha,tx-channel = <11>;'
printf '%s\n' "$uboot_gdm3" | grep -Fq 'airoha,nboq = <5>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'phy-handle = <&phy5>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'pcs = <&eth_pcs>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'airoha,usb-hsgmii;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'airoha,source-port = <0x18>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'airoha,tx-channel = <13>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'secondary-pcs = <&usb_pcs>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'secondary-phy = <&phy15>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'airoha,secondary-source-port = <0x19>;'
printf '%s\n' "$uboot_gdm4" | grep -Fq 'airoha,secondary-tx-channel = <12>;'
printf '%s\n' "$uboot_gdm1" | grep -Fq 'status = "okay";'
printf '%s\n' "$uboot_eth_pcs" | grep -Fq 'status = "okay";'
if printf '%s\n' "$uboot_pcie_pcs" | grep -Fq 'resets ='; then
	echo 'PCIe PCS must not claim the shared Ethernet XSI reset lines' >&2
	exit 1
fi
printf '%s\n' "$uboot_pcie_pcs" | grep -Fq 'status = "okay";'
if grep -Fq 'gdm3_pcs_rsts' "$uboot_driver"; then
	echo 'GDM3 must not reacquire and pulse the parent-owned HSI1 reset' >&2
	exit 1
fi
printf '%s\n' "$uboot_switch" | grep -Fq 'status = "okay";'
printf '%s\n' "$uboot_switch" | grep -Fq 'airoha,phy-poll-start = <5>;'
printf '%s\n' "$uboot_switch" | grep -Fq 'airoha,phy-poll-end = <15>;'
printf '%s\n' "$uboot_switch" | grep -Fq \
	'airoha,recovery-switch-port-mask = <0x10>;'
printf '%s\n' "$uboot_switch" | grep -Fq \
	'airoha,recovery-phy-mask = <0x1000>;'
require_fixed 'eth->gdm4_dual_hsgmii = usb_primary && ofnode_valid(secondary_pcs);' \
	"$uboot_driver" 'U-Boot GDM4 dual-endpoint parser'
require_fixed 'AIROHA_FPORT_GDM4_USB' "$uboot_driver" \
	'U-Boot LAN3 recovery egress'
require_fixed 'if (sport == 1 || sport == 2 || sport == 4)' "$uboot_driver" \
	'U-Boot switch-port receive mapping'
require_fixed 'eth->default_tx_fport = 1;' "$uboot_driver" \
	'U-Boot LAN4 recovery default egress'

echo 'XG2010G LAN/PON topology and multi-SerDes sharing contract are source-verified'
