# board_info - /sys/devices/soc1 identity nodes

OriginOS' "About phone" reads `core_num`, `cpu_type`, `user_cpu_freq` and
`cpu_freq` from ours socinfo driver.

| node | source |
|---|---|
| `core_num` | `num_possible_cpus()` → `8` |
| `cpu_freq` | max `cpuinfo_max_freq` over all cpufreq policies → `3187200` |
| `user_cpu_freq` | that frequency rounded to the 100 MHz marketing step → `3.20` |
| `cpu_type` | SoC platform code → `8550` (Settings maps it to `Snapdragon® 8 Gen 2 Mobile Platform`) |

`cpu_type` must be the **platform code**, not the marketing name. vivo's
Settings resolves it through `android.util.FtCpuInfo` →
`com.vivo.adapter.cpuinfo.CpuInfoImpl` (`framework-adapter.jar`), which builds
the resource name `cpuinfo_qcom<value>` and looks it up in `vivo-res.apk`:

* `8550` → `cpuinfo_qcom8550` = `Snapdragon® 8 Gen 2 Mobile Platform` ✅
* anything else (e.g. `8 Gen 2 Mobile Platform`) → lookup fails and the code
  falls back to `cpuinfo_qcom` (`Snapdragon`, **no ®**) + `" "` + value, which
  is what produced `Snapdragon 8 Gen 2 Mobile Platform`.

## Build
```sh
  make -C ~/PATH/TO/KERNEL O=~/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M=$PWD modules
```
