/*
 * Bluetooth on the POCO F6 / Redmi Turbo 3 (peridot): the WCN6750 ("moselle")
 * behind QUPv3 serial engine 2 on UART 0x888000, as a raw H4 transport for
 * iOS's bluetoothd (sys/hakios.bluetooth.h).
 *
 * There is no clock, pin, regulator, RPMh or tty framework here, so this does
 * what the vendor's Bluetooth power and UART drivers do, in their order:
 *   clocks   the 150 MHz qspi_ref root, then the QUP wrapper's core,
 *            core_2x and s2 branches, then the s2 divider; the GENI serial
 *            clock is programmed for 16x oversampling at 115200
 *   power    the WCN6750's rails through RPMh, then BT_EN low/high with the
 *            WLAN reset and SW_CTRL handshake (drivers/bluetooth/btpower.c)
 *   pins     GPIO40-43 to qup1_se2_l0..l3, GPIO156 as the reset output,
 *            GPIO80 as SW_CTRL input, GPIO35 as the shared WLAN reset input
 *   firmware the ROM version request, patch config, ELF/TLV patch, NVM, then
 *            HCI reset and the controller's address and command mask
 *   host     a PF_SYSTEM stream endpoint named com.hakios.bluetooth
 *
 * Boot args: bt=1 starts it; bt_step=1..4 stops after that many steps.  Only
 * the Peridot wiring is known here, and nothing is touched without bt= or
 * bt_step=.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/hakios.bluetooth.h>
#include <sys/hakios.board.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

#define HAKIOS_SOC_BASE         0x00400000ul
extern vm_offset_t pe_arm_get_soc_base_phys(void);

static inline uint32_t
bt_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);

	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static inline void
bt_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static inline uint32_t
bt_read_addr(vm_offset_t address)
{
	return bt_read(address, 0);
}

static inline void
bt_write_addr(vm_offset_t address, uint32_t value)
{
	bt_write(address, 0, value);
}

static void
bt_sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
bt_sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- board -------------------------------------------------------------------------------------- */

#define BT_UART_PHYS            0x00888000ul
#define BT_UART_SIZE            0x4000ul
#define BT_GCC_PHYS             0x00100000ul
#define BT_GCC_SIZE             0x00080000ul
#define BT_TLMM_PHYS            0x0f100000ul
#define BT_TLMM_SIZE            0x00100000ul

static vm_offset_t uart, gcc, tlmm;
static vm_offset_t uart_phys = BT_UART_PHYS;
static vm_offset_t gcc_phys = BT_GCC_PHYS;
static vm_offset_t tlmm_phys = BT_TLMM_PHYS;
static unsigned bt_reset_pin = 156;
static unsigned bt_sw_pin = 80;
static unsigned wlan_reset_pin = 35;

/* ---- pins (TLMM) -------------------------------------------------------------------------------- */

#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	bt_write_addr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	bt_write_addr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return bt_read_addr(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/*
 * Peridot's UART pins select qup1_se2_l0..l3.  Those are function indices
 * 193..196 in pinctrl-cliffs, but each is the first alternate (f1) of its
 * pin, so the TLMM mux field is 1.  The pulls are the stock pinctrl's.
 */
static void
bt_pins_uart(void)
{
	gpio_configure(40, 1, PULL_NONE, 2, false);     /* CTS */
	gpio_configure(41, 1, PULL_DOWN, 2, false);     /* RTS */
	gpio_configure(42, 1, PULL_UP, 2, false);       /* TX */
	gpio_configure(43, 1, PULL_NONE, 2, false);     /* RX */
}

/* ---- RPMh --------------------------------------------------------------------------------------- */

