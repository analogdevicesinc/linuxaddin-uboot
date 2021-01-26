/*
 * Copyright (C) 2012
 * Altera Corporation <www.altera.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <fdtdec.h>
#include <malloc.h>
#include <spi.h>
#include <asm/errno.h>
#include "cadence_qspi.h"

#define CQSPI_STIG_READ			0
#define CQSPI_STIG_WRITE		1
#define CQSPI_INDIRECT_READ		2
#define CQSPI_INDIRECT_WRITE	3

#ifdef CONFIG_SC59X
#define CQSPI_DIRECT_READ		4
#define CQSPI_DIRECT_WRITE		5
#endif

DECLARE_GLOBAL_DATA_PTR;

static int cadence_spi_write_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);

	cadence_qspi_apb_config_baudrate_div(priv->regbase,
					     CONFIG_CQSPI_REF_CLK, hz);

	/* Reconfigure delay timing if speed is changed. */
	cadence_qspi_apb_delay(priv->regbase, CONFIG_CQSPI_REF_CLK, hz,
			       plat->tshsl_ns, plat->tsd2d_ns,
			       plat->tchsh_ns, plat->tslch_ns);

	return 0;
}

/* Calibration sequence to determine the read data capture delay register */
static int spi_calibration(struct udevice *bus)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	void *base = priv->regbase;
	u8 opcode_rdid = 0x9F;
	unsigned int idcode = 0, temp = 0;
	int err = 0, i, range_lo = -1, range_hi = -1;

	/* start with slowest clock (1 MHz) */
	cadence_spi_write_speed(bus, 1000000);

	/* configure the read data capture delay register to 0 */
	cadence_qspi_apb_readdata_capture(base, 1, 0);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(base);

	/* read the ID which will be our golden value */
	err = cadence_qspi_apb_command_read(base, 1, &opcode_rdid,
		3, (u8 *)&idcode);
	if (err) {
		puts("SF: Calibration failed (read)\n");
		return err;
	}

	/* use back the intended clock and find low range */
	cadence_spi_write_speed(bus, plat->max_hz);
	for (i = 0; i < CQSPI_READ_CAPTURE_MAX_DELAY; i++) {
		/* Disable QSPI */
		cadence_qspi_apb_controller_disable(base);

		/* reconfigure the read data capture delay register */
		cadence_qspi_apb_readdata_capture(base, 1, i);

		/* Enable back QSPI */
		cadence_qspi_apb_controller_enable(base);

		/* issue a RDID to get the ID value */
		err = cadence_qspi_apb_command_read(base, 1, &opcode_rdid,
			3, (u8 *)&temp);
		if (err) {
			puts("SF: Calibration failed (read)\n");
			return err;
		}

		/* search for range lo */
		if (range_lo == -1 && temp == idcode) {
			range_lo = i;
			continue;
		}

		/* search for range hi */
		if (range_lo != -1 && temp != idcode) {
			range_hi = i - 1;
			break;
		}
		range_hi = i;
	}

	if (range_lo == -1) {
		puts("SF: Calibration failed (low range)\n");
		return err;
	}

	/* Disable QSPI for subsequent initialization */
	cadence_qspi_apb_controller_disable(base);

	/* configure the final value for read data capture delay register */
	cadence_qspi_apb_readdata_capture(base, 1, (range_hi + range_lo) / 2);
	debug("SF: Read data capture delay calibrated to %i (%i - %i)\n",
	      (range_hi + range_lo) / 2, range_lo, range_hi);

	/* just to ensure we do once only when speed or chip select change */
	priv->qspi_calibrated_hz = plat->max_hz;
	priv->qspi_calibrated_cs = spi_chip_select(bus);

	return 0;
}

static int cadence_spi_set_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	int err;

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(priv->regbase);

	cadence_spi_write_speed(bus, hz);

	/* Calibration required for different SCLK speed or chip select */
	if (priv->qspi_calibrated_hz != plat->max_hz ||
	    priv->qspi_calibrated_cs != spi_chip_select(bus)) {
		err = spi_calibration(bus);
		if (err)
			return err;
	}

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(priv->regbase);

	debug("%s: speed=%d\n", __func__, hz);

	return 0;
}

static int cadence_spi_probe(struct udevice *bus)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);

	priv->regbase = plat->regbase;
	priv->ahbbase = plat->ahbbase;

	if (!priv->qspi_is_init) {
		cadence_qspi_apb_controller_init(plat);
		priv->qspi_is_init = 1;
	}

	return 0;
}

