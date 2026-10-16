#!/system/bin/sh

PATH=/system/bin:/sbin
export PATH

LOGMSG() {
    echo "[aera-audio] $*" >> /dev/kmsg
}

fail() {
    LOGMSG "ERROR: $*"
    exit 1
}

SLOT="$(getprop ro.boot.slot_suffix)"
STOCK_ROOT="/mnt/aera-stock"
STOCK_VENDOR="${STOCK_ROOT}/vendor"
STOCK_SYSTEM="${STOCK_ROOT}/system"
STOCK_ODM="${STOCK_ROOT}/odm"
STOCK_LIB64="/vendor/aera-stock-lib64"
AUDIO_ABI="/vendor/aera-audio-abi"

# Serialise bootstrap invocations from init and the audio bridge.
# Keep the lock outside the mounted vendor/system/odm subdirectories.
AUDIO_BOOTSTRAP_LOCK="${STOCK_ROOT}/.audio-lock"

mkdir -p "${STOCK_ROOT}" || \
    fail "could not create ${STOCK_ROOT}"

lock_attempt=0
while ! mkdir "${AUDIO_BOOTSTRAP_LOCK}"; do
    lock_attempt=$((lock_attempt + 1))
    [ "${lock_attempt}" -ge 300 ] && \
        fail "another audio bootstrap did not finish"
    sleep 0.1
done

# Only the invocation that acquired the lock installs its cleanup.
trap 'rmdir "${AUDIO_BOOTSTRAP_LOCK}"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

LOGMSG "Bootstrap lock acquired, pid=$$"

LOGMSG "Bootstrap start, slot=${SLOT}"

if grep -q " /vendor " /proc/mounts; then
    fail "/vendor is still mounted from the live ROM; refusing audio bootstrap"
fi

mkdir -p "${STOCK_ROOT}" "${STOCK_VENDOR}" "${STOCK_SYSTEM}" "${STOCK_ODM}"

for target in \
    "${STOCK_LIB64}" \
    "${AUDIO_ABI}" \
    /vendor/dsp \
    /vendor/lib/rfsa/adsp \
    /vendor/firmware_mnt; do
    [ -e "${target}" ] || fail "missing ramdisk bind target ${target}"
done

for target in \
    "${AUDIO_ABI}/libc++.so" \
    "${AUDIO_ABI}/libbinder.so" \
    "${AUDIO_ABI}/libbinder_ndk.so"; do
    [ -e "${target}" ] || fail "missing ramdisk ABI bind target ${target}"
done

for target in \
    /vendor/bin/hw/android.hardware.audio.service \
    /vendor/lib64/vendor.qti.hardware.AGMIPC@1.0-impl.so \
    /vendor/lib64/vendor.qti.hardware.AGMIPC@1.0.so \
    /vendor/lib64/vendor.qti.hardware.pal@1.0-impl.so \
    /vendor/lib64/vendor.qti.hardware.pal@1.0.so \
    /vendor/lib64/hw/android.hardware.audio@7.1-impl.so \
    /vendor/lib64/hw/android.hardware.audio.effect@7.0-impl.so \
    /vendor/lib64/hw/audio.primary.pineapple.so \
    /vendor/lib64/hw/audio.r_submix.default.so \
    /vendor/lib64/hw/audio.usb.default.so \
    /vendor/lib64/hw/android.hardware.bluetooth.audio-impl-qti.so \
    /vendor/lib64/hw/vendor.qti.hardware.bluetooth_audio@2.0-impl.so \
    /vendor/lib64/hw/vendor.qti.hardware.bluetooth_audio@2.1-impl.so; do
    [ -e "${target}" ] || fail "missing stock audio HAL bind target ${target}"
done

for target in \
    /vendor/lib64/gyro_fusion_interface.so \
    /vendor/etc/mixer_paths_cliffs_mtp.xml \
    /vendor/etc/mixer_paths_overlay_static.xml \
    /vendor/etc/mixer_paths_overlay_dynamic.xml \
    /vendor/etc/resourcemanager_cliffs_mtp.xml \
    /vendor/etc/backend_conf.xml \
    /vendor/etc/backend_conf_fs.xml \
    /vendor/etc/usecaseKvManager.xml; do
    [ -e "${target}" ] || fail "missing stock PAL bind target ${target}"
done

find_partition() {
    part="$1"
    for candidate in \
        "/dev/block/mapper/${part}${SLOT}" \
        "/dev/block/mapper/${part}_a" \
        "/dev/block/mapper/${part}_b"; do
        [ -b "${candidate}" ] && {
            echo "${candidate}"
            return 0
        }
    done
    return 1
}

