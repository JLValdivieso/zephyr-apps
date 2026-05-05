# Zephyr RTOS demos

Run **Zephyr RTOS** on the **QEMU emulator** using the **RISC‑V
architecture**.

This guide shows how to:

-   Install QEMU
-   Install Zephyr and its dependencies
-   Build a Zephyr application
-   Run it in QEMU

------------------------------------------------------------------------

# Prerequisites

Minimum required versions:

-   **CMake ≥ 3.20.5**
-   **Python ≥ 3.12**
-   **Devicetree Compiler ≥ 1.4.6**

------------------------------------------------------------------------

# Installing QEMU

For **Debian-based distributions**:

``` bash
sudo apt update
sudo apt upgrade
sudo apt-get install qemu-system
```

This installs full system emulation support.

For other Linux distributions, follow the official instructions:

https://www.qemu.org/download/#linux

------------------------------------------------------------------------

# Installing Zephyr

Install the required dependencies:

``` bash
sudo apt install --no-install-recommends git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget python3-dev python3-venv python3-tk xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1
```

## Create a Virtual Environment

``` bash
python3 -m venv .venv
```

## Activate the Virtual Environment

``` bash
source .venv/bin/activate
```

Once activated, your shell will be prefixed with **(.venv)**.

To deactivate:

``` bash
deactivate
```

## Install west

``` bash
pip install west
```

## Retrieve the Zephyr Source Code

``` bash
west init -m https://github.com/JLValdivieso/zephyr.git --mr cheshire zephyrproject
cd zephyrproject
west update
```

## Export the Zephyr CMake Package

``` bash
west zephyr-export
```

This allows **CMake** to automatically load the required Zephyr build
configuration.

## Install Python Dependencies

``` bash
west packages pip --install
```

------------------------------------------------------------------------

# Installing the Zephyr SDK

The **Zephyr SDK** contains the toolchains required to build
applications for supported architectures.

Install it with:

``` bash
cd zephyr
west sdk install
```
------------------------------------------------------------------------

# Building the Example Application

This repository provides an external application that can be compiled
and executed on **QEMU**.

Navigate to the `zephyr_demo` directory and run:

``` bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
west build -p always -b qemu_riscv64 app
```

to run the application for cva6 core run:
``` bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
west build -p always -b cv64a6_genesys_2 app
``` 
After compilation, the build system will generate a **build** directory.

The generated binary is located at:

    build/zephyr/zephyr.elf

------------------------------------------------------------------------

# Building Network Stack

Zephyr provides a network stack that can be accessed through a serial terminal. To enable it, configure the required options in the `prj.conf file of your application (see the example in the icmp app).

To allow communication between the Zephyr instance (running in QEMU) and the host, you need to create and configure a TAP interface:

``` bash
sudo ip tuntap add dev tap0 mode tap 
sudo ip addr add 192.168.100.1/24 dev tap0 
sudo ip link set tap0 up 
```

Once the network interface is ready, build the application:
``` bash
export ZEPHYR_BASE=$(pwd)/zephyrproject/zephyr
west build -p always -b qemu_riscv64 icmp
``` 
------------------------------------------------------------------------

# Running the Application

## Using west

Run QEMU directly with:

``` bash
west build -t run
```

------------------------------------------------------------------------

## Running QEMU Manually

You can also run QEMU directly with the generated ELF:

``` bash
qemu-system-riscv64 \
  -nographic -M virt -cpu rv64 -m 256M -smp 1 \
  -bios none -kernel build/zephyr/zephyr.elf \
  -chardev stdio,id=con,mux=on -serial chardev:con \
  -mon chardev=con,mode=readline \
  -global virtio-mmio.force-legacy=false \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-device,netdev=net0
```
To run in the cva6 fork (no network stack) run:
``` bash
./path/to/qemu/fork/cva6/build/qemu-system-riscv64 -nographic -M cva6 -m 1G -smp 1 \ 
-kernel zephyr.elf \ 
-serial mon:stdio 
```

Running QEMU manually is recommended when more control over the virtual
machine configuration is required.

Exit QEMU by pressing `CTRL` + `A` `x`.
