LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

# Imported from cm-14.1/device/motoroal/xt897
LOCAL_SDK_VERSION := 25
LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := $(call all-java-files-under, src)

LOCAL_PACKAGE_NAME := AsantiKeypad
LOCAL_CERTIFICATE := platform

include $(BUILD_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))