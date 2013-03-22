# Copyright 2006 The Android Open Source Project

LOCAL_SRC_FILES += power/power.c
ifeq ($(BOARD_SOC_CLASS),IMX5X)
ifeq ($(HAVE_FSL_IMX_IPU),true)
LOCAL_C_INCLUDES += external/linux-lib/ipu
LOCAL_CFLAGS += -DCHECK_MX5X_HARDWARE
endif
endif

ifeq ($(QEMU_HARDWARE),true)
  LOCAL_SRC_FILES += power/power_qemu.c
  LOCAL_CFLAGS    += -DQEMU_POWER=1
endif