#define CMD_DB_SIZE             0x20000ul
#define RSC_SIZE                0x10000ul
#define TCS_ID                  0
#define TCS(reg)                (rsc + 0xd00 + 672 * TCS_ID + (reg))
#define TCS_COMMON(reg)         (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CMD_STRIDE          24
#define TCS_WAIT_FOR_COMPLETION 0x20
#define TCS_CONTROL             0x24
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x28
#define TCS_CMD_ENABLE          0x2c
#define TCS_CMD_MSGID(n)        (0x34 + TCS_CMD_STRIDE * (n))
#define TCS_CMD_ADDR(n)         (TCS_CMD_MSGID(n) + 4)
#define TCS_CMD_DATA(n)         (TCS_CMD_MSGID(n) + 8)
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_HPM            7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static vm_offset_t rsc, cmd_db;

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db == NULL || db->magic[0] != 0xdb || db->magic[1] != 0x30 ||
	    db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		hakios_tl("bt: no RPMh command database\n");
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512 ||
		    slave->count > (CMD_DB_SIZE - slave->header_offset) / sizeof(struct cmd_db_entry)) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((bt_read_addr(TCS_COMMON(reg)) & mask) == value) {
			return true;
		}
		bt_sleep_us(100);
	}
	hakios_tl("bt: RPMh timeout waiting for %s (register %#x = %#x)\n", what, reg,
	    bt_read_addr(TCS_COMMON(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = bt_read_addr(TCS_COMMON(TCS_IRQ_ENABLE));
	uint32_t irq_bit = 1u << TCS_ID;
	bool done;

	bt_write_addr(TCS(TCS_CMD_ENABLE), 0);
	bt_write_addr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	bt_write_addr(TCS(TCS_CONTROL), bt_read_addr(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	bt_write_addr(TCS(TCS_CONTROL), bt_read_addr(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	bt_write_addr(TCS_COMMON(TCS_IRQ_CLEAR), irq_bit);
	for (unsigned i = 0; i < count; i++) {
		bt_write_addr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		bt_write_addr(TCS(TCS_CMD_ADDR(i)), address[i]);
		bt_write_addr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	bt_write_addr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	bt_write_addr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	bt_write_addr(TCS_COMMON(TCS_IRQ_ENABLE), irq_enable | irq_bit);
	bt_write_addr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	bt_write_addr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, irq_bit, irq_bit, "the request");
	bt_write_addr(TCS(TCS_CONTROL), bt_read_addr(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	bt_write_addr(TCS(TCS_CONTROL), bt_read_addr(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	bt_write_addr(TCS(TCS_CMD_ENABLE), 0);
	bt_write_addr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	bt_write_addr(TCS_COMMON(TCS_IRQ_CLEAR), irq_bit);
	bt_write_addr(TCS_COMMON(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static bool
rpmh_maps(void)
{
	if (rsc == 0) {
		rsc = ml_io_map(hakios_board_addr("rsc", 0x17a20000ul), RSC_SIZE);
	}
	if (cmd_db == 0) {
		cmd_db = ml_io_map_wcomb(hakios_board_addr("cmd_db", 0x81c60000ul), CMD_DB_SIZE);
	}
	if (rsc == 0 || cmd_db == 0) {
		hakios_tl("bt: cannot map RPMh (rsc %#lx, cmd-db %#lx)\n", (unsigned long)rsc,
		    (unsigned long)cmd_db);
		return false;
	}
	return true;
}

static bool
regulator_on(const char *name, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16 */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		hakios_tl("bt: RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	hakios_tl("bt: %s on (%u mV)\n", name, millivolts);
	return true;
}

/* Stock order: I/O, AON (shared by rfacmn/dig/rfa-0p8), RFA1, RFA2, IPA-2p2. */
static bool
bt_power_rails(void)
{
	static const struct {
		const char *name;
		uint32_t mv;
	} rails[] = {
		{ "ldob7", 1800 },
		{ "smpb3", 984 },
		{ "smpb1", 1856 },
		{ "smpb2", 1256 },
		{ "smpe3", 1600 },
	};
	bool ok = true;

	if (!rpmh_maps()) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		ok &= regulator_on(rails[i].name, rails[i].mv);
	}
	return ok;
}

/* ---- UART clock --------------------------------------------------------------------------------- */

#define GCC_QUPV3_WRAP1_CORE_2X_CBCR  0x52008
#define GCC_QUPV3_WRAP1_CORE_CBCR     0x52008
#define GCC_QUPV3_WRAP1_S2_CBCR       0x52008
#define GCC_QUPV3_WRAP1_CORE_2X_HALT  0x23154
#define GCC_QUPV3_WRAP1_CORE_HALT     0x23144
#define GCC_QUPV3_WRAP1_S2_HALT       0x18274
#define GCC_QUPV3_WRAP1_S2_DIV        0x18280
#define GCC_QUPV3_WRAP1_QSPI_REF_CMD  0x188a0
#define GCC_QUPV3_WRAP1_QSPI_REF_CFG  0x188a4

#define GENI_SER_M_CLK_CFG            0x48
#define GENI_SER_S_CLK_CFG            0x4c
#define SER_CLK_EN                    (1u << 0)
#define CLK_DIV_SHFT                  4

static bool
wait_halt(uint32_t reg, const char *what)
{
	for (unsigned i = 0; i < 10000; i++) {
		if (!(bt_read(gcc, reg) & (1u << 31))) {
			return true;
		}
		bt_sleep_us(1);
	}
	hakios_tl("bt: clock %s did not come out of halt (%#x)\n", what, bt_read(gcc, reg));
	return false;
}

/*
 * QUPv3 wrap 1: configure the qspi_ref root, then the core, core_2x and s2
 * branches. Peridot's qspi_ref has 150 MHz from GPLL0_OUT_EVEN/4 and 240 MHz
 * from GPLL0_OUT_MAIN/2.5; 150 MHz with the s2 divider is what the stock UART
 * uses.
 */
static uint32_t bt_uart_ser_cfg;
static uint32_t bt_uart_s2_div;
static uint32_t bt_uart_rate;

static bool
bt_uart_clock(void)
{
	uint32_t vote = bt_read(gcc, GCC_QUPV3_WRAP1_CORE_2X_CBCR);
	uint32_t cfg;
	uint32_t div;
	uint32_t clk_div;

	/* Configure the root before its consumers, then bring the three branches out of halt. */
	cfg = bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CFG);
	cfg &= ~((0x1fu) | (0x7u << 8) | (0x3u << 12) | (1u << 20));
	cfg |= 3u | (6u << 8) | (1u << 20);      /* /4: 600 MHz GPLL0_OUT_EVEN -> 150 MHz */
	bt_write(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CFG, cfg);
	bt_write(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD, bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD) | 1u);
	for (unsigned i = 0; i < 10000; i++) {
		if (!(bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD) & 1u)) {
			break;
		}
		bt_sleep_us(1);
	}
	if (bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD) & 1u) {
		hakios_tl("bt: qspi_ref root did not update\n");
		return false;
	}

	bt_write(gcc, GCC_QUPV3_WRAP1_CORE_2X_CBCR, vote | (1u << 18) | (1u << 19) | (1u << 24));
	if (!wait_halt(GCC_QUPV3_WRAP1_CORE_2X_HALT, "core_2x") ||
	    !wait_halt(GCC_QUPV3_WRAP1_CORE_HALT, "core") ||
	    !wait_halt(GCC_QUPV3_WRAP1_S2_HALT, "s2")) {
		return false;
	}

	div = bt_read(gcc, GCC_QUPV3_WRAP1_S2_DIV) & 0xf;
	bt_uart_s2_div = div;
	bt_uart_rate = 150000000u / (div + 1);
	clk_div = (bt_uart_rate + 1843200u - 1) / 1843200u;    /* 115200 * 16 */
	if (clk_div == 0 || clk_div > 0x7ff) {
		hakios_tl("bt: cannot divide %u Hz for 115200 (%u)\n", bt_uart_rate, clk_div);
		return false;
	}
	bt_uart_ser_cfg = SER_CLK_EN | (clk_div << CLK_DIV_SHFT);
	hakios_tl("bt: UART clock %u Hz, s2 divider %u, ser cfg %#x\n", bt_uart_rate, div,
	    bt_uart_ser_cfg);
	return true;
}

/* ---- GENI UART ---------------------------------------------------------------------------------- */

static void
bt_uart_quiesce(void)
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint32_t active = bt_read(uart, 0x40);

		if (!(active & 0x1001)) {
			break;
		}
		if (active & 1) {
			bt_write(uart, 0x604, attempt ? 2 : 4);
		}
		if (active & 0x1000) {
			bt_write(uart, 0x634, attempt ? 2 : 4);
		}
		for (unsigned t = 0; t < 10000 && (bt_read(uart, 0x40) & 0x1001); t++) {
			bt_sleep_us(1);
		}
	}
	if (bt_read(uart, 0x40) & 0x1001) {
		hakios_tl("bt: UART did not stop (%#x)\n", bt_read(uart, 0x40));
		return;
	}
	unsigned depth = (bt_read(uart, 0xe28) >> 16) & 0xff;
	unsigned words = bt_read(uart, 0x804) & 0x1ffffff;

	if (depth && words <= depth) {
		while (words--) {
			(void)bt_read(uart, 0x780);
		}
	}
	bt_write(uart, 0x618, 0xffffffff);
	bt_write(uart, 0x648, 0xffffffff);
}

static void
bt_uart_init(void)
{
	unsigned rx_depth = (bt_read(uart, 0xe28) >> 16) & 0xff;

	bt_write(uart, GENI_SER_M_CLK_CFG, bt_uart_ser_cfg);
	bt_write(uart, GENI_SER_S_CLK_CFG, bt_uart_ser_cfg);
	bt_write(uart, 0x258, 0);              /* DMA mode off */
	bt_write(uart, 0x22c, 0);              /* no loopback */
	bt_write(uart, 0x25c, 2);              /* TX: CTS mask */
	bt_write(uart, 0x280, 0);              /* RX: no parity */
	bt_write(uart, 0x260, 0xf);
	bt_write(uart, 0x264, 0);
	bt_write(uart, 0x284, 0x4380e);
	bt_write(uart, 0x288, 0xc3e0e);
	bt_write(uart, 0x268, 8);              /* 8 data bits */
	bt_write(uart, 0x28c, 8);
	bt_write(uart, 0x26c, 0);              /* one stop bit */
	bt_write(uart, 0x294, 160);
	bt_write(uart, 0x2a4, 0);
	bt_write(uart, 0x2a8, 0);
	bt_write(uart, 0x80c, 1);
	bt_write(uart, 0x810, 0);
	bt_write(uart, 0x814, rx_depth > 2 ? rx_depth - 2 : 1);
	bt_write(uart, 0x618, 0xffffffff);
	bt_write(uart, 0x648, 0xffffffff);
	bt_write(uart, 0x630, (1u << 27) | 0x80);      /* START_READ, RFR_OPEN */
}

/* ---- H4 transport ------------------------------------------------------------------------------- */

#define BT_EVENT_MAX            258u
#define BT_RX_RAW_SIZE          16384u
#define BT_TX_QUEUE_SIZE        65536u

static struct hakios_bt_h4 bt_rx_packet;
static bool bt_ibs_ack_pending;
static unsigned bt_ibs_wakes, bt_ibs_sleeps;
static uint8_t bt_nvm_work[16384];
static uint8_t bt_public_address[6];
static uint8_t bt_supported_commands[64];
static uint8_t bt_rx_raw[BT_RX_RAW_SIZE];
static unsigned bt_rx_head, bt_rx_tail;
static bool bt_host_awake;

static int
bt_rx_capture(void)
{
	uint32_t irq = bt_read(uart, 0x640);

	if (irq) {
		bt_write(uart, 0x648, irq);
	}
	for (unsigned budget = 0; budget < 64; budget++) {
		uint32_t status = bt_read(uart, 0x804);
		uint32_t words = status & 0x1ffffff;

		if (!words) {
			return 0;
		}
		if (words > ((bt_read(uart, 0xe28) >> 16) & 0xff)) {
			return EIO;
		}
		unsigned last = (status >> 28) & 7;
		unsigned bytes = words == 1 && (status >> 31) && last ? last : 4;

		if (bytes > 4) {
			return EIO;
		}
		if (BT_RX_RAW_SIZE - (bt_rx_head - bt_rx_tail) < bytes) {
			return ENOBUFS;
		}
		uint32_t word = bt_read(uart, 0x780);

		for (unsigned b = 0; b < bytes; b++) {
			bt_rx_raw[bt_rx_head++ & (BT_RX_RAW_SIZE - 1)] = (word >> (8 * b)) & 0xff;
		}
	}
	return 0;
}

static int
bt_rx_frame(uint8_t *frame, unsigned capacity, unsigned *length, bool events_only)
{
	int error = bt_rx_capture();

	if (error) {
		return -error;
	}
	while (bt_rx_head != bt_rx_tail) {
		uint8_t byte = bt_rx_raw[bt_rx_tail++ & (BT_RX_RAW_SIZE - 1)];
		int result = hakios_bt_h4_push(&bt_rx_packet, byte);

		if (result < 0) {
			hakios_tl("bt: H4 framing error=%d byte=%#x\n", result, byte);
			memset(&bt_rx_packet, 0, sizeof(bt_rx_packet));
			return -EILSEQ;
		}
		if (result == 2) {
			if (byte == 0xfd) {
				bt_ibs_ack_pending = true;
				bt_ibs_wakes++;
				bt_host_awake = true;   /* the controller is asking us to ack */
			}
			if (byte == 0xfe) {
				bt_ibs_sleeps++;
				bt_host_awake = false;
				bt_ibs_ack_pending = false;
			}
			if (byte == 0xfc) {
				bt_host_awake = true;
			}
		}
		if (result != 1 || (events_only && bt_rx_packet.bytes[0] != 4)) {
			continue;
		}
		if (bt_rx_packet.used > capacity) {
			return -EMSGSIZE;
		}
		*length = bt_rx_packet.used;
		memcpy(frame, bt_rx_packet.bytes, *length);
		return 1;
	}
	return 0;
}

static int
bt_rx_event(uint8_t event[BT_EVENT_MAX], unsigned *length)
{
	return bt_rx_frame(event, BT_EVENT_MAX, length, true);
}

static int
bt_uart_tx(const uint8_t *packet, unsigned length)
{
	unsigned sent = 0;
	unsigned depth = (bt_read(uart, 0xe24) >> 16) & 0xff;

	if (!depth || depth > 256 || !length || length > HAKIOS_BT_H4_MAX) {
		return EINVAL;
	}
	bt_write(uart, 0x618, 0xffffffff);
	bt_write(uart, 0x270, length);
	bt_write(uart, 0x600, 1u << 27);
	for (unsigned elapsed = 0; elapsed < length * 180 + 10000; elapsed++) {
		int rx_error = bt_rx_capture();

		if (rx_error) {
			return rx_error;
		}
		unsigned used = bt_read(uart, 0x800) & 0x0fffffff;

		if (used > depth) {
			return EIO;
		}
		for (unsigned free = depth - used; free && sent < length; free--) {
			bt_write(uart, 0x700, packet[sent++]);
		}
		uint32_t irq = bt_read(uart, 0x610);

		if (sent == length && (irq & 1)) {
			bt_write(uart, 0x618, irq);
			return 0;
		}
		bt_sleep_us(1);
	}
	hakios_tl("bt: TX timeout sent=%u/%u irq=%#x fifo=%#x\n", sent, length,
	    bt_read(uart, 0x610), bt_read(uart, 0x800));
	bt_write(uart, 0x604, 4);
	return ETIMEDOUT;
}

static int
bt_service_ibs(void)
{
	if (!bt_ibs_ack_pending) {
		return 0;
	}
	const uint8_t ack = 0xfc;
	int error = bt_uart_tx(&ack, 1);

	if (!error) {
		bt_ibs_ack_pending = false;
	}
	return error;
}

static int
bt_hci_command(uint16_t opcode, const uint8_t *params, unsigned param_length, bool wait,
    uint8_t event[BT_EVENT_MAX], unsigned *event_length)
{
	uint8_t packet[259];

	if (param_length > 255) {
		return EINVAL;
	}
	packet[0] = 1;
	packet[1] = opcode & 0xff;
	packet[2] = opcode >> 8;
	packet[3] = param_length;
	if (param_length) {
		memcpy(packet + 4, params, param_length);
	}
	*event_length = 0;
	int error = bt_uart_tx(packet, param_length + 4);

	if (error || !wait) {
		return error;
	}
	for (unsigned elapsed = 0; elapsed < 100000; elapsed++) {
		int result = bt_rx_event(event, event_length);

		if (result < 0) {
			return -result;
		}
		error = bt_service_ibs();
		if (error) {
			return error;
		}
		if (result == 1) {
			int status = hakios_bt_cc_status(event, *event_length, opcode);

			if (status >= 0) {
				if (status) {
					hakios_tl("bt: HCI %#x status=%#x\n", opcode, status);
				}
				return status ? EIO : 0;
			}
			if (opcode == 0xfc00 && hakios_bt_qca_tlv_ack(event, *event_length)) {
				return 0;
			}
			if (*event_length == 7 && event[1] == 0x0f &&
			    event[5] == (uint8_t)opcode && event[6] == (uint8_t)(opcode >> 8)) {
				return event[3] ? EIO : 0;
			}
			hakios_tl("bt: unsolicited event=%#x while waiting for %#x\n", event[1], opcode);
		}
		*event_length = 0;
		bt_sleep_us(10);
	}
	hakios_tl("bt: HCI %#x timed out RX=%#x SIRQ=%#x\n", opcode, bt_read(uart, 0x804),
	    bt_read(uart, 0x640));
	return ETIMEDOUT;
}

/* ---- WCN6750 firmware --------------------------------------------------------------------------- */

static uint16_t
bt_le16(const uint8_t *p)
{
	return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t
bt_le32(const uint8_t *p)
{
	return bt_le16(p) | (uint32_t)bt_le16(p + 2) << 16;
}

static uint32_t
bt_le24(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size,
    int *disk_out);

static const uint8_t *
bt_image(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	if (data == NULL) {
		hakios_tl("bt: no fw/%s in the small image\n", name);
	}
	return data;
}

static int
bt_qca_version(uint32_t *product, uint16_t *rom, uint32_t *soc, uint16_t *patch)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	const uint8_t request = 0x19;
	int error = bt_hci_command(0xfc00, &request, 1, true, event, &length);

	if (error) {
		return error;
	}
	if (length != 21 || event[7] != 0x19) {
		hakios_tl("bt: unsupported QCA version response length=%u\n", length);
		return ENODEV;
	}
	*product = bt_le32(event + 9);
	*patch = bt_le16(event + 13);
	*rom = bt_le16(event + 15);
	*soc = bt_le32(event + 17);
	if (!*product || !*rom || !*soc) {
		return EILSEQ;
	}
	hakios_tl("bt: QCA product=%#x ROM=%#x SoC=%#x patch=%#x\n", *product, *rom, *soc, *patch);
	return 0;
}

static int
bt_qca_patch_config(void)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	const uint8_t cmd[] = { 0x28, 0x01, 0, 0, 0 };
	int error = bt_hci_command(0xfc00, cmd, sizeof(cmd), true, event, &length);

	if (error) {
		return error;
	}
	if (length != 8 || hakios_bt_cc_status(event, length, 0xfc00) != 0 || event[7] != 0x28) {
		hakios_tl("bt: patch config response length=%u\n", length);
		return EIO;
	}
	return 0;
}

static int
bt_qca_nvm_prepare(uint8_t *data, size_t size)
{
	size_t start = 4;
	bool hci = false;

	if (size < 4) {
		return EINVAL;
	}
	if (data[0] == 4) {
		if (size < 8 || data[4] != 2) {
			return EINVAL;
		}
		start = 8;
	} else if (data[0] != 2) {
		return EINVAL;
	}
	if (bt_le24(data + (start - 3)) > size - start) {
		return EINVAL;
	}
	for (size_t off = start; off < size;) {
		if (size - off < 12) {
			return EINVAL;
		}
		unsigned id = bt_le16(data + off);
		unsigned length = bt_le16(data + off + 2);

		if (length > size - off - 12) {
			return EINVAL;
		}
		if (id == 17) {
			if (length < 3 || hci) {
				return EINVAL;
			}
			data[off + 12] |= 0x80;       /* in-band sleep */
			data[off + 13] = 0;           /* 115200 baud */
			hci = true;
		} else if (id == 27) {
			if (length < 1) {
				return EINVAL;
			}
			data[off + 12] |= 1;          /* deep sleep */
		}
		off += 12 + length;
	}
	return hci ? 0 : EINVAL;
}

static int
bt_qca_download(const uint8_t *data, size_t size, bool skip_events, const char *what)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned segment = 0;

	if (data == NULL || size < 4 || size > 512 * 1024) {
		return EINVAL;
	}
	for (size_t offset = 0; offset < size;) {
		unsigned part = (unsigned)(size - offset);
		uint8_t params[245];
		unsigned length = 0;

		if (part > 243) {
			part = 243;
		}
		params[0] = 0x1e;
		params[1] = part;
		memcpy(params + 2, data + offset, part);
		offset += part;
		segment++;
		bool ack = !skip_events || offset == size;
		int error = bt_hci_command(0xfc00, params, part + 2, ack, event, &length);

		if (!error && ack && !hakios_bt_qca_tlv_ack(event, length)) {
			error = EIO;
		}
		if (error) {
			hakios_tl("bt: %s segment %u failed errno=%d\n", what, segment, error);
			return error;
		}
	}
	hakios_tl("bt: %s accepted: %u bytes, %u segments\n", what, (unsigned)size, segment);
	return 0;
}

static int
bt_qca_build_info(void)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	const uint8_t cmd = 0x20;
	int error = bt_hci_command(0xfc00, &cmd, 1, true, event, &length);

	if (error) {
		return error;
	}
	if (length < 9 || event[7] != 0x20) {
		return EILSEQ;
	}
	unsigned label_length = event[8];
	char label[64];

	if (label_length > length - 9) {
		label_length = length - 9;
	}
	if (label_length >= sizeof(label)) {
		label_length = sizeof(label) - 1;
	}
	memcpy(label, event + 9, label_length);
	label[label_length] = 0;
	hakios_tl("bt: firmware build %s\n", label);
	return 0;
}

static int
bt_qca_firmware(void)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	uint32_t product, soc;
	uint16_t rom, patch;
	uint8_t rom_ver;
	char name[32];
	const uint8_t *data;
	uint64_t size = 0;
	int error = bt_qca_version(&product, &rom, &soc, &patch);

	if (error) {
		return error;
	}
	error = bt_qca_patch_config();
	if (error) {
		return error;
	}
	uint32_t soc_ver = (soc << 16) | rom;

	rom_ver = ((soc_ver & 0x00000f00) >> 4) | (soc_ver & 0x0000000f);
	hakios_tl("bt: ROM version byte %#x\n", rom_ver);

	snprintf(name, sizeof(name), "msbtfw%02x.mbn", rom_ver);
	data = bt_image(name, &size);
	if (data != NULL) {
		if (size < 7 || memcmp(data, "\177ELF", 4) != 0) {
			hakios_tl("bt: %s is not an ELF patch\n", name);
			return EINVAL;
		}
		hakios_tl("bt: loading %s at 115200 baud\n", name);
		error = bt_qca_download(data, (size_t)size, true, "ELF patch");
	} else {
		snprintf(name, sizeof(name), "msbtfw%02x.tlv", rom_ver);
		data = bt_image(name, &size);
		if (data == NULL) {
			return ENOENT;
		}
		if (size < 28 || data[0] != 1 || bt_le24(data + 1) != size - 4 ||
		    bt_le16(data + 16) != product || bt_le16(data + 18) != rom) {
			hakios_tl("bt: %s does not match the controller\n", name);
			return EINVAL;
		}
		unsigned mode = data[14];

		if (mode != 0 && mode != 3) {
			hakios_tl("bt: %s download mode %u is not supported\n", name, mode);
			return EINVAL;
		}
		hakios_tl("bt: loading %s at 115200 baud (mode %u)\n", name, mode);
		error = bt_qca_download(data, (size_t)size, mode == 3, "TLV patch");
	}
	if (error) {
		return error;
	}
	bt_sleep_ms(10);

	snprintf(name, sizeof(name), "msnv%02x.bin", rom_ver);
	data = bt_image(name, &size);
	if (data == NULL) {
		return ENOENT;
	}
	if (size > sizeof(bt_nvm_work)) {
		return E2BIG;
	}
	memcpy(bt_nvm_work, data, (size_t)size);
	error = bt_qca_nvm_prepare(bt_nvm_work, (size_t)size);
	if (error) {
		hakios_tl("bt: %s is not a usable NVM file\n", name);
		return error;
	}
	error = bt_qca_download(bt_nvm_work, (size_t)size, false, "NVM");
	if (error) {
		return error;
	}

	const uint8_t logging[] = { 0x14, 0x00 };
	error = bt_hci_command(0xfc17, logging, sizeof(logging), true, event, &length);
	if (error) {
		return error;
	}
	error = bt_hci_command(0x0c03, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	error = bt_hci_command(0x1001, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 15 || bt_le16(event + 11) != 29) {
		hakios_tl("bt: unexpected local version length=%u manufacturer=%u\n", length,
		    length >= 13 ? bt_le16(event + 11) : 0);
		return EILSEQ;
	}
	hakios_tl("bt: firmware running: HCI=%#x LMP=%#x subversion=%#x\n", event[7], event[10],
	    bt_le16(event + 13));

	error = bt_hci_command(0x1002, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 71) {
		hakios_tl("bt: supported commands length=%u\n", length);
		return EILSEQ;
	}
	memcpy(bt_supported_commands, event + 7, sizeof(bt_supported_commands));

	error = bt_hci_command(0x1009, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 13) {
		hakios_tl("bt: read BD_ADDR length=%u\n", length);
		return EILSEQ;
	}
	unsigned nonzero = 0;

	for (unsigned i = 0; i < 6; i++) {
		bt_public_address[i] = event[12 - i];
		nonzero |= bt_public_address[i];
	}
	if (!nonzero || (bt_public_address[0] & 1)) {
		hakios_tl("bt: controller has no usable public address\n");
		return EINVAL;
	}
	hakios_tl("bt: address %02x:%02x:%02x:%02x:%02x:%02x\n", bt_public_address[0],
	    bt_public_address[1], bt_public_address[2], bt_public_address[3], bt_public_address[4],
	    bt_public_address[5]);

	error = bt_qca_build_info();
	if (error) {
		hakios_tl("bt: build info unavailable errno=%d\n", error);
	}
	return 0;
}

/* ---- host endpoint ------------------------------------------------------------------------------ */

static LCK_GRP_DECLARE(bt_control_group, "hakios-bluetooth");
static LCK_MTX_DECLARE(bt_control_lock, &bt_control_group);
static kern_ctl_ref bt_control;
static uint32_t bt_control_unit, bt_control_generation;
static uint8_t bt_tx_queue[BT_TX_QUEUE_SIZE];
static unsigned bt_tx_head, bt_tx_tail;
static bool bt_control_fault;
static struct termios bt_control_termios;

static errno_t
bt_control_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *address, __unused void **info)
{
	uid_t uid = kauth_cred_getuid(kauth_cred_get());

	if (uid != 0 && uid != 501) {
		return EPERM;
	}
	lck_mtx_lock(&bt_control_lock);
	int error = bt_control_fault ? EIO : (bt_control_unit ? EBUSY : 0);

	if (!error) {
		bt_control_unit = address->sc_unit;
		bt_control_generation++;
		bt_tx_head = bt_tx_tail = 0;
		hakios_tl("bt: H4 client connected uid=%u unit=%u\n", uid, bt_control_unit);
	}
	lck_mtx_unlock(&bt_control_lock);
	return error;
}

static errno_t
bt_control_disconnect(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info)
{
	lck_mtx_lock(&bt_control_lock);
	if (bt_control_unit == unit) {
		bt_control_unit = 0;
		bt_control_generation++;
		bt_tx_head = bt_tx_tail = 0;
		hakios_tl("bt: H4 client disconnected unit=%u\n", unit);
	}
	lck_mtx_unlock(&bt_control_lock);
	return 0;
}

static errno_t
bt_control_send(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info, mbuf_t message,
    __unused int flags)
{
	size_t length = mbuf_pkthdr_len(message);
	int error = 0;

	lck_mtx_lock(&bt_control_lock);
	if (bt_control_fault) {
		error = EIO;
	} else if (unit != bt_control_unit) {
		error = ENOTCONN;
	} else if (length > BT_TX_QUEUE_SIZE - (bt_tx_head - bt_tx_tail)) {
		error = ENOBUFS;
	}
	for (size_t offset = 0; !error && offset < length;) {
		uint8_t bytes[256];
		size_t count = length - offset;

		if (count > sizeof(bytes)) {
			count = sizeof(bytes);
		}
		error = mbuf_copydata(message, offset, count, bytes);
		if (error) {
			break;
		}
		for (size_t i = 0; i < count; i++) {
			bt_tx_queue[bt_tx_head++ & (BT_TX_QUEUE_SIZE - 1)] = bytes[i];
		}
		offset += count;
	}
	lck_mtx_unlock(&bt_control_lock);
	mbuf_freem(message);
	return error;
}

static errno_t
bt_control_getopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option,
    void *data, size_t *length)
{
	int modem = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
	size_t wanted;

	if (option == TIOCGETA) {
		wanted = sizeof(bt_control_termios);
	} else if (option == TIOCMGET) {
		wanted = sizeof(modem);
	} else {
		hakios_tl("bt: unknown H4 get option=%#x\n", option);
		return ENOPROTOOPT;
	}
	if (!data) {
		*length = wanted;
		return 0;
	}
	if (*length < wanted) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&bt_control_lock);
	memcpy(data, option == TIOCGETA ? (void *)&bt_control_termios : (void *)&modem, wanted);
	lck_mtx_unlock(&bt_control_lock);
	*length = wanted;
	return 0;
}

static errno_t
bt_control_setopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option,
    void *data, size_t length)
{
	uint32_t opt = (uint32_t)option;

	if (opt == (uint32_t)TIOCSETA || opt == (uint32_t)TIOCSETAW || opt == (uint32_t)TIOCSETAF) {
		if (length != sizeof(struct termios) || !data) {
			return EINVAL;
		}
		const struct termios *settings = data;

		if ((settings->c_ispeed && settings->c_ispeed != 115200) ||
		    (settings->c_ospeed && settings->c_ospeed != 115200)) {
			return EINVAL;
		}
		lck_mtx_lock(&bt_control_lock);
		bt_control_termios = *settings;
		lck_mtx_unlock(&bt_control_lock);
		return 0;
	}
	if (option == TIOCSDTR || option == TIOCCDTR || option == TIOCMSET ||
	    option == TIOCMBIS || option == TIOCMBIC || option == TIOCFLUSH) {
		return 0;
	}
	hakios_tl("bt: unknown H4 set option=%#x length=%u\n", option, (unsigned)length);
	return ENOPROTOOPT;
}

