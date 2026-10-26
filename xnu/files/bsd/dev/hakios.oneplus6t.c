/*
 * The touch screen of the OnePlus 6T (fajita): a Synaptics S3706B talking the RMI4 protocol on
 * the I2C bus of the SoC's serial engine 12 (slave 0x20), as a source of touches for
 * /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg touchscreen=2: every address below
 * belongs to this phone and its SoC (Snapdragon 845, SDM845), not to the POCO X5 Pro that
 * bsd/dev/hakios.goodix.c drives.
 *
 * There is no clock, pin, regulator or I2C framework in this kernel, so the driver does all of it
 * itself, in the order the mainline Linux drivers do, the same shape as hakios.goodix.c does for
 * the POCO's SPI touch screen:
 *   power    the chip's analog supply is LDO28 of the PM8998, switched through RPMh (one request
 *            written into the application processor's command slots); the 1.8 V I/O supply is a
 *            fixed regulator behind GPIO 88
 *   clocks   the serial engine's core, AHB and serial-engine clocks, by vote in the global clock
 *            controller, with the serial-engine clock sourced from the 19.2 MHz crystal
 *   pins     SDA (GPIO 49) and SCL (GPIO 50) to serial engine 12 ("qup12"), plus reset (GPIO 99),
 *            the interrupt line (GPIO 125) and the I/O-supply switch (GPIO 88)
 *   I2C      the serial engine ("GENI") in FIFO mode, polled, fast mode (400 kHz); or, if the
 *            engine cannot be used, the same two pins driven by hand
 *   chip     scan the RMI4 Page Description Table for function 0x01 (device control) and 0x12
 *            (two-dimensional touch), put the device in its operating mode, then read the F12 touch
 *            report when the interrupt line signals
 * No interrupt is used: a thread looks at the interrupt line 125 times a second (boot-arg
 * touch_poll_us=N: every N microseconds instead of 8000), the same as the Goodix driver.
 *
 * Provenance of the numbers (a port to another Qualcomm phone would change these, from its own
 * device tree and its SoC's drivers, and nothing else):
 *   - the register bases and clock offsets:  arch/arm64/boot/dts/qcom/sdm845.dtsi,
 *     drivers/clk/qcom/gcc-sdm845.c, drivers/pinctrl/qcom/pinctrl-sdm845.c
 *   - the serial engine, RPMh and command-database layout: the same GENI/RPMh/cmd-db drivers the
 *     Goodix driver cites (drivers/i2c/busses/i2c-qcom-geni.c for the I2C side)
 *   - the touch screen's bus, slave address, supplies and GPIOs:
 *     arch/arm64/boot/dts/qcom/sdm845-oneplus-common.dtsi (synaptics-rmi4-i2c@20 on &i2c12,
 *     interrupt GPIO 125 falling edge, the ts-int-state pins GPIO 99 and 125, vio behind GPIO 88,
 *     vdd = vreg_l28a_3p0 = PM8998 LDO28)
 *   - the RMI4 protocol: drivers/input/rmi4/{rmi_i2c,rmi_driver,rmi_f01,rmi_f12}.c
 * A few values cannot be read back and so are written here as this phone's and marked below: the
 * "qup12" pin function (1), the RPMh resource name of LDO28 ("ldoa28"), and the panel's reported
 * coordinate range (1080 x 2340, overridable with touch_max_x / touch_max_y).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x03400000UL
#define TLMM_SIZE               0x100000UL                      /* GPIO 125 is at 0x7d000, so 0x100000 is enough */
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_UP                 3

