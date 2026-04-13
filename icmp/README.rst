this commands are made for an individual application within the workspace

west build -b qemu_riscv64 appwest build -p always -b qemu_riscv64 icmp

to force a good building after some changes you made, run a clean comman as following inside the build folder: 

west build -t clean