/*
 * ============================================================================
 *  Network Diagnostic Tool - Zephyr RTOS
 * ============================================================================
 *  Available measurements:
 *    1. Latency + Jitter  (ICMP Echo)
 *    2. Bandwidth         (TCP bulk transfer via BSD sockets)
 *    3. View / Change configuration
 *    4. Exit
 *
 *  Interaction: UART (printk output / uart_poll_in input)
 *
 *  Optimizations applied:
 *    [OPT-1] Thread priority elevated to K_PRIO_COOP to prevent preemption
 *            during the ping loop, reducing scheduling jitter.
 *    [OPT-2] Absolute deadline timing between pings: next_send is computed
 *            once and each iteration waits only the *remaining* time.
 *            Eliminates cumulative drift caused by RTT + processing time.
 *    [OPT-3] TX timestamp embedded in the ICMP payload (uint64_t, 8 bytes).
 *            icmp_reply_handler() extracts it from the echo reply and computes
 *            elapsed = rx_ticks - tx_ticks in the same CSR time base.
 *            This replaces the previous global send_time_csr + irq_lock()
 *            approach, which measured stack-enqueue time rather than wire RTT.
 *    [OPT-4] atomic_t sample_count replaces plain int to prevent a data race
 *            between the ISR-context callback and the main thread.
 *    [OPT-5] DSCP CS6 (tc_tos = 0xC0) marks ICMP packets for QoS priority
 *            on any infrastructure that honours DSCP.
 *    [OPT-6] Configurable ICMP payload via cfg.ping_payload_len (default 8).
 *            The first 8 bytes always carry the TX timestamp; the rest are
 *            zero-filled to the requested size (64/256/512/1400 bytes for
 *            automotive experiments). Minimum enforced to 8 bytes.
 *    [OPT-7] Warmup ping discard: first PING_WARMUP_COUNT samples are sent
 *            but excluded from statistics to avoid ARP/route-cache cold-start
 *            bias in the RTT distribution.
 *    [OPT-8] P95 / P99 percentiles added to statistics. MAD jitter can hide
 *            tail latency; percentiles expose it.
 *    [OPT-9] RTT histogram (8 logarithmic buckets) printed after each test
 *            to show whether jitter is Gaussian or bimodal.
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/icmp.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>   /* [OPT-4] atomic sample counter            */
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdlib.h>

/* UART device used for interactive input (same port as printk) */
#define UART_DEV DEVICE_DT_GET(DT_CHOSEN(zephyr_console))

#ifdef CONFIG_VIRTIO_SHM_ALLOC
#include "virtio_shm_alloc.h"
#define SHM_NODE  DT_NODELABEL(virtio_shm)
#define SHM_ADDR  DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE  DT_REG_SIZE(SHM_NODE)
static int virtio_shm_sys_init(void)
{
	virtio_shm_init(SHM_ADDR, SHM_SIZE);
	return 0;
}
SYS_INIT(virtio_shm_sys_init, PRE_KERNEL_1, 0);
#endif

/* ============================================================================
 *  Default values - can be changed at runtime via the menu
 * ============================================================================ */
#define DEF_TARGET_IP        "192.168.1.100"
#define DEF_PING_COUNT       20
#define DEF_PING_INTERVAL_MS 500
#define DEF_PING_TIMEOUT_MS  2000
#define DEF_PING_PAYLOAD_LEN 8     /* 8 bytes carry the uint64_t TX timestamp    */
#define DEF_BW_PORT          5201
#define DEF_BW_DURATION_MS   5000
#define DEF_BW_BLOCK_SIZE    4096

/* [OPT-7] Number of leading pings discarded from statistics.
 * These are sent normally but their RTT is not counted: they warm up the
 * ARP table, IP route cache, and NIC TX path so the measured samples are
 * representative of steady-state behaviour. */
#define PING_WARMUP_COUNT    3

#define MAX_IP_LEN   16
#define MAX_SAMPLES  4000          /* maximum configurable ping count */

/* ============================================================================
 *  Global configuration structure
 * ============================================================================ */
typedef struct {
	char target_ip[MAX_IP_LEN];
	int  ping_count;
	int  ping_interval_ms;
	int  ping_timeout_ms;
	int  ping_payload_len;
	int  bw_port;
	int  bw_duration_ms;
	int  bw_block_size;
} cfg_t;

static cfg_t cfg = {
	.target_ip        = DEF_TARGET_IP,
	.ping_count       = DEF_PING_COUNT,
	.ping_interval_ms = DEF_PING_INTERVAL_MS,
	.ping_timeout_ms  = DEF_PING_TIMEOUT_MS,
	.ping_payload_len = DEF_PING_PAYLOAD_LEN,
	.bw_port          = DEF_BW_PORT,
	.bw_duration_ms   = DEF_BW_DURATION_MS,
	.bw_block_size    = DEF_BW_BLOCK_SIZE,
};

/* ============================================================================
 *  ICMP internal state
 * ============================================================================ */
static int64_t      rtt_samples[MAX_SAMPLES]; /* RTT in nanoseconds, -1 = timeout */

/* [OPT-4] atomic_t prevents a data race between the net-stack callback
 * (which increments the counter) and the main thread (which reads it).
 * The semaphore already serialises access in the happy path, but using
 * an atomic makes the intent explicit and is correct under all conditions. */