mount_read_only() {
    part="$1"
    target="$2"
    grep -q " ${target} " /proc/mounts && return 0

    source=""
    attempt=0
    while [ -z "${source}" ] && [ "${attempt}" -lt 100 ]; do
        source="$(find_partition "${part}")"
        [ -n "${source}" ] && break
        attempt=$((attempt + 1))
        sleep 0.1
    done
    [ -n "${source}" ] || return 1

    mount -t erofs -o ro "${source}" "${target}" || \
        mount -t ext4 -o ro,noload "${source}" "${target}"
}

bind_file_read_only() {
    source_file="$1"
    target_file="$2"
    [ -f "${source_file}" ] || fail "missing stock file ${source_file}"
    [ -e "${target_file}" ] || fail "missing bind target ${target_file}"
    if ! grep -q " ${target_file} " /proc/mounts; then
        mount -o bind "${source_file}" "${target_file}" || \
            fail "could not bind ${source_file}"
        mount -o remount,bind,ro "${target_file}"
    fi
}

mount_read_only vendor "${STOCK_VENDOR}" || fail "Unable to mount live ROM vendor"
mount_read_only system "${STOCK_SYSTEM}" || fail "Unable to mount live ROM system"
mount_read_only odm "${STOCK_ODM}" || fail "Unable to mount live ROM odm"
LOGMSG "Live ROM vendor/system/odm mounted privately"

for REQUIRED in \
    lib64/libadsprpc.so \
    lib64/libadsp_default_listener.so \
    lib64/vendor.qti.hardware.dsp@1.0.so; do
    [ -e "${STOCK_VENDOR}/${REQUIRED}" ] || fail "Missing stock ${REQUIRED}"
done

for REQUIRED in \
    /system/bin/aera-pd-mapper \
    /vendor/bin/aera-dspservice \
    /vendor/bin/aera-audioadsprpcd; do
    [ -x "${REQUIRED}" ] || fail "Missing ramdisk prebuilt ${REQUIRED}"
done

# Bind mount live vendor libraries without overlaying /vendor itself
if ! grep -q " ${STOCK_LIB64} " /proc/mounts; then
    mount -o bind "${STOCK_VENDOR}/lib64" "${STOCK_LIB64}" || \
        fail "Unable to expose stock vendor lib64"
    mount -o remount,bind,ro "${STOCK_LIB64}"
fi
LOGMSG "Stock vendor lib64 exposed"

SYSTEM_LIB64="${STOCK_SYSTEM}/system/lib64"
[ -d "${SYSTEM_LIB64}" ] || SYSTEM_LIB64="${STOCK_SYSTEM}/lib64"
[ -d "${SYSTEM_LIB64}" ] || fail "Unable to locate stock system lib64"

bind_file_read_only "${SYSTEM_LIB64}/libc++.so" \
    "${AUDIO_ABI}/libc++.so"
bind_file_read_only "${SYSTEM_LIB64}/libbinder.so" \
    "${AUDIO_ABI}/libbinder.so"
bind_file_read_only "${SYSTEM_LIB64}/libbinder_ndk.so" \
    "${AUDIO_ABI}/libbinder_ndk.so"
LOGMSG "Stock system audio ABI exposed"

# Expose only the stock audio HAL pieces that Android's HIDL/libhardware loaders
# expect at fixed /vendor paths. Do not overlay /vendor or /vendor/lib64/hw as a
# whole, because recovery has its own vendor services and graphics stack.
bind_file_read_only "${STOCK_VENDOR}/bin/hw/android.hardware.audio.service" \
    /vendor/bin/hw/android.hardware.audio.service

for lib in \
    vendor.qti.hardware.AGMIPC@1.0-impl.so \
    vendor.qti.hardware.AGMIPC@1.0.so \
    vendor.qti.hardware.pal@1.0-impl.so \
    vendor.qti.hardware.pal@1.0.so; do
    bind_file_read_only "${STOCK_VENDOR}/lib64/${lib}" "/vendor/lib64/${lib}"
done

for module in \
    android.hardware.audio@7.1-impl.so \
    android.hardware.audio.effect@7.0-impl.so \
    audio.primary.pineapple.so \
    audio.r_submix.default.so \
    audio.usb.default.so \
    android.hardware.bluetooth.audio-impl-qti.so \
    vendor.qti.hardware.bluetooth_audio@2.0-impl.so \
    vendor.qti.hardware.bluetooth_audio@2.1-impl.so; do
    bind_file_read_only "${STOCK_VENDOR}/lib64/hw/${module}" "/vendor/lib64/hw/${module}"
