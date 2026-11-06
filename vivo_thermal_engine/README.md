# vivo thermals — Xiaomi 13

ONLY DO THIS IF THE BASE HAS SAME SOC!

## Files needed:
/vendor/bin/thermal-engine-v2                (from base)
/vendor/etc/init/init_thermal-engine-v2.rc   (from base)
/vendor_dlkm/lib/modules/pem_driver          (from base)

Add to: /vendor_dlkm/lib/modules/modules.load:
pem_driver.ko

Add to: /vendor_dlkm/lib/modules/modules.dep:
/vendor/lib/modules/pem_driver.ko:


Delete /vendor/etc/init/hw/init.mi_thermald.rc

Delete /vendor_dlkm/lib/modules/mi_thermal_interface.ko
And its enteries from modules.dep and modules.load