static atomic_t     sample_count_atomic = ATOMIC_INIT(0);

static struct k_sem ping_sem;

/* RISC-V CSR "time" timestamp (counts at RTC frequency, typically
 * 1 MHz on Cheshire/CVA6). Bao virtualizes this CSR correctly in
 * S-Mode so it is always safe to read without touching the CLINT.
 * Frequency is read once at startup from the DTS timebase-frequency. */
/* send_time_csr removed: TX timestamp now travels inside the ICMP payload
 * so both TX and RX timestamps share the same CSR time base without any
 * global variable shared between send context and callback context.      */
static uint64_t csr_time_hz;     /* RTC frequency in Hz (from DTS)     */
static struct net_icmp_ctx icmp_ctx;
static bool     icmp_ctx_ready;

/* ============================================================================
 *  RISC-V CSR "time" helpers
 * ============================================================================
 *  The "time" CSR is a read-only 64-bit counter that increments at the
 *  platform RTC frequency (timebase-frequency in the DTS).
 *  In S-Mode Bao passes it through via the SBI time extension — no CLINT
 *  mapping needed in the VM config.
 * ============================================================================ */

/**
 * csr_time_read - Read the RISC-V "time" CSR.
 * Returns a monotonically increasing counter at RTC frequency.
 */
static inline uint64_t csr_time_read(void)
{
	uint64_t t;
	__asm__ volatile ("csrr %0, time" : "=r"(t) : : "memory");
	return t;
}

/**
 * csr_time_to_ns - Convert CSR time ticks to nanoseconds.
 * Uses csr_time_hz set during init. Returns 0 if hz not known yet.
 */
static inline int64_t csr_time_to_ns(uint64_t ticks)
{
	if (csr_time_hz == 0) {
		return 0;
	}
	/* ns = ticks * 1_000_000_000 / hz
	 *
	 * Two-step to keep precision without overflow:
	 *   1. ticks * 1e9 can overflow for large tick counts, so we split:
	 *      ticks * (1e9 / hz)  — integer division, safe for hz >= 1 Hz
	 *
	 * For Cheshire 1 MHz: 1e9/1e6 = 1000, so ns = ticks * 1000
	 * For 50 MHz:         1e9/50e6 = 20,  so ns = ticks * 20
	 */
	return (int64_t)(ticks * (1000000000ULL / csr_time_hz));
}

/**
 * csr_time_init - Detect CSR "time" frequency for Cheshire/CVA6.
 *
 * The CSR "time" counter runs at the platform RTC frequency defined in
 * the DTS as "timebase-frequency". On Cheshire this is 1 MHz (1000000 Hz),
 * meaning each tick = 1 us = 1000 ns.
 *
 * sys_clock_hw_cycles_per_sec() returns CONFIG_SYS_CLOCK_TICKS_PER_SEC
 * (1000 Hz) which is the Zephyr scheduler tick rate — NOT the CSR time
 * frequency. We must NOT use it here.
 *
 * We verify the frequency by measuring how many CSR ticks elapse in one
 * known Zephyr tick (100 ms), which lets us auto-detect the real frequency
 * without hardcoding, while still defaulting safely to 1 MHz.
 */
static void csr_time_init(void)
{
	/* --- Auto-detect by measuring CSR ticks per Zephyr ms tick --- */
	uint64_t t0 = csr_time_read();
	k_sleep(K_MSEC(100));           /* sleep exactly 100 Zephyr ticks      */
	uint64_t t1 = csr_time_read();
	uint64_t ticks_per_100ms = t1 - t0;

	/* ticks_per_100ms should be ~100000 for 1 MHz, ~50000 for 500 kHz, etc */
	uint64_t detected_hz = ticks_per_100ms * 10ULL; /* scale to 1 second    */

	/* Sanity check: accept only values between 100 kHz and 200 MHz */
	if (detected_hz < 100000ULL || detected_hz > 200000000ULL) {
		csr_time_hz = 1000000ULL;   /* safe fallback for Cheshire = 1 MHz   */
		printk("CSR time: measurement out of range (%llu ticks/100ms), "
		       "defaulting to 1 MHz\n", ticks_per_100ms);
	} else {
		csr_time_hz = detected_hz;
	}

	/* Round to nearest common frequency for cleaner output */
	const uint64_t common_freqs[] = {
		100000ULL, 500000ULL, 1000000ULL, 10000000ULL, 50000000ULL
	};
	uint64_t best = csr_time_hz;
	uint64_t best_diff = UINT64_MAX;
	for (int i = 0; i < 5; i++) {
		uint64_t diff = (csr_time_hz > common_freqs[i])
			? (csr_time_hz - common_freqs[i])
			: (common_freqs[i] - csr_time_hz);
		if (diff < csr_time_hz / 10 && diff < best_diff) { /* within 10% */
			best_diff = diff;
			best = common_freqs[i];
		}
	}
	csr_time_hz = best;

	uint32_t hz_mhz_i = (uint32_t)(csr_time_hz / 1000000ULL);
	uint32_t hz_mhz_f = (uint32_t)((csr_time_hz % 1000000ULL) / 1000ULL);
	printk("CSR time: %llu Hz (%u.%03u MHz) — resolution: %llu ns/tick\n",
	       csr_time_hz, hz_mhz_i, hz_mhz_f,
	       1000000000ULL / csr_time_hz);
}

