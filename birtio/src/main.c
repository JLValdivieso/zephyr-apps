
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/net/net_if.h>
#ifdef CONFIG_VIRTIO_SHM_ALLOC
#include "virtio_shm_alloc.h"

/* Shared memory region from DTS overlay */
#define SHM_NODE  DT_NODELABEL(virtio_shm)
#define SHM_ADDR  DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE  DT_REG_SIZE(SHM_NODE)

/* Init shared memory allocator before drivers (PRE_KERNEL_1).
 * The virtio_mmio driver inits at POST_KERNEL, so this runs first.
 */
static int virtio_shm_sys_init(void)
{
	virtio_shm_init(SHM_ADDR, SHM_SIZE);
	return 0;
}

SYS_INIT(virtio_shm_sys_init, PRE_KERNEL_1, 0);
#endif

int main(void)
{
	printk("*** Virtio-net shared memory demo ***\n");
	printk("SHM base: 0x%lx, size: 0x%lx\n",
	       (unsigned long)SHM_ADDR, (unsigned long)SHM_SIZE);

	/* Wait for the network interface to come up */
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		printk("No network interface found\n");
		return -1;
	}

	printk("Network interface ready, use shell: net ping <ip>\n");

	return 0;
}


// #include <zephyr/kernel.h>
// #include <zephyr/net/net_if.h>
// #include <zephyr/net/net_core.h>
// #include <zephyr/net/icmp.h>
// #include <zephyr/net/net_ip.h>

// static struct net_icmp_ctx icmp_ctx;
// static struct k_sem ping_sem;
// static bool ping_received;

// static int ping_handler(struct net_icmp_ctx *ctx,
//                         struct net_pkt *pkt,
//                         struct net_icmp_ip_hdr *hdr,
//                         struct net_icmp_hdr *icmp_hdr,
//                         void *user_data)
// {
//     ping_received = true;
//     k_sem_give(&ping_sem);
//     return 0;
// }

// int main(void)
// {
//     struct net_if *iface;
//     struct sockaddr_in dst;
//     int ret;

//     printk("\n=== LowRISC Ethernet Ping Test ===\n");

//     k_sleep(K_SECONDS(3));

//     iface = net_if_get_default();
//     if (iface == NULL) {
//         printk("ERROR: No interface\n");
//         return 0;
//     }

//     struct net_linkaddr *ll = net_if_get_link_addr(iface);
//     printk("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
//            ll->addr[0], ll->addr[1], ll->addr[2],
//            ll->addr[3], ll->addr[4], ll->addr[5]);
//     printk("Link: %s\n", net_if_is_carrier_ok(iface) ? "UP" : "DOWN");

//     k_sem_init(&ping_sem, 0, 1);

//     /* Register ICMP echo reply handler */
//     ret = net_icmp_init_ctx(&icmp_ctx, AF_INET, NET_ICMPV4_ECHO_REPLY, 0, ping_handler);
//     if (ret < 0) {
//         printk("ERROR: ICMP init failed: %d\n", ret);
//         return 0;
//     }

//     /* Target: your PC */
//     memset(&dst, 0, sizeof(dst));
//     dst.sin_family = AF_INET;
//     net_addr_pton(AF_INET, "192.168.1.100",
//                   &dst.sin_addr);

//     printk("Pinging 192.168.1.100 ...\n\n");

//     for (int i = 0; i < 5; i++) {
//         ping_received = false;

//         struct net_icmp_ping_params params = {
//             .identifier = 0x1234,
//             .sequence = i,
//             .tc_tos = 0,
//         };

//         int64_t start = k_uptime_get();

//         ret = net_icmp_send_echo_request(&icmp_ctx, iface,
//                                          (struct sockaddr *)&dst,
//                                          &params, NULL);
//         if (ret < 0) {
//             printk("Ping %d: send failed (%d)\n", i, ret);
//         } else {
//             /* Wait up to 3 seconds for reply */
//             ret = k_sem_take(&ping_sem, K_SECONDS(3));
//             if (ret == 0 && ping_received) {
//                 int64_t elapsed = k_uptime_get() - start;
//                 printk("Ping %d: reply in %lld ms\n", i, elapsed);
//             } else {
//                 printk("Ping %d: timeout\n", i);
//             }
//         }

//         k_sleep(K_SECONDS(1));
//     }

//     net_icmp_cleanup_ctx(&icmp_ctx);
//     printk("\n=== Ping test complete ===\n");
//     return 0;
// }