#define PIN_SDA                 49      /* serial engine 12, function "qup12" */
#define PIN_SCL                 50
#define PIN_INT                 125     /* low = the chip has a report (interrupt is falling-edge in Linux) */
#define PIN_RESET               99      /* low = in reset (the ts-int-state group's other pin) */
#define PIN_VIO                 88      /* high = 1.8 V I/O supply on */
#define PIN_FUNCTION_I2C        1       /* "qup12" is mux option 1 on both GPIO 49 and 50 (pinctrl-sdm845.c) */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the analog supply (RPMh) -------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x85fe0000UL                    /* aop-cmd-db-mem@85fe0000 (sdm845.dtsi) */
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x179e0000UL                    /* apps_rsc's "DRV 2" (rsc@179c0000 + drv-2) */
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0 (qcom,tcs-offset = 0xd00) */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

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

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("oneplus6t: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
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
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("oneplus6t: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	/* the slot set must be idle, and switched off the way the hardware wants it: trigger, then enable */
	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("oneplus6t: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);        /* the status bit follows the enable; the interrupt itself stays masked */
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/*
 * The chip's analog supply: 3.0 V from LDO28 of the PM8998 ("ldoa28" -- "ldo" + the PMIC's
 * qcom,pmic-id "a" + "28", as drivers/regulator/qcom-rpmh-regulator.c builds it). The 6T leaves
 * this rail on from the boot loader, so a failure here is not fatal: the chip may already have it.
 */
static bool
analog_supply_on(void)
{
	uint32_t base = rpmh_address("ldoa28");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 3008, 1, VRM_MODE_LDO_HPM };

	/* addresses of regulators ("VRM") have 4 in bits 19:16; anything else is not what we are looking for */
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("oneplus6t: RPMh: no regulator \"ldoa28\" (address %#x); assuming it is already on\n", base);
		return false;
	}
	printf("oneplus6t: RPMh: ldoa28 is at %#x; slot status %#x, control %#x\n", base, rd(TCS(TCS_STATUS)),
	    rd(TCS(TCS_CONTROL)));
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("oneplus6t: analog supply on (3.0 V)\n");
	return true;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x5200c)         /* GCC_APCS_CLOCK_BRANCH_ENA_VOTE */
#define VOTE_QUP1_CORE_2X       (1u << 18)              /* gcc_qupv3_wrap_1_core_2x_clk */
#define VOTE_QUP1_CORE          (1u << 19)              /* gcc_qupv3_wrap_1_core_clk */
#define VOTE_QUP1_M_AHB         (1u << 20)              /* gcc_qupv3_wrap_1_m_ahb_clk */
#define VOTE_QUP1_S_AHB         (1u << 21)              /* gcc_qupv3_wrap_1_s_ahb_clk */
#define VOTE_QUP1_SE4           (1u << 26)              /* gcc_qupv3_wrap1_s4_clk -- serial engine 12 is wrapper 1's SE4 */
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x1800c)         /* bit 31: the clock is off */
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x18010)
#define GCC_QUP1_SE4_CBCR       (gcc + 0x184d4)
#define GCC_QUP1_SE4_CMD        (gcc + 0x184d8)         /* bit 0: take the new configuration */
#define GCC_QUP1_SE4_CFG        (gcc + 0x184dc)         /* [10:8] source, [4:0] 2*divider-1 */
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);

	/* source the serial engine's clock from the 19.2 MHz crystal (source select 0), RCG divider bypassed
	 * (the field is 2*D-1, so 1 means D=1); the I2C rate is then set by the serial-clock divider and the
	 * SCL counters in geni_i2c_init */
	wr(GCC_QUP1_SE4_CFG, (rd(GCC_QUP1_SE4_CFG) & ~0x371fu) | 1);
	wr(GCC_QUP1_SE4_CMD, rd(GCC_QUP1_SE4_CMD) | 1);
	for (int i = 0; i < 500 && (rd(GCC_QUP1_SE4_CMD) & 1); i++) {
		sleep_us(10);
	}
	if (rd(GCC_QUP1_SE4_CMD) & 1) {
		printf("oneplus6t: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP1_SE4_CMD));
		return false;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_SE4);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_S_AHB_CBCR) | rd(GCC_QUP1_SE4_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	printf("oneplus6t: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
	    rd(GCC_QUP1_SE4_CBCR));
	return false;
}