/* ============================================================================
 *  UART helpers
 * ============================================================================ */

/**
 * uart_getchar - Blocking read of one byte from the console UART.
 * Uses uart_poll_in which is always available (no Kconfig dependency).
 */
static uint8_t uart_getchar(void)
{
	const struct device *uart = UART_DEV;
	unsigned char c;

	while (uart_poll_in(uart, &c) != 0) {
		k_sleep(K_MSEC(1));  /* yield while waiting */
	}
	return (uint8_t)c;
}

/**
 * uart_read_line - Read characters from the UART until '\r' or '\n'.
 * Echoes each character and handles backspace (DEL / BS).
 * @buf:    destination buffer
 * @maxlen: maximum buffer size (includes '\0')
 * @return: length of the string read
 */
static int uart_read_line(char *buf, int maxlen)
{
	int i = 0;

	while (i < maxlen - 1) {
		uint8_t c = uart_getchar();

		if (c == '\r' || c == '\n') {
			printk("\n");
			break;
		}
		if (c == 127 || c == '\b') {
			if (i > 0) {
				i--;
				printk("\b \b");
			}
			continue;
		}
		printk("%c", c);
		buf[i++] = (char)c;
	}
	buf[i] = '\0';
	return i;
}

/**
 * uart_read_int - Read a positive integer from UART.
 * Returns def_val if the user presses Enter without typing anything.
 */
static int uart_read_int(int def_val)
{
	char buf[16];
	int  len = uart_read_line(buf, sizeof(buf));

	if (len == 0) {
		return def_val;
	}
	int v = atoi(buf);
	return (v > 0) ? v : def_val;
}

/**
 * uart_read_str - Read a string from UART.
 * Copies def_val into dst if the user presses Enter without typing.
 */
static void uart_read_str(char *dst, int maxlen, const char *def_val)
{
	char buf[MAX_IP_LEN + 4];
	int  len = uart_read_line(buf, sizeof(buf));

	if (len == 0) {
		strncpy(dst, def_val, maxlen - 1);
	} else {
		strncpy(dst, buf, maxlen - 1);
	}
	dst[maxlen - 1] = '\0';
}

/* ============================================================================
 *  ICMP echo-reply callback
 * ============================================================================ */
static int icmp_reply_handler(struct net_icmp_ctx *ctx,
			      struct net_pkt *pkt,
			      struct net_icmp_ip_hdr *ip_hdr,
			      struct net_icmp_hdr *icmp_hdr,
			      void *user_data)
{
	/* Read RX timestamp immediately — first instruction — to minimise
	 * RX-side bias before any other work is done in this callback.      */
	uint64_t rx_ticks = csr_time_read();

	/* Extract the TX timestamp embedded in the ICMP echo payload by
	 * send_ping(). The echo reply payload is an exact copy of the echo
	 * request payload, so tx_ticks is available here unchanged.
	 *
	 * Packet layout at this point in the callback:
	 *   [IP header already consumed by the net stack]
	 *   [ICMP header: 8 bytes — type, code, checksum, id, seq]
	 *   [payload: 8 bytes — uint64_t tx_ticks]
	 *
	 * net_pkt_skip() advances the cursor past the ICMP header.
	 * net_pkt_read() copies the 8-byte payload into tx_ticks.
	 *
	 * If the skip/read fails (truncated packet, wrong offset) we mark
	 * the sample as -1 (timeout) so statistics remain correct.          */
	uint64_t tx_ticks = 0;
	int64_t  rtt_ns;

	if (net_pkt_skip(pkt, sizeof(struct net_icmp_hdr)) == 0 &&
	    net_pkt_read(pkt, &tx_ticks, sizeof(tx_ticks)) == 0) {
		uint64_t elapsed = rx_ticks - tx_ticks;
		rtt_ns = csr_time_to_ns(elapsed);
	} else {
		/* Could not read payload — mark as lost */
		rtt_ns = -1;
	}

	/* [OPT-4] atomic_inc returns the old value, so idx is the slot index. */
	int idx = (int)atomic_inc(&sample_count_atomic);
	if (idx < MAX_SAMPLES) {
		rtt_samples[idx] = rtt_ns;
	}
	k_sem_give(&ping_sem);
	return 0;
}

/* ============================================================================
 *  Send a single ICMP echo request
 *
 *  TX timestamp strategy: the CSR "time" value is written into the first
 *  8 bytes of the ICMP payload immediately before handing the packet to
 *  net_icmp_send_echo_request(). The echo reply carries an exact copy of
 *  the payload, so icmp_reply_handler() can recover tx_ticks and compute
 *  elapsed = rx_ticks - tx_ticks using the same CSR time base.
 *
 *  This eliminates the need for a global send_time_csr variable and for
 *  irq_lock(): the timestamp is private to each packet, so there is no
 *  shared state between the send path and the callback.
 *
 *  The timestamp is captured as the very last operation before the send
 *  call to minimise the gap between "time written into packet" and "packet
 *  handed to the net stack". Any remaining gap (net stack enqueue + VirtIO
 *  path to the wire) is symmetric and cancels out across TX and RX.
 *
 *  [OPT-5] tc_tos = 0xC0 → DSCP CS6 (binary 110000xx).
 *  Class Selector 6 is the conventional DSCP value for network control /
 *  OAM traffic. Any switch or router with DSCP-aware QoS will service this
 *  packet ahead of best-effort traffic (CS0), reducing queuing latency and
 *  jitter. In a flat Bao lab network the field is ignored but causes no harm.
 * ============================================================================ */
