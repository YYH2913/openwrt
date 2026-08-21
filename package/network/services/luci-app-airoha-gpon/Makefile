# SPDX-License-Identifier: Apache-2.0

include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for the Airoha PON optical port
LUCI_DEPENDS:=+luci-base +airoha-omcid +airoha-epon-oamd +uci
LUCI_PKGARCH:=all
PKG_VERSION:=26.223.48674~1458887
PKG_RELEASE:=13

PKG_LICENSE:=Apache-2.0

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
