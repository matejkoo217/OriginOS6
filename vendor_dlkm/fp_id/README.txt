fp_id by itself isnt enough, you need to modify VivoFrameworksResRRO.apk to present correct fp style

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
