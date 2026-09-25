#
#	This file is part of the OrangeFox Recovery Project
# 	Copyright (C) 2025 The OrangeFox Recovery Project
#
#	OrangeFox is free software: you can redistribute it and/or modify
#	it under the terms of the GNU General Public License as published by
#	the Free Software Foundation, either version 3 of the License, or
#	any later version.
#
#	OrangeFox is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU General Public License for more details.
#
# 	This software is released under GPL version 3 or any later version.
#	See <http://www.gnu.org/licenses/>.
#
# 	Please maintain this if you use this script or any part of it
#

# screen settings
# e.g. if the aspect ratio is 19:9 then use 19*120 (=2280)
AERA_MAINTAINER := Jonas Salo & Daniel Springer
AERA_UI_ADAPTIVE_RESOLUTION := true
AERA_SCREEN_H := 2400
AERA_STATUS_H := 115
AERA_HIDE_NOTCH := 1
AERA_CLOCK_POS := 1
AERA_STATUS_INDENT_LEFT := 56
AERA_STATUS_INDENT_RIGHT := 48
AERA_ALLOW_DISABLE_NAVBAR := 0
AERA_USE_GREEN_LED := 0

# other stuff
AERA_QUICK_BACKUP_LIST := /boot;/data;
AERA_ENABLE_LPTOOLS := 1
AERA_NO_TREBLE_COMPATIBILITY_CHECK := 1
AERA_DYNAMIC_FULL_SIZE := 9126805504

# number of list options before scrollbar creation
AERA_OPTIONS_LIST_NUM := 11

# A/B with recovery partition
AERA_AB_DEVICE_WITH_RECOVERY_PARTITION := 1

# ----- data format stuff -----
# ensure that /sdcard is bind-unmounted before f2fs data repair or format
AERA_UNBIND_SDCARD_F2FS := 1

# automatically wipe /metadata after data format
AERA_WIPE_METADATA_AFTER_DATAFORMAT := 1

# avoid MTP issues after data format
AERA_BIND_MOUNT_SDCARD_ON_FORMAT := 1

# Set to 1 to attempt to unmount the SD cards before rebooting
AERA_UNMOUNT_SDCARDS_BEFORE_REBOOT := 1

# don't spam the console with loop errors
AERA_LOOP_DEVICE_ERRORS_TO_LOG := 1

# lz4 compression
AERA_USE_LZ4_COMPRESSION := 1

# build all the partition tools
AERA_ENABLE_ALL_PARTITION_TOOLS := 1

ifeq ($(FIXED_DECRYPT),false)
	# Set to 1 to skip the FBE decryption routines when device-specific crypto hangs.
	AERA_SKIP_FBE_DECRYPTION := 1
endif

# Set this to 1 to replace the "Swipe up" lockscreen screen with a button
AERA_USE_LOCKSCREEN_BUTTON := 1

# Called just before formatting /data; only useful for devices/ROMs that have dynamic partitions
AERA_USE_DMCTL := 1

# Use this to change the default time zone
AERA_DEFAULT_TIMEZONE := GMT0;BST,M3.5.0,M10.5.0

# Set this to 1 to avoid the new 'NO KERNEL CONFIG' error, when using a prebuilt kernel
AERA_FORCE_PREBUILT_KERNEL := 1

# Set this to 1 if your device uses aidl (as opposed to hidl) to handle boot control, particularly changing slots
AERA_USE_AIDL_BOOT_CONTROL := 1

# Set this to 1 to remove the toggleable "Reflash AERA after flashing a ROM" option.
# Implemented forced method using /system/bin/pre_rom_flash.sh and .../post_rom_flash.sh
# AERA_NO_REFLASH_CURRENT_RECOVERY := 1

# Set this to 1 to disable automatic rebooting after openrecoveryscript finishes
AERA_DISABLE_ORS_AUTO_REBOOT := 1

# Set this to 1 to force the selection of f2fs when formatting data
AERA_FORCE_DATA_FORMAT_F2FS := 1

# Set this to 1 to include an addon for removing factory reset protection (FRP)
AERA_ENABLE_FRP_ADDON := 1

# Set to 1 to force the casefolding props to true. Useful for devices that shipped with Android 11+/FBEv2, where casefolding is always used
AERA_FORCE_CASEFOLDING := 1

# Set to 1 to enable the flashlight feature
AERA_FLASHLIGHT_ENABLE := 1
AERA_FL_PATH1 := /tmp/flashlight

# Set to 1 to block operations after flashing a ROM i.e. formatting data, flashing other zips etc.
AERA_BLOCK_OPERATIONS_AFTER_ROM_FLASH := 1

# Set to 1 to include WLAN features
AERA_ENABLE_WLAN := 1
