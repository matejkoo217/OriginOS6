# battery_compat.ko

Native-only compatibility module for the OriginOS/Vivo battery code running on
Xiaomi/QTI power-supply devices.

It does **not** replace or modify `qti_battery_charger.ko`. It adds read-only attributes to the existing `battery` and `usb` power-supply devices:

```text
/sys/class/power_supply/battery/chg_type
/sys/class/power_supply/battery/fg_passed_charge
/sys/class/power_supply/usb/present
```

The `usb/present` attribute is created through the kernel device model and may
fail with `-EEXIST` if the running kernel already provides a `present` attribute.
That failure is intentional safety behavior; inspect the error before changing
the name.

## Build
```sh
  make -C ~/PATH/TO/KERNEL O=~/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD modules
```

## `fg_passed_charge`

Default `fg_passed_charge=-1` derives `1` only when both
`CHARGE_FULL` and `CHARGE_FULL_DESIGN` are positive. Override for testing:

Do not force this value in production until the Vivo consumer's expected
semantics are confirmed.

## Current node surface

`/sys/class/power_supply/battery/`: `chg_type`, `fg_passed_charge`,
`soc_decimal`, `engine`, `charging_speed`, `board_temp_state`,
`primary_board_temp`.
`/sys/class/power_supply/usb/`: `present`.
`/sys/class/fuelsummary/`: `amount`, `age_amount`, `amount_total`, `cycle`,
`roll_current`, `soh`.
`/sys/class/cms_class/`: 94 nodes.

Derived from real QTI state:
| node | source |
|---|---|
| `chg_type` | `usb` `POWER_SUPPLY_PROP_USB_TYPE` |
| `engine` | USB type (PD/PD_DRP/PD_PPS → `3`) |
| `adapter_power` | usb `VOLTAGE_NOW` * `CURRENT_NOW` |
| `board_temp_state` / `tboard` | `qcom-battery/connector_temp` (≥ 45.0 °C) |
| `primary_board_temp` | `qcom-battery/connector_temp` (0.1 °C, raw) |
| `md_status` / `md_scene` / `md_data` | `qcom-battery/moisture_detection_status` |
| `fuelsummary/soh`, `ui_soh`, `aging_soh` | `qcom-battery/soh` (83 on this unit), else aged/design capacity |
| `bat_wpc_tx_chg_state`, `wls_otg_status` | `reverse_chg_mode` **or** `wireless_boost_en` ≠ 0 → `1` |
| `bat_wpc_tx_to_tx_state` | `reverse_chg_mode` ≠ 0 **and** `reverse_chg_state` = 0 → `1` |
| `weakcharger` | usb `CURRENT_MAX` ≤ 500 mA while online |
| `coulomb`, `*_voltage`, `capacity_*`, `cycle`, `health`, `*soh*` | battery/usb power-supply props |
| `backlight_brightness` | falls back to the live backlight file |
| `batid` | battery `model_name` |

`reverse_chg_state` is deliberately **not** used for "is it on": that firmware
value latches.  After one `wireless_boost_en=1 → 0` cycle it stays `2` while
mode/boost/`WLS_FW_STATE` are all 0, which would keep the framework's
reverse-charging dialog up forever.

## Reverse wireless charging

fuxi does have (50 W RX, 10 W reverse). The port's toggle lives in
`com.iqoo.powersaving`, which only sets the property
`sys.vivo.enable_reverse_wireless_charge` (verified in its decompiled
`BatteryChargeUtils.Q()`), and vivo's kernel driver would normally consume it.

Measured on the device:
| `reverse_chg_state` | meaning |
|---|---|
| `0` | off |
| `2` | armed, no receiver / TX not charging yet |
| `4` | **TX active, charging the device on the coil** |

Arming recipe - **one write each way** (3/3 arms measured with the phone on the
coil):

```text
write reverse_chg_mode 1     # arm   (charger sets wireless_boost_en itself)
write reverse_chg_mode 0     # stop  (wireless_boost_en follows)
```

Each sysfs write is a separate charger-firmware round trip (~0.5-1 s), so the
older 4-write `0/0/1/1` cycle wasted ~4 s for nothing. Writing only
`wireless_boost_en=1` does *not* work - it leaves `reverse_chg_state` at 2 and
never transmits.

