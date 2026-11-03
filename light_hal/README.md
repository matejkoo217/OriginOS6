# Standalone light sub-HAL (`sensors.light.so`)

it implements the AOSP multi-HAL **2.1 sub-HAL ABI** and talks to the sensors DSP directly through `libssc`.

## Build
```bash
./build.sh
```

## Testing
```bash
adb shell su -c 'logcat -d -s sensors-light'        # engine + sub-HAL
adb shell su -c 'logcat -d -s SensorsMultiHal'      # HalProxy sub-HAL loader
```

## Brightness calibration
`src/lux_table.inc` is the log10-interpolated count→lux table, converted from
the MIUI plateaus through vivo's `_V_Sensor_CurveSwitch` curve.

```bash
setprop persist.vendor.light_hal.scale_mul 1.0    # table gain (default 1.0 = MIUI 1:1)
setprop persist.vendor.light_hal.lux_override -1  # test hook; MUST stay -1
```
Increase `scale_mul` if auto-brightness is too dim, decrease it if too bright.
