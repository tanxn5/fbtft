#include <linux/export.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include "fbtft.h"

#define gpio_get_value(__id) \
			unlikely(gpio_cansleep(__id)==0) ? \
			gpio_get_value(__id) : \
			gpio_get_value_cansleep(__id)
#define gpio_set_value(__id,__val) \
		{ \
			if (unlikely(gpio_cansleep(__id)==0)) \
				gpio_set_value(__id, __val); \
			else \
				gpio_set_value_cansleep(__id, __val); \
		}

int fbtft_write_spi(struct fbtft_par *par, void *buf, size_t len)
{
	int ret;
	struct spi_transfer t = {
		.tx_buf = buf,
		.len = len,
	};
	struct spi_message m;

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (!par->spi) {
		dev_err(par->info->device,
			"%s: par->spi is unexpectedly NULL\n", __func__);
		return -1;
	}

	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	spi_message_init(&m);
	if (par->txbuf.dma && buf == par->txbuf.buf) {
		t.tx_dma = par->txbuf.dma;
		m.is_dma_mapped = 1;
	}
	spi_message_add_tail(&t, &m);
	ret = spi_sync(par->spi, &m);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);
	return ret;
}
EXPORT_SYMBOL(fbtft_write_spi);

/**
 * fbtft_write_spi_emulate_9() - write SPI emulating 9-bit
 * @par: Driver data
 * @buf: Buffer to write
 * @len: Length of buffer (must be divisible by 8)
 *
 * When 9-bit SPI is not available, this function can be used to emulate that.
 * par->extra must hold a transformation buffer used for transfer.
 */
int fbtft_write_spi_emulate_9(struct fbtft_par *par, void *buf, size_t len)
{
	u16 *src = buf;
	u8 *dst = par->extra;
	size_t size = len / 2;
	size_t added = 0;
	int bits, i, j, ret;
	u64 val, dc, tmp;

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (!par->extra) {
		dev_err(par->info->device, "%s: error: par->extra is NULL\n",
			__func__);
		return -EINVAL;
	}
	if ((len % 8) != 0) {
		dev_err(par->info->device,
			"error: len=%zu must be divisible by 8\n", len);
		return -EINVAL;
	}

	for (i = 0; i < size; i += 8) {
		tmp = 0;
		bits = 63;
		for (j = 0; j < 7; j++) {
			dc = (*src & 0x0100) ? 1 : 0;
			val = *src & 0x00FF;
			tmp |= dc << bits;
			bits -= 8;
			tmp |= val << bits--;
			src++;
		}
		tmp |= ((*src & 0x0100) ? 1 : 0);
		*(u64 *)dst = cpu_to_be64(tmp);
		dst += 8;
		*dst++ = (u8)(*src++ & 0x00FF);
		added++;
	}

	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	ret = spi_write(par->spi, par->extra, size + added);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);
	return ret;
}
EXPORT_SYMBOL(fbtft_write_spi_emulate_9);

int fbtft_read_spi(struct fbtft_par *par, void *buf, size_t len)
{
	int ret;
	u8 txbuf[32] = { 0, };
	struct spi_transfer	t = {
			.speed_hz = 2000000,
			.rx_buf		= buf,
			.len		= len,
		};
	struct spi_message	m;

	if (!par->spi) {
		dev_err(par->info->device,
			"%s: par->spi is unexpectedly NULL\n", __func__);
		return -ENODEV;
	}

	if (par->startbyte) {
		if (len > 32) {
			dev_err(par->info->device,
				"len=%zu can't be larger than 32 when using 'startbyte'\n",
				len);
			return -EINVAL;
		}
		txbuf[0] = par->startbyte | 0x3;
		t.tx_buf = txbuf;
		fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8,
			txbuf, len, "%s(len=%d) txbuf => ", __func__, len);
	}

	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(par->spi, &m);
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);
	fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8, buf, len,
		"%s(len=%d) buf <= ", __func__, len);

	return ret;
}
EXPORT_SYMBOL(fbtft_read_spi);

