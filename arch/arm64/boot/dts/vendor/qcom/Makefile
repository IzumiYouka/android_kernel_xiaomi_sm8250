ifeq ($(CONFIG_MACH_XIAOMI),y)

ifeq ($(CONFIG_MACH_XIAOMI_SM8250),y)
ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
        dtbo-$(CONFIG_ARCH_KONA) += \
                alioth-sm8250-overlay.dtbo

alioth-sm8250-overlay.dtbo-base := kona-v2.1.dtb

endif # CONFIG_BUILD_ARM64_DT_OVERLAY
else
dtb-$(CONFIG_ARCH_KONA) += kona-v2.1.dtb
endif # CONFIG_MACH_XIAOMI_SM8250

endif # CONFIG_MACH_XIAOMI

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files    := *.dtb *.dtbo
