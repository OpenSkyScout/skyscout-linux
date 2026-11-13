// SPDX-License-Identifier: GPL-2.0-only
/*
 * Celestron SkyScout board support
 *
 * The S3C2410 core devices are populated from DT.  The framebuffer and SDI
 * drivers still require legacy platform data in Linux 6.1.
 */

#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/mmc/host.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/platform_data/fb-s3c2410.h>
#include <linux/platform_data/mmc-s3cmci.h>

#include <asm/mach/arch.h>

#include "cpu.h"
#include "devs.h"
#include "gpio-samsung-s3c24xx.h"
#include "pm.h"
#include "regs-gpio-s3c24xx.h"
#include "s3c24xx.h"

/*
 * Register values recovered from firmware 1.30.22:
 *
 *   LCDCON1 0x00005824 (before ENVID)
 *   LCDCON2 0x0013c000
 *   LCDCON3 0x00003b00
 *   LCDCON4 0x00000d00
 *   LCDCON5 0x0000000a (while enabled)
 */
static struct s3c2410fb_display skyscout_lcd __initdata = {
	.type		= S3C2410_LCDCON1_STN4,
	.width		= 240,
	.height		= 80,
	.xres		= 240,
	.yres		= 80,
	.bpp		= 4,
	/*
	 * At the observed 67.5 MHz HCLK this makes calc_pixclk() return 176;
	 * DIV_ROUND_UP(176, 2) reproduces LCDCON1.CLKVAL=88 (0x5800).
	 */
	.pixclock	= 2607408,
	.left_margin	= 16,
	.right_margin	= 0,
	.hsync_len	= 16,
	.upper_margin	= 0,
	.lower_margin	= 0,
	.vsync_len	= 0,
	.lcdcon5	= S3C2410_LCDCON5_BSWP |
			  S3C2410_LCDCON5_PWREN,
};

static struct s3c2410fb_mach_info skyscout_fb_info __initdata = {
	.displays		= &skyscout_lcd,
	.num_displays		= 1,
	.default_display	= 0,
	.lcdcon4_extra		= 0x00000d00,

	.gpccon			= 0xaaaaaaaa,
	.gpccon_mask		= 0xffffffff,
	.gpccon_reg		= S3C2410_GPCCON,
	.gpcup			= 0x0000ffff,
	.gpcup_mask		= 0x0000ffff,
	.gpcup_reg		= S3C2410_GPCUP,

	.gpdcon			= 0xaaaaaaaa,
	.gpdcon_mask		= 0xffffffff,
	.gpdcon_reg		= S3C2410_GPDCON,
	.gpdup			= 0x0000ffff,
	.gpdup_mask		= 0x0000ffff,
	.gpdup_reg		= S3C2410_GPDUP,
};

/*
 * The SkyScout routes only DAT0, in addition to CLK and CMD.  The legacy
 * s3cmci probe obtains all six standard bus descriptors, but this callback
 * deliberately muxes only GPE5..GPE7 so the electrical bus remains one bit.
 */
static void skyscout_mci_set_power(unsigned char power_mode, unsigned short vdd)
{
	u32 con;

	con = readl(S3C2410_GPECON);
	con &= ~(0x3f << 10);

	if (power_mode == MMC_POWER_ON || power_mode == MMC_POWER_UP) {
		u32 up;

		/* GPE5=SDCLK, GPE6=SDCMD, GPE7=SDDAT0: function 2. */
		con |= 0x2a << 10;
		writel(con, S3C2410_GPECON);

		up = readl(S3C2410_GPEUP);
		up |= GENMASK(7, 5);
		writel(up, S3C2410_GPEUP);
	} else {
		u32 dat;

		/* Hold SDCLK low while the controller is powered down. */
		con |= 0x1 << 10;
		writel(con, S3C2410_GPECON);
		dat = readl(S3C2410_GPEDAT);
		dat &= ~BIT(5);
		writel(dat, S3C2410_GPEDAT);
	}
}

static struct s3c24xx_mci_pdata skyscout_mci_pdata __initdata = {
	.no_wprotect	= 1,
	.no_detect	= 1,
	.use_dma	= 0,
	.bus_width	= 1,
	.ocr_avail	= MMC_VDD_32_33 | MMC_VDD_33_34,
	.set_power	= skyscout_mci_set_power,
};

/*
 * The descriptor lookup is also useful on the DT boot path: the S3C2412
 * pinctrl data used by the board names the register-compatible bank "gpe".
 */
static struct gpiod_lookup_table skyscout_mmc_gpio_table = {
	.dev_id = "s3c2410-sdi",
	.table = {
		GPIO_LOOKUP_IDX("gpe", 5, "bus", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpe", 6, "bus", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpe", 7, "bus", 2, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpe", 8, "bus", 3, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpe", 9, "bus", 4, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX("gpe", 10, "bus", 5, GPIO_ACTIVE_HIGH),
		{ }
	},
};

static struct platform_device *skyscout_devices[] __initdata = {
	&s3c_device_lcd,
	&s3c_device_sdi,
};

static void __init skyscout_map_legacy_irq(struct platform_device *pdev,
					   irq_hw_number_t hwirq)
{
	struct device_node *np;
	struct irq_domain *domain;
	struct resource *resource;
	unsigned int irq;

	/*
	 * The LCD and SDI drivers still use legacy platform devices, while the
	 * interrupt controller is initialized from DT and therefore allocates
	 * Linux IRQ numbers dynamically.  Replace the old fixed IRQ resources
	 * with mappings from the controller's hardware IRQ numbers.
	 */
	np = of_find_compatible_node(NULL, NULL, "samsung,s3c2410-irq");
	if (!np)
		panic("SkyScout: interrupt controller node is missing\n");

	domain = irq_find_host(np);
	of_node_put(np);
	if (!domain)
		panic("SkyScout: interrupt domain is unavailable\n");

	irq = irq_create_mapping(domain, hwirq);
	if (!irq)
		panic("SkyScout: cannot map hardware IRQ %lu\n",
		      (unsigned long)hwirq);

	resource = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!resource)
		panic("SkyScout: %s has no IRQ resource\n", pdev->name);

	resource->start = irq;
	resource->end = irq;
}

static void __init skyscout_map_io(void)
{
	s3c24xx_init_io(NULL, 0);
	s3c24xx_set_timer_source(S3C24XX_PWM3, S3C24XX_PWM4);
}

static void __init skyscout_machine_init(void)
{
	s3c24xx_fb_set_platdata(&skyscout_fb_info);
	s3c24xx_mci_set_platdata(&skyscout_mci_pdata);
	gpiod_add_lookup_table(&skyscout_mmc_gpio_table);

	skyscout_map_legacy_irq(&s3c_device_lcd, 16);
	skyscout_map_legacy_irq(&s3c_device_sdi, 21);
	platform_add_devices(skyscout_devices, ARRAY_SIZE(skyscout_devices));
	s3c_pm_init();
}

static const char *const skyscout_dt_compat[] __initconst = {
	"celestron,skyscout",
	NULL,
};

DT_MACHINE_START(SKYSCOUT, "Celestron SkyScout")
	.dt_compat	= skyscout_dt_compat,
	.map_io		= skyscout_map_io,
	.init_irq	= irqchip_init,
	.init_machine	= skyscout_machine_init,
MACHINE_END
