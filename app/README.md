# Zephyr Demo for Cheshire / CVA6 / QEMU (RISCV)

## Getting Started

### Set Up the Environment

Before building, export the Zephyr base path navigate to root folder and run:

```bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
```

### Build

To build the application (e.g., using QEMU):

```bash
west build -p always -b qemu_riscv64 app
```

To build for the Cheshire board:

```bash
west build -p always -b cheshire app
```

## Switching Between M-Mode and S-Mode

The execution mode is controlled through the `prj.conf` file in your application directory.

### M-Mode (Machine Mode) — Default

Use the following configuration in `prj.conf`:

```properties
# Default settings for M-Mode
CONFIG_RISCV_MACHINE_TIMER=y
CONFIG_RISCV_SUPERVISOR_TIMER=n

# Supervisor mode (comment the configs below to enable S-Mode)
# CONFIG_RISCV_SBI_BOOT=y
# CONFIG_RISCV_S_MODE=y
# CONFIG_RISCV_SUPERVISOR_TIMER=y
# CONFIG_RISCV_MACHINE_TIMER=n
```

### S-Mode (Supervisor Mode)

To switch to S-Mode, uncomment the supervisor configs and comment out the M-Mode ones:

```properties
# Default settings for M-Mode
# CONFIG_RISCV_MACHINE_TIMER=y
# CONFIG_RISCV_SUPERVISOR_TIMER=n

# Supervisor mode
CONFIG_RISCV_SBI_BOOT=y
CONFIG_RISCV_S_MODE=y
CONFIG_RISCV_SUPERVISOR_TIMER=y
CONFIG_RISCV_MACHINE_TIMER=n
```

When building in S-Mode, you must also apply the S-Mode devicetree overlay:

```bash
west build -p always -b cheshire app/ -- -DDTC_OVERLAY_FILE="app/boards/cheshire_smode.overlay"
```

The overlay adjusts the PLIC interrupt connection and memory base address for supervisor mode (OpenSBI occupies the first 2MB of DRAM).