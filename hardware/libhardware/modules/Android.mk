hardware_modules := gralloc hwcomposer audio nfc lights
include $(call all-named-subdir-makefiles,$(hardware_modules))
