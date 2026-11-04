# Xiaomi13 fake splash
Paints a static image into the framebuffer of bootloader already owns, so our splash replaces the imagefv one after like 0.3s
which is better than nothing imo. maybe someone can improve this to make it instant :D 🐥.


## Build
```
make -C /PATH/TO/KERNEL O=/PATH/TO/KERNEL/out ARCH=arm64 LLVM=1 LLVM_IAS=1 M="$PWD" modules
```

## Parameters for fuxi
```
base=0xb8000000   (DT splash_region)
size=0x2c00000    (DT overrides it)
panel_w=1080 panel_h=2400
bpp=4             XRGB8888
pitch=0           bytes per line; 0 = ALIGN(panel_w,64)*bpp = 4320 = 1080*4
```

## init_boot:
```
/init_boot/ramdisk/init                        shim
/init_boot/ramdisk/init.real                   the stock init
/init_boot/ramdisk/fuxi_splash.ko  the kernel module
/init_boot/ramdisk/splash.rgb      splash
```
🐥 hello