static void
bt_control_worker(__unused void *unused, __unused wait_result_t result)
{
	static struct hakios_bt_h4 tx;
	static uint8_t receive[HAKIOS_BT_H4_MAX];
	unsigned receive_length = 0;
	uint32_t generation = 0;
	unsigned commands = 0;
	bool waking = false;
	uint64_t wake_deadline = 0;

	for (;;) {
		lck_mtx_lock(&bt_control_lock);
		uint32_t unit = bt_control_unit;
		bool fault = bt_control_fault;

		if (generation != bt_control_generation) {
			generation = bt_control_generation;
			memset(&tx, 0, sizeof(tx));
			receive_length = 0;
			commands = 0;
			waking = false;
		}
		while (unit && !fault && !tx.complete && bt_tx_head != bt_tx_tail) {
			uint8_t byte = bt_tx_queue[bt_tx_tail++ & (BT_TX_QUEUE_SIZE - 1)];
			int state = hakios_bt_h4_push(&tx, byte);

			if (state < 0 || state == 2 || (tx.used && tx.bytes[0] == 4)) {
				fault = bt_control_fault = true;
				hakios_tl("bt: rejected invalid host H4 stream\n");
			}
		}
		lck_mtx_unlock(&bt_control_lock);

		int error = 0;

		if (unit && !fault) {
			error = bt_service_ibs();
		}
		if (!error && unit && !fault && tx.complete) {
			if (!bt_host_awake) {
				if (!waking) {
					const uint8_t wake = 0xfd;

					error = bt_uart_tx(&wake, 1);
					clock_interval_to_deadline(1000, NSEC_PER_MSEC, &wake_deadline);
					waking = true;
				} else if (mach_absolute_time() >= wake_deadline) {
					error = ETIMEDOUT;
				}
			} else {
				waking = false;
				if (tx.bytes[0] == 1 && commands++ < 128) {
					hakios_tl("bt: iOS HCI opcode=%#x length=%u\n", bt_le16(tx.bytes + 1),
					    tx.used);
				}
				error = bt_uart_tx(tx.bytes, tx.used);
				memset(&tx, 0, sizeof(tx));
			}
		}
		if (!error && !receive_length) {
			int state = bt_rx_frame(receive, sizeof(receive), &receive_length, false);

			if (state < 0) {
				error = -state;
			}
			if (state > 0 && receive_length >= 7 && receive[0] == 4 &&
			    receive[1] == 0x0e && receive[6]) {
				hakios_tl("bt: HCI command rejected opcode=%#x status=%#x\n",
				    bt_le16(receive + 4), receive[6]);
			}
		}
		if (!error) {
			error = bt_service_ibs();
		}
		if (error && !fault) {
			lck_mtx_lock(&bt_control_lock);
			bt_control_fault = true;
			lck_mtx_unlock(&bt_control_lock);
			HakiosBluetoothSetReady(false);
			hakios_tl("bt: H4 transport fault errno=%d\n", error);
		}
		if (receive_length) {
			int queued = unit ? ctl_enqueuedata(bt_control, unit, receive, receive_length, 0) :
			    ENOTCONN;

			if (queued != ENOBUFS) {
				receive_length = 0;
			}
		}
		bt_sleep_us(1000);
	}
}

