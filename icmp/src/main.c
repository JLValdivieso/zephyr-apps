#include <zephyr/kernel.h>

void main(void)
{
    printk("Zephyr Ready\n");
    printk("Networking initialized\n");
    printk("Do: net iface to see interfaces\n");
    printk("Do: net ping 192.168.100.x to ping localhost\n");
}