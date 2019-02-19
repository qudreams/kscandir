## kscandir

### Note
> kscandir is a simple kernel module to scan a directory in kernel like scan_dir(2) function.

### Usage
1. compile
> make
2. add kernel-module kscandir.ko:
> /sbin/insmod kscandir.ko dir_root=\`pwd\`
3. show result
> dmesg
4. remove kernel-module
> /sbin/rmmod kscandir.ko