static int
bt_control_start(void)
{
	struct kern_ctl_reg registration = { 0 };
	thread_t thread;
	kern_return_t started;
	int error;

	strlcpy(registration.ctl_name, "com.hakios.bluetooth", sizeof(registration.ctl_name));
	registration.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	registration.ctl_sendsize = BT_TX_QUEUE_SIZE;
	registration.ctl_recvsize = BT_TX_QUEUE_SIZE;
	registration.ctl_connect = bt_control_connect;
	registration.ctl_disconnect = bt_control_disconnect;
	registration.ctl_send = bt_control_send;
	registration.ctl_getopt = bt_control_getopt;
	registration.ctl_setopt = bt_control_setopt;
	bt_control_termios.c_cflag = CS8 | CREAD | CLOCAL;
	bt_control_termios.c_ispeed = bt_control_termios.c_ospeed = 115200;
	error = ctl_register(&registration, &bt_control);

	if (error) {
		return error;
	}
	error = HakiosBluetoothPublish(bt_public_address, bt_supported_commands);
	if (error) {
		ctl_deregister(bt_control);
		return error;
	}
	bt_host_awake = false;

	HakiosBluetoothSetReady(true);
	started = kernel_thread_start(bt_control_worker, NULL, &thread);

	if (started != KERN_SUCCESS) {
		HakiosBluetoothSetReady(false);
		ctl_deregister(bt_control);
		return EIO;
	}
	thread_deallocate(thread);
	hakios_tl("bt: H4 endpoint ready: com.hakios.bluetooth\n");
	return 0;
}

