# SmartScreenOn — fix for "Keep screen on while viewing"

## Why is it needed?
`FaceWakeService` hardcodes camera id **"1"** — that is *vivo's* front-camera
numbering (0 = rear wide, 1 = front wide).  On fuxi the QTI camera provider
numbers the cameras the other way round:
| id | facing | |
|----|--------|---|
| 0  | Front  | front wide ← what the service should use |
| 1  | Back   | rear main ← what it actually opens |

You might need to have working camera stack to make it actually work.
For that refeer to vivocam once its opensourced, for now its not,
becouse im not happy with the state it is rn.

## Build
```
./build.sh
```

## Verify
```
adb logcat | grep -E 'smartscreenond|smartscreenon'
```
Expected per screen-on cycle:
```
smartscreenond: pid 30022 age 70ms, injecting
smartscreenon : lsplant init ok
smartscreenon : hooked openCamera
smartscreenon : front camera id = 0
smartscreenon : openCamera(1) -> 0 (front camera)
```
