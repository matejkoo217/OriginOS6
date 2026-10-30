FOD fix for xiaomi 13, miui 14 vendor, not tested on other versions but it depends on mfp daemon.

before building you should replace libs in sysroot!

./build.sh to build it

mfp daemon needs patching to fix mismatch (./tools/patch_mfp_daemon.py)

it also needs some more things such as editing fp_config.conf to change position,
also stuff in vendor_dlkm/fp_id/README.txt
