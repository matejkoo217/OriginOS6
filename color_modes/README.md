# Vivo Display Colour Management

Vivo's framework picks the colour scheme in its own vocabulary (`Standard` / `Professional` / `Bright` / `ZEISS`) and tells SurfaceFlinger almost nothing about it.
SF transaction `31013` only ever asks for `ColorMode SRGB` (Professional) or `NATIVE`.

Everything that makes the schemes look different (the STC gamut and the ZEISS PCC) has to be applied to composer's colour-mode engine with a render intent (`258` vivid / `267` sRGB / `268` DCI-P3 / `269` auto) that no framework component ever sets. And `dfset`/HAL PCC calls for ZEISS and the temperature slider.

---

### How does it enforce it?
Every attempt to point `HWCDisplayBuiltIn`'s slot `64` at code dies with a fault at the target inside the composer:

- `SIGILL/ILL_ILLOPC` with BTI landing pads declared.
- `SIGSEGV/ACCERR` or `MAPERR` without.

Meanwhile, the same function runs fine when the STC manager calls it via `dlsym`.

So the override is done with a code patch inside the composer's own text.

The first 23 instructions of `HWCDisplayBuiltIn::SetColorModeWithRenderIntent` (`0x8e5f4..0x8e64c`) are replaced by a sequence that:

1. Reproduces the frame setup.
2. Records the display object.
3. Forces `(ColorMode 7 = SRGB, RenderIntent = <the word we own>)` when the requested mode is `7` (SRGB) or `0` (NATIVE).
   - These are exactly the two things vivo's framework and SurfaceFlinger ask for on the SDR desktop.
   - HDR modes are left alone.
4. Jumps back into the composer's own untouched tail at `0x8e650`, which does the colour manager virtual call and the epilogue.

#### Native vs. patched instructions

| Native (`0x8e5f4..`) | Patched (23 words) |
|---|---|
| `paciasp` / `sub sp,#64` / `stp x29,x30` | Keep, verbatim |
| `add x29` / `stp x22,x21` / `stp x20,x19` | |
| `ldr x21,[x0,#408]` (`color_mgr_`) | |
| `nop .. mov w19,w2` (CFI check, 9 instructions) | |
| | `mov x22, x0` (tail needs it!) |
| | `mov w20, w1` |
| | `mov w19, w2` |
| | `adrp x9, <word page>` |
| | `str x0, [x9, #16]` ← this |
| | `ldr w9, [x9]` ← intent |
| | `cbz w9, call` (0 = stock) |
| | `cmp w20, #7` |
| | `b.eq force` |
| | `cbnz w20, call` (HDR: stock) |
| | **force:** `mov w20, #7` |
| | `mov w19, w9` |
| | **call:** `ldr x8, [x21]` |
| | `b 0x8e650` (native tail) |
| | `nop ; nop` |

> **Important:** `mov x22, x0` is needed. The native tail uses `x22` after the colour manager call.
🐥
---

### Why not just request?

The SF request alone cannot identify the vivo mode:
- `0`, `2`, and `3` all arrive as `ColorMode::NATIVE`.
- Only `1` arrives as `SRGB`.

Worse, when SF is already `NATIVE`, a `NATIVE → NATIVE` scheme change does not reach this function at all.

So the module watches vivo's own state instead:

| Source | Setting |
|---|---|
| `/data/system/users/0/settings_system.xml` | `vivo_display_color_mode` (`0..3`) |
| `/data/system/users/0/settings_secure.xml` | `vivo_night_display_color_temperature` |
| `/sys/lcm/colour_gamut` | `lcm_bridge` |

---

### Why the module calls back into the composer

Because a `NATIVE → NATIVE` scheme change never reaches the composer, publishing a new intent is not enough, because nobody would apply it.

So the patch records `HWCDisplayBuiltIn` on every call, and when the mode changes, the helper thread calls the patched function itself with `(SRGB, intent)`.

Its prologue goes straight to `color_mgr_->SetColorModeWithRenderIntent`, so a helper-thread call is not locking the class; it is the existing qservice one.

---

### PCC / Colour Temperature

ZEISS and the temperature slider are DSPP PCC, not gamut, and the only working route to them is the Xiaomi HIDL HAL through `/vendor/bin/dfset`.

Example log:

```text
DisplayFeatureHal: setFeatureEnable displayId = 0, FeatureId=EXPERT_STATE modeId=<gain> cookie=<slot>
```

---

### True Tone (ZEISS Only)

The HAL's `COLOR_TEMP_STATE` (`1/2/3`) has no gradient to interpolate, while PCC (`EXPERT_STATE`) takes R/G/B gains and is visible on this panel.

So the ambient shift is done there:🐥

```bash
dfset -e <R> <G> <B> <H> <S> <V> <contrast> <gamma>
```

With:
- **G** as the anchor (always `255`).
- **R/B** moved around the ZEISS base (`R = 255` / `B = 235`).
- **W** set to `ZEISS_SAT`.


### Ambient CCT Input

The input is `sensors.light.so`'s correlated colour temperature (CCT), derived from the Clear/Red/Green/Blue channels of the front TCS3720.

It is published as an 8-byte ASCII record at:

```text
/data/vendor/display/light_cct
```


### Update Throttling

`dfset` pushes all eight slots, so it is only spawned when:

- The computed gains actually move by at least `TT_STEP / 255`.
- At least `TT_MIN_MS` has elapsed since the previous update.

Both are constants and can be changed easily below.

Ambient CCT moves slowly, and the HAL itself publishes at most once a second, so in a typical room this is silent.
🐥

---

### Game Assistant filters

The game colour presets (Vivid / Bright / Soft / Shadow boost / Anti-glare /
Old movies) and the **Image quality tuning** sliders (Tone / Saturation /
Contrast / Brightness) are not SurfaceFlinger colour modes. `com.vivo.gamecube`
sends them to system_server:

**`/vendor/etc/vivo_config.ini`** `[vivo.hardware.game.sdr2hdr]` needs
   `is_support_game_display_enhance_platform=true`. Without it system_server
   takes the Nova/Iris branch instead. It also needs **`is_support_zygote=1`**
   becouse the feature is read in zygote, so the configstore HAL publishes
   `ro.vendor.vivo.hardware.game.sdr2hdr:<key>` at boot; without the
   attribute those properties are absent and zygote/system_server fall back
   to the defaults (`false`).
