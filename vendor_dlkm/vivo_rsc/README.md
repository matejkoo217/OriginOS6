# vivo_rsc FOR 5.15 A13 !

Userspace ABI shim for vivo's RSC (resource scheduler) on the OriginOS 6 port.

The stock vivo kernel exposes `/sys/rsc/*` and `/proc/vivo_rsc/*`, consumed by OriginOS services.
These interfaces are missing from the Xiaomi kernel, causing repeated `ENOENT` errors.

This module recreates the RSC interface and implements the functionality that can be mapped to existing kernel facilities. This includes task QoS, CPU limits, scheduler boosts, frequency controls, memory QoS and related features.

Unsupported features remain stubbed. (might change in future)

## Implementation
- Recreates the `/sys/rsc` and `/proc/vivo_rsc` directory trees.
- Implements real functionality where equivalent kernel interfaces are available.
- Uses uclamp, scheduler policies, cpufreq, cpusets and memory management interfaces.
- Automatically reverts temporary boosts and limits to prevent them from remaining active indefinitely.

## Build

```sh
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
```