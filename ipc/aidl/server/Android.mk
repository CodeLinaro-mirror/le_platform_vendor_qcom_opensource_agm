
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE        := libagmipcservice
LOCAL_MODULE_OWNER  := qti
LOCAL_VENDOR_MODULE := true

LOCAL_C_INCLUDES    := $(LOCAL_PATH)/inc

LOCAL_CLANG             := true
LOCAL_TIDY              := true
LOCAL_CFLAGS            += -v -Wall -Wthread-safety

LOCAL_VINTF_FRAGMENTS := Manifest_IAGM.xml

LOCAL_SRC_FILES     :=  \
    Service.cpp \
    AgmServerWrapper.cpp

LOCAL_STATIC_LIBRARIES := libagmaidltypeconverter libaidlcommonsupport


LOCAL_SHARED_LIBRARIES := \
    liblog \
    libbinder_ndk \
    libbase \
    libcutils \
    libutils \
    libagm \
    vendor.qti.hardware.agm-V1-ndk

ifeq ($(ENABLE_HYP), true)
LOCAL_SHARED_LIBRARIES += libar-gsl_fe
LOCAL_HEADER_LIBRARIES += libar-gsl_fe_headers
else
LOCAL_SHARED_LIBRARIES += libar-gsl
endif

include $(BUILD_SHARED_LIBRARY)
