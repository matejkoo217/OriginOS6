# msm_thermal.ko
CPU core-control for vivo's PEM on the OriginOS 6 port (fuxi).

## Why
vivo's power manager writes a CPU mask to
```text
/sys/module/msm_thermal/core_control/cpus_offlined
```
when it enters/leaves a thermal scene (`0` = "all cores online").

## What it does
Creates the node and applies the mask using the normal hotplug interface 
(`/sys/devices/system/cpu/cpuN/online`) because `cpu_down()` is not
exported to modules.

Rules:
- CPU0 can never be offlined - writing bit 0 returns `-EINVAL`.
- Reading the node returns the **current** offline mask, not the last write.
- No-op writes (mask already matches) do nothing.

Example:

```sh
echo 0x80 > /sys/module/msm_thermal/core_control/cpus_offlined   # cpu7 off
echo 0    > /sys/module/msm_thermal/core_control/cpus_offlined   # all on
```

## Build
```sh
make -C ~/PATH/TO/KERNEL O=~/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD KASAN_SANITIZE=n modules
```
same as every other ko....

## Test
first insmod msm_thermal.ko
```sh
cat /sys/module/msm_thermal/core_control/cpus_offlined   # current offline mask
echo 0x80 > /sys/module/msm_thermal/core_control/cpus_offlined
cat /sys/devices/system/cpu/online                      # 0-6
echo 0 > /sys/module/msm_thermal/core_control/cpus_offlined
cat /sys/devices/system/cpu/online                      # 0-7
```

Expected dmesg:
```text
msm_thermal: core_control up (8 cpus, offline mask 0)
msm_thermal: cpu7 -> offline (rc=0)
msm_thermal: cpus_offlined 0 -> 128
```
