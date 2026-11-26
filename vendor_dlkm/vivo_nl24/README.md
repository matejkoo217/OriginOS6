# vivo_nl24.ko = Netlink protocol 24
For vivo's Wi-Fi SLA/QoE dispatcher.

OriginOS6 keeps a socket on `AF_NETLINK` protocol 24 and retries it at 1 Hz
(`com.android.server.wifi.qosmonitor.NetLinkDispatcher` in `system_server`;
vivo's newer V2 dispatcher, family 26/30, is never used here because its
`NL_MODULE_SPEC` probe fails on this kernel). The Xiaomi kernel has no handler,
so `socket()` fails with `EPROTONOSUPPORT` ~20 lines/s under load, plus a
10 Hz `recv()` poll once a request is outstanding.

This module registers protocol 24 and acks `type 11` with an empty 16-byte
message. Vivo's parser treats anything shorter than 36 bytes as "no data"
(`TcpInfo.parseKernelReply()` returns null) but still counts the reply as
handled, which drains `RttManager`'s request counter and stops the poll.

## Messages
| type | meaning | reply |
|---|---|---|
| 0 | start SLA | none |
| 9 | video choke query | none (would need 4 ints) |
| 10 | choke / wifiswitch control | none |
| 11 | per-uid TCP info, sent by `RttManager` every 1 s | empty 16-byte message |
| 13 / 14 / 15 | video thresholds / enable / params | none |
| 19 / 20 | ACC per-uid hints | none |
| 27 / 28 / 29 | UDP traffic and loss monitors | none |

## Params and counters
| name | meaning |
|---|---|
| `/sys/module/vivo_nl24/parameters/ack_mask` | which types get the empty ack, bitmask; default `2048` (`1<<11`), `0` = pure drain |
| `/sys/module/vivo_nl24/parameters/verbose` | log received messages to dmesg |
| `/proc/vivo_nl24` | `ack_mask`, `verbose`, `bad`, `other`, `acked`, `type[<n>] <count>` |

## Build
```sh
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL/out M=$PWD ARCH=arm64 LLVM=1 LLVM_IAS=1 modules
```
