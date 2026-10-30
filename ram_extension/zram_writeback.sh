#!/system/bin/sh
# SPDX-License-Identifier: MIT
INTERVAL=${ZRAM_WB_INTERVAL:-7200}
log() { echo "zram_writeback: $*" > /dev/kmsg; }
i=0
while [ $i -lt 120 ]; do
    if [ -f /sys/block/zram0/writeback ] && \
       [ "$(cat /sys/block/zram0/backing_dev 2>/dev/null)" != "none" ]; then
        break
    fi
    sleep 5
    i=$((i + 1))
done
if [ "$(cat /sys/block/zram0/backing_dev 2>/dev/null)" = "none" ]; then
    log "no backing device, not starting"
    exit 0
fi
log "started, interval=${INTERVAL}s"
while true; do
    sleep "$INTERVAL"
    echo idle > /sys/block/zram0/writeback 2>/dev/null
    echo all > /sys/block/zram0/idle 2>/dev/null
done
