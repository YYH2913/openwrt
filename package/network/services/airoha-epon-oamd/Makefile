# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk

PKG_NAME:=airoha-epon-oamd
PKG_VERSION:=0.1.0
PKG_RELEASE:=26
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=XG2010G OpenWrt maintainers

include $(INCLUDE_DIR)/package.mk

define Package/airoha-epon-oamd
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Network Devices
  TITLE:=Airoha EPON OAM control plane
  DEPENDS:=@TARGET_airoha_an7581 +kmod-airoha-epon +ethtool +libuci +tc-full \
	+kmod-nft-bridge +nftables-json
endef

define Package/airoha-epon-oamd/description
 Dedicated IEEE 802.3ah EPON OAM control plane for the XG2010G. It owns the
 EPON OAM character device and never starts or calls the ITU-T OMCI service.
 Organization-specific CTC/CUC and DPoE traffic is kept behind independent
 dispatchers and unsupported operations fail closed.
endef

define Package/airoha-epon-oamd/conffiles
/etc/config/airoha-epon
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 \
		-Wall -Wextra -Werror -D_GNU_SOURCE \
		-o $(PKG_BUILD_DIR)/airoha-epon-oamd \
		$(PKG_BUILD_DIR)/airoha-epon-oamd.c \
		$(TARGET_LDFLAGS) -luci
endef

define Package/airoha-epon-oamd/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/airoha-epon-oamd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/airoha-epon-ctc-platform $(1)/usr/libexec/
	$(INSTALL_BIN) ./files/airoha-epon-oamd.init $(1)/etc/init.d/airoha-epon-oamd
	$(INSTALL_CONF) ./files/airoha-epon.config $(1)/etc/config/airoha-epon
endef

$(eval $(call BuildPackage,airoha-epon-oamd))
