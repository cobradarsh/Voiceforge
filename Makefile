TARGET := iphone:clang:latest:15.0
ARCHS = arm64 arm64e
# roothide by default; CI also builds a rootless package (THEOS_PACKAGE_SCHEME=rootless)
THEOS_PACKAGE_SCHEME ?= roothide
INSTALL_TARGET_PROCESSES = SpringBoard

include $(THEOS)/makefiles/common.mk

TWEAK_NAME = VoiceForge
VoiceForge_FILES = Tweak.x VFDSP.c
VoiceForge_CFLAGS = -fobjc-arc -O2 -Wno-deprecated-declarations
VoiceForge_FRAMEWORKS = Foundation AudioToolbox CallKit

include $(THEOS_MAKE_PATH)/tweak.mk