/* ---- I2C through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00a90000UL            /* i2c12: i2c@a90000 (sdm845.dtsi) */
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)    /* bit 0 enable, [15:4] divider */
#define GENI_IF_DISABLE_RO      (se + 0x064)    /* bit 0: no FIFO interface */
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define I2C_TX_TRANS_LEN        (se + 0x26c)
#define I2C_RX_TRANS_LEN        (se + 0x270)
#define I2C_SCL_COUNTERS        (se + 0x278)    /* [29:20] high, [19:10] low, [9:0] cycle */
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] operation, [23:0] parameters */
#define GENI_M_CMD_CTRL         (se + 0x604)    /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)    /* [27:0] words */
#define GENI_RX_FIFO_STATUS     (se + 0x804)    /* bit 31 last, [30:28] bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_GP_IRQ(n)             (1u << (10 + (n)))      /* I2C protocol interrupts: bit 10 = NACK */
#define M_ERRORS                ((1u << 2) | (1u << 3) | M_GP_IRQ(0) | M_GP_IRQ(1) | M_GP_IRQ(2) | M_GP_IRQ(3) | \
                                 (1u << 24) | (1u << 25))
/* I2C master commands (i2c-qcom-geni.c) */
#define I2C_WRITE               1
#define I2C_READ                2
#define M_OPCODE_SHIFT          27
#define SLV_ADDR_SHIFT          9               /* the 7-bit slave address sits in bits [15:9] of the parameters */
#define STOP_STRETCH            (1u << 2)        /* set = do not issue STOP (hold the bus for a repeated start) */

static uint32_t fifo_depth;

/* 0 = ready; otherwise why the engine cannot be used */
static const char *
geni_i2c_init(void)
{
	uint32_t high = 5, low = 11, cycle = 22, divider = 2;    /* fast mode (400 kHz) from a 19.2 MHz source */

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 3) {
		return "the serial engine has no I2C firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	/* the status bits we poll follow these enables; the interrupt itself stays masked at the GIC */
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* eight bits, most significant first, four bytes to a FIFO word -- the packing i2c-qcom-geni uses */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);

	/* the SCL timing and the serial-engine clock divider for 400 kHz off the crystal */
	wr(GENI_CLK_SEL, 0);
	wr(GENI_SER_M_CLK_CFG, divider << 4 | 1);
	wr(I2C_SCL_COUNTERS, high << 20 | low << 10 | cycle);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/*
 * One I2C message to the slave: `write`, if any, is sent first (the chip's register address), then
 * `read` bytes are read back. A read keeps the bus between the two with a repeated start (STOP_STRETCH
 * on the write), the way a register read works; a message that only writes ends with a STOP.
 */
static uint8_t slave_address = 0x20;

static bool
geni_i2c_one(uint32_t opcode, bool stretch, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	if (opcode == I2C_WRITE) {
		wr(I2C_TX_TRANS_LEN, length);
		wr(GENI_TX_WATERMARK, 1);
	} else {
		wr(I2C_RX_TRANS_LEN, length);
	}
	wr(GENI_M_CMD0, opcode << M_OPCODE_SHIFT | (uint32_t)slave_address << SLV_ADDR_SHIFT | (stretch ? STOP_STRETCH : 0));
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if (opcode == I2C_WRITE) {
			while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
				uint32_t word = 0;

				for (int j = 0; j < 4 && sent < length; j++, sent++) {
					word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
				}
				wr(GENI_TX_FIFO, word);
			}
			if (sent >= length) {
				wr(GENI_TX_WATERMARK, 0);
			}
		} else {
			geni_drain(rx, length, &received);
		}
		status = rd(GENI_M_IRQ_STATUS);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			if (opcode == I2C_READ) {
				geni_drain(rx, length, &received);
			}
			wr(GENI_M_IRQ_CLEAR, status);
			if (opcode == I2C_WRITE || received == length) {
				return true;
			}
			break;
		}
	}
	geni_recover();
	return false;
}

static bool
geni_i2c_txn(const uint8_t *write, uint32_t write_length, uint8_t *read, uint32_t read_length)
{
	if (write_length && !geni_i2c_one(I2C_WRITE, read_length != 0, write, NULL, write_length)) {
		return false;
	}
	if (read_length && !geni_i2c_one(I2C_READ, false, NULL, read, read_length)) {
		return false;
	}
	return true;
}

/* ---- I2C by hand, on the same two pins ------------------------------------------------------------- */

/*
 * Open-drain SDA and SCL by hand: a line is let go high (input, the external pull-up raises it) or
 * pulled low (output zero). Used only if the serial engine cannot be brought up.
 */
static void
bitbang_release(unsigned pin)
{
	gpio_configure(pin, 0, PULL_UP, 2, false);
}

