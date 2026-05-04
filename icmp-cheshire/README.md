# Zephyr networking demo for Cheshire

## Getting Started

### Set Up the Environment

Before building, export the Zephyr base path:

```bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
```

### Build and Run

To build and run the application:

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
west build -p always -b cheshire icmp-cheshire/ -- -DDTC_OVERLAY_FILE="${PWD}/icmp-cheshire/boards/cheshire_smode.overlay"
```

The overlay adjusts the PLIC interrupt connection and memory base address for supervisor mode (OpenSBI occupies the first 2MB of DRAM).