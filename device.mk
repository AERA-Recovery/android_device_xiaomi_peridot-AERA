#
# Copyright (C) 2023 The Android Open Source Project
#
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/xiaomi/peridot

# Base product configuration
$(call inherit-product, $(SRC_TARGET_DIR)/product/base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)

# Virtual A/B OTA configuration
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota/compression_with_xor.mk)

# Emulated storage support
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Configure twrp common.mk
$(call inherit-product, vendor/twrp/config/common.mk)

# Device-specific OrangeFox configuration
$(call inherit-product, $(DEVICE_PATH)/fox_peridot.mk)

# API
PRODUCT_SHIPPING_API_LEVEL := 34
PRODUCT_TARGET_VNDK_VERSION := 34
BOARD_SHIPPING_API_LEVEL := 34
SHIPPING_API_LEVEL := 34

# Dynamic partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Poco F6 Adreno 735 backend. The kernel module, Gen7.11 firmware, EGL/GLES
# userspace, and Qualcomm allocator/mapper closure are sourced from the matching
# stock dump and remain device-local so generic AERA keeps its software fallback.
PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.egl=adreno

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += $(DEVICE_PATH)
