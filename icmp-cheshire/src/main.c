#include <zephyr/kernel.h>

int main(void)
{
    printk("Zephyr Ready\n");
    printk("Networking initialized\n");
    printk("Do: net iface to see interfaces\n");
    printk("Do: net ping 192.168.1.x to ping localhost\n");
    printk("Test 2\n");
    
    return 0;
}
