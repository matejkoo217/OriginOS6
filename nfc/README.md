For xiaomi 13, probably wont work for you but you can try :D


1. Install the vivo HAL and its libraries into `/vendor` (see payload below).
2. Install vivo NFC configs (vendor/etc, vendor root and odm/etc).
3. Add an init service `vendor.nfc_hal_service` that starts the AIDL HAL.
4. Declare the AIDL services in the vendor VINTF manifest
   (`android.hardware.nfc.INfc/default`, `vendor.nxp.nxpnfc_aidl.INxpNfc/default`).
   Without the VINTF declaration `ServiceManager_addService` fails
   (`Check failed: status == STATUS_OK`) because servicemanager enforces VINTF.
5. Disable the Xiaomi HIDL HALs:
   - delete `/vendor/etc/init/android.hardware.nfc@1.2-service-st.rc`
   - delete `/odm/etc/init/vendor.nxp.hardware.nfc@2.0-service.rc`
   - delete `/odm/etc/permissions/com.android.nfc_extras.xml`
6. Remove the HIDL NFC declarations from `/vendor/etc/vintf/manifest_kalama.xml`
   (`android.hardware.nfc` @1.2 and `vendor.nxp.hardware.nfc` @2.0).
   Otherwise hwservicemanager keeps lazy-starting the disabled HAL and
   `vendor.qti.esepowermanager@1.1-service` retries `INfc@1.2::getService()`
   every second forever.


Verification:
adb shell "dumpsys nfc | head -3"                   # expect mState=on
adb shell "service list | grep nfc"                 # expect android.hardware.nfc.INfc/default and
adb shell "logcat -d | grep -E 'NxpNci[XR]' | tail" # vendor.nxp.nxpnfc_aidl.INxpNfc/default


Expected: HAL open + full config + RF settings blocks all ACKed with
`4002020000`, then RF discovery (`21060100`) runs. No stuck `i2c_read`, no
`ctl.interface_start` retry spam for `android.hardware.nfc@1.2`.
