# LRA / haptics — vivo (iQOO 11) haptics on Xiaomi 13 (fuxi)

For fuxi, MIUI 14 vendor/odm + OriginOS 6 (PD2243 system). Replaces MIUI's
haptics driver and effect engine with vivo's, so the phone runs vivo's
RichTap/scene effects on fuxi's own PM8550B LRA.

## Step 1 dlkm
Copy from `files/vendor_dlkm/`:

```
lib/modules/vivo_haptic_core.ko
lib/modules/vivo-hv-haptics.ko       (patched, see below)
```

Also add the snippets in `files/vendor_dlkm/`

## Step 2 — dtbo
Read `files/dtbo/README.md`.

## Step 3 — vendor + odm
vendor:
```
lib64/vendor.aac.hardware.vibrator.custom.impl.so
lib64/libVibratorTool.so
lib64/vendor.aac.hardware.richtap.vibrator-V3-ndk.so
firmware/*.bin
```

odm:
```
bin/hw/vendor.xiaomi.hardware.vibratorfeature.service      ← vivo's HAL binary
etc/init/vendor.xiaomi.hardware.vibratorfeature.service.rc ← files/odm/etc/init/… (has RICHTAP_* env + /dev perms)
etc/vintf/manifest/…vibratorfeature.service.xml            ← files/odm/etc/vintf/manifest/… 
etc/aac_richtap.config, etc/aac_richtap_1.config
```

## Step 4 — props
odm:
```
ro.vivo.lra.touchType=1
```
vendor:
```
persist.vivo.support.lra=1
persist.sys.vivo.support.double.lra=0
```

you can skip the vendor ones if the system build.prop already provides them

For the ringtone haptics you need to use vivos audio stack, guide on doing so is in audio folder.
