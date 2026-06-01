# How To Use

## Download the correct version of sdfgen
```
git clone git@github.com:au-ts/microkit_sdf_gen.git
cd microkit_sdf_gen
git checkout joshua/unbacked_pd
```
And install.

## Download Microkit
```
git clone git@github.com:au-ts/microkit.git
cd microkit
git checkout joshua/pdpager
python build_sdk.py --sel4=/path/to/seL4 --board=maaxboard,qemu_virt_aarch64 --configs=benchmark
```
## Run LionsOs System
```
cd examples/pager_memory_manager
make qemu 
# OR
make
# and load the image to the maaxboard
```
## Requirements
On maaxboard:
- sd card with 4 partitions, last two fat32
- partition 2 (0 indexed) with benchmark data files
- large partition 3 (for swap)
On qemu
- mmcblk0 with 4 partitions, last two fat32
- partition 2 (0 indexed) with benchmark data files
- large partition 3 (for swap)
