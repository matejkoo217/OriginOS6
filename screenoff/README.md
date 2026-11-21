# screenoff vivo gestures on fuxi

Vivo letter gestures (`c`, `m`, `e`, `o`, `w`) (global has extra for facebook etc but global isnt good base)
and double-tap-to-wake while the panel is off, with vivo's touch HAL loaded for comparison and a DTW matcher
doing the recognition.

## Layout
| path | role |
|---|---|
| `/vendor/bin/vtsgesture` | daemon |
| `/vendor/etc/init/vtsgesture.rc` | init service |
| `/vendor_dlkm/etc/vts_gestures/*.sig` | **base** calibrated templates |
| `/vendor_dlkm/lib/modules/vts_bridge.ko` | vivo touch ABI + resume |
| `/data/vts_gestures/*.sig` | **learned** samples |

The daemon loads both template directories, de-duplicates identical samples,
and saves new self-learned samples.

## Smart Wake gating
* The gates are captured at startup and then whenever the framework changes
  `debug.tracing.screen_state`.
* If no screen-off gesture is enabled (letters/swipes) **and** double-tap is
  off, the daemon writes `auto_resume=0`, ignores the input stream and does
  nothing at all.
* Every action is checked against vivo switch before it runs
  (`action c: suppressed (vivo setting off)`). the switches are captured as a
  set when the panel locks.
* Pocket Detection: the `vtsprox` helper watches the proximity sensor and writes
  `/data/vts_gestures/proximity` (0 = covered, 1 = uncovered).  The daemon
  drops strokes while covered.
* `vtsprox` is event driven and it blocks in `ALooper_pollOnce(-1)`,
  writes the state file on transition.

## Actions
| gesture | action |
|---|---|
| `c` | `am start -a android.intent.action.DIAL` |
| `m` | `input keyevent KEYCODE_MEDIA_PLAY_PAUSE` (does not wake) |
| `e` | `am start -a android.intent.action.VIEW -d https://www.google.com` |
| `o` | QQ launcher |
| `w` | WeChat launcher |
| `swipe_up` | wake |
| `swipe_down` | vivo secure camera over the lockscreen |

## Triggering of actions
They trigger like vivos own implementation: `VivoSmartwakeKeyHandler.launchApp()`

```
input keyevent KEYCODE_WAKEUP
wm dismiss-keyguard
service call activity_task 65 i32 4     # keyguardGoingAway(NO_WINDOW_ANIMATIONS)
<action>
```

Transaction `65` is `IActivityTaskManager.keyguardGoingAway()`. The camera does
**not** dismiss the keyguard: it wakes the panel and starts vivo's
`com.android.camera/.SecureCameraActivity` with
`android.media.action.VIVO_STILL_IMAGE_CAMERA_SECURE`, so it opens over the
lock screen and the phone stays locked.

## Build
```sh
./build.sh
```

For staging, copy `out/vtsgesture` to `/vendor/bin/vtsgesture`, `vtsprox` to
`/vendor/etc/vtsprox` (vtsgesture copies it to `/data/local/tmp` and runs it
from there), `vtsgesture.rc` to `/vendor/etc/init/`, and `base/` to
`/vendor_dlkm/etc/vts_gestures/`.

🐥