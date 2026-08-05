include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=qca-nss-eip
PKG_VERSION:=2026.07.17
PKG_RELEASE:=1

PKG_SOURCE_VERSION:=10a89a95f814cd325e23105f102428644f45014f
PKG_SOURCE:=nss-eip-$(PKG_SOURCE_VERSION).tar.gz
PKG_SOURCE_URL:=https://git.codelinaro.org/clo/qsdk/oss/lklm/nss-eip/-/archive/$(PKG_SOURCE_VERSION)/
PKG_HASH:=7e75c63491fb1a9be76e3e2cc40d75d2aa7f0f8518e7f811ba12abfa81f10d03
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/nss-eip-$(PKG_SOURCE_VERSION)

PKG_LICENSE:=ISC
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/qca-nss-eip
  SUBMENU:=Cryptographic API modules
  TITLE:=Qualcomm IPQ95xx EIP-197 driver
  DEPENDS:=@TARGET_qualcommbe_ipq95xx +kmod-qcom-ppe +qca-nss-fw-eip
  FILES:=$(PKG_BUILD_DIR)/driver/eip197/qca-nss-eip.$(LINUX_KMOD_SUFFIX)
  AUTOLOAD:=$(call AutoLoad,52,qca-nss-eip)
endef

define KernelPackage/qca-nss-eip/description
  Low-level driver for the EIP-197 cryptographic accelerator in IPQ95xx SoCs.
endef

define KernelPackage/qca-nss-eip-crypto
  SUBMENU:=Cryptographic API modules
  TITLE:=Qualcomm IPQ95xx EIP-197 Crypto API client
  DEPENDS:=+kmod-qca-nss-eip +kmod-crypto-authenc +kmod-crypto-aead \
	+kmod-crypto-des +kmod-crypto-md5 +kmod-crypto-sha1 \
	+kmod-crypto-sha256 +kmod-crypto-sha512
  FILES:=$(PKG_BUILD_DIR)/clients/crypto/qca-nss-eip-crypto.$(LINUX_KMOD_SUFFIX)
  AUTOLOAD:=$(call AutoLoad,53,qca-nss-eip-crypto)
endef

define KernelPackage/qca-nss-eip-crypto/description
  Registers the EIP-197 look-aside engine with the Linux Crypto API.
endef

define KernelPackage/qca-nss-eip-ipsec
  SUBMENU:=Cryptographic API modules
  TITLE:=Qualcomm IPQ95xx EIP-197 IPsec client
  DEPENDS:=+kmod-qca-nss-eip +kmod-ipsec +kmod-ipsec4 +kmod-ipsec6 \
	+kmod-crypto-echainiv
  FILES:=$(PKG_BUILD_DIR)/clients/ipsec/qca-nss-eip-ipsec.$(LINUX_KMOD_SUFFIX)
  AUTOLOAD:=$(call AutoLoad,54,qca-nss-eip-ipsec)
endef

define KernelPackage/qca-nss-eip-ipsec/description
  Offloads XFRM ESP tunnel and transport SAs to the EIP-197 packet engine.
endef

define Build/Configure
endef

define Build/Compile
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		NSS_EIP_DIR=eip197 \
		SoC=ipq95xx \
		eip_crypto=m \
		eip_ipsec=m \
		modules
endef

$(eval $(call KernelPackage,qca-nss-eip))
$(eval $(call KernelPackage,qca-nss-eip-crypto))
$(eval $(call KernelPackage,qca-nss-eip-ipsec))
