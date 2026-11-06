#!/usr/bin/env bash
# Builds the Stage 0 probe into build/. Usage: ./build.sh
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p build
F="-mcpu=cortex-m7 -mthumb -O1 -g -ffreestanding -nostdlib -Wall -Wextra -Wno-override-init -Tprobe.ld"
for n in 1 2 3 4; do arm-none-eabi-gcc $F -DNODE_ID=${n}u -DUSE_PLL=1 probe.c -o build/probe$n.elf; done
arm-none-eabi-gcc $F -DNODE_ID=1u -DUSE_PLL=0 probe.c -o build/probe1_hsi.elf
arm-none-eabi-size build/*.elf
