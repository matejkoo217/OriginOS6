================================================================================
0. Fingerprint
================================================================================
resources.arsc/res/values/arrays.xml:
<?xml version="1.0" encoding="utf-8"?>
<resources>
    <array name="config_biometric_sensors">
    </array>
</resources>

change it to:
<resources>
    <array name="config_biometric_sensors">
        <item>0:2:15</item>
    </array>
</resources>

this sets it to optical, lazy to check other values rn so good luck :P

================================================================================
1. BATTERY CAPACITY
================================================================================
<item name="battery.capacity">4500</item>
in VivoFrameworksResRRO/res/xml/power_profile.xml


================================================================================
2. PUNCH-HOLE OVERLAY
================================================================================
1. Adapt vigour_hole.png for your device and place it at:
   /system/system/vigour_hole.png
   - Stock fuxi camera hole width = 68px.

2. Add prop:
   debug.hole.fromsystem=true (in odm)

3. In /vendor/etc/vivo_config.ini:
   [vivo.hardware.holescreen]
   hole_x=507
   hole_y=30
   hole_radius=33
   cliptype=native
   align=middle
   * hole_x = 507, hole_radius = 33 -> Center X = 507 + 33 = 540 (exact 1080/2 center).
   * hole_y = 30, hole_radius = 33 -> Center Y = 30 + 33 = 63px.


================================================================================
3. DYNAMIC ISLAND & COLLAPSE MECHANISM
================================================================================
The dynamic island ("Atomic Island" / SmartIsland) is rendered by SystemUIPlugin.apk
(com.vivo.island.*) inside com.android.systemui:
- Overlay Window: type=2009, gravity=TOP|CENTER_HORIZONTAL, height=48dp.
- State Machine (DynamicCapsuleStateController):
  0: STATE_NONE        (idle / collapsed into camera hole)
  1: STATE_TRANSITION
  2: STATE_SINGLE_CAP  (pill around cutout)
  3: STATE_DUAL_CAP    (split pill)
  4: STATE_CARD_EXPANDED (large card)

When opening notifications, pulling down the notification shade, or switching apps,
the island transitions to STATE_NONE (0).

Here:
  - halfWidth = (rect.right - rect.left) / 2  (cutout radius R)
  - centerY = rect.bottom - halfWidth         (vertical center of the cutout)
  - paddingTop = centerY - (mIslandHeight / 2)


================================================================================
4. COLLAPSE ANIMATION
================================================================================

Tuned vivo_config.ini hole: [vivo.hardware.holescreen]
    hole_y = 30, hole_radius = 33 -> Center Y = 30 + 33 = 63 px

Cutout Tuning Reference:
Center Y = 63px (hole_y=30 + hole_radius=33):
    M -34,33 L -34,97 L 34,97 L 34,33 Z


================================================================================
5. BUILDING
================================================================================
./build_overlay.sh

🐥 hi
