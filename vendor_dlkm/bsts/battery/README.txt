In order to get it to work we need additional steps:

/vendor/bin/hw/android.hardware.health-service.qti
move it to > /vendor/bin/hw/android.hardware.health-service.qti.real

and modify /vendor/etc/init/android.hardware.health-service.qti.rc
service vendor.charger /vendor/bin/hw/android.hardware.health-service.qti.real --charger

and then add /vendor/bin/hw/android.hardware.health-service.qti
from your base rom. (i used IQOO11 base).