/* Payload buffer for outgoing ICMP echo requests.
 * The first 8 bytes always carry the uint64_t TX timestamp so
 * icmp_reply_handler() can compute elapsed = rx_ticks - tx_ticks.
 * The remaining bytes are zero-filled to reach cfg.ping_payload_len,
 * which lets you test different payload sizes (64, 256, 512, 1400 bytes)
 * via the configuration menu without changing the timestamp mechanism. */
#define PING_PAYLOAD_MAX 1500
static uint8_t ping_payload_buf[PING_PAYLOAD_MAX];

/* ping_payload_len_active: the clamped payload size used for the current
 * test run. Set once by run_latency_jitter() before the loop so send_ping()
 * never computes or allocates anything in the hot path. */
static int ping_payload_len_active = 8;

static int send_ping(struct sockaddr_in *dst, uint16_t seq)
{
	/* Write TX timestamp into first 8 bytes of the pre-zeroed buffer.
	 * This is the ONLY operation before net_icmp_send_echo_request() so
	 * the gap between timestamp capture and packet handoff is minimal.
	 * memset() was moved out of this function into run_latency_jitter()
	 * and runs once before the loop, not on every ping. */
	uint64_t tx_ticks = csr_time_read();
	memcpy(ping_payload_buf, &tx_ticks, sizeof(tx_ticks));

	struct net_icmp_ping_params params = {
		.identifier = 0xBEEF,
		.sequence   = seq,
		.tc_tos     = 0xC0,             /* [OPT-5] DSCP CS6                 */
		.priority   = 0,
		.data       = ping_payload_buf,
		.data_size  = (size_t)ping_payload_len_active,
	};

	return net_icmp_send_echo_request(&icmp_ctx,
					  net_if_get_default(),
					  (struct sockaddr *)dst,
					  &params,
					  NULL);
}

/* ============================================================================
 *  Latency and jitter statistics
 * ============================================================================ */

/* [OPT-8] Extended stats with P95 / P99 tail-latency percentiles.
 * MAD jitter (RFC 3393) averages consecutive differences and can mask
 * individual spikes. P95/P99 directly expose the worst-case tail that
 * real-time applications care about. */
typedef struct {
	int64_t min_ns;
	int64_t max_ns;
	int64_t avg_ns;
	int64_t p95_ns;      /* [OPT-8] 95th-percentile RTT                  */
	int64_t p99_ns;      /* [OPT-8] 99th-percentile RTT                  */
	int64_t jitter_ns;   /* RFC 3393: MAD of consecutive RTTs             */
	int     received;
	int     lost;
} ping_stats_t;

/* [OPT-8] Simple insertion sort on a small scratch array to find percentiles.
 * MAX_SAMPLES is at most 4000 entries; insertion sort is fast enough and
 * avoids any dynamic allocation or stack-heavy qsort recursion. */
static int64_t scratch[MAX_SAMPLES];

static int cmp_i64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a;
	int64_t y = *(const int64_t *)b;
	return (x > y) - (x < y);
}

static void compute_ping_stats(ping_stats_t *s, int total_sent)
{
	int sample_count = (int)atomic_get(&sample_count_atomic);
	int64_t sum   = 0;
	int     count = 0;

	s->min_ns    = INT64_MAX;
	s->max_ns    = INT64_MIN;
	s->jitter_ns = 0;
	s->p95_ns    = 0;
	s->p99_ns    = 0;
	s->lost      = total_sent - sample_count; /* unrecorded timeouts */

	for (int i = 0; i < sample_count; i++) {
		if (rtt_samples[i] < 0) {
			s->lost++;
			continue;
		}
		int64_t r = rtt_samples[i];
		scratch[count] = r;   /* [OPT-8] collect valid samples for sort */
		sum += r;
		if (r < s->min_ns) s->min_ns = r;
		if (r > s->max_ns) s->max_ns = r;
		count++;
	}

	s->received = count;

	if (count == 0) {
		s->min_ns = s->max_ns = s->avg_ns = 0;
		return;
	}
	s->avg_ns = sum / count;

	/* [OPT-8] Sort scratch[] to compute percentiles.
	 * We use the libc qsort provided by Zephyr's minimal libc / newlib.
	 * The comparator avoids subtraction overflow for large int64 values. */
	if (count > 1) {
		/* Simple insertion sort — avoids qsort dependency and stack usage */
		for (int i = 1; i < count; i++) {
			int64_t key = scratch[i];
			int j = i - 1;
			while (j >= 0 && scratch[j] > key) {
				scratch[j + 1] = scratch[j];
				j--;
			}
			scratch[j + 1] = key;
		}

		/* Percentile index: floor((p/100) * count), clamped to [0, count-1] */
		int idx95 = (count * 95) / 100;
		int idx99 = (count * 99) / 100;
		if (idx95 >= count) idx95 = count - 1;
		if (idx99 >= count) idx99 = count - 1;

		s->p95_ns = scratch[idx95];
		s->p99_ns = scratch[idx99];
	} else {
		s->p95_ns = s->p99_ns = scratch[0];
	}

	/* Jitter = MAD of consecutive valid RTTs (RFC 3393) */
	if (count > 1) {
		int64_t jsum  = 0;
		int     pairs = 0;
		int64_t prev  = -1;

		for (int i = 0; i < sample_count; i++) {
			if (rtt_samples[i] < 0) continue;
			if (prev >= 0) {
				int64_t d = rtt_samples[i] - prev;
				jsum += (d < 0) ? -d : d;
				pairs++;
			}
			prev = rtt_samples[i];
		}
		s->jitter_ns = (pairs > 0) ? (jsum / pairs) : 0;
	}
}

