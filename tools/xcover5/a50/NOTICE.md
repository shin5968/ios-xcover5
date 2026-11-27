# The Galaxy A50 port's Wi-Fi files

From [nespaces/ios-a50-exynos9610](https://github.com/nespaces/ios-a50-exynos9610) at commit `eaccebe`, taken on
2026-11-27 to replace the XCover5's own Wi-Fi driver (`tools/xcover5/xc5_wlbt.c`, in git history). The A50 runs the
same Samsung SCSC Wi-Fi (Maxwell mx140) on an Exynos 9610; the XCover5's is on an Exynos 850.

That port's kernel files (`xnu/hakios.patch`, `xnu/files/`, `drivers/`) are a derivative work of Apple's XNU and are
licensed under the Apple Public Source License, Version 2.0 (its `LICENSE`, `LICENSE.APSL`); its tools are MIT.

| here | there | changed |
| --- | --- | --- |
| `hakios.sta.c` | `core/drivers/wifi/hakios.sta.c` | the standing network's file is `/private/var/xc5/hakios-wifi` (the drop zone) |
| `hakios.apple80211.c` | `core/drivers/wifi/hakios.apple80211.c` | a request repeated with the same answer is logged twice only |
| `hakios.net.c` | `core/drivers/net/hakios.net.c` | none |
| `hakios.wlan.cpp` | `core/drivers/wifi/hakios.wlan.cpp` | none |
| `hakios.lx_wpa.c`, `.h` | `core/drivers/wifi/hakios.lx_wpa.c`, `.h` | the kernel branch on XNU's `KERNEL` as well as `_KERNEL` |
| `hakios.sta.h`, `hakios.net.h`, `hakios.macaddr.h` | `core/xnu/files/bsd/sys/` | none |
| `hakios.input.h` | `core/xnu/files/bsd/sys/hakios.input.h` | only the three functions the Wi-Fi files use |
| `../xc5_scsc.c` | `core/drivers/wifi/hakios.scsc.c` | the Exynos 850's platform part (see its first comment) |
| `../dev/scsc-country.py` | `core/tools/a50-scsc-country.py` (MIT) | none but its header |

`../xc5_hakios_glue.c` is ours: what those files ask of the rest of the A50's kernel (its loader's log, its firmware
image, its partitions), made on this one. Firmware, the KA patch, the MIB files and the regulatory database come from
the user's own phone (`tools/xcover5/dev/wlbtfw.sh`) and are never committed.