static void
bitbang_low(unsigned pin)
{
	gpio_output(pin, false, PULL_UP, 2);
}

static void
bitbang_init(void)
{
	bitbang_release(PIN_SDA);
	bitbang_release(PIN_SCL);
}

static void
bitbang_start(void)
{
	bitbang_release(PIN_SDA);
	bitbang_release(PIN_SCL);
	sleep_us(5);
	bitbang_low(PIN_SDA);
	sleep_us(5);
	bitbang_low(PIN_SCL);
}

static void
bitbang_stop(void)
{
	bitbang_low(PIN_SDA);
	sleep_us(5);
	bitbang_release(PIN_SCL);
	sleep_us(5);
	bitbang_release(PIN_SDA);
	sleep_us(5);
}

static bool
bitbang_write_byte(uint8_t value)
{
	bool ack;

	for (int bit = 7; bit >= 0; bit--) {
		if (value >> bit & 1) {
			bitbang_release(PIN_SDA);
		} else {
			bitbang_low(PIN_SDA);
		}
		sleep_us(3);
		bitbang_release(PIN_SCL);
		sleep_us(5);
		bitbang_low(PIN_SCL);
		sleep_us(2);
	}
	bitbang_release(PIN_SDA);       /* let the slave drive the acknowledgement bit */
	sleep_us(3);
	bitbang_release(PIN_SCL);
	sleep_us(3);
	ack = !gpio_get(PIN_SDA);        /* low = acknowledged */
	bitbang_low(PIN_SCL);
	sleep_us(2);
	return ack;
}

static uint8_t
bitbang_read_byte(bool ack)
{
	uint8_t value = 0;

	bitbang_release(PIN_SDA);
	for (int bit = 7; bit >= 0; bit--) {
		sleep_us(3);
		bitbang_release(PIN_SCL);
		sleep_us(4);
		value = (uint8_t)(value << 1 | gpio_get(PIN_SDA));
		bitbang_low(PIN_SCL);
		sleep_us(2);
	}
	if (ack) {
		bitbang_low(PIN_SDA);
	} else {
		bitbang_release(PIN_SDA);
	}
	sleep_us(3);
	bitbang_release(PIN_SCL);
	sleep_us(5);
	bitbang_low(PIN_SCL);
	sleep_us(2);
	return value;
}

static bool
bitbang_txn(const uint8_t *write, uint32_t write_length, uint8_t *read, uint32_t read_length)
{
	if (write_length) {
		bitbang_start();
		if (!bitbang_write_byte((uint8_t)(slave_address << 1))) {        /* address + write */
			bitbang_stop();
			return false;
		}
		for (uint32_t i = 0; i < write_length; i++) {
			if (!bitbang_write_byte(write[i])) {
				bitbang_stop();
				return false;
			}
		}
		if (read_length == 0) {
			bitbang_stop();
			return true;
		}
	}
	bitbang_start();                                                        /* repeated start for the read */
	if (!bitbang_write_byte((uint8_t)(slave_address << 1 | 1))) {           /* address + read */
		bitbang_stop();
		return false;
	}
	for (uint32_t i = 0; i < read_length; i++) {
		read[i] = bitbang_read_byte(i + 1 < read_length);               /* acknowledge all but the last byte */
	}
	bitbang_stop();
	return true;
}

static bool (*i2c_txn)(const uint8_t *write, uint32_t write_length, uint8_t *read, uint32_t read_length);

/* ---- the Synaptics S3706, RMI4 ---------------------------------------------------------------------- */

/*
 * RMI4 maps the chip's registers into a 16-bit space of 256-byte pages. The low byte is addressed
 * directly; the high byte (the page) is selected by writing it to register 0xff. A Page Description
 * Table at the top of each page (entries of six bytes, from 0xe9 downward, ending at a zero function
 * number) says which "functions" are present and where each keeps its query, command, control and
 * data registers. Function 0x01 is device control; function 0x12 is two-dimensional touch.
 */
#define RMI_PAGE_SELECT         0xff
#define RMI_PDT_START           0xe9
#define RMI_PDT_END             0x05
#define RMI_PDT_ENTRY_SIZE      6
#define RMI_MAX_PAGE            4

#define RMI_FUNCTION_F01        0x01
#define RMI_FUNCTION_F12        0x12

