# vddr_stats

Real `/dev/vddr_stats` for vivo's `com.vivo.pem` (OriginOS 6 on Xiaomi 13
"fuxi", SM8550/kalama).

## Why

`com.vivo.pem` (uid `system`) reads `/dev/vddr_stats` on Qualcomm to report DDR
frequency residency (`com/vivo/pem/bd/b.L2`, Qualcomm branch). The vivo driver
is proprietary, so the node did not exist and PEM logged:

```
E _V_pem: catNode error:java.io.FileNotFoundException: /dev/vddr_stats ...
```

## How it works

The QTI `soc_sleep_stats` driver keeps DDR residency in RPMh MSGRAM and exposes
it via debugfs, but **that node never asks the AOP to snapshot the counters** -
a consumer has to trigger it.

## Build

```sh
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL/out ARCH=arm64 \
  LLVM=1 LLVM_IAS=1 M="$PWD" modules
```

## Caveats

- **Load once.** The `qmp-aop` mailbox provider runs out of channels after
  several `rmmod`/`insmod` cycles (`mbox_request_channel` starts returning an
  error and the node then serves stale data). A single boot-time load is fine;
  if you are testing, reboot between reloads.
- `send_sync` (default 1) controls the QMP refresh; `send_sync=0` reads MSGRAM
  without refreshing.
- If the mailbox cannot be acquired or MSGRAM magic is wrong, the node falls
  back to a zeroed template so PEM still parses it.
