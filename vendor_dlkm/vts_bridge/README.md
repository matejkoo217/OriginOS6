# vts_bridge vivo touch/VTS ABI for the fuxi

OriginOS drives **screen-off gestures** entirely from userspace. But FocalTech IC is
driven by the MIUI `fts_touch_spi` so the nodes dont exist.. so `vts_bridge` provides missing userspace ABI.

## Nodes

### `/sys/touchscreen/`

| node | mode | behaviour |
|---|---|---|
| `app_name` | 0666 | **command parser**: `<cmd>:<name>:<state>`, see below |
| `tp_count` | 0444 | real-ish |
| `gesture_point`, `gesture_time_stamp`, `imei_ctl`, `gesture_points` | 0666 | int storage + log |
| `gesture_switch`, `gesture_switch_export`, `gesture_letter_switch`, `gesture_sign_switch`, `swipe_switch`, `finger_gesture_switch` | 0666 | int storage + log |
| `dclick_switch`, `dclick_simulate_switch`, `edge_suppress_switch`, `gloves_mode_switch`, `calling_switch`, `interrupt_enable`, `lcd_state_set`, `screen_clock_point`, `screen_clock_support`, `user_defined_gesture_enable`, `sensor_rx_tx` | 0666 | int storage + log |

### `/sys/class/vts/vivo_ts`

| node | value |
|---|---|
| `version` | `0x0033` |
| `touch_times` | `0 0 0 0 0 0 0` (the Java side splits on spaces, wants 5–7 numbers) |
| `gesture_bitmap` | last `vts_gs` bitmap, hex (debug) |
| `properties/lcmid` | `/sys/lcm/lcm_id` if readable, else `0x20` |
| `properties/support_singletap` | module parameter `support_singletap` (default **0** = status quo) |
| `status/charging` | last `vts_charge` |
| `status/game_mode` | last `vts_game_mode` / `GAME_IN` / `GAME_OUT` |
| `status/lcd_on` | last `vts_lcd` |
| `status/finger_up`, `status/fingerprint` | `0` |

## Game mode

`status/game_mode` itself is write-only state: the only reader in OriginOS is
`VtsVcdJumpPoint` (vivo cloud diagnostics), it does not tune anything.

The real path is

```
Settings.System is_game_mode
  → TouchScreenService
  → /sys/touchscreen/app_name "vts_game_mode:vivo_ts:1"
  → vivo_ts.ko vts_game_mode_store()  → IC set_game_mode(high_rate, idle_time)
```

`vivo_ts.ko` is not loaded on the port, so `vts_bridge` replays that write onto
the MIUI stack: `Touch_Game_Mode` (xiaomi-touch mode 0) on `/dev/xiaomi-touch`,
i.e. `fts_set_cur_value(0, on)`. Verified effect on the FTS IC: game parameter
block (`0xc0,0x5,…`), report-rate/idle-time switch (3s → 30s) and a grip /
corner-rejection re-tune. `GAME_IN` / `GAME_OUT` are accepted too, and writes
addressed to `vivo_ts_second` are ignored. Module parameter
`game_mode_bridge` (default **1**) disables the mirroring.

The misc device cannot be driven from kernel context (its unlocked_ioctl does
a 1 KiB `copy_from_user`/`copy_to_user`), so the FTS callback is called
directly through `no_sanitize("cfi")` like `fts_resume_work` already is.

## Build
```sh
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
```