#define F01_CTRL0_CONFIGURED    0x80            /* mark the device configured; sleep bits [2:0] = 0 is normal operation */

#define F12_MAX_OBJECTS         10
#define F12_OBJECT_SIZE         8               /* type, x low, x high, y low, y high, z, wx, wy */
#define F12_OBJECT_FINGER       1
#define F12_OBJECT_STYLUS       2
#define F12_OBJECT_PALM         3
#define F12_OBJECT_GLOVE        5

static uint16_t f01_control, f01_data, f12_data;
static uint32_t f12_objects = F12_MAX_OBJECTS;
static uint32_t f12_data_offset;                /* boot-arg touch_f12_data_offset: where the object array starts */
static uint8_t rmi_page = 0xff;                 /* unknown until first set */
static uint32_t max_x = 1080, max_y = 2340;     /* the 6T's display; override with touch_max_x / touch_max_y */
static uint32_t flip;                           /* boot-arg touch_flip: 1 = mirror left-right, 2 = mirror top-bottom */

static bool
rmi_set_page(uint8_t page)
{
	uint8_t command[2] = { RMI_PAGE_SELECT, page };

	if (rmi_page == page) {
		return true;
	}
	if (!i2c_txn(command, sizeof(command), NULL, 0)) {
		return false;
	}
	rmi_page = page;
	return true;
}

static bool
rmi_read(uint16_t address, uint8_t *data, uint32_t length)
{
	uint8_t low = (uint8_t)address;

	if (!rmi_set_page((uint8_t)(address >> 8))) {
		return false;
	}
	return i2c_txn(&low, 1, data, length);
}

static bool
rmi_write(uint16_t address, const uint8_t *data, uint32_t length)
{
	uint8_t buffer[8];

	if (length + 1 > sizeof(buffer)) {
		return false;
	}
	if (!rmi_set_page((uint8_t)(address >> 8))) {
		return false;
	}
	buffer[0] = (uint8_t)address;
	memcpy(buffer + 1, data, length);
	return i2c_txn(buffer, length + 1, NULL, 0);
}

/* Walk the Page Description Table and remember where functions 0x01 and 0x12 keep their registers. */
static bool
rmi_scan_pdt(bool verbose)
{
	f01_control = f01_data = f12_data = 0;
	for (unsigned page = 0; page <= RMI_MAX_PAGE; page++) {
		bool anything = false;

		for (int address = RMI_PDT_START; address >= RMI_PDT_END; address -= RMI_PDT_ENTRY_SIZE) {
			uint8_t entry[RMI_PDT_ENTRY_SIZE];
			uint8_t function;
			uint16_t control_base, data_base;

			if (!rmi_read((uint16_t)(page << 8 | address), entry, sizeof(entry))) {
				if (verbose) {
					printf("oneplus6t: RMI4: cannot read the page table at page %u\n", page);
				}
				return false;
			}
			function = entry[5];
			if (function == 0 || function == 0xff) {
				break;          /* no more functions on this page */
			}
			anything = true;
			control_base = (uint16_t)(page << 8 | entry[2]);
			data_base = (uint16_t)(page << 8 | entry[3]);
			if (verbose) {
				printf("oneplus6t: RMI4: F%02x at page %u (control %#x, data %#x)\n", function, page,
				    control_base, data_base);
			}
			if (function == RMI_FUNCTION_F01) {
				f01_control = control_base;
				f01_data = data_base;
			} else if (function == RMI_FUNCTION_F12) {
				f12_data = data_base;
			}
		}
		if (!anything && page > 0) {
			break;                  /* an empty page past page 0: the table has ended */
		}
	}
	if (f01_data == 0 || f12_data == 0) {
		if (verbose) {
			printf("oneplus6t: RMI4: F01 %s, F12 %s\n", f01_data ? "found" : "missing",
			    f12_data ? "found" : "missing");
		}
		return false;
	}
	return true;
}

/* Put the device in its operating mode and read its status once to clear a pending interrupt. */
static bool
rmi_configure(void)
{
	uint8_t control0 = F01_CTRL0_CONFIGURED;
	uint8_t status[2];

	if (f01_control && !rmi_write(f01_control, &control0, 1)) {
		printf("oneplus6t: RMI4: cannot write F01 device control\n");
		return false;
	}
	sleep_ms(20);
	(void)rmi_read(f01_data, status, sizeof(status));       /* device status and interrupt status */
	return true;
}