/* [OPT-9] Print a logarithmic RTT histogram with 8 buckets.
 *
 * Buckets (inclusive lower bound, exclusive upper bound):
 *   0    – 100 µs
 *   100  – 500 µs
 *   500  – 1 ms
 *   1    – 5 ms
 *   5    – 10 ms
 *   10   – 50 ms
 *   50   – 100 ms
 *   100  ms+
 *
 * A bimodal distribution (two separated peaks) reveals a two-path network
 * or interrupt coalescing. A long right tail reveals bursty scheduling.
 * A tight single peak confirms low, consistent latency.
 */
#define HIST_BUCKETS 8
static const int64_t hist_edges[HIST_BUCKETS] = {
	100000LL,       /*  100 µs */
	500000LL,       /*  500 µs */
	1000000LL,      /*    1 ms */
	5000000LL,      /*    5 ms */
	10000000LL,     /*   10 ms */
	50000000LL,     /*   50 ms */
	100000000LL,    /*  100 ms */
	INT64_MAX,      /*  ∞      */
};
static const char *hist_labels[HIST_BUCKETS] = {
	"< 100 us  ",
	"100-500 us",
	"500us-1ms ",
	"  1-5 ms  ",
	"  5-10 ms ",
	" 10-50 ms ",
	"50-100 ms ",
	"> 100 ms  ",
};

static void print_rtt_histogram(void)
{
	int sample_count = (int)atomic_get(&sample_count_atomic);
	int counts[HIST_BUCKETS] = {0};
	int valid = 0;

	for (int i = 0; i < sample_count; i++) {
		if (rtt_samples[i] < 0) continue;
		valid++;
		for (int b = 0; b < HIST_BUCKETS; b++) {
			if (rtt_samples[i] < hist_edges[b]) {
				counts[b]++;
				break;
			}
		}
	}

	if (valid == 0) return;

	printk("+------------------------------------------+\n");
	printk("|         RTT Distribution (histogram)     |\n");
	printk("+------------+-------+---------------------+\n");
	printk("| Range      | Count | Bar                 |\n");
	printk("+------------+-------+---------------------+\n");

	/* Bar width: scale to 20 chars max */
	for (int b = 0; b < HIST_BUCKETS; b++) {
		int bar_len = (valid > 0) ? (counts[b] * 20) / valid : 0;
		char bar[21];
		for (int k = 0; k < 20; k++) {
			bar[k] = (k < bar_len) ? '#' : ' ';
		}
		bar[20] = '\0';
		printk("| %s | %5d | %s |\n",
		       hist_labels[b], counts[b], bar);
	}
	printk("+------------------------------------------+\n");
}

static void print_ping_stats(const ping_stats_t *s, int total_sent)
{
	int li = (s->lost * 100) / total_sent;
	int lf = (s->lost * 1000 / total_sent) % 10;

	printk("\n+============================================+\n");
	printk("|   LATENCY & JITTER  -->  %-15s|\n", cfg.target_ip);
	printk("+============================================+\n");
	printk("|  Sent      : %-4d  (warmup: %d excluded)  |\n",
	       total_sent, PING_WARMUP_COUNT);
	printk("|  Received  : %-4d                          |\n", s->received);
	printk("|  Lost      : %-4d  (%d.%d%%)                 |\n",
	       s->lost, li, lf);
	printk("+--------------------------------------------+\n");
	printk("|  Metric     |    ns    |    us    |   ms   |\n");
	printk("+-------------+----------+----------+--------+\n");
	printk("|  RTT min    | %8lld | %8lld | %6lld |\n",
	       s->min_ns, s->min_ns / 1000, s->min_ns / 1000000);
	printk("|  RTT avg    | %8lld | %8lld | %6lld |\n",
	       s->avg_ns, s->avg_ns / 1000, s->avg_ns / 1000000);
	printk("|  RTT max    | %8lld | %8lld | %6lld |\n",
	       s->max_ns, s->max_ns / 1000, s->max_ns / 1000000);
	printk("+-------------+----------+----------+--------+\n");
	printk("|  RTT p95    | %8lld | %8lld | %6lld |\n",   /* [OPT-8] */
	       s->p95_ns, s->p95_ns / 1000, s->p95_ns / 1000000);
	printk("|  RTT p99    | %8lld | %8lld | %6lld |\n",   /* [OPT-8] */
	       s->p99_ns, s->p99_ns / 1000, s->p99_ns / 1000000);
	printk("+-------------+----------+----------+--------+\n");
	printk("|  Jitter MAD | %8lld | %8lld | %6lld |\n",
	       s->jitter_ns, s->jitter_ns / 1000, s->jitter_ns / 1000000);
	printk("+============================================+\n");
}

