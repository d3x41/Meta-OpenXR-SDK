LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := scenesharing

LOCAL_CFLAGS += -Werror

LOCAL_C_INCLUDES := \
                    $(LOCAL_PATH)/../../../Src/SimpleXrInput.h \
					$(LOCAL_PATH)/../../../Src/SceneSharingGl.h \
					$(LOCAL_PATH)/../../../Src/SceneSharingXr.h \
					$(LOCAL_PATH)/../../../../../1stParty/OVR/Include \
					$(LOCAL_PATH)/../../../../../OpenXr/Include \
					$(LOCAL_PATH)/../../../../../3rdParty/khronos/openxr/OpenXR-SDK/include/ \
					$(LOCAL_PATH)/../../../../../3rdParty/khronos/openxr/OpenXR-SDK/src/common/

# 
LOCAL_SRC_FILES	:= 	../../../Src/SimpleXrInput.cpp \
					../../../Src/SceneSharingGl.cpp \
					../../../Src/SceneSharingXr.cpp \

LOCAL_LDLIBS := -lEGL -lGLESv3 -landroid -llog

LOCAL_LDFLAGS := -u ANativeActivity_onCreate

LOCAL_STATIC_LIBRARIES := android_native_app_glue
LOCAL_SHARED_LIBRARIES := openxr_loader

include $(BUILD_SHARED_LIBRARY)

$(call import-module,OpenXR/Projects/AndroidPrebuilt/jni)
$(call import-module,android/native_app_glue)