static void
touch_reset(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	sleep_ms(2);
	gpio_set(PIN_RESET, true);
	sleep_ms(200);          /* syna,reset-delay-ms / startup-delay-ms are 200 ms on the 6T */
	rmi_page = 0xff;        /* the chip forgets which page was selected across a reset */
}

static bool
chip_probe(bool verbose)
{
	touch_reset();
	if (!rmi_scan_pdt(verbose)) {
		return false;
	}
	return rmi_configure();
}

/* ---- touch reports --------------------------------------------------------------------------------- */

/* What has been seen so far: said once a minute, so that the end of the log always tells. */
static struct {
	uint32_t polls, reports, touches, bad, last_x, last_y;
} seen;

/* Which fingers were down in the last report, and where. */
static struct {
	bool down, seen;
	uint32_t x, y;
} contact[F12_MAX_OBJECTS];

static void
report_object(uint32_t id, uint32_t type, uint32_t x, uint32_t y)
{
	static unsigned shown;
	bool touching = type == F12_OBJECT_FINGER || type == F12_OBJECT_GLOVE;

	if (id >= F12_MAX_OBJECTS) {
		return;
	}
	contact[id].seen = true;
	if (!touching) {
		return;         /* a palm, a stylus or nothing: not a finger we report */
	}
	seen.touches++;
	if (shown < 40) {
		shown++;
		printf("oneplus6t: finger %u at %u, %u (of %u x %u)\n", id, x, y, max_x, max_y);
	}
	seen.last_x = x;
	seen.last_y = y;
	x = x >= max_x ? 65535 : x * 65535 / max_x;
	y = y >= max_y ? 65535 : y * 65535 / max_y;
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	if (!contact[id].down) {
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
	} else if (contact[id].x != x || contact[id].y != y) {
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
	}
	contact[id].down = true;
	contact[id].x = x;
	contact[id].y = y;
}

static void
chip_poll(void)
{
	static uint8_t report[8 + F12_MAX_OBJECTS * F12_OBJECT_SIZE];
	static unsigned bad;
	uint8_t status[2];
	uint32_t length = f12_data_offset + f12_objects * F12_OBJECT_SIZE;

	seen.polls++;
	(void)rmi_read(f01_data, status, sizeof(status));       /* read the interrupt status to release the line */
	if (length > sizeof(report)) {
		length = sizeof(report);
	}
	if (!rmi_read(f12_data, report, length)) {
		seen.bad++;
		if (bad++ < 8) {
			printf("oneplus6t: cannot read the F12 touch report\n");
		}
		return;
	}
	seen.reports++;
	for (uint32_t i = 0; i < F12_MAX_OBJECTS; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < f12_objects && i < F12_MAX_OBJECTS; i++) {
		const uint8_t *object = report + f12_data_offset + i * F12_OBJECT_SIZE;
		uint32_t type = object[0];
		uint32_t x = object[1] | (uint32_t)object[2] << 8;
		uint32_t y = object[3] | (uint32_t)object[4] << 8;

		report_object(i, type, x, y);
	}
	/* a finger that is no longer in the report has lifted */
	for (uint32_t id = 0; id < F12_MAX_OBJECTS; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	printf("oneplus6t: %s: votes %#x, clocks %#x %#x %#x, rate cfg %#x; pins sda/scl cfg %#x %#x, reset %#x/%u, "
	    "int %#x/%u, vio %#x/%u; engine firmware %#x, interface %#x\n", when, rd(GCC_VOTE), rd(GCC_QUP1_M_AHB_CBCR),
	    rd(GCC_QUP1_S_AHB_CBCR), rd(GCC_QUP1_SE4_CBCR), rd(GCC_QUP1_SE4_CFG), rd(GPIO_CFG(PIN_SDA)), rd(GPIO_CFG(PIN_SCL)),
	    rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)), rd(GPIO_CFG(PIN_VIO)),
	    rd(GPIO_IO(PIN_VIO)), rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO));
}