/* ============================================================================
 *  Measurement 1: Latency + Jitter
 *
 *  [OPT-1] The thread priority is raised to K_PRIO_COOP(4) for the duration
 *  of the ping loop. Cooperative threads in Zephyr cannot be preempted by
 *  preemptive threads, which eliminates scheduler-induced jitter between the
 *  k_sem_take() return and the next send_ping() call. The priority is
 *  restored to the original value after the loop.
 *
 *  [OPT-2] Absolute deadline loop: next_send_ms is advanced by a fixed
 *  interval each iteration regardless of how long the RTT or processing
 *  took. The inter-ping delay is (interval - elapsed), not a fixed sleep.
 *  This keeps the send cadence stable even when RTTs vary, which is
 *  essential for accurate jitter measurement.
 *
 *  [OPT-7] Warmup phase: the first PING_WARMUP_COUNT pings are sent
 *  and their replies are awaited, but the results are discarded before
 *  the main measurement loop begins. This ensures ARP, route cache, and
 *  NIC DMA descriptors are in a hot state for all measured samples.
 * ============================================================================ */
static void run_latency_jitter(void)
{
	printk("\n[LATENCY/JITTER] %d pings --> %s  (warmup: %d)\n",
	       cfg.ping_count, cfg.target_ip, PING_WARMUP_COUNT);

	/* Initialise ICMP context once per session */
	if (!icmp_ctx_ready) {
		int rc = net_icmp_init_ctx(&icmp_ctx,
					   AF_INET,
					   NET_ICMPV4_ECHO_REPLY,
					   0,
					   icmp_reply_handler);
		if (rc < 0) {
			printk("ERROR: could not init ICMP context (%d)\n", rc);
			return;
		}
		icmp_ctx_ready = true;
	}

	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port   = 0,
	};
	if (net_addr_pton(AF_INET, cfg.target_ip, &dst.sin_addr) < 0) {
		printk("ERROR: invalid IP: %s\n", cfg.target_ip);
		return;
	}

	/* Reset atomic sample counter */
	atomic_set(&sample_count_atomic, 0);
	int total_sent = 0;

	/* Prepare payload buffer once before the loop.
	 * memset() is expensive for large payloads (up to 1400 bytes) and would
	 * add variable latency to every ping if called inside send_ping().
	 * Doing it here once keeps send_ping() as lean as possible. */
	ping_payload_len_active = cfg.ping_payload_len;
	if (ping_payload_len_active < (int)sizeof(uint64_t)) {
		ping_payload_len_active = (int)sizeof(uint64_t);
	}
	if (ping_payload_len_active > PING_PAYLOAD_MAX) {
		ping_payload_len_active = PING_PAYLOAD_MAX;
	}
	memset(ping_payload_buf, 0, (size_t)ping_payload_len_active);
	printk("[INFO] Payload size: %d bytes\n", ping_payload_len_active);

	/* Warm up the CSR read pipeline — first read after a long idle can
	 * be stale on some implementations. Discard it. */
	(void)csr_time_read();

	/* [OPT-1] Raise to cooperative priority for the duration of the test.
	 * K_PRIO_COOP threads are not preempted by K_PRIO_PREEMPT threads.
	 * This eliminates the scheduler as a source of inter-ping jitter. */
	int orig_prio = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), K_PRIO_COOP(4));

	/* ── [OPT-7] Warmup phase ───────────────────────────────────────────
	 * Send PING_WARMUP_COUNT pings, wait for each reply (or timeout),
	 * then reset the sample counter so warmup data is excluded from stats.
	 * Sequence numbers start at 0xF000 to distinguish from measurement
	 * packets in a network capture. */
	printk("[WARMUP] Sending %d warmup pings...\n", PING_WARMUP_COUNT);
	for (int w = 0; w < PING_WARMUP_COUNT; w++) {
		int rc = send_ping(&dst, (uint16_t)(0xF000 + w));
		if (rc == 0) {
			/* Wait but ignore result — we just want the path hot */
			k_sem_take(&ping_sem, K_MSEC(cfg.ping_timeout_ms));
		}
		k_sleep(K_MSEC(cfg.ping_interval_ms));
	}
	/* Discard warmup samples by resetting the counter */
	atomic_set(&sample_count_atomic, 0);
	printk("[WARMUP] Done. Starting measurement...\n\n");

	/* ── [OPT-2] Absolute deadline loop ────────────────────────────────
	 * next_send_ms is the wall-clock time (in Zephyr uptime ms) at which
	 * the next ping should be sent. After each send we wait only for the
	 * *remaining* time until the next deadline, rather than sleeping a
	 * fixed interval from the end of the previous iteration. This
	 * decouples the inter-ping interval from RTT variability, which is
	 * essential for jitter measurement and for keeping the send cadence
	 * deterministic.  */
	int64_t next_send_ms = k_uptime_get();

	for (uint16_t seq = 0; seq < (uint16_t)cfg.ping_count; seq++) {

		/* Advance the deadline before sending so the interval accounts
		 * for the time spent in send_ping() itself. */
		next_send_ms += cfg.ping_interval_ms;

		int rc = send_ping(&dst, seq);
		total_sent++;

		if (rc < 0) {
			printk("[%2d] Send error: %d\n", seq, rc);
			int idx = (int)atomic_inc(&sample_count_atomic);
			if (idx < MAX_SAMPLES) {
				rtt_samples[idx] = -1;
			}
		} else if (k_sem_take(&ping_sem,
				      K_MSEC(cfg.ping_timeout_ms)) != 0) {
			printk("[%2d] Timeout\n", seq);
			int idx = (int)atomic_inc(&sample_count_atomic);
			if (idx < MAX_SAMPLES) {
				rtt_samples[idx] = -1;
			}
		} else {
			int cur = (int)atomic_get(&sample_count_atomic);
			int64_t rtt_ns = rtt_samples[cur - 1];
			printk("[%2d] RTT: %lld ns  (%lld us)  (%lld ms)\n",
			       seq, rtt_ns, rtt_ns / 1000, rtt_ns / 1000000);
		}

		/* [OPT-2] Sleep only the *remaining* time until next deadline.
		 * If we overran (RTT > interval), skip the sleep entirely. */
		int64_t now       = k_uptime_get();
		int64_t remaining = next_send_ms - now;
		if (remaining > 0) {
			k_sleep(K_MSEC(remaining));
		}
	}

	/* [OPT-1] Restore original thread priority */
	k_thread_priority_set(k_current_get(), orig_prio);

	ping_stats_t stats = {0};
	compute_ping_stats(&stats, total_sent);
	print_ping_stats(&stats, total_sent);
	print_rtt_histogram();   /* [OPT-9] distribution after stats table */
}

