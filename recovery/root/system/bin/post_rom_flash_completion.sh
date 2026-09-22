#!/system/bin/sh

SCRIPT_NAME="$(basename "$0")"

LOGMSG() {
    echo "I:$@" >> /tmp/recovery.log
}

LOGMSG "---$SCRIPT_NAME start---"

if [ -s /tmp/aera_backup.img ]; then
	for slot in _a _b; do
		if [ -e /dev/block/bootdevice/by-name/recovery${slot} ]; then
			LOGMSG "Restoring AERA to slot ${slot}..."
			if dd if="/tmp/aera_backup.img" of="/dev/block/bootdevice/by-name/recovery${slot}" bs=1M; then
				sync
			else
				LOGMSG "Failed to flash to slot ${slot}..."
			fi
		else
	        LOGMSG "Recovery partition not found for slot ${slot}, skipping restore..."
		fi
	done
else
	LOGMSG "Unable to find the AERA recovery backup"
fi

# LOGMSG "Clearing previous DFE installation logs..."
# rm -rf /sdcard/neo_file_*

# LOGMSG "Setting instructions for next reboot..."
# echo "install /FFiles/DFE.zip" > /cache/recovery/openrecoveryscript

# LOGMSG "Preserving recovery.log before recovery reboot..."
LOGMSG "---$SCRIPT_NAME end---"
# mkdir -p /persist/AERA/logs
# cp /tmp/recovery.log "/persist/AERA/logs/dfe_recovery_$(date +"%Y%m%d_%H%M%S").log"

# reboot recovery