static int cadence_spi_set_mode(struct udevice *bus, uint mode)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	unsigned int clk_pol = (mode & SPI_CPOL) ? 1 : 0;
	unsigned int clk_pha = (mode & SPI_CPHA) ? 1 : 0;

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(priv->regbase);

	/* Set SPI mode */
	cadence_qspi_apb_set_clk_mode(priv->regbase, clk_pol, clk_pha);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(priv->regbase);

	return 0;
}

#ifdef CONFIG_SC59X
	static uint8_t enable_4byte[1] = {0xB7};
	static uint8_t disable_4byte[1] = {0xE9};
#endif

static int cadence_spi_xfer(struct udevice *dev, unsigned int bitlen,
			    const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct cadence_spi_platdata *plat = bus->platdata;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	void *base = priv->regbase;
	u8 *cmd_buf = priv->cmd_buf;
	size_t data_bytes;
	int err = 0;
	u32 mode = CQSPI_STIG_WRITE;

	if (flags & SPI_XFER_BEGIN) {
		/* copy command to local buffer */
		priv->cmd_len = bitlen / 8;
		memcpy(cmd_buf, dout, priv->cmd_len);
	}

	if (flags == (SPI_XFER_BEGIN | SPI_XFER_END)) {
		/* if start and end bit are set, the data bytes is 0. */
		data_bytes = 0;
	} else {
		data_bytes = bitlen / 8;
	}
	debug("%s: len=%d [bytes]\n", __func__, data_bytes);

	/* Set Chip select */
	cadence_qspi_apb_chipselect(base, spi_chip_select(dev),
				    CONFIG_CQSPI_DECODER);

	if ((flags & SPI_XFER_END) || (flags == 0)) {
		if (priv->cmd_len == 0) {
			printf("QSPI: Error, command is empty.\n");
			return -1;
		}

		if (din && data_bytes) {
			/* read */
			/* Use STIG if no address. */
			if (!CQSPI_IS_ADDR(priv->cmd_len)){
				mode = CQSPI_STIG_READ;
			}else{
				#ifdef CONFIG_SC59X
				mode = CQSPI_DIRECT_READ;
				#else
				mode = CQSPI_INDIRECT_READ;
				#endif
			}
		} else if (dout && !(flags & SPI_XFER_BEGIN)) {
			/* write */
			if (!CQSPI_IS_ADDR(priv->cmd_len)){
				mode = CQSPI_STIG_WRITE;
			}else{
				#ifdef CONFIG_SC59X
				mode = CQSPI_DIRECT_WRITE;
				#else
				mode = CQSPI_INDIRECT_READ;
				#endif
			}
		}

		switch (mode) {
		case CQSPI_STIG_READ:
		{
#ifdef CONFIG_SC59X	
			if(priv->cmd_len == 5){
				cadence_qspi_apb_command_write(plat->regbase, 1, enable_4byte, 0, NULL);
			}else if(priv->cmd_len == 4){
				cadence_qspi_apb_command_write(plat->regbase, 1, disable_4byte, 0, NULL);
			}
#endif
			err = cadence_qspi_apb_command_read(
				base, priv->cmd_len, cmd_buf,
				data_bytes, din);


		}
		break;
		case CQSPI_STIG_WRITE:
		{
#ifdef CONFIG_SC59X			
			static int octalDDR = 0;
			if(!octalDDR){
				octalDDR = 1;
				printf("Set VCR to Octal DDR without DQS\r\n");
				uint8_t cmd[1] = {0x81};
				uint8_t data[1] = {0xC7};
				cadence_qspi_apb_command_write(base,
								1, cmd,
								1, data);
			}

			if(priv->cmd_len == 5){
				cadence_qspi_apb_command_write(plat->regbase, 1, enable_4byte, 0, NULL);
			}else if(priv->cmd_len == 4){
				cadence_qspi_apb_command_write(plat->regbase, 1, disable_4byte, 0, NULL);
			}
#endif

			err = cadence_qspi_apb_command_write(base,
				priv->cmd_len, cmd_buf,
				data_bytes, dout);
		}
		break;
		case CQSPI_INDIRECT_READ:
			err = cadence_qspi_apb_indirect_read_setup(plat,
				priv->cmd_len, cmd_buf);
			if (!err) {
				err = cadence_qspi_apb_indirect_read_execute
				(plat, data_bytes, din);
			}
		break;
		case CQSPI_INDIRECT_WRITE:
			err = cadence_qspi_apb_indirect_write_setup
				(plat, priv->cmd_len, cmd_buf);
			if (!err) {
				err = cadence_qspi_apb_indirect_write_execute
				(plat, data_bytes, dout);
			}
		break;
#ifdef CONFIG_SC59X
		case CQSPI_DIRECT_WRITE:
			if(priv->cmd_len == 5){
				cadence_qspi_apb_command_write(plat->regbase, 1, enable_4byte, 0, NULL);
			}else if(priv->cmd_len == 4){
				cadence_qspi_apb_command_write(plat->regbase, 1, disable_4byte, 0, NULL);
			}

			err = cadence_qspi_direct_write
				(plat, priv->cmd_len, cmd_buf, data_bytes, dout);
		break;
		case CQSPI_DIRECT_READ:
			if(priv->cmd_len == 5){
				cadence_qspi_apb_command_write(plat->regbase, 1, enable_4byte, 0, NULL);
			}else if(priv->cmd_len == 4){
				cadence_qspi_apb_command_write(plat->regbase, 1, disable_4byte, 0, NULL);
			}

			err = cadence_qspi_direct_read
				(plat, priv->cmd_len, cmd_buf, data_bytes, din);
		break;
#endif
		default:
			err = -1;
			break;
		}

		if (flags & SPI_XFER_END) {
			/* clear command buffer */
			memset(cmd_buf, 0, sizeof(priv->cmd_buf));
			priv->cmd_len = 0;
		}
	}

	return err;
}

