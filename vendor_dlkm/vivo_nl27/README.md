# vivo_nl27.ko = Netlink protocol 27
For vivo's telephony TCP-info dispatcher.

OriginOS6 opens `AF_NETLINK` protocol 27 on phone start and on every screen-state
change (`vivo-network-priv-telephony-common.jar`, `com.vivo.internal.telephony.o2`,
path `o2.A -> o2.W -> o2$j.handleMessage`). Xiaomi kernel has no handler, so
`socket()` fails with `EPROTONOSUPPORT` and the dispatcher throws every time.

This module registers protocol 27 and drains it. It is **drain-only on purpose**.
So vivo's TCP-info features stay as inactive as they already were.

## Messages:
| type | meaning | reply |
|---|---|---|
| 0 | enable at phone start | none |
| 2 | poll uid TCP info | none |
| 3 / 4 | UDP-loss monitor start / stop | none |
| 6 | screen state (`seq=1` on, `seq=0` off) | none |
| 7 / 8 | uid blacklist add / remove | none |

## Why there is no real-data mode
Real reply is possible (`tcp_hashinfo` and `tcp_get_info()` are exported),
but a sum over live sockets is not safe. closing a connection subtracts its
cumulative counters, which reads to vivo as "tx moving, rx frozen" = exactly the
stall-recovery trigger. Doing it correctly needs a per-uid delta accumulator with
per-socket state, and `handshakeAll/Fail` is inside (vivo's `handshake_counter.ko`
but it needs external calling from kernel, for nothing, its not worth it imo).

## Params and counters
| name | meaning |
|---|---|
| `/sys/module/vivo_nl27/parameters/reply` | `0` drain (default); `1`/`2` synthetic lab replies, never on a device |
| `/sys/module/vivo_nl27/parameters/verbose` | log received messages to dmesg |
| `/proc/vivo_nl27` | `reply`, `verbose`, `bad`, `other`, `sent`, `type[<n>] <count>` |

## Build
```sh
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL/out M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
```
