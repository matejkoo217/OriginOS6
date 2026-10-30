The vivo driver reads its scene/effect tables from its own device tree node
(`vhc_parse_dt`). Without them every upload fails with
`vhc_haptics_load_periodic_effect: scene:N not support` and nothing moves.

The data comes from iQOO 11's decompiled dtbo

## Target node
```
/soc/qcom,spmi@c42d000/qcom,pm8550b@7/qcom,hv-haptics@f000
```
(fuxi's dtbo has **32 entries**; all of them carry a full copy of this node = patch every entry, the bootloader picks one by board/panel id.)

## Properties to insert (indentation: 5 tabs)
```
lra_info = <0x257b>;
vib_num = <0x01>;
is_major_motor;
richtap_type = <0x01>;
resistance_min = <0x1388>;
resistance_max = <0x4e20>;
freq_min = <0x640>;
freq_max = <0x73a>;
scene_array = <&lra_scene_array>;
ram_effects = <&qti_ram_effects>;
rtp_effects = <&qti_minor_rtp_effects>;
```

Change `qcom,vmax-mv` to your device one! (1200 mV on fuxi) and `qcom,lra-period-us`.

## Child nodes (insert before the node's closing brace)
```
lra_scene_array: lra_scene_array {
    phandle = <0x1e0>;          ← any value above the tree's max phandle
    <contents of blk_scene.txt>
};

qti_ram_effects: qti_ram_effects {
    phandle = <0x1e1>;
    <contents of blk_ram.txt>
};

qti_minor_rtp_effects: qti_minor_rtp_effects {
    phandle = <0x1e2>;
    <contents of blk_rtp.txt>
};
```

## Clamp — per entry
The copied effects carry `qcom,wf-vmax-mv` up to **9000 mV**! fuxi's own effects
use at most its own value, which differs per board entry:
| entries | node `qcom,vmax-mv` | effect `wf-vmax-mv` | rule |
|---|---|---|---|
| 0, 29 | 0x4b0 (1200) | 0xe10 (3600) | clamp to 0xe10 |
| 17–19, 23–26 | 0x514 | **0x708 (1800)** | clamp to **0x708** |
| 27 | 0x672 | 0xe10 | clamp to 0xe10 |
| rest | 0xe10 | 0xe10 | clamp to 0xe10 |

i.e. for each entry, `cap = max(qcom,wf-vmax-mv over its effect_0..5)`, then
rewrite every inserted `qcom,wf-vmax-mv` above `cap` to `cap`.
(`blk_ram.txt` / `blk_rtp.txt` are already clamped to 3600; entries with the
1800 mV cap need one more pass.)

The bootloader **renumbers every phandle** when it merges the dtbo. It only
rewrites references it knows about, and dtc only records those it generated
itself. Our three properties are not phandle-typed as far as dtc is concerned,
so without this they keep pointing at `0x1e0` while the node moves to `0x76c`:
```
Get base effect elements count failed, ret=-22
```
Add the three entries next to the existing `nvmem` fixup in **every entry**
(the fragment index differs — 2 or 3):
```
__local_fixups__ {
  fragment@N { __overlay__ { qcom,pm8550b@7 {
    qcom,hv-haptics@f000 {
        nvmem = <0x00>;          ← pre-existing
        scene_array = <0x00>;    ← add
        ram_effects = <0x00>;    ← add
        rtp_effects = <0x00>;    ← add
    };
  }; }; };
}
```
