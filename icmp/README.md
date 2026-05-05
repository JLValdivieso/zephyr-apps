# Zephyr QEMU RISC-V Networking Demo

## Build

Navigate to root folder and run:

```bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
west build -p always -b qemu_riscv64 icmp
```

## Switching Between M-Mode and S-Mode

Edit `prj.conf` to select the execution mode.

### M-Mode (Default)

```properties
CONFIG_RISCV_MACHINE_TIMER=y
CONFIG_RISCV_SUPERVISOR_TIMER=n
# CONFIG_RISCV_SBI_BOOT=y
# CONFIG_RISCV_S_MODE=y
# CONFIG_RISCV_SUPERVISOR_TIMER=y
# CONFIG_RISCV_MACHINE_TIMER=n
```

### S-Mode

```properties
# CONFIG_RISCV_MACHINE_TIMER=y
# CONFIG_RISCV_SUPERVISOR_TIMER=n
CONFIG_RISCV_SBI_BOOT=y
CONFIG_RISCV_S_MODE=y
CONFIG_RISCV_SUPERVISOR_TIMER=y
CONFIG_RISCV_MACHINE_TIMER=n
```