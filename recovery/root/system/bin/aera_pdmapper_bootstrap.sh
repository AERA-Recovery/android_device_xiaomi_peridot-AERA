#!/system/bin/sh

PATH=/system/bin:/sbin
export PATH
umask 022
exec >> /tmp/aera-pdmapper-boot.log 2>&1

TAG="[aera-pdmapper-boot]"
RESULT=/tmp/aera-pdmapper-boot.result
LOCK=/mnt/aera-stock/.audio-lock
MAPPER=/system/bin/aera-pd-mapper
LIBDIR=/system/lib64/aera-pdmapper
HAVE_LOCK=0
STARTED_MAPPER=0
ADSP_REQUESTED=0
READY_PROP=vendor.aera.pdmapper.ready

log() {
    echo "${TAG} $*"
    echo "${TAG} $*" >> /dev/kmsg
}
fail() {
    log "ERROR: $*"
    echo "FAILED: $*" > "${RESULT}"
    exit 1
}
skip() {
    log "SKIP: $*"
    echo "SKIPPED: $*" > "${RESULT}"
    exit 0
}
cleanup() {
    [ "${HAVE_LOCK}" = 0 ] || setprop "${READY_PROP}" 0
    # Do not stop an existing mapper or deliberately remove one after ADSP boot
    if [ "${STARTED_MAPPER}" = 1 ] && [ "${ADSP_REQUESTED}" = 0 ]; then
        setprop ctl.stop aera-pd-mapper
    fi
    [ "${HAVE_LOCK}" = 0 ] || rmdir "${LOCK}"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

echo STARTING > "${RESULT}"
log "Startup request; uptime=$(cat /proc/uptime)"
case "$(getprop ro.aera.fastbootd):$(getprop sys.usb.config.aera_fastbootd)" in
    1:*|true:*|*:1) skip "fastbootd" ;;
esac
[ "$(getprop twrp.modules.loaded)" = true ] || skip "modules not ready"
[ "$(getprop post.decrypt.modules)" != true ] || skip "post-decrypt setup owns this session"
SLOT="$(getprop ro.boot.slot_suffix)"
case "${SLOT}" in _a|_b) ;; *) fail "invalid active slot: ${SLOT}" ;; esac

mkdir -p /mnt/aera-stock || fail "cannot create lock parent"
attempt=0
while ! mkdir "${LOCK}"; do
    [ "${attempt}" -lt 100 ] || fail "audio setup is still holding the lock"
    attempt=$((attempt + 1))
    sleep 0.1
done
HAVE_LOCK=1
setprop "${READY_PROP}" 0 || fail "cannot reset mapper handoff"
[ "$(getprop post.decrypt.modules)" != true ] || skip "post-decrypt setup started while waiting"

# Wait briefly for existing mounts to go away - do not detach them ourselves
rom_view_present() {
    awk '$2 == "/vendor" || $2 == "/odm" || $2 == "/system" { found=1 } END { exit !found }' /proc/mounts
}
check_recovery_view() {
    if rom_view_present; then
        fail "a temporary ROM mount still covers /vendor, /odm or /system"
    fi
}
attempt=0
while rom_view_present; do
    [ "$(getprop post.decrypt.modules)" != true ] || skip "post-decrypt setup now owns this session"
    [ "${attempt}" -lt 50 ] || fail "temporary ROM mounts did not clear within 5 seconds"
    attempt=$((attempt + 1))
    sleep 0.1
done
check_recovery_view
[ -x "${MAPPER}" ] || fail "missing ramdisk mapper"
for lib in libc++.so libjson.so libbase.so libcutils.so libutils.so \
           libqmi_cci.so libqmi_common_so.so libqmi_csi.so libqmi_encdec.so; do
    [ -r "${LIBDIR}/${lib}" ] || fail "missing private library ${lib}"
done
[ -w /sys/kernel/boot_adsp/boot ] || fail "ADSP boot node unavailable"

MODEM=/dev/block/bootdevice/by-name/modem${SLOT}
[ -b "${MODEM}" ] || MODEM=/dev/block/by-name/modem${SLOT}
[ -b "${MODEM}" ] || fail "active-slot modem partition unavailable"
MODEM_REAL="$(readlink -f "${MODEM}")"
mount_count() { awk -v target="$1" '$2 == target { n++ } END { print n+0 }' /proc/mounts; }
check_firmware_mount() {
    [ "$(mount_count "$1")" = 1 ] || fail "missing or stacked firmware mount: $1"
    src="$(awk -v target="$1" '$2 == target { print $1 }' /proc/mounts)"
    [ "$(readlink -f "${src}")" = "${MODEM_REAL}" ] || fail "firmware mount is not from the active slot: $1"
    awk -v target="$1" '$2 == target && $3 == "vfat" && $4 ~ /(^|,)ro(,|$)/ { ok=1 } END { exit !ok }' /proc/mounts || \
        fail "firmware mount is not read-only vfat: $1"
}
mkdir -p /firmware || fail "cannot create /firmware"
if [ "$(mount_count /firmware)" = 0 ]; then
    mount -t vfat -o ro,uid=0,gid=0,fmask=0133,dmask=0022 "${MODEM}" /firmware || \
        fail "cannot mount current-slot firmware"