/* Power the chip up from nothing, in the vendor driver's order. */
static void
power_on(void)
{
	gpio_output(PIN_RESET, false, PULL_UP, 8);
	gpio_configure(PIN_INT, 0, PULL_UP, 8, false);
	analog_supply_on();             /* 3.0 V from LDO28 (or already on from the boot loader) */
	sleep_ms(3);
	gpio_output(PIN_VIO, true, PULL_NONE, 2);       /* 1.8 V I/O supply */
	sleep_ms(15);
}

static void
use_engine_pins(void)
{
	gpio_configure(PIN_SDA, PIN_FUNCTION_I2C, PULL_UP, 2, false);
	gpio_configure(PIN_SCL, PIN_FUNCTION_I2C, PULL_UP, 2, false);
}

static bool
bring_up(void)
{
	const char *problem = "its clocks cannot be turned on";

	show_state("as found");
	power_on();

	if (clocks_on()) {
		printf("oneplus6t: serial engine: firmware %#x, interface %#x, parameters %#x\n", rd(GENI_FW_REVISION_RO),
		    rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0));
		problem = geni_i2c_init();
	}
	if (problem == NULL) {
		use_engine_pins();
		i2c_txn = geni_i2c_txn;
		show_state("powered, on the serial engine");
		if (chip_probe(true)) {
			return true;
		}
		printf("oneplus6t: the chip did not answer through the serial engine; trying the pins by hand\n");
	} else {
		printf("oneplus6t: %s; driving the pins by hand\n", problem);
	}

	i2c_txn = bitbang_txn;
	bitbang_init();
	if (chip_probe(true)) {
		printf("oneplus6t: the chip answers on pins driven by hand\n");
		return true;
	}
	show_state("no answer");
	return false;
}

static uint64_t
micros_now(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static void
oneplus6t_thread(__unused void *parameter, __unused wait_result_t result)
{
	unsigned idle = 0, idle_looks;
	uint32_t poll_us = 8000, reads = 0;
	uint64_t reading = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 8000;
	}
	idle_looks = 80000 / poll_us;   /* a report read every 80 ms even with the line high, in case it is missed */

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);      /* memory, not registers: no alignment rules */

	if (!bring_up()) {
		printf("oneplus6t: no Synaptics touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("oneplus6t: Synaptics S3706 ready; the interrupt line is looked at every %u us\n", poll_us);
	for (unsigned tick = 1;; tick++) {
		uint64_t looked = micros_now();

		/* the interrupt line is low while a report waits; look anyway every 80 ms in case it is missed */
		if (!gpio_get(PIN_INT)) {
			idle = 0;
			chip_poll();
			reads++;
			reading += micros_now() - looked;
		} else if (++idle >= idle_looks) {
			idle = 0;
			chip_poll();
			reads++;
			reading += micros_now() - looked;
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("oneplus6t: so far %u reports in %u looks (%u bad), %u finger positions, the last at %u, %u; "
			    "interrupt line %s; a report takes %llu us to read; the line looked at every %u us\n",
			    seen.reports, seen.polls, seen.bad, seen.touches, seen.last_x, seen.last_y,
			    gpio_get(PIN_INT) ? "high" : "low", reads ? reading / reads : 0, poll_us);
		}
		sleep_us(poll_us);
	}
}

void
hakios_oneplus6t_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	/* touchscreen=2 selects this phone; touchscreen=1 is the POCO's Goodix/FocalTech (hakios.goodix.c) */
	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || enabled != 2) {
		return;
	}
	PE_parse_boot_argn("touch_flip", &flip, sizeof(flip));
	PE_parse_boot_argn("touch_max_x", &max_x, sizeof(max_x));
	PE_parse_boot_argn("touch_max_y", &max_y, sizeof(max_y));
	PE_parse_boot_argn("touch_f12_objects", &f12_objects, sizeof(f12_objects));
	PE_parse_boot_argn("touch_f12_data_offset", &f12_data_offset, sizeof(f12_data_offset));
	if (f12_objects == 0 || f12_objects > F12_MAX_OBJECTS) {
		f12_objects = F12_MAX_OBJECTS;
	}
	if (max_x == 0 || max_y == 0) {
		max_x = 1080;
		max_y = 2340;
	}
	if (kernel_thread_start(oneplus6t_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
