# lcm_bridge - /sys/lcm{,1} and /sys/fingerprint for the vivo FOD/display stack

## Nodes that are real

| vivo node | what happens |
|---|---|
| `oled_hbm` (24/25/26/27/`0x20000000|level`) | → mi_disp `disp_param` feature 1 (hbm) |
| `aod_bl_level`, `aod_disp_info` partial flags | → mi_disp feature 3 (doze brightness) |
| **`aod_disp_info` `support_full_screen`** | selects SystemUI's AOD transition path, which decides the AOD refresh rate: `1` → FixRate "6 pulse" at **120 Hz** (fuxi has no `0x2000` config, so AOD idles at 120), `3` → LH30 path (only writes `full_aod_on`, kernel is expected to switch the panel — the winning request drops to 60 but RRM keeps the last config in doze), **`4` → LTPS path, acquires "LtpsFullAodTransition to switch 60hz", so AOD actually runs at 60 Hz**. `support_1hz=0` only says the panel has no 1 Hz mode |
| `refresh_monitor` | mirrors `dynamic_fps` (`120`) |
| `lcm_fps` | → `fps=<n>Hz` from `dynamic_fps` |
| **`hbm_max_brightness`** | **read from `panel0-backlight/max_brightness` (4095)**; a userspace write overrides it |
| **`bl_level`** | **forwarded to `panel0-backlight/brightness`** (12-bit, same scale as vivo's). `0` is deliberately *not* forwarded: off/AOD is owned by the framework's doze path |
| **`ltpo_mode`** (`/sys/lcm` and `/sys/lcm1`) | `ltpo_min_fps` is **derived from the DRM rate list** (`card0-DSI-1/modes`: `x60/x90/x120` → `60`). `high_te` is the panel's **max TE (tearing-effect) rate**, *not* the refresh rate: SF parses it into `PromotionConfig{maxTe=…}` and enables vivo's Promotion engine only when it is `>= 240`, so it defaults to vivo's contract value **240**; `high_te=0` reports what the panel actually runs (TE = SDE hw vsync, 121 Hz measured → `120`), which disables Promotion |
| **`dimming_speed`** | → mi_disp `disp_param` feature 0 (dimming): `0` = off, non-zero = on. Verified: `dimming_speed 0/60` sends `feature: dimming, value: 0/1` |
| **`lcm_id`** | reads the real `disp-DSI-0/panel_build_id` when it is a real id (≥ 10 chars, not `Unsupported`); otherwise the 25-nit placeholder |
| **`panel_name`** (×2) | **read from `disp-DSI-0/panel_info`**: `panel_name=mdss_dsi_m3_38_0c_0a_dsc_cmd` → `mdss_dsi_m3_38_0c_0a_dsc_cmd panel`. A userspace write overrides it. Reports the real panel, so a matching `LcmConfig` has to be shipped — the bundled PD2243 file does **not** match it |

`bl_bridge=0`, `fps_bridge=0` and `dimming_bridge=0` disable the backlight, LTPO
and dimming bridges individually if one ever has to be ruled out. `high_te=<n>`
overrides the reported TE ceiling (`240` default, `0` = derive from the panel).

`ltpo_min_fps` is honest (60 = lowest DRM mode), but vivo's panel string says
`1`; the panel's 1 Hz AOD mode is driven by mi_disp's doze machinery, not by a
DRM mode, so it cannot be discovered from the rate list.

## Nodes that are stored only
`colour_gamut`, `hdr10p_bl_remap`, `error_info`, `hbm_notify_enable`.
`ltpo_mode` still accepts the bottom-rate request that vivo RRM writes, but it
can only record it (the panel's rate list is fixed) — the read-back keeps
reporting the capability string, which is what SF/RRM parse.

Notes on the remaining nodes:
- `panel_info=local_hbm=true` is the capability flag that selects FOD's direct
  press-animation path.
- `colour_gamut` / `hdr10p_bl_remap` have no mi_disp equivalent: gamut is an
  SDE/DRM color mode applied by SurfaceFlinger itself, and there is no HDR
  remap stage in mi_disp (`hbm_backlight_level` is the feature 22 knob, but it
  is not an HDR10+ remap).

## Build
```sh
make -C ~/PATH/TO/KERNEL O=/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD modules
```
