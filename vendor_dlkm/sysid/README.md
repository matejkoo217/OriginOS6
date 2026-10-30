# sysid.ko

Device-id nodes that OriginOS userspace expects but the Xiaomi/fuxi kernel doesnt provide:

```text
/sys/ufs/ufsid                  UFS device unique id
/sys/block/mmcblk0/device/cid   eMMC CID (fuxi has no eMMC)
```

## Who reads them

| consumer | use |
|---|---|
| `com.vivo.ic.SystemUtils.getUfsid()` → `getEmmcId()` | device id; `getUfsid()` is tried first and falls back to the CID. Logged `FileNotFoundException: /sys/ufs/ufsid` every few seconds before this module. |
| `vendor.vivo.hardware.bspservice@1.0-service` | logs `emmcid=` / `get emmcid or cpuid failed`, used by the vivo key-provisioning (`BSPUtils`) path |
| `/system/bin/vold` | reads both paths |

## Build

```sh
make -C ~/PATH/TO/KERNEL O=~/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD modules
```
