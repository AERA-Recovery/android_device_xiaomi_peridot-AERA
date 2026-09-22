#!/system/bin/sh

SCRIPT_NAME="$(basename "$0")"

LOGMSG() {
    echo "I:$@" >> /tmp/recovery.log
}

LOGMSG "---$SCRIPT_NAME start---"

MARKER="/persist/AERA/.format_cleanup_marker"

mkdir -p /persist/AERA
touch "$MARKER"

LOGMSG "Post-format cleanup marker set"

LOGMSG "---$SCRIPT_NAME end---"
