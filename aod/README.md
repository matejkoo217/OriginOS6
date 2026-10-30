Hi

Add ro.vivo.keyguard_full_aod.enable=1 to odm

set `support_full_screen` to 1/4 (inside lcm_bridge)

`support_full_screen` values:
| value | meaning
|-------|
|   1   | generic full-screen AOD support (use this one ig)
|   2   | fold `keyguard_aod_show`
|   3   | full AOD, plugin writes `full_aod_on=`
|   4   | LTPS full AOD