/* ---- bring-up ----------------------------------------------------------------------------------- */

static uint32_t
bt_saved_uart_regs[27];
static const uint32_t bt_uart_regs[] = {
	0x48, 0x4c, 0x22c, 0x258, 0x25c, 0x260, 0x264,
	0x268, 0x26c, 0x270, 0x280, 0x284, 0x288, 0x28c, 0x294, 0x2a4, 0x2a8,
	0x600, 0x604, 0x618, 0x630, 0x634, 0x648, 0x80c, 0x810, 0x814
};
static uint32_t bt_saved_pins[6], bt_saved_pin_ios[6];
static const unsigned bt_pins[] = { 40, 41, 42, 43, 156, 80 };
static uint32_t bt_saved_gcc_vote, bt_saved_gcc_cfg, bt_saved_gcc_cmd;
static bool bt_saved, bt_uart_saved;

static void
bt_save_board(void)
{
	for (unsigned i = 0; i < sizeof(bt_pins) / sizeof(bt_pins[0]); i++) {
		bt_saved_pins[i] = bt_read_addr(GPIO_CFG(bt_pins[i]));
		bt_saved_pin_ios[i] = bt_read_addr(GPIO_IO(bt_pins[i]));
	}
	bt_saved_gcc_vote = bt_read(gcc, GCC_QUPV3_WRAP1_S2_CBCR);
	bt_saved_gcc_cfg = bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CFG);
	bt_saved_gcc_cmd = bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD);
	bt_saved = true;
}