int fbtft_write_gpio8_rd(struct fbtft_par *par, void *buf, size_t len)
{
	u8 *data;
	int i;
	size_t bytesLeft = len;
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	while (bytesLeft--) {
		data = (u8 *) buf;

		/* Start reading by pulling down /RD */
		gpio_set_value(par->gpio.rd, 0);

		mdelay(1);

		/* Get data */
		*data = 0;
		for (i = 7; i >= 0; i--) {
			*data <<= 1;
			*data |= gpio_get_value(par->gpio.db[i]) & 1;
		}

		/* Pullup /RD */
		gpio_set_value(par->gpio.rd, 1);
		buf++;
	}
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);
	fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8, buf, len,
		"%s(len=%d) buf <= ", __func__, len);

	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio8_rd);

/*
 * Optimized use of gpiolib is twice as fast as no optimization
 * only one driver can use the optimized version at a time
 */
int fbtft_write_gpio8_wr(struct fbtft_par *par, void *buf, size_t len)
{
	u8 data;
	int i;
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
	static u8 prev_data;
#endif

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	while (len--) {
		data = *(u8 *) buf;

		/* Start writing by pulling down /WR */
		gpio_set_value(par->gpio.wr, 0);

		/* Set data */
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		if (data == prev_data) {
			gpio_set_value(par->gpio.wr, 0); /* used as delay */
		} else {
			for (i = 0; i < 8; i++) {
				if ((data & 1) != (prev_data & 1))
					gpio_set_value(par->gpio.db[i],
								data & 1);
				data >>= 1;
				prev_data >>= 1;
			}
		}
#else
		for (i = 0; i < 8; i++) {
			gpio_set_value(par->gpio.db[i], data & 1);
			data >>= 1;
		}
#endif

		/* Pullup /WR */
		gpio_set_value(par->gpio.wr, 1);

#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		prev_data = *(u8 *) buf;
#endif
		buf++;
	}
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);

	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio8_wr);

int fbtft_write_gpio16_rd(struct fbtft_par *par, void *buf, size_t len)
{
	u16 *data;
	int i;
	size_t bytesLeft = len;
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	while (bytesLeft) {
		data = (u16 *) buf;

		/* Start reading by pulling down /RD */
		gpio_set_value(par->gpio.rd, 0);

		mdelay(1);

		/* Get data */
		*data = 0;
		for (i = 15; i >= 0; i--) {
			*data <<= 1;
			*data |= gpio_get_value(par->gpio.db[i]) & 1;
		}

		/* Pullup /RD */
		gpio_set_value(par->gpio.rd, 1);
		buf += 2;
		bytesLeft += 2;
	}
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);
	fbtft_par_dbg_hex(DEBUG_READ, par, par->info->device, u8, buf, len,
		"%s(len=%d) buf <= ", __func__, len);

	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio16_rd);

int fbtft_write_gpio16_wr(struct fbtft_par *par, void *buf, size_t len)
{
	u16 data;
	int i;
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
	static u16 prev_data;
#endif

	fbtft_par_dbg_hex(DEBUG_WRITE, par, par->info->device, u8, buf, len,
		"%s(len=%d): ", __func__, len);

	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 0);
	while (len) {
		data = *(u16 *) buf;

		/* Start writing by pulling down /WR */
		gpio_set_value(par->gpio.wr, 0);

		/* Set data */
#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		if (data == prev_data) {
			gpio_set_value(par->gpio.wr, 0); /* used as delay */
		} else {
			for (i = 0; i < 16; i++) {
				if ((data & 1) != (prev_data & 1))
					gpio_set_value(par->gpio.db[i],
								data & 1);
				data >>= 1;
				prev_data >>= 1;
			}
		}
#else
		for (i = 0; i < 16; i++) {
			gpio_set_value(par->gpio.db[i], data & 1);
			data >>= 1;
		}
#endif

		/* Pullup /WR */
		gpio_set_value(par->gpio.wr, 1);

#ifndef DO_NOT_OPTIMIZE_FBTFT_WRITE_GPIO
		prev_data = *(u16 *) buf;
#endif
		buf += 2;
		len -= 2;
	}
	if (par->gpio.cs != -1)
		gpio_set_value(par->gpio.cs, 1);

	return 0;
}
EXPORT_SYMBOL(fbtft_write_gpio16_wr);

int fbtft_write_gpio16_wr_latched(struct fbtft_par *par, void *buf, size_t len)
{
	dev_err(par->info->device, "%s: function not implemented\n", __func__);
	return -1;
}
EXPORT_SYMBOL(fbtft_write_gpio16_wr_latched);