done
LOGMSG "Stock Qualcomm audio HAL runtime exposed"

# audio.primary.pineapple.so expects this Xiaomi sensor/audio helper through the
# normal /vendor/lib64 search path.
bind_file_read_only \
    "${STOCK_VENDOR}/lib64/hw/gyro_fusion_interface.so" \
    /vendor/lib64/gyro_fusion_interface.so
LOGMSG "Stock gyro fusion audio dependency exposed"

# Xiaomi's Awinic audio extension loads calibration support using the absolute
# /odm/lib64/libawcal.so path, so expose the active slot's complete ODM tree.
mkdir -p /odm
if ! grep -q " /odm " /proc/mounts; then
    mount -o bind "${STOCK_ODM}" /odm || \
        fail "Unable to expose stock ODM root"
    mount -o remount,bind,ro /odm
fi
LOGMSG "Stock ODM root exposed"

# PAL looks for the Peridot cliffs configuration at fixed /vendor/etc paths.
CLIFFS_AUDIO="${STOCK_ODM}/etc/audio/sku_cliffs"

for config in \
    mixer_paths_cliffs_mtp.xml \
    mixer_paths_overlay_static.xml \
    mixer_paths_overlay_dynamic.xml \
    resourcemanager_cliffs_mtp.xml; do
    bind_file_read_only \
        "${CLIFFS_AUDIO}/${config}" \
        "/vendor/etc/${config}"
done

for config in \
    backend_conf.xml \
    backend_conf_fs.xml \
    usecaseKvManager.xml; do
    bind_file_read_only \
        "${STOCK_ODM}/etc/${config}" \
        "/vendor/etc/${config}"
done

LOGMSG "Stock Peridot PAL routing configuration exposed"

mkdir -p /firmware

if ! grep -q " /firmware " /proc/mounts; then
    MODEM_DEV="/dev/block/bootdevice/by-name/modem${SLOT}"
    [ -b "${MODEM_DEV}" ] || MODEM_DEV="/dev/block/by-name/modem${SLOT}"
    [ -b "${MODEM_DEV}" ] || fail "No active modem firmware partition"

    mount -t vfat \
    -o ro,uid=0,gid=0,fmask=0133,dmask=0022 \
    "${MODEM_DEV}" /firmware || \
        fail "Unable to remount modem firmware"
fi
if ! grep -q " /vendor/firmware_mnt " /proc/mounts; then
    mount -o bind /firmware /vendor/firmware_mnt || \
        fail "Unable to expose stock firmware_mnt"
fi

# peridot has a logical DSP partition
if ! grep -q " /vendor/dsp " /proc/mounts; then
    DSP_DEV="/dev/block/bootdevice/by-name/dsp${SLOT}"
    [ -b "${DSP_DEV}" ] || DSP_DEV="/dev/block/by-name/dsp${SLOT}"
    [ -b "${DSP_DEV}" ] || fail "No active DSP partition"
    mount -t ext4 -o ro,noload "${DSP_DEV}" /vendor/dsp || \
        fail "Unable to mount DSP partition"
fi
if ! grep -q " /dsp " /proc/mounts; then
    mount -o bind /vendor/dsp /dsp || fail "Unable to expose legacy /dsp path"
fi
LOGMSG "Stock DSP files exposed"

if [ -d "${STOCK_VENDOR}/lib/rfsa/adsp" ] && \
   ! grep -q " /vendor/lib/rfsa/adsp " /proc/mounts; then
    mount -o bind "${STOCK_VENDOR}/lib/rfsa/adsp" /vendor/lib/rfsa/adsp || \
        fail "Unable to expose stock RFSA ADSP directory"
    mount -o remount,bind,ro /vendor/lib/rfsa/adsp
fi

# Expose the active ROM ODM configuration expected by PAL
if ! grep -q " /vendor/odm/etc " /proc/mounts; then
    mount -o bind "${STOCK_ODM}/etc" /vendor/odm/etc || \
        fail "Unable to expose stock ODM etc"
    mount -o remount,bind,ro /vendor/odm/etc
fi