## Things found the hard way:
- **Do not send a battery uevent while the charger is armed-but-idle.** Calling
  `power_supply_changed()` (to make SystemUI update quickly) makes vivo's own
  framework switch reverse charging back off ~200-500 ms later. Proved by
  unloading the module: with it unloaded the property stays at 1, loaded it
  reverted. The module therefore only pokes the uevent once
  `reverse_chg_state == 4`, i.e. when the coil is really charging.
- `wls_tx_speed`, `wls_die_temp`, `tx_adapter` stay **0 even while charging** -
  they are not usable as "is it on" indicators; `reverse_chg_state` is.
- `reverse_chg_mode` reads back `1` for any value > 0, so it cannot be used to
  select a mode.

The two nodes `/sys/class/power_supply/bat_encryptor/{present,battery_info}`
that the same app reads belong to vivo's encrypted-battery authentication
driver and are still missing - `battery_info` is a signed blob that cannot be
synthesised, so the app falls back to the normal battery data.
Nodes that userspace controls (init.vivo.rc, FuelSummary, PEM scenes) store and
echo the last write, which is what the real cms driver does. The
`*_bigdata_info` / `soh_loss_table` / `battery_health_info` telemetry blobs are
read-only and empty: only vivo's charging-IC driver can produce that content.
Everything the HAL logs is visible as `dmesg | grep 'healthd: battery'`
(`... eng= sd= cs= bts= bes= bwtcs= bwttts= p= last_flag= last_time= wts= md=`),
so any mapping can be checked live.

### Contract details that matter
* `health_status` is an **int bitmask** polled by `BatteryWarningService`
  (`Integer.parseInt`, whole file): bit1 = battery over-temperature, bit3 =
  battery over-voltage, bit5 = battery temperature too low, bit13 = charging
  high-temperature (the qcom variant). Only bits with an unambiguous
  `POWER_SUPPLY_HEALTH` equivalent are set; everything else stays 0.
* `soh_loss_table` and `loss_condition_sum_table` are **written by the HAL**, not
  read by it (`init.rc` chmods both 0666). They used to be read-only here, which
  made the HAL log `healthd_soh: write ... fail` on every weekly rebuild. They
  are now 0666 storage with real sizes: 1024 B (one `<date> <cycle> <loss[12]>`
  row) and 4096 B (one line of 22x17 `%9e` cells, ~3.0-3.7 KB). The store
  returns the full count, which the HAL checks.
* `soh_life_loss` is parsed as `"%d %d"`, so its default is `0 0`, not `0`.
* `charging_speed` was a constant `0` until 2026-10-29; it is now derived from
  the real USB input power (`V*I`): 0 = not charging, 1 = < 7.5 W, 2 = 7.5-18 W,
  3 = 18-40 W, 4 = > 40 W. The bucket edges are ours (vivo's
  `POWER_SUPPLY_PROP_CHARGING_SPEED` scale - their enum 175, served by the
  charging-IC stack - is not recoverable), so it is display/telemetry only.
* `soh_loss_{gamma,theta}_param` are `"<N>,<int>,<double>[,...]"` lists (N=0
  means "no override" and is valid); the empty default produced
  `formula index is error`. They now default to `0`.
* `battery_health_learning_capacity_state` must parse as an int (empty logged
  `can't read file int`); it is `0` = "not ready". `..._learning_soh` is wired to
  the real gauge (`qcom-battery/soh`).
* `soh_formula` is `f_index<sep><0|1>` **plus** a coefficient blob: the real
  string comes from the device-tree property `vivo,soh-formula`
  (`cms_parse_dt()` in vivo's `vivo-chg-cms.ko`, default literal
  `"no formula"`) and vivo's driver exposes it read-only. fuxi's DTB has no such
  property and the coefficients are not in the HAL binary, so the node reports
  the vendor's own `"no formula"`. Do **not** fabricate coefficients: with them
  the engine starts and writes invented SOH/loss data into the system.
