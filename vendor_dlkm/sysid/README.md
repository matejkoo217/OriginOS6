# sysid.ko

Storage-identity and storage-info nodes that OriginOS userspace expects but
the Xiaomi/fuxi kernel doesn't provide:

```text
/sys/ufs/ufsid                     UFS device unique id
/sys/ufs/io_sum                    cumulative sda I/O
/sys/ufs/nand_info_dec             decoded UFS device descriptor
/sys/class/block/sda/device/hr     storage health 0-100
/sys/block/mmcblk0/device/cid      eMMC CID (fuxi has no eMMC)
/sys/block/mmcblk0/device/{name,serial,manfid}
/sys/block/mmcblk0/queue/max_bg_depth
/sys/block/mmcblk0/queue/wbt_lat_usec
```

`/sys/class/block/sda/device` is the SCSI device directory that owns `sda` (`sdev_gendev`), so `hr` is visible as both
`/sys/class/block/sda/device/hr` and `/sys/block/sda/device/hr`.

## Nodes:
| node | real / derived | source | format |
|---|---|---|---|
| `/sys/ufs/ufsid` | real | `vpd_pg80` (VPD page 0x80) of the sda SCSI device, i.e. the UFS device serial; falls back to `/sys/devices/soc0/serial_number` | `%s\n` |
| `/sys/ufs/io_sum` | real | `/proc/diskstats`: sum of the `sda`, `sda<N>` and `dm-*` lines (the UFS disk and the device-mapper devices stacked on it) | `"%llu %llu\n"`, two integers, 512-byte sectors, cumulative since boot |
| `/sys/ufs/nand_info_dec` | real (decoded) | `device_descriptor/*` and `health_descriptor/*` of `1d84000.ufshc`, plus `model` and the UFS serial | one `key: value` per line, see below |
| `/sys/class/block/sda/device/hr` | derived | `health_descriptor/life_time_estimation_{a,b}`, JEDEC bucket midpoint | `"%d\n"`, 0-100 |
| `/sys/block/mmcblk0/device/cid` | derived | synthetic eMMC CID built from the SoC serial and `wManufactureDate` (fuxi has no eMMC) | 32 hex chars |
| `/sys/block/mmcblk0/device/name` | real | `/sys/block/sda/device/model` | `%s\n` |
| `/sys/block/mmcblk0/device/serial` | real | same source as `ufsid` | `%s\n` |
| `/sys/block/mmcblk0/device/manfid` | real | `device_descriptor/manufacturer_id` (eMMC-style `0x%06x`) | `0x%06x\n` |
| `/sys/block/mmcblk0/queue/max_bg_depth` | real (mirrored) | `/sys/block/sda/queue/max_bg_depth`, read at runtime; **0 if the file doesn't exist** (this 5.15 kernel has no such queue attribute) | `"%u\n"`, 0444 |
| `/sys/block/mmcblk0/queue/wbt_lat_usec` | real (mirrored) | `/sys/block/sda/queue/wbt_lat_usec`, read at runtime; writes are forwarded to it | `"%u\n"` µs, 0644 (store forwards to the sda file with the caller's credentials) |

## Who reads them

| consumer | use |
|---|---|
| `com.vivo.ic.SystemUtils.getUfsid()` → `getEmmcId()` | device id; `getUfsid()` is tried first and falls back to the CID. Logged `FileNotFoundException: /sys/ufs/ufsid` every few seconds before this module. |
| `vendor.vivo.hardware.bspservice@1.0-service` | logs `emmcid=` / `get emmcid or cpuid failed`, used by the vivo key-provisioning (`BSPUtils`) path |
| `/system/bin/vold` | reads the ufsid/CID paths, logs `Reading lifetime estimation failed` for the missing `life_time_estimation_c` |
| vivo storage code (`_V_pie_ufs`) | `read io_sum failed! ufs init failed` when `/sys/ufs/io_sum` is absent; `nand_info_dec` / `hr` carry the same information in decoded form |

## Build

```sh
make -C ~/PATH/TO/KERNEL O=~/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD KASAN_SANITIZE=n modules
```
