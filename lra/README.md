# LRA / Vivo haptics iQOO 11 (+ iQOO 13) haptics on Xiaomi 13 (fuxi)

For fuxi, MIUI 14 vendor/odm + OriginOS 6 (PD2243 system). Replaces MIUI's
haptics driver and effect engine with vivo's, so the phone runs vivo's
RichTap/scene effects on fuxi's own PM8550B LRA.

## Step 1: vendor_dlkm
Copy from `files/vendor_dlkm/`:

```
lib/modules/vivo_haptic_core.ko
lib/modules/vivo-hv-haptics.ko       (patched, see below)
```

Also add the snippets in `files/vendor_dlkm/`

## Step 2: dtbo
```
tools/patch_dtbo.py <stock-dtbo.img> <out-dtbo.img> \
    --iqoo-dts <iQOO 11 decompiled dtbo dts> \
    --iqoo13-dts <iQOO 13 decompiled dtbo dts>
```

One pass does everything:
* vivo props + the three child tables (`scene_array` / `ram` / `rtp`) and the
  vmax envelope - from the **iQOO 11** dts;
* the whole per-family waveform table (`R_` ringtones, `N_` notifications,
  `A_` alarms, `T_`, `S_`, `G_`) - from the **iQOO 13** dts, because iQOO 11's
  DT only names six ringtones; each block is clamped to fuxi's own envelope;
* the two missing sound-vibration ladder scenes (89 / 90).

## Step 3: vendor + odm 
vendor:
```
lib64/vendor.aac.hardware.vibrator.custom.impl.so
lib64/libVibratorTool.so
lib64/vendor.aac.hardware.richtap.vibrator-V3-ndk.so
firmware/*.bin              waveform blobs for every family (A_ R_ N_ S_ T_ G_),
                            taken from the iQOO 11 firmware dump; the id → name
                            mapping for the ringtones/notifications comes from
                            the iQOO 13 table used in Step 2
```

odm:
```
bin/hw/vendor.xiaomi.hardware.vibratorfeature.service      ← vivo's HAL binary
etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc ← files/odm/etc/init/… (has RICHTAP_* env + /dev perms)
etc/vintf/manifest/…vibratorfeature.service.xml            ← files/odm/etc/vintf/manifest/… 
etc/aac_richtap.config, etc/aac_richtap_1.config
```

## Step 4: Props
odm:
```
ro.vivo.lra.touchType=1
```
vendor:
```
persist.vivo.support.lra=1
persist.sys.vivo.support.double.lra=0
```

you can skip the vendor ones if the system build.prop already provides them.

For the ringtone haptics you need to use vivos audio stack, guide on doing so is in audio folder.
# 🐥
