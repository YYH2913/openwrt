# SPDX-License-Identifier: GPL-2.0-only

OTHER_MENU:=Other modules


define KernelPackage/pwm-airoha
  SUBMENU:=$(OTHER_MENU)
  TITLE:=Airoha AN7581 and AN7583 PWM
  DEPENDS:=@TARGET_airoha_an7581||TARGET_airoha_an7583
  KCONFIG:= \
        CONFIG_PWM=y \
        CONFIG_PWM_AIROHA=y
  FILES:= \
        $(LINUX_DIR)/drivers/pwm/pwm-airoha.ko
  AUTOLOAD:=$(call AutoProbe,pwm-airoha)
endef

define KernelPackage/pwm-airoha/description
 Kernel module to use the PWM channel on Airoha SoC
endef

$(eval $(call KernelPackage,pwm-airoha))


define KernelPackage/airoha-net-debug
  SUBMENU:=$(OTHER_MENU)
  TITLE:=Airoha Ethernet dynamic debug support
  DEPENDS:=@(TARGET_airoha_an7581)
  KCONFIG:= \
	CONFIG_DYNAMIC_DEBUG=y \
	CONFIG_DYNAMIC_DEBUG_CORE=y
endef

define KernelPackage/airoha-net-debug/description
 Enables runtime dynamic debug controls for Airoha Ethernet, PCS, phylink,
 and PHY drivers. Debug messages remain disabled until explicitly enabled.
endef

$(eval $(call KernelPackage,airoha-net-debug))