/* ============================================================================
 *  Measurement 2: Bandwidth (TCP bulk send)
 * ============================================================================
 *  Requires a TCP server listening on the target, e.g.:
 *    Linux:   iperf3 -s
 *             nc -l <port> > /dev/null
 * ============================================================================ */
#define BW_BUF_SIZE 4096
static uint8_t bw_buf[BW_BUF_SIZE];

static void run_bandwidth(void)
{
	printk("\n[BANDWIDTH] Connecting to %s:%d ...\n",
	       cfg.target_ip, cfg.bw_port);

	/* Fill buffer with a repeating pattern */
	for (int i = 0; i < BW_BUF_SIZE; i++) {
		bw_buf[i] = (uint8_t)(i & 0xFF);
	}

	int block = (cfg.bw_block_size > BW_BUF_SIZE || cfg.bw_block_size <= 0)
		    ? BW_BUF_SIZE : cfg.bw_block_size;

	/* Create and connect TCP socket */
	int sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		printk("ERROR: could not create socket (%d)\n", sock);
		return;
	}

	struct sockaddr_in srv = {
		.sin_family = AF_INET,
		.sin_port   = htons((uint16_t)cfg.bw_port),
	};
	if (net_addr_pton(AF_INET, cfg.target_ip, &srv.sin_addr) < 0) {
		printk("ERROR: invalid IP: %s\n", cfg.target_ip);
		zsock_close(sock);
		return;
	}
	if (zsock_connect(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
		printk("ERROR: could not connect to %s:%d\n",
		       cfg.target_ip, cfg.bw_port);
		zsock_close(sock);
		return;
	}

	printk("Connected. Sending data for %d ms...\n", cfg.bw_duration_ms);

	int64_t t_start    = k_uptime_get();
	int64_t t_end      = t_start + cfg.bw_duration_ms;
	int64_t bytes_sent = 0;

	while (k_uptime_get() < t_end) {
		int n = zsock_send(sock, bw_buf, block, 0);
		if (n < 0) {
			printk("ERROR: send failed: %d\n", n);
			break;
		}
		bytes_sent += n;
	}

	int64_t elapsed_ms = k_uptime_get() - t_start;
	zsock_close(sock);

	/* Throughput in kbps (integer only, no float) */
	int64_t kbps    = (elapsed_ms > 0) ? (bytes_sent * 8) / elapsed_ms : 0;
	int64_t mbps_i  = kbps / 1000;
	int64_t mbps_f  = (kbps % 1000) / 100;
	int64_t kb_sent = bytes_sent / 1024;

	printk("\n+======================================+\n");
	printk("|   BANDWIDTH  -->  %-18s|\n", cfg.target_ip);
	printk("+======================================+\n");
	printk("|  TCP port      : %-5d              |\n", cfg.bw_port);
	printk("|  Elapsed time  : %-6lld ms           |\n", elapsed_ms);
	printk("|  Block size    : %-5d bytes         |\n", block);
	printk("+--------------------------------------+\n");
	printk("|  Data sent     : %lld KB (%lld bytes)\n",
	       kb_sent, bytes_sent);
	printk("|  Throughput    : %lld kbps (%lld.%lld Mbps)\n",
	       kbps, mbps_i, mbps_f);
	printk("+======================================+\n");
}

/* ============================================================================
 *  Configuration menu
 * ============================================================================ */