static int cadence_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct cadence_spi_platdata *plat = bus->platdata;
	const void *blob = gd->fdt_blob;
	int node = bus->of_offset;
	int subnode;
	u32 data[4];
	int ret;

	/* 2 base addresses are needed, lets get them from the DT */
	ret = fdtdec_get_int_array(blob, node, "reg", data, ARRAY_SIZE(data));
	if (ret) {
		printf("Error: Can't get base addresses (ret=%d)!\n", ret);
		return -ENODEV;
	}

	plat->regbase = (void *)data[0];
	plat->ahbbase = (void *)data[2];

	/* Use 500KHz as a suitable default */
	plat->max_hz = fdtdec_get_int(blob, node, "spi-max-frequency",
				      500000);

	/* All other paramters are embedded in the child node */
	subnode = fdt_first_subnode(blob, node);
	if (subnode < 0) {
		printf("Error: subnode with SPI flash config missing!\n");
		return -ENODEV;
	}

	/* Read other parameters from DT */
	plat->page_size = fdtdec_get_int(blob, subnode, "page-size", 256);
	plat->block_size = fdtdec_get_int(blob, subnode, "block-size", 16);
	plat->tshsl_ns = fdtdec_get_int(blob, subnode, "tshsl-ns", 200);
	plat->tsd2d_ns = fdtdec_get_int(blob, subnode, "tsd2d-ns", 255);
	plat->tchsh_ns = fdtdec_get_int(blob, subnode, "tchsh-ns", 20);
	plat->tslch_ns = fdtdec_get_int(blob, subnode, "tslch-ns", 20);

	debug("%s: regbase=%p ahbbase=%p max-frequency=%d page-size=%d\n",
	      __func__, plat->regbase, plat->ahbbase, plat->max_hz,
	      plat->page_size);

	return 0;
}

static const struct dm_spi_ops cadence_spi_ops = {
	.xfer		= cadence_spi_xfer,
	.set_speed	= cadence_spi_set_speed,
	.set_mode	= cadence_spi_set_mode,
	/*
	 * cs_info is not needed, since we require all chip selects to be
	 * in the device tree explicitly
	 */
};

static const struct udevice_id cadence_spi_ids[] = {
	{ .compatible = "cadence,qspi" },
	{ }
};

U_BOOT_DRIVER(cadence_spi) = {
	.name = "cadence_spi",
	.id = UCLASS_SPI,
	.of_match = cadence_spi_ids,
	.ops = &cadence_spi_ops,
	.ofdata_to_platdata = cadence_spi_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct cadence_spi_platdata),
	.priv_auto_alloc_size = sizeof(struct cadence_spi_priv),
	.per_child_auto_alloc_size = sizeof(struct spi_slave),
	.probe = cadence_spi_probe,
};
