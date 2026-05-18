# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr)
project(birtio-qemu)

zephyr_include_directories(src)
target_sources(app PRIVATE src/main.c src/virtio_shm_alloc.c)

