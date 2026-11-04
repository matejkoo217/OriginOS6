!MADE FOR XIAOMI 13 YOUR DEVICE MIGHT NEED DIFFRENT VBMETA FORMAT!

IMPORTANT IS TO MODIFY THE ODM AND VGC TO MATCH YOUR BASE FINGERPRINT AND VERSION


! Make sure you didnt merge vgc and oem to odm and vendor before !


Create new folder in vendor called vgc so it has mount point!

fstab:
oem                                                     /oem                   erofs   ro                                                   wait,slotselect,logical,first_stage_mount,nofail,avb_keys=/avb/oem.avbpubkey
vgc                                                     /vendor/vgc            erofs   ro                                                   wait,slotselect,logical,first_stage_mount,nofail,avb_keys=/avb/vgc.avbpubkey
/dev/block/bootdevice/by-name/cust                      /logdata               ext4    noatime,nosuid,nodev,barrier=1,context=u:object_r:logdata_data_file:s0 wait,check,formattable,nofail

to both vendor and vendor_boot. if your device doenst have cust then you are cooked (jk add cust img to super, give it atleast 200mb or so idk. and change fstab line.)

1. run.sh
Output in out/:
    new_oem.img
    new_vgc.img
    new_vbmeta_noverify.img
    new_digest_flags3.txt
run.sh signs ./oem.img and ./vgc.img. Put the unsigned images there first.


2. Super
add the new images to super and repack.


3. vendor:
add avb keys to /vendor/avb/
vgc.avbpubkey
oem.avbpubkey


4. vendor_boot
add avb keys to /vendor_boot/avb/
vgc.avbpubkey
oem.avbpubkey

add to vendor_boot cmdline:
androidboot.vbmeta.hash_alg=sha256 androidboot.vbmeta.size=PLACEHOLDER androidboot.vbmeta.digest=PLACEHOLDER

replace PLACEHOLDER with the values from ./out/new_digest_flags3.txt

btw also add:
androidboot.vc.magic=none androidboot.vbmeta.device_state=locked

based on /vendor/etc/sw_selfcheck_items/security_items.xml enteries.

5. Flash vbmeta

6. cust
cust is needed for vivo vold and other bs.
i provided fuxi cust.img , it should work on other phones with 2gb custs or just run script to change it.
if your phone doesnt have cust , you will have to add it to Super, recomended size of cust is "SIZE"
it has to be writable in Super!

7. Boot.

# dont cry to me if you brick i will laugh at you :D.