/* Only after bt_uart_clock(): an unclocked GENI read can stall the interconnect. */
static void
bt_save_uart(void)
{
	for (unsigned i = 0; i < sizeof(bt_uart_regs) / sizeof(bt_uart_regs[0]); i++) {
		bt_saved_uart_regs[i] = bt_read(uart, bt_uart_regs[i]);
	}
	bt_uart_saved = true;
}

static void
bt_restore(void)
{
	if (!bt_saved) {
		return;
	}
	if (bt_uart_saved) {
		gpio_output(bt_reset_pin, false, PULL_DOWN, 2);
		bt_uart_quiesce();
		for (unsigned i = 0; i < sizeof(bt_uart_regs) / sizeof(bt_uart_regs[0]); i++) {
			bt_write(uart, bt_uart_regs[i], bt_saved_uart_regs[i]);
		}
	}
	for (unsigned i = 0; i < sizeof(bt_pins) / sizeof(bt_pins[0]); i++) {
		bt_write_addr(GPIO_IO(bt_pins[i]), bt_saved_pin_ios[i]);
		bt_write_addr(GPIO_CFG(bt_pins[i]), bt_saved_pins[i]);
	}
	bt_write(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CFG, bt_saved_gcc_cfg);
	bt_write(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD, bt_saved_gcc_cmd | 1u);
	for (unsigned i = 0; i < 10000 && (bt_read(gcc, GCC_QUPV3_WRAP1_QSPI_REF_CMD) & 1u); i++) {
		bt_sleep_us(1);
	}
	bt_write(gcc, GCC_QUPV3_WRAP1_S2_CBCR, bt_saved_gcc_vote);
	bt_saved = false;
	bt_uart_saved = false;
}

