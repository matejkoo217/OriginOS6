# 🐥 :O
The vivo driver reads its scene/effect tables from its own device tree node
(`vhc_parse_dt`). Without them every upload fails with
`vhc_haptics_load_periodic_effect: scene:N not support` and nothing moves.

## Images in this folder
| file | what it is |
|---|---|
| `fuxi_dtbo.img` | vivo props + the three tables from iQOO 11, the whole waveform table from iQOO 13, and the sound-vibration ladder scenes 89/90. 32 entries, 1119 named waveform blocks. |
| `iqoo11-dtbo.dts` | iQOO 11 decompiled dtbo entry, base source: props, `lra_scene_array` / `qti_ram_effects` / `qti_minor_rtp_effects`, vmax envelope |
| `iqoo13-dtbo.dts` | iQOO 13 decompiled dtbo entry: full `qti_rtp_effects` table, 1069 waveform blocks |

## Regenerate it:
```
tools/patch_dtbo.py dtbo-orig.img dtbo-patched.img \
    --iqoo-dts  files/dtbo/iqoo11-dtbo.dts \
    --iqoo13-dts files/dtbo/iqoo13-dtbo.dts \
    --firmware  files/vendor/firmware
```

* **iQOO 11 dts:** props, `lra_scene_array` / `qti_ram_effects` /
  `qti_minor_rtp_effects`, vmax envelope.
* **iQOO 13 dts:** the complete per-family waveform table (`R_` ringtones,
  `N_` notifications, `A_` alarms, `T_`, `S_`, `G_`); iQOO 11's DT names only
  six ringtones.  `--firmware` drops blocks whose `.bin` is missing.
* The two missing sound-vibration ladder scenes (89/90) are filled in
  automatically (`--no-scene-fill` to skip).

## Firmware blobs
The `.bin` payloads themselves come from the **iQOO 11** firmware dump and must
sit in `vendor/firmware/`: `A_*` 18, `R_*` 48, `N_*` 48, `S_*` 14, `T_*` 2,
`G_*` 1112.

## Notes
* **Per-entry clamp.** fuxi's own effect vmax is `0xe10` (3600 mV) on most
  entries and `0x708` (1800 mV) on the `qcom,vmax-mv = 0x514` ones (17–19,
  23–26).  The tool clamps every inserted `qcom,wf-vmax-mv` **and** every
  `base_scene` / `ext_scene` vmax to that entry's own maximum. **iQOO ships
  8000/9000 mV!**
* **`__local_fixups__`.** The three properties must be listed next to the
  existing `nvmem` fixup in every entry, or the bootloader's phandle renumbering
  leaves them stale (`Get base effect elements count failed, ret=-22`).  The
  tool adds them.