* To keep the HAL's SOH block silent (it otherwise logs two lines at boot), the
  vendor's own kill switch works: `touch
  /mnt/vendor/persist/charger/soh_predict_disable` (presence only, reversible).
  With it, a HAL restart logs nothing beyond `soh_init: uid:1000, euid:1000`.
* `soh_ver_transition` must read **`3`**. FuelSummary
  (`BackgroundBatteryHealthUtils.l()`) only uses the real SOH (`ui_soh`) when
  this node is "3"; otherwise it falls back to vivo's legacy 100-entry
  cycle-count table, which is what made the Settings battery-health percentage
  (86/87 for 659 cycles) disagree with the gauge SOH (81). The app only writes
  the node back when it currently reads "1", so a default of "3" sticks.
  `battery_health_framework_flag` is the companion node FuelSummary persists its
  health state into (it must exist or the write is dropped).

## Reverse wireless: removal detection (2026-10-29)

The charger keeps `reverse_chg_mode` / `wireless_boost_en` set for up to ~1 min
after the device is taken off the coil, so deriving `wls_otg_status` from "is it
armed" kept the vivo dialog up long after removal. The node now follows the live
`reverse_chg_state` and a 1 s poller pushes a battery uevent the moment the value
flips:

* `state == 4` (device charging) -> 1 (and the session is marked as "had a device")
* armed but never had a device -> 1 (waiting for placement)
* device seen, then `state != 4` -> 0 immediately + uevent (dialog dismisses)
* TX fully off (`mode`/`boost` == 0) -> 0, session forgotten

Verified on device: `wls_otg_status 0 -> 1` on arm, `1 -> 0` at `state=5`
with mode/boost still armed ~13 s later, i.e. instantly instead of after the
charger timeout.

## Bypass charging: what fuxi can and cannot do

vivo's `cms_class/game_bypass_charge_flag` is a real store in their driver; here
it is wired to stop the charge current (default `bypass_mech=0`: write FCC 0,
restore the saved value when cleared; `bypass_mech=1`: `qcom-battery/restrict_chg`).

Measured on fuxi (100 %, on a 0.5 A SDP port):

| control | result |
|---|---|
| `constant_charge_current` (FCC) | **writes silently ignored** — reads back 5280000 uA for 0/0.1/0.5/3 A |
| `battery/charge_control_limit` 16..1 | **does not stick** (always reads 1) |
| `qcom-battery/restrict_chg` | accepted, but no measurable change while `status=Full` |
| `qcom-battery/input_suspend` | **works** — but that cuts VBUS (phone runs on battery), not bypass |

So fuxi's PMIC driver exposes no "charging enabled/disable" property: there is no
node that keeps VBUS powering the system while the cell idles. The bypass toggle
is therefore UI-only until the charger driver is patched (or the test is repeated
at < 90 % SOC on the 67 W charger to see whether `restrict_chg` acts there).
Note the phone net-discharges on a weak port anyway (`fg1_ai` +1.2 A with
`POWER_SUPPLY_STATUS=Full`), which hides the effect at 100 %.


## Charge limit — final behaviour (2026-10-29)

Source of truth is **only** `cms_class/keep_soc` (`"<mode> <percent>"`, the vivo
service writes e.g. `6 90`).  No Settings parsing.

* percent write -> hold at that % (stop = `qcom-battery/input_suspend=1`);
* **zero is always ignored** (keep the last value): the vivo service writes
  `6 0` as a reaction to the hold and re-asserts the percent when it sees
  charging resume, so honouring it flaps VBUS and buzzes the phone every ~3 s;
* release with the parameter (no reload needed, it latches over the service's
  echo until the node reports a different percent):
  `echo 0 > /sys/module/bsts/parameters/soc_limit`;
* `soc_limit` module parameter forces a value and latches over the service's
  echo until the node reports a different percent;
* release happens 2 % below the limit (`SOC_HYST`) so it does not cycle.

There is no "stop charging, keep VBUS" control on fuxi's charger: `restrict_chg`
only clamps the charge current, `constant_charge_current` / `charge_control_limit`
/ `restrict_cur` writes are ignored, `op_mode` is read-only.  Cutting the input
is the only option, so the charger announces a disconnect when the hold engages.