static int
bt_power_on(void)
{
	if (!bt_power_rails()) {
		return EIO;
	}
	/* BT_EN low while the supplies settle; the stock driver holds it for ~50 ms. */
	gpio_output(bt_reset_pin, false, PULL_DOWN, 2);
	bt_sleep_ms(50);
	hakios_tl("bt: reset low, wlan reset %u, sw ctrl %u\n", gpio_get(wlan_reset_pin),
	    gpio_get(bt_sw_pin));
	if (gpio_get(wlan_reset_pin)) {
		gpio_output(bt_reset_pin, true, PULL_DOWN, 2);
	} else {
		bt_sleep_ms(100);
		gpio_output(bt_reset_pin, true, PULL_DOWN, 2);
	}
	bt_sleep_ms(50);
	gpio_configure(bt_sw_pin, 0, PULL_NONE, 2, false);
	hakios_tl("bt: reset high, sw ctrl %u\n", gpio_get(bt_sw_pin));
	return 0;
}

static int
bt_cold_version(void)
{
	uint8_t event[BT_EVENT_MAX];
	unsigned length = 0;
	int error = bt_hci_command(0x0c03, NULL, 0, true, event, &length);

	if (error) {
		return error;
	}
	error = bt_hci_command(0x1001, NULL, 0, true, event, &length);
	if (error) {
		return error;
	}
	if (length != 15) {
		hakios_tl("bt: cold local version length=%u\n", length);
		return EILSEQ;
	}
	hakios_tl("bt: cold HCI=%#x manufacturer=%u LMP subversion=%#x\n", event[7],
	    bt_le16(event + 11), bt_le16(event + 13));
	return 0;
}