fi
check_firmware_mount /firmware
check_recovery_view
[ -d /vendor/firmware_mnt ] || fail "missing ramdisk firmware target"
if [ "$(mount_count /vendor/firmware_mnt)" = 0 ]; then
    mount -o bind /firmware /vendor/firmware_mnt || fail "cannot expose mapper firmware path"
    mount -o remount,bind,ro /vendor/firmware_mnt || fail "cannot protect firmware alias"
fi
check_firmware_mount /vendor/firmware_mnt
locator_count=0
for locator in /vendor/firmware_mnt/image/*.jsn; do
    [ ! -r "${locator}" ] || locator_count=$((locator_count + 1))
done
[ "${locator_count}" -gt 0 ] || fail "no readable service-locator files"
log "Current-slot firmware ready; locator files=${locator_count}"

if [ "$(getprop init.svc.aera-pd-mapper)" != running ]; then
    STARTED_MAPPER=1
    setprop ctl.start aera-pd-mapper || fail "init rejected mapper start"
fi
attempt=0
while [ "$(getprop init.svc.aera-pd-mapper)" != running ]; do
    [ "${attempt}" -lt 50 ] || fail "mapper did not start"
    attempt=$((attempt + 1))
    sleep 0.1
done
mapper_pid() {
    value="$(getprop init.svc_debug_pid.aera-pd-mapper)"
    case "${value}" in ''|*[!0-9]*) value="$(pidof aera-pd-mapper)" ;; esac
    case "${value}" in ''|*[!0-9]*) return 1 ;; esac
    echo "${value}"
}
PID="$(mapper_pid)" || fail "cannot identify one mapper process"
sleep 0.5
[ "$(getprop init.svc.aera-pd-mapper)" = running ] || fail "mapper exited during startup"
[ "$(mapper_pid)" = "${PID}" ] || fail "mapper restarted during startup"
[ "$(readlink /proc/${PID}/exe)" = "${MAPPER}" ] || fail "unexpected mapper executable"

# Check actual loaded files, not only the requested library search path
awk -v dir="${LIBDIR}" '
BEGIN {
    n=split("libc++.so libjson.so libbase.so libcutils.so libutils.so libqmi_cci.so libqmi_common_so.so libqmi_csi.so libqmi_encdec.so", libs, " ")
    for (i=1; i<=n; i++) wanted[dir "/" libs[i]]=1
}
$6 ~ /^\// && $6 !~ /^\/dev\// {
    if ($6 !~ /^\/system\// || $4 !~ /^0+:/ || $7 == "(deleted)") {
        print "Unexpected mapper mapping: " $0; bad=1
    }
    if ($6 in wanted) seen[$6]=1
}
END {
    for (p in wanted) if (!(p in seen)) { print "Private library not mapped: " p; bad=1 }
    exit bad
}' /proc/${PID}/maps || fail "mapper runtime is not wholly recovery-backed"
log "Private-library mapper verified; pid=${PID}"

ADSP=""
for candidate in /sys/class/remoteproc/remoteproc*; do
    [ -r "${candidate}/name" ] || continue
    case "$(cat "${candidate}/name")" in
        *remoteproc-adsp) [ -z "${ADSP}" ] || fail "multiple ADSP devices"; ADSP="${candidate}" ;;
    esac
done
[ -n "${ADSP}" ] || fail "cannot identify ADSP"
check_recovery_view
case "$(cat "${ADSP}/state")" in
    offline)
        # Keep mapper available even if the write fails after starting preparation
        ADSP_REQUESTED=1
        log "Mapper checks passed; handing the ADSP request to init"
        setprop "${READY_PROP}" 1 || fail "cannot publish mapper handoff"
        ;;
    running)
        ADSP_REQUESTED=1
        log "ADSP already running; no restart requested"
        ;;
    *) fail "ADSP is neither offline nor running" ;;
esac

attempt=0
while [ "${attempt}" -lt 100 ]; do
    capacity=""
    [ ! -r /sys/class/power_supply/battery/capacity ] || capacity="$(cat /sys/class/power_supply/battery/capacity)"
    case "${capacity}" in
        ''|*[!0-9]*) ;;
        *) [ "${capacity}" -gt 100 ] || { [ ! -e /dev/snd/controlC0 ] || break; } ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.1
done
[ "${attempt}" -lt 100 ] || fail "battery or sound card did not appear within 10 seconds"
[ "$(getprop init.svc.aera-pd-mapper)" = running ] || fail "mapper stopped after ADSP boot"
[ "$(mapper_pid)" = "${PID}" ] || fail "mapper restarted after ADSP boot"
log "Battery ready: capacity=${capacity}; sound card present; mapper pid=${PID}"
echo "READY: capacity=${capacity}; mapper_pid=${PID}" > "${RESULT}"
