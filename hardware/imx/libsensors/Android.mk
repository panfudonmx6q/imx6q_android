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

ifeq ($(BOARD_HAS_SENSOR),true)
LOCAL_PATH := $(call my-dir)

ifneq ($(TARGET_SIMULATOR),true)

# HAL module implemenation, not prelinked, and stored in
# hw/<SENSORS_HARDWARE_MODULE_ID>.<ro.product.board>.so
include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE := sensors.freescale
LOCAL_MODULE_TAGS := eng

LOCAL_CFLAGS := -DLOG_TAG=\"Sensors\"
ifeq ($(SENSOR_MMA8451),true)
LOCAL_CPPFLAGS += -DACCELEROMETER_SENSOR_MMA8451
else
 ifeq ($(SENSOR_MMA8450),true)
 LOCAL_CPPFLAGS += -DACCELEROMETER_SENSOR_MMA8450
 endif
endif

LOCAL_SRC_FILES := 						\
				sensors.cpp 			\
				SensorBase.cpp			\
				LightSensor.cpp			\
				AccelSensor.cpp			\
				MagSensor.cpp			\
				PressSensor.cpp			\
				InputEventReader.cpp

LOCAL_SHARED_LIBRARIES := liblog libcutils libdl

include $(BUILD_SHARED_LIBRARY)

endif # !TARGET_SIMULATOR

endif #
