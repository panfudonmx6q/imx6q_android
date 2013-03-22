# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq (true, false)
ifeq ($(HAVE_FSL_IMX_GPU2D),true)
ifeq ($(BOARD_SOC_CLASS),IMX5X)

LOCAL_PATH := $(call my-dir)

# HAL module implemenation, not prelinked and stored in
# hw/<OVERLAY_HARDWARE_MODULE_ID>.<ro.product.board>.so
include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := true
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_C_INCLUDES += hardware/imx/mx5x/libgralloc
LOCAL_SHARED_LIBRARIES := liblog
ifeq ($(BOARD_SOC_TYPE),IMX50)
LOCAL_SHARED_LIBRARIES += libc2d_z160
else
LOCAL_SHARED_LIBRARIES += libc2d_z430
endif

LOCAL_SRC_FILES := 	\
	copybit.cpp
	
LOCAL_MODULE := copybit.$(TARGET_BOARD_PLATFORM)
LOCAL_CFLAGS:= -DLOG_TAG=\"$(TARGET_BOARD_PLATFORM).copybit\" -D_LINUX

LOCAL_MODULE_TAGS := eng

include $(BUILD_SHARED_LIBRARY)

endif
endif
endif