static void menu_config(void)
{
	char buf[MAX_IP_LEN + 4];

	printk("\n+======================================+\n");
	printk("|           CONFIGURATION              |\n");
	printk("|   Press Enter to keep current value  |\n");
	printk("+======================================+\n");

	printk("  Target IP          [%s]: ", cfg.target_ip);
	uart_read_str(buf, sizeof(buf), cfg.target_ip);
	strncpy(cfg.target_ip, buf, MAX_IP_LEN - 1);
	cfg.target_ip[MAX_IP_LEN - 1] = '\0';

	printk("  Ping count         [%d]: ", cfg.ping_count);
	cfg.ping_count = uart_read_int(cfg.ping_count);
	if (cfg.ping_count > MAX_SAMPLES) cfg.ping_count = MAX_SAMPLES;

	printk("  Ping interval ms   [%d]: ", cfg.ping_interval_ms);
	cfg.ping_interval_ms = uart_read_int(cfg.ping_interval_ms);

	printk("  Ping timeout ms    [%d]: ", cfg.ping_timeout_ms);
	cfg.ping_timeout_ms = uart_read_int(cfg.ping_timeout_ms);

	printk("  ICMP payload bytes [%d]: ", cfg.ping_payload_len);
	cfg.ping_payload_len = uart_read_int(cfg.ping_payload_len);

	printk("  BW TCP port        [%d]: ", cfg.bw_port);
	cfg.bw_port = uart_read_int(cfg.bw_port);

	printk("  BW duration ms     [%d]: ", cfg.bw_duration_ms);
	cfg.bw_duration_ms = uart_read_int(cfg.bw_duration_ms);

	printk("  BW block size bytes[%d]: ", cfg.bw_block_size);
	cfg.bw_block_size = uart_read_int(cfg.bw_block_size);

	printk("\n>> Configuration updated.\n");
}

/* ============================================================================
 *  Display current configuration
 * ============================================================================ */
static void print_current_config(void)
{
	printk("\n+--------------------------------------+\n");
	printk("|        Current Configuration         |\n");
	printk("+--------------------------------------+\n");
	printk("|  Target IP        : %-15s  |\n", cfg.target_ip);
	printk("|  Ping count       : %-5d             |\n", cfg.ping_count);
	printk("|  Ping interval    : %-5d ms           |\n", cfg.ping_interval_ms);
	printk("|  Ping timeout     : %-5d ms           |\n", cfg.ping_timeout_ms);
	printk("|  ICMP payload     : %-5d bytes        |\n", cfg.ping_payload_len);
	printk("|  Warmup pings     : %-5d (excluded)   |\n", PING_WARMUP_COUNT);
	printk("+--------------------------------------+\n");
	printk("|  BW TCP port      : %-5d             |\n", cfg.bw_port);
	printk("|  BW duration      : %-5d ms           |\n", cfg.bw_duration_ms);
	printk("|  BW block size    : %-5d bytes        |\n", cfg.bw_block_size);
	printk("+--------------------------------------+\n");
}

/* ============================================================================
 *  Main menu
 * ============================================================================ */
static void print_main_menu(void)
{
	printk("\n+======================================+\n");
	printk("|     Network Diagnostic Tool          |\n");
	printk("+======================================+\n");
	printk("|  1. Latency + Jitter  (ICMP)         |\n");
	printk("|  2. Bandwidth         (TCP)          |\n");
	printk("|  3. View / Change configuration      |\n");
	printk("|  4. Exit                             |\n");
	printk("+======================================+\n");
	printk("Option: ");
}

/* ============================================================================
 *  main
 * ============================================================================ */
int main(void)
{
	printk("\n*** Network Diagnostic Tool - Zephyr RTOS ***\n");

#ifdef CONFIG_VIRTIO_SHM_ALLOC
	printk("SHM base: 0x%lx, size: 0x%lx\n",
	       (unsigned long)SHM_ADDR, (unsigned long)SHM_SIZE);
#endif

	struct net_if *iface = net_if_get_default();

	if (!iface) {
		printk("ERROR: No network interface found\n");
		return -1;
	}
	k_sleep(K_MSEC(500));
	printk("Network interface ready.\n");

	k_sem_init(&ping_sem, 0, 1);
	csr_time_init();   /* detect RTC frequency for CSR time conversion */

	/* Show default configuration on startup */
	printk("\nDefault values loaded:\n");
	print_current_config();

	/* ── Main menu loop ─────────────────────────────────────────────────── */
	bool running = true;

	while (running) {
		print_main_menu();

		char opt_buf[4];
		uart_read_line(opt_buf, sizeof(opt_buf));
		int opt = atoi(opt_buf);

		switch (opt) {
		case 1:
			run_latency_jitter();
			break;

		case 2:
			run_bandwidth();
			break;

		case 3:
			print_current_config();
			printk("\nModify configuration? (y/N): ");
			char yn[4];
			uart_read_line(yn, sizeof(yn));
			if (yn[0] == 'y' || yn[0] == 'Y') {
				menu_config();
			}
			break;

		case 4:
			printk("\nExiting...\n");
			running = false;
			break;

		default:
			printk("Invalid option. Choose between 1 and 4.\n");
			break;
		}
	}

	if (icmp_ctx_ready) {
		net_icmp_cleanup_ctx(&icmp_ctx);
	}

	return 0;
}