void
hakios_bt_init(void)
{
	uint32_t on = 0, step = 0;

	if (!PE_parse_boot_argn("bt_step", &step, sizeof(step)) || step == 0) {
		if (!PE_parse_boot_argn("bt", &on, sizeof(on)) || on != 1) {
			return;
		}
		step = 4;
	}
	if (step < 1 || step > 4) {
		hakios_tl("bt: bt_step=%u is not understood (1..4)\n", step);
		return;
	}
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		hakios_tl("bt: bt=1 and bt_step=N are for the phone's WCN6750, and this is not the phone\n");
		return;
	}
	uart_phys = hakios_board_addr("bt_uart", BT_UART_PHYS);
	gcc_phys = hakios_board_addr("gcc", BT_GCC_PHYS);
	tlmm_phys = hakios_board_addr("tlmm", BT_TLMM_PHYS);
	bt_reset_pin = hakios_board_number("bt_reset", bt_reset_pin);
	bt_sw_pin = hakios_board_number("bt_sw", bt_sw_pin);
	wlan_reset_pin = hakios_board_number("wlan_reset", wlan_reset_pin);

	uart = ml_io_map(uart_phys, BT_UART_SIZE);
	gcc = ml_io_map(gcc_phys, BT_GCC_SIZE);
	tlmm = ml_io_map(tlmm_phys, BT_TLMM_SIZE);
	if (uart == 0 || gcc == 0 || tlmm == 0) {
		hakios_tl("bt: cannot map UART/GCC/TLMM (%#lx/%#lx/%#lx)\n", (unsigned long)uart,
		    (unsigned long)gcc, (unsigned long)tlmm);
		return;
	}
	hakios_tl("bt: UART %#lx, GCC %#lx, TLMM %#lx; reset %u, sw %u, wlan %u\n",
	    (unsigned long)uart_phys, (unsigned long)gcc_phys, (unsigned long)tlmm_phys,
	    bt_reset_pin, bt_sw_pin, wlan_reset_pin);
	bt_save_board();
	if (!bt_uart_clock()) {
		bt_restore();
		return;
	}
	bt_save_uart();
	bt_uart_quiesce();
	if (bt_power_on()) {
		bt_restore();
		return;
	}
	bt_pins_uart();
	bt_uart_init();
	bt_rx_head = bt_rx_tail = 0;
	bt_host_awake = false;
	bt_ibs_ack_pending = false;
	bt_ibs_wakes = bt_ibs_sleeps = 0;
	memset(&bt_rx_packet, 0, sizeof(bt_rx_packet));
	hakios_tl("bt: step 1 done: UART clock, rails and pins\n");
	if (step == 1) {
		bt_restore();
		return;
	}
	int error = bt_cold_version();

	if (error) {
		hakios_tl("bt: cold HCI failed errno=%d\n", error);
		bt_restore();
		return;
	}
	hakios_tl("bt: step 2 done: ROM HCI answered\n");
	if (step == 2) {
		bt_restore();
		return;
	}
	error = bt_qca_firmware();
	if (error) {
		hakios_tl("bt: firmware failed errno=%d\n", error);
		bt_restore();
		return;
	}
	hakios_tl("bt: step 3 done: WCN6750 firmware running\n");
	if (step == 3) {
		bt_restore();
		return;
	}
	error = bt_control_start();
	if (error) {
		hakios_tl("bt: host endpoint failed errno=%d\n", error);
		bt_restore();
		return;
	}
	hakios_tl("bt: step 4 done: Bluetooth is up\n");
}
