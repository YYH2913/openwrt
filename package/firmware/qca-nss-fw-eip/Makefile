include $(TOPDIR)/rules.mk

PKG_NAME:=qca-nss-fw-eip
PKG_VERSION:=3.3
PKG_RELEASE:=1

PKG_SOURCE_VERSION:=c49273e8354ab0e9c9a74b3a1a490ea74dc04c71
PKG_SOURCE:=qca-nss-fw-eip-al-$(PKG_SOURCE_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/be7000/qca-nss-fw-eip-al/tar.gz/$(PKG_SOURCE_VERSION)?
PKG_HASH:=fa98c9bf376852269fdc2e9af0aa9609a5754bcae89a05eba9137b22be53f1cf
PKG_BUILD_DIR:=$(BUILD_DIR)/qca-nss-fw-eip-al-$(PKG_SOURCE_VERSION)

PKG_LICENSE:=ISC

include $(INCLUDE_DIR)/package.mk

define Package/qca-nss-fw-eip
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Qualcomm IPQ95xx EIP-197 firmware
  DEPENDS:=@TARGET_qualcommbe_ipq95xx
endef

define Package/qca-nss-fw-eip/description
  Packet-engine firmware for the IPQ95xx EIP-197 cryptographic accelerator.
endef

define Build/Compile
endef

define Package/qca-nss-fw-eip/install
	$(INSTALL_DIR) $(1)/lib/firmware
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/files/ifpp.bin $(1)/lib/firmware/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/files/ipue.bin $(1)/lib/firmware/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/files/ofpp.bin $(1)/lib/firmware/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/files/opue.bin $(1)/lib/firmware/
endef

$(eval $(call BuildPackage,qca-nss-fw-eip))
