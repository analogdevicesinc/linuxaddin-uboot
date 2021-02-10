/*
 * U-boot - main board file
 *
 * Copyright (c) 2013-2014 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <common.h>
#include <netdev.h>
#include <phy.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/mach-types.h>
#include <asm/arch/portmux.h>
#include <asm/arch/sc58x.h>
#include <asm/arch-sc58x/dwmmc.h>
#include <watchdog.h>

DECLARE_GLOBAL_DATA_PTR;
int board_early_init_f(void)
{
#ifdef CONFIG_HW_WATCHDOG
	hw_watchdog_init();
#endif

	return 0;
}

void set_spu_securep_msec(int n, bool msec)
{
	void __iomem *p = (void __iomem *)(REG_SPU0_CTL + 0xA00 + 4 * n);
	u32 securep = readl(p);

	if (msec)
		writel(securep | 0x2, p);
	else
		writel(securep & ~0x2, p);
}

/* miscellaneous platform dependent initialisations */
int misc_init_r(void)
{
	printf("other init\n");
	set_spu_securep_msec(55, 1);
	set_spu_securep_msec(56, 1);
	set_spu_securep_msec(58, 1);
	set_spu_securep_msec(153, 1);
	return 0;
}

unsigned long flash_init(void)
{

#if 0
	/*  Enable bank 1 of smc because AMS1 is connected to enable of SRAM */
	*pREG_SMC0_GCTL = 0x10;
	*pREG_SMC0_B1CTL = 0x00000001;
	*pREG_SMC0_B1TIM = 0x05220522;
#endif
	return 0;
}

int dram_init()
{
	gd->ram_size = CONFIG_SYS_SDRAM_SIZE;
	return 0;
}

void s_init(void)
{
}

#ifdef CONFIG_DESIGNWARE_ETH
int board_eth_init(bd_t *bis)
{
	int ret = 0;
	static const unsigned short pins[] = P_RGMII0;

	gpio_request(GPIO_PB7, "ethphy_reset");
	gpio_request(GPIO_PF6, "ethphy_pwdn");
	gpio_direction_output(GPIO_PF6, 1);
	gpio_direction_output(GPIO_PB7, 1);
	mdelay(20);
	gpio_direction_output(GPIO_PB7, 0);
	mdelay(90);
	gpio_direction_output(GPIO_PB7, 1);
	mdelay(20);

	writel((readl(REG_PADS0_PCFG0) | 0xc), REG_PADS0_PCFG0);
	if (!peripheral_request_list(pins, "emac0"))
		ret += designware_initialize(REG_EMAC0_MACCFG,
				PHY_INTERFACE_MODE_RGMII);
	return ret;
}

int board_phy_config(struct phy_device *phydev)
{
	int  phy_data = 0;

	/* enable RGMII mode */
	phy_data = phy_read(phydev, MDIO_DEVAD_NONE, 0x32);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x32, (1 << 7) | phy_data);
#ifdef CONFIG_PHY_TI
	int cfg3 = 0;
	#define MII_DP83867_CFG3    (0x1e)
	/*
	 * Pin INT/PWDN on DP83867 should be configured as an Interrupt Output
	 * instead of a Power-Down Input on ADI SC5XX boards in order to
	 * prevent the signal interference from other peripherals during they
	 * are running at the same time.
	 */
	 cfg3 = phy_read(phydev, MDIO_DEVAD_NONE, MII_DP83867_CFG3);
	 cfg3 |= (1 << 7);
	 phy_write(phydev, MDIO_DEVAD_NONE, MII_DP83867_CFG3, cfg3);
#endif

	if (phydev->drv->config)
		phydev->drv->config(phydev);

	return 0;
}
#endif

#ifdef CONFIG_GENERIC_MMC
int board_mmc_init(bd_t *bis)
{
	int ret;
#ifdef CONFIG_DWMMC
	ret = sc5xx_dwmmc_init(bis);
	if (ret)
		printf("dwmmc init failed\n");
#endif
	return ret;
}
#endif

int board_init(void)
{
	gd->bd->bi_arch_number = MACH_TYPE_SC589_MINI;
	/* boot param addr */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + (0x100);

	return 0;
}
#ifdef CONFIG_SPL_BUILD
#include <common.h>
#include <asm/io.h>
#include <spl.h>
#include <asm/spl.h>
#include <environment.h>
#ifdef CONFIG_SPL_BOARD_INIT
void spl_board_init(void)
{
	board_init();
}
#endif

void board_init_f(ulong dummy)
{
	/* Clear the BSS. */
	memset(__bss_start, 0, __bss_end - __bss_start);

	/* Set global data pointer. */
	gd = &gdata;

	preloader_console_init();
	board_init_r(NULL, 0);
}

u32 spl_boot_device(void)
{
	/* Currently we only support SPI nor boot*/
#if defined(CONFIG_SPL_SPI_LOAD)
#if SPL_VERBOSE
	puts("SPI Boot\n");
#endif
	return BOOT_DEVICE_SPI;
#endif
}

#ifdef CONFIG_SPL_OS_BOOT
enum e_bootmode {
	e_boot_os,
	e_boot_uboot
};

int spl_start_uboot(void)
{
	int ret;
	int bootmode = e_boot_os;

	/*
	 * PB1 PF_00 selects SPL bootmode:
	 * 0: boot OS
	 * 1: (button pressed) boot uboot
	 * if error accessing gpio boot U-Boot
	 *
	 */
	ret = gpio_request(SC5XX_SPL_OS_BOOT_KEY, "spl bootmode");
	if (ret) {
		bootmode = e_boot_uboot;
	} else {
		ret = gpio_direction_input(SC5XX_SPL_OS_BOOT_KEY);
		if (ret) {
			bootmode = e_boot_uboot;
		} else {
			ret = gpio_get_value(SC5XX_SPL_OS_BOOT_KEY);
			if (ret == 1)
				bootmode = e_boot_uboot;
		}
	}

#if SPI_VERBOSE
	if (bootmode == e_boot_uboot)
		printf("Starting U-Boot\n");
	else
		printf("Starting OS\n");
#endif
	return bootmode;
}
#endif
#endif