for config_dir in audio acdbdata; do
    source_dir="${STOCK_VENDOR}/etc/${config_dir}"
    target_dir="/vendor/etc/${config_dir}"

    [ -d "${source_dir}" ] || continue

    if ! grep -q " ${target_dir} " /proc/mounts; then
        mount -o bind "${source_dir}" "${target_dir}" || \
            fail "Unable to expose stock vendor ${config_dir}"
        mount -o remount,bind,ro "${target_dir}"
    fi
done

for config in card-defs.xml kvh2xml.xml; do
    source_config="${STOCK_VENDOR}/etc/${config}"
    target_config="/vendor/etc/${config}"

    if [ -f "${source_config}" ] && [ -f "${target_config}" ]; then
        bind_file_read_only "${source_config}" "${target_config}"
    fi
done

source_policy="${STOCK_VENDOR}/etc/seccomp_policy/vendor.qti.hardware.dsp.policy"
target_policy="/vendor/etc/seccomp_policy/vendor.qti.hardware.dsp.policy"

if [ -f "${source_policy}" ] && [ -f "${target_policy}" ]; then
    bind_file_read_only "${source_policy}" "${target_policy}"
fi

start_init_service() {
    service="$1"

    if [ "$(getprop "init.svc.${service}")" != "running" ]; then
        setprop ctl.start "${service}"

        attempt=0
        while [ "$(getprop "init.svc.${service}")" != "running" ] && \
              [ "${attempt}" -lt 50 ]; do
            attempt=$((attempt + 1))
            sleep 0.1
        done
    fi

    [ "$(getprop "init.svc.${service}")" = "running" ] || \
        fail "init service ${service} failed to start"

    # Sanity check that service is still running
    sleep 0.5

    [ "$(getprop "init.svc.${service}")" = "running" ] || \
        fail "init service ${service} exited after starting"

    LOGMSG "${service} confirmed running"
}

LOGMSG "Starting pd-mapper"
start_init_service aera-pd-mapper

LOGMSG "Starting dspservice"
start_init_service aera-dspservice

LOGMSG "Starting AudioPD daemon"
start_init_service aera-audioadsprpcd

LOGMSG "Audio userspace ready: pd=$(getprop init.svc.aera-pd-mapper) dsp=$(getprop init.svc.aera-dspservice) audiopd=$(getprop init.svc.aera-audioadsprpcd)"

LOGMSG "Booting ADSP with audio userspace ready"
[ -e /sys/kernel/boot_adsp/boot ] || fail "ADSP boot node missing"
echo 1 > /sys/kernel/boot_adsp/boot || fail "Unable to boot ADSP"

LOGMSG "Waiting for machine sound card"

attempt=0
while [ ! -e /dev/snd/controlC0 ] && [ "${attempt}" -lt 150 ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done

LOGMSG "Post-ADSP service states: pd=$(getprop init.svc.aera-pd-mapper) dsp=$(getprop init.svc.aera-dspservice) audiopd=$(getprop init.svc.aera-audioadsprpcd)"

if [ -e /dev/snd/controlC0 ]; then
    LOGMSG "Machine sound card is present after ${attempt} checks"

    LOGMSG "Starting stock Qualcomm audio HAL for AGMIPC/PAL"
    start_init_service aera-stock-audio-hal
    LOGMSG "Stock Qualcomm audio HAL is running"

    # Android normally opens the primary audio module from AudioFlinger.
    # Recovery has no AudioFlinger, so explicitly open the primary device to
    # force audio.primary.pineapple.so and the Qualcomm PAL/AGM stack to load.
    #
    # If this bootstrap is called again by the browser audio bridge, do not
    # restart an opener that is already holding a successfully opened
    # IPrimaryDevice.
    if [ "$(getprop init.svc.aera-audio-primary-opener)" = "running" ]; then
        LOGMSG "Primary Qualcomm audio HAL opener already running"
    else
        rm -f /tmp/aera-audio-primary-opened

        LOGMSG "Opening stock primary Qualcomm audio HAL"
        start_init_service aera-audio-primary-opener

        primary_attempt=0
        while [ ! -e /tmp/aera-audio-primary-opened ] && \
              [ "$(getprop init.svc.aera-audio-primary-opener)" = "running" ] && \
              [ "${primary_attempt}" -lt 100 ]; do
            primary_attempt=$((primary_attempt + 1))
            sleep 0.1
        done

        [ -e /tmp/aera-audio-primary-opened ] || \
            fail "Primary Qualcomm audio HAL did not open within 10 seconds"
    fi

    LOGMSG "Primary Qualcomm audio HAL opened"
else
    LOGMSG "Machine sound card still absent after 15 seconds"
fi

LOGMSG "Bootstrap complete"

exit 0
