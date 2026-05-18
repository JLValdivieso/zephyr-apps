# Zephyr VirtIO Networking with Bao Hypervisor (RemoteIO)

## Overview

This application runs Zephyr as a frontend VM under the Bao hypervisor, using VirtIO over RemoteIO for network connectivity. The VirtIO drivers were modified to support Bao's RemoteIO backend, using a bump allocator over a reserved shared memory region for virtqueue and data buffers.

## Key Changes

- **Shared memory allocator**: `CONFIG_VIRTIO_SHM_ALLOC=y` enables a bump allocator on a reserved memory region (`0xAF000000`, 16MB) shared between the frontend (Zephyr) and backend VMs.
- **VirtIO MMIO device**: mapped at `0x30000000` with IRQ 61 on the PLIC.
- **Memory layout**: Zephyr's DRAM starts at `0xA0000000` (240MB), separate from the shared buffer region.

## Build

```bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
west build -p always -b qemu_riscv64 birtio-qemu/
```

## Switching Between M-Mode and S-Mode

Edit `prj.conf` to select the execution mode. Bao requires S-Mode.

### S-Mode (Default for Bao)

```properties
CONFIG_RISCV_SBI_BOOT=y
CONFIG_RISCV_S_MODE=y
CONFIG_RISCV_SUPERVISOR_TIMER=y
CONFIG_RISCV_MACHINE_TIMER=n
```

### M-Mode (standalone, without Bao)

```properties
CONFIG_RISCV_MACHINE_TIMER=y
CONFIG_RISCV_SUPERVISOR_TIMER=n
# CONFIG_RISCV_SBI_BOOT=y
# CONFIG_RISCV_S_MODE=y
# CONFIG_RISCV_SUPERVISOR_TIMER=y
# CONFIG_RISCV_MACHINE_TIMER=n
```

When using M-Mode, disable `CONFIG_VIRTIO_SHM_ALLOC` and use an overlay without the shared memory region.