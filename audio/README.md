# vivo audio stack for fuxi

This makes vivos audio features work on the MIUI 14 vendor:
sound quality / EQ / headphone effects ("Sound customization"), 
Audio Super Resolution, and vivo's per-ringtone haptic patterns.
The audio HAL is **not** replaced.

## 1. vendor
all libs are in rom/

and add prop, in `vendor/build.prop`:

```
persist.vivo.audio.feature.vafx_eq.enable=true
```
`vafxvcp` (in-call voice processing) needs the vcode chain above; it is ~10 MB
and is the only piece that touches call audio, so add it last and test calls.

Left out on purpose: they load fine, they are just not needed:
`libawinicSKTune.so` (AWINIC amp tuning, fuxi has Cirrus amps),
`libvafx_spkr_sfx.so` (iQOO speaker tables, only Super Audio drives it).

## 2. dtbo
Both tools are in `lra/tools/`; run them in order on the stock dtbo:
```sh
python3 lra/tools/patch_dtbo.py  <stock dtbo.img> <out.img>
python3 lra/tools/patch_rtp_ringtones.py <out.img> -o <final dtbo.img>
```

## 3. Verify
```sh
# effects are live
adb shell dumpsys media.audio_flinger | grep -A1 'Effect Chains'
#   -> "- name: VAFX EQ", and "VAFX RESTORATION" while media plays

# EQ present in the UI (gated on the prop)
adb shell am start -n com.vivo.audiofx/.soundquality.activity.SoundQualityActivity
#   -> "Equalizer" row

# ringtone haptics use vivo's real waveform
adb shell dmesg | grep -E 'convert rtp file|not supported'
#   -> "convert rtp file [A_<Ringtone>.bin][...]" and no "not supported"
```
