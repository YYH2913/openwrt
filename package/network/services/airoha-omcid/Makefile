# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-omcid
PKG_VERSION:=0.31.0
PKG_RELEASE:=26

# Development checkout. Replace this URL with the public standalone repository
# before publishing feeds or release artifacts.
PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=file://$(abspath $(TOPDIR)/../airoha-omci)
PKG_SOURCE_VERSION:=dababb660fecde12204d369d4831dfea79cf569f
PKG_SOURCE:=airoha-omcid-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.zst
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=Apache-2.0
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_DEPENDS:=golang/host
PKG_BUILD_PARALLEL:=1

GO_PKG:=github.com/xg2010g/airoha-omci
GO_PKG_BUILD_PKG:=$(GO_PKG)/cmd/airoha-omcid $(GO_PKG)/cmd/airoha-mcastd
GO_PKG_LDFLAGS_X:=main.version=$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk
include $(TOPDIR)/feeds/packages/lang/golang/golang-package.mk

GO_MOD_ARGS += -mod=vendor

define Package/airoha-omcid
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Network Devices
  TITLE:=Airoha ONU OMCI control plane
  URL:=https://github.com/xg2010g/airoha-omci
  DEPENDS:=$(GO_ARCH_DEPENDS) +kmod-airoha-gpon +kmod-airoha-xgspon +kmod-veth +fitblk +fwtool +ip-full +ip-bridge \
    +jsonfilter +kmod-sched-act-police +kmod-sched-act-vlan +kmod-sched-bpf +kmod-sched-core +kmod-sched-flower \
    +tc-full +ubi-utils +uboot-envtools +uci +ucode
endef

define Package/airoha-omcid/description
 ONU-side ITU-T G.988 control plane for the Airoha EN7581. It owns the ONU
 MIB and applies OLT-provisioned traffic and Ethernet service configuration.
 XGS-PON mode requires the trusted secure-OMCC kernel transport and remains
 fail-closed when its capabilities are unavailable.
endef

define Package/airoha-omcid/conffiles
/etc/config/airoha-gpon
/etc/config/airoha-omci-images
/etc/airoha-omci/
endef

define Package/airoha-omcid/install
	$(call GoPackage/Package/Install/Bin,$(PKG_INSTALL_DIR))
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/airoha-omcid $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/airoha-mcastd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/airoha-xgspon-evidence $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/airoha-xpon-evidence $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/airoha-omcid.init $(1)/etc/init.d/airoha-omcid
	$(INSTALL_BIN) ./files/airoha-gpon-config.init $(1)/etc/init.d/airoha-gpon-config
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/airoha-gpon.config $(1)/etc/config/airoha-gpon
	$(INSTALL_CONF) ./files/airoha-omci-images.config $(1)/etc/config/airoha-omci-images
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/99-airoha-pon-mode $(1)/etc/uci-defaults/
	$(INSTALL_DIR) $(1)/etc/airoha-omci
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) ./files/airoha-gpon-config $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-apply $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-control $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-events $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-platform $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-software $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-omci-render.uc $(1)/usr/libexec/airoha-omci-render
endef

$(eval $(call GoBinPackage,airoha-omcid))
$(eval $(call BuildPackage,airoha-omcid))
