# vivo thermals for Xiaomi 13 🐥

ONLY DO THIS IF THE BASE HAS SAME SOC!

## Files needed:
/vendor/bin/thermal-engine-v2                (from base)
/vendor/etc/init/init_thermal-engine-v2.rc   (from base)
/vendor_dlkm/lib/modules/pem_driver.ko       (from base)
/vendor_dlkm/lib/modules/vivo-tshell.ko      (from base)
/vendor_dlkm/lib/modules/hall_sensor.ko      (from base)

Add to: /vendor_dlkm/lib/modules/modules.load:
pem_driver.ko
vivo-tshell.ko
hall_sensor.ko

Add to: /vendor_dlkm/lib/modules/modules.dep:
/vendor/lib/modules/pem_driver.ko:
/vendor/lib/modules/hall_sensor.ko:
/vendor/lib/modules/vivo-tshell.ko: /vendor/lib/modules/hall_sensor.ko

Delete /vendor/etc/init/hw/init.mi_thermald.rc

Delete /vendor_dlkm/lib/modules/mi_thermal_interface.ko
And its enteries from modules.dep and modules.load

## DTBO edit:
To sucessfully load vivos tshell (temperature of phone shell and other things)
you need to add its definitions from your base dtbo to yours and modify targeted sensors.
🐥
