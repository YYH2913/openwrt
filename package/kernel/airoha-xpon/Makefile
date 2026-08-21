# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=airoha-xpon
PKG_RELEASE:=52
PKG_LICENSE:=GPL-2.0-only AND LicenseRef-Airoha-Proprietary

include $(INCLUDE_DIR)/package.mk

define KernelPackage/airoha-en7572
  SUBMENU:=Network Devices
  TITLE:=Airoha EN7572 BOSA controller
  DEPENDS:=@TARGET_airoha_an7581 +kmod-i2c-core +airoha-en7572-firmware
  FILES:=$(PKG_BUILD_DIR)/airoha-en7572.ko
  AUTOLOAD:=$(call AutoProbe,airoha-en7572)
endef

define KernelPackage/airoha-en7572/description
 Linux I2C driver for the EN7572 BOSA controller used by the XG2010G.
	 It loads the optical MD32 firmware and the device's dual-eye 512-byte BOB
	 table, selecting the SDK-defined XG/XGS or XE/XES calibration bank.
endef

define KernelPackage/airoha-gpon
  SUBMENU:=Network Devices
  TITLE:=Airoha EN7581 GPON MAC support
  DEPENDS:=@TARGET_airoha_an7581 +kmod-airoha-xpon-core
  FILES:=$(PKG_BUILD_DIR)/airoha-gpon.ko
  AUTOLOAD:=$(call AutoProbe,airoha-gpon)
endef

define KernelPackage/airoha-xpon-core
  SUBMENU:=Network Devices
  TITLE:=Airoha EN7581 runtime XPON mode owner
  DEPENDS:=@TARGET_airoha_an7581 +kmod-airoha-en7572
  FILES:=$(PKG_BUILD_DIR)/airoha-xpon-core.ko
  AUTOLOAD:=$(call AutoProbe,airoha-xpon-core)
endef

define KernelPackage/airoha-xpon-core/description
 Single fail-closed owner for the EN7581 PON PCS and EN7572 BOSA. It runs
 protocol backend stop/start, PHY reconfiguration, calibration and rollback
 as one runtime mode-switch transaction while optical TX remains disabled.
endef

define KernelPackage/airoha-gpon/description
 Linux platform driver for the EN7581 GPON MAC used by the XG2010G. It
 provides MAC/PHY initialization, GPON activation-message handling, GEM/TCONT
 data-path configuration and hard-IRQ rogue-ONU optical protection.
endef

define KernelPackage/airoha-xgspon
  SUBMENU:=Network Devices
  TITLE:=Airoha EN7581 XGS-PON control endpoint
  DEPENDS:=@TARGET_airoha_an7581 +kmod-crypto-cmac +kmod-crypto-ecb +kmod-airoha-xpon-core
  FILES:=$(PKG_BUILD_DIR)/airoha-xgs-security.ko \
    $(PKG_BUILD_DIR)/airoha-xgspon.ko
  AUTOLOAD:=$(call AutoProbe,airoha-xgs-security airoha-xgspon)
endef

define KernelPackage/airoha-xgspon/description
 Fail-closed EN7581 XGS-PON resource owner with trusted G.9807.1 key
 derivation, gated upstream PLOAM activation and secure OMCC ABI endpoint.
 Optical TX and secure OMCC capabilities remain disabled until the complete
 runtime PHY, calibration, O5, key-slot and QDMA gates pass.
endef

define KernelPackage/airoha-epon
  SUBMENU:=Network Devices
  TITLE:=Airoha AN7581 10G-EPON MAC and MPCP support
  DEPENDS:=@TARGET_airoha_an7581 +kmod-airoha-xpon-core
  FILES:=$(PKG_BUILD_DIR)/airoha-epon.ko
  AUTOLOAD:=$(call AutoProbe,airoha-epon)
endef

define KernelPackage/airoha-epon/description
 Runtime 10G/1G and 10G/10G EPON protocol backend for the AN7581. It owns
 MPCP discovery and registration, 32 LLIDs, DBA report controls and the shared
 PON interrupts while optical transmission remains calibration-gated.
endef

define Package/airoha-en7572-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Airoha EN7572 firmware and XG2010G optical calibration
  PKGARCH:=all
endef

define Package/airoha-en7572-firmware/description
	 EN7572 MD32 PM/DM firmware, the GPON base optical table and the 10G optical
	 table recovered from the XG2010G backup's live rootfs overlay. The 10G table
	 contains a validated XG/XGS A2 eye. A separately named experimental table
	 copies the A2 optical data into A0 for temporary XE/XES hardware testing;
	 it is not a substitute for device-specific A0 calibration.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

define Package/airoha-en7572-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/airoha/xg2010g
	$(INSTALL_DATA) \
		./firmware/airoha/xg2010g/en7572-A60993.pm \
		./firmware/airoha/xg2010g/en7572-A60993.dm \
		$(1)/lib/firmware/airoha/xg2010g/
	$(INSTALL_DATA) \
		./firmware/airoha/xg2010g/en7572-bob.bin \
		$(1)/lib/firmware/airoha/xg2010g/en7572-bob-gpon.bin
	$(INSTALL_DATA) \
		./firmware/airoha/xg2010g/en7572-bob-xgspon.bin \
		$(1)/lib/firmware/airoha/xg2010g/en7572-bob-xgspon.bin
	$(SHELL) ./scripts/en7572-a2-as-a0.sh \
		./firmware/airoha/xg2010g/en7572-bob-xgspon.bin \
		$(1)/lib/firmware/airoha/xg2010g/en7572-bob-a2-as-a0.bin
endef

$(eval $(call KernelPackage,airoha-en7572))
$(eval $(call KernelPackage,airoha-xpon-core))
$(eval $(call KernelPackage,airoha-gpon))
$(eval $(call KernelPackage,airoha-xgspon))
$(eval $(call KernelPackage,airoha-epon))
$(eval $(call BuildPackage,airoha-en7572-firmware))
