/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <sys/errno.h>

#include <glib/gstdio.h>

#include "fu-chunk.h"
#include "fu-superio-common.h"
#include "fu-superio-device.h"

#define FU_PLUGIN_SUPERIO_TIMEOUT	0.25 /* s */

/* EC Status Register (see ec/google/chromeec/ec_commands.h) */
#define SIO_STATUS_EC_OBF		(1 << 0)	/* o/p buffer full */
#define SIO_STATUS_EC_IBF		(1 << 1)	/* i/p buffer full */
#define SIO_STATUS_EC_IS_BUSY		(1 << 2)
#define SIO_STATUS_EC_IS_CMD		(1 << 3)
#define SIO_STATUS_EC_BURST_ENABLE	(1 << 4)
#define SIO_STATUS_EC_SCI		(1 << 5)	/* 1 if more events in queue */

/* EC Command Register (see KB3700-ds-01.pdf) */
#define SIO_CMD_EC_READ			0x80
#define SIO_CMD_EC_WRITE		0x81
#define SIO_CMD_EC_BURST_ENABLE		0x82
#define SIO_CMD_EC_BURST_DISABLE	0x83
#define SIO_CMD_EC_QUERY_EVENT		0x84

/* unknown source, IT87 only */
#define SIO_CMD_EC_GET_NAME_STR		0x92
#define SIO_CMD_EC_GET_VERSION_STR	0x93

struct _FuSuperioDevice {
	FuDevice		 parent_instance;
	gint			 fd;
	gchar			*chipset;
	guint16			 port;
	guint16			 pm1_iobad0;
	guint16			 pm1_iobad1;
	guint16			 id;
	guint32			 size;
};

G_DEFINE_TYPE (FuSuperioDevice, fu_superio_device, FU_TYPE_DEVICE)

static void
fu_superio_device_to_string (FuDevice *device, GString *str)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	g_string_append (str, "  FuSuperioDevice:\n");
	g_string_append_printf (str, "    fd:\t\t\t%i\n", self->fd);
	g_string_append_printf (str, "    chipset:\t\t%s\n", self->chipset);
	g_string_append_printf (str, "    id:\t\t\t0x%04x\n", (guint) self->id);
	g_string_append_printf (str, "    port:\t\t0x%04x\n", (guint) self->port);
	g_string_append_printf (str, "    pm1-iobad0:\t\t0x%04x\n", (guint) self->pm1_iobad0);
	g_string_append_printf (str, "    pm1-iobad1:\t\t0x%04x\n", (guint) self->pm1_iobad1);
	g_string_append_printf (str, "    size:\t\t0x%04x\n", (guint) self->size);
}

static guint16
fu_superio_device_check_id (FuSuperioDevice *self, GError **error)
{
	guint16 id_tmp;

	/* check ID, which can be done from any LDN */
	if (!fu_superio_regval16 (self->fd, self->port,
				  SIO_LDNxx_IDX_CHIPID1, &id_tmp, error))
		return FALSE;
	if (self->id != id_tmp) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "SuperIO chip not supported, got %04x, expected %04x",
			     (guint) id_tmp, (guint) self->id);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_superio_device_wait_for (FuSuperioDevice *self, guint8 mask, gboolean set, GError **error)
{
	g_autoptr(GTimer) timer = g_timer_new ();
	do {
		guint8 status = 0x00;
		if (!fu_superio_inb (self->fd, self->pm1_iobad1, &status, error))
			return FALSE;
		if (g_timer_elapsed (timer, NULL) > FU_PLUGIN_SUPERIO_TIMEOUT)
			break;
		if (set && (status & mask) != 0)
			return TRUE;
		if (!set && (status & mask) == 0)
			return TRUE;
	} while (TRUE);
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_TIMED_OUT,
		     "timed out whilst waiting for 0x%02x:%i", mask, set);
	return FALSE;
}

static gboolean
fu_superio_device_ec_read (FuSuperioDevice *self,
			   guint16 port,
			   guint8 *data,
			   GError **error)
{
	if (!fu_superio_device_wait_for (self, SIO_STATUS_EC_OBF, TRUE, error)) {
		g_prefix_error (error, "ec-read: ");
		return FALSE;
	}
	return fu_superio_inb (self->fd, port, data, error);
}

static gboolean
fu_superio_device_ec_write (FuSuperioDevice *self,
			    guint16 port,
			    guint8 data,
			    GError **error)
{
	if (!fu_superio_device_wait_for (self, SIO_STATUS_EC_IBF, FALSE, error)) {
		g_prefix_error (error, "ec-write: ");
		return FALSE;
	}
	return fu_superio_outb (self->fd, port, data, error);
}

static gboolean
fu_superio_device_ec_flush (FuSuperioDevice *self, GError **error)
{
	guint8 status = 0x00;
	g_autoptr(GTimer) timer = g_timer_new ();
	do {
		guint8 unused = 0;
		if (!fu_superio_inb (self->fd, self->pm1_iobad1, &status, error))
			return FALSE;
		if ((status & SIO_STATUS_EC_OBF) == 0)
			break;
		if (!fu_superio_inb (self->fd, self->pm1_iobad0, &unused, error))
			return FALSE;
		if (g_timer_elapsed (timer, NULL) > FU_PLUGIN_SUPERIO_TIMEOUT) {
			g_set_error_literal (error,
					     G_IO_ERROR,
					     G_IO_ERROR_TIMED_OUT,
					     "timed out whilst waiting for flush");
			return FALSE;
		}
	} while (TRUE);
	return TRUE;
}

static gboolean
fu_superio_device_ec_get_param (FuSuperioDevice *self, guint8 param, guint8 *data, GError **error)
{
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_CMD_EC_READ, error))
		return FALSE;
	if (!fu_superio_device_ec_write (self, self->pm1_iobad0, param, error))
		return FALSE;
	return fu_superio_device_ec_read (self, self->pm1_iobad0, data, error);
}

#if 0
static gboolean
fu_superio_device_ec_set_param (FuSuperioDevice *self, guint8 param, guint8 data, GError **error)
{
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_CMD_EC_WRITE, error))
		return FALSE;
	if (!fu_superio_device_ec_write (self, self->pm1_iobad0, param, error))
		return FALSE;
	return fu_superio_device_ec_write (self, self->pm1_iobad0, data, error);
}
#endif

static gchar *
fu_superio_device_ec_get_str (FuSuperioDevice *self, guint8 idx, GError **error)
{
	GString *str = g_string_new (NULL);
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1, idx, error))
		return NULL;
	for (guint i = 0; i < 0xff; i++) {
		guint8 c = 0;
		if (!fu_superio_device_ec_read (self, self->pm1_iobad0, &c, error))
			return NULL;
		if (c == '$')
			break;
		g_string_append_c (str, c);
	}
	return g_string_free (str, FALSE);
}

static gboolean
fu_superio_device_open (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);

	/* open device */
	self->fd = g_open (fu_device_get_physical_id (device), O_RDWR);
	if (self->fd < 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "failed to open %s: %s",
			     fu_device_get_physical_id (device),
			     strerror (errno));
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_superio_device_probe (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	g_autofree gchar *devid = NULL;

	/* use the chipset name as the logical ID and for the GUID */
	fu_device_set_logical_id (device, self->chipset);
	devid = g_strdup_printf ("SuperIO-%s", self->chipset);
	fu_device_add_instance_id (device, devid);
	return TRUE;
}

static gboolean
fu_superio_device_setup_it85xx (FuSuperioDevice *self, GError **error)
{
	guint8 size_tmp = 0;
	g_autofree gchar *name = NULL;
	g_autofree gchar *version = NULL;

	/* get EC size */
	if (!fu_superio_device_ec_flush (self, error)) {
		g_prefix_error (error, "failed to flush: ");
		return FALSE;
	}
	if (!fu_superio_device_ec_get_param (self, 0xe5, &size_tmp, error)) {
		g_prefix_error (error, "failed to get EC size: ");
		return FALSE;
	}
	self->size = ((guint32) size_tmp) << 10;

	/* get EC strings */
	name = fu_superio_device_ec_get_str (self, SIO_CMD_EC_GET_NAME_STR, error);
	if (name == NULL) {
		g_prefix_error (error, "failed to get EC name: ");
		return FALSE;
	}
	fu_device_set_name (FU_DEVICE (self), name);
	version = fu_superio_device_ec_get_str (self, SIO_CMD_EC_GET_VERSION_STR, error);
	if (version == NULL) {
		g_prefix_error (error, "failed to get EC version: ");
		return FALSE;
	}
	fu_device_set_version (FU_DEVICE (self), version);
	return TRUE;
}

static gboolean
fu_superio_device_it89xx_read_ec_register (FuSuperioDevice *self,
					   guint16 addr,
					   guint8 *outval,
					   GError **error)
{
	if (!fu_superio_regwrite (self->fd, self->port,
				  SIO_LDNxx_IDX_D2ADR,
				  SIO_DEPTH2_I2EC_ADDRH,
				  error))
		return FALSE;
	if (!fu_superio_regwrite (self->fd, self->port,
				  SIO_LDNxx_IDX_D2DAT,
				  addr >> 8,
				  error))
		return FALSE;
	if (!fu_superio_regwrite (self->fd, self->port,
				  SIO_LDNxx_IDX_D2ADR,
				  SIO_DEPTH2_I2EC_ADDRL,
				  error))
		return FALSE;
	if (!fu_superio_regwrite (self->fd, self->port,
				  SIO_LDNxx_IDX_D2DAT,
				  addr & 0xff, error))
		return FALSE;
	if (!fu_superio_regwrite (self->fd, self->port,
				  SIO_LDNxx_IDX_D2ADR,
				  SIO_DEPTH2_I2EC_DATA,
				  error))
		return FALSE;
	return fu_superio_regval (self->fd, self->port,
				  SIO_LDNxx_IDX_D2DAT,
				  outval,
				  error);
}

static gboolean
fu_superio_device_it89xx_ec_size (FuSuperioDevice *self, GError **error)
{
	guint8 tmp = 0;

	/* not sure why we can't just use SIO_LDNxx_IDX_CHIPID1,
	 * but lets do the same as the vendor flash tool... */
	if (!fu_superio_device_it89xx_read_ec_register (self,
							GCTRL_ECHIPID1,
							&tmp,
							error))
		return FALSE;
	if (tmp == 0x85) {
		g_warning ("possibly IT85xx class device");
		self->size = 0x20000;
		return TRUE;
	}

	/* can't we just use SIO_LDNxx_IDX_CHIPVER... */
	if (!fu_superio_device_it89xx_read_ec_register (self,
							GCTRL_ECHIPVER,
							&tmp,
							error))
		return FALSE;
	if (tmp >> 4 == 0x00) {
		self->size = 0x20000;
		return TRUE;
	}
	if (tmp >> 4 == 0x04) {
		self->size = 0x30000;
		return TRUE;
	}
	if (tmp >> 4 == 0x08) {
		self->size = 0x40000;
		return TRUE;
	}
	g_warning ("falling back to default size");
	self->size = 0x20000;
	return TRUE;
}

static gboolean
fu_superio_device_setup_it89xx (FuSuperioDevice *self, GError **error)
{
	guint8 version_tmp[2] = { 0x00 };
	g_autofree gchar *version = NULL;

	/* get version */
	if (!fu_superio_device_ec_flush (self, error)) {
		g_prefix_error (error, "failed to flush: ");
		return FALSE;
	}
	if (!fu_superio_device_ec_get_param (self, 0x00, &version_tmp[0], error)) {
		g_prefix_error (error, "failed to get version major: ");
		return FALSE;
	}
	if (!fu_superio_device_ec_get_param (self, 0x01, &version_tmp[1], error)) {
		g_prefix_error (error, "failed to get version minor: ");
		return FALSE;
	}
	version = g_strdup_printf ("%02u.%02u", version_tmp[0], version_tmp[1]);
	fu_device_set_version (FU_DEVICE (self), version);

	/* get size from the EC */
	if (!fu_superio_device_it89xx_ec_size (self, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_superio_device_setup (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);

	/* check ID is correct */
	if (!fu_superio_device_check_id (self, error)) {
		g_prefix_error (error, "failed to probe id: ");
		return FALSE;
	}

	/* dump LDNs */
	if (g_getenv ("FWUPD_SUPERIO_VERBOSE") != NULL) {
		for (guint j = 0; j < SIO_LDN_LAST; j++) {
			if (!fu_superio_regdump (self->fd, self->port, j, error))
				return FALSE;
		}
	}

	/* set Power Management I/F Channel 1 LDN */
	if (!fu_superio_set_ldn (self->fd, self->port, SIO_LDN_PM1, error))
		return FALSE;

	/* get the PM1 IOBAD0 address */
	if (!fu_superio_regval16 (self->fd, self->port,
				  SIO_LDNxx_IDX_IOBAD0,
				  &self->pm1_iobad0, error))
		return FALSE;

	/* get the PM1 IOBAD1 address */
	if (!fu_superio_regval16 (self->fd, self->port,
				  SIO_LDNxx_IDX_IOBAD1,
				  &self->pm1_iobad1, error))
		return FALSE;

	/* dump PMC register map */
	if (g_getenv ("FWUPD_SUPERIO_VERBOSE") != NULL) {
		guint8 buf[0xff] = { 0x00 };
		for (guint i = 0x00; i < 0xff; i++) {
			g_autoptr(GError) error_local = NULL;
			if (!fu_superio_device_ec_get_param (self, i, &buf[i], &error_local)) {
				g_debug ("param: 0x%02x = %s", i, error_local->message);
				continue;
			}
		}
		fu_common_dump_raw (G_LOG_DOMAIN, "EC Registers", buf, 0x100);
	}

	/* IT85xx */
	if (self->id >> 8 == 0x85) {
		if (!fu_superio_device_setup_it85xx (self, error))
			return FALSE;
	}

	/* IT89xx */
	if (self->id >> 8 == 0x89) {
		if (!fu_superio_device_setup_it89xx (self, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_superio_device_attach (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);

	/* re-enable HOSTWA -- use 0xfd for LCFC */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1, 0xfc, error))
		return FALSE;

	/* success */
	fu_device_remove_flag (self, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	return TRUE;
}

static gboolean
fu_superio_device_detach (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	guint8 tmp = 0x00;

	/* turn off HOSTWA bit, keeping HSEMIE and HSEMW high */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1, 0xdc, error))
		return FALSE;
	if (!fu_superio_device_ec_read (self, self->pm1_iobad0, &tmp, error))
		return FALSE;
	if (tmp != 0x33) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "failed to clear HOSTWA, got 0x%02x, expected 0x33",
			     tmp);
		return FALSE;
	}

	/* success */
	fu_device_add_flag (self, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	return TRUE;
}

static gboolean
fu_superio_device_close (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	if (!g_close (self->fd, error))
		return FALSE;
	self->fd = 0;
	return TRUE;
}

static gboolean
fu_superio_device_ec_pm1do_sci (FuSuperioDevice *self, guint8 val, GError **error)
{
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DOSCI, error))
		return FALSE;
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1, val, error))
		return FALSE;
	return TRUE;
}

static gboolean
fu_superio_device_ec_pm1do_smi (FuSuperioDevice *self, guint8 val, GError **error)
{
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DOCMI, error))
		return FALSE;
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1, val, error))
		return FALSE;
	return TRUE;
}

static gboolean
fu_superio_device_ec_read_status (FuSuperioDevice *self, GError **error)
{
	guint8 tmp = 0x00;

	/* read status register */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_RDSR, error))
		return FALSE;

	/* wait for write */
	do {
		if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
						 SIO_EC_PMC_PM1DI, error))
			return FALSE;
		if (!fu_superio_device_ec_read (self, self->pm1_iobad0,
						&tmp, error))
			return FALSE;
	} while ((tmp & SIO_STATUS_EC_OBF) != 0);

	/* watch SCI events */
	return fu_superio_device_ec_write (self, self->pm1_iobad1,
					   SIO_EC_PMC_PM1DISCI, error);
}

static gboolean
fu_superio_device_ec_write_disable (FuSuperioDevice *self, GError **error)
{
	guint8 tmp = 0x00;

	/* read existing status */
	if (!fu_superio_device_ec_read_status (self, error))
		return FALSE;

	/* write disable */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_WRDI, error))
		return FALSE;

	/* read status register */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_RDSR, error))
		return FALSE;

	/* wait for read */
	do {
		if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
						 SIO_EC_PMC_PM1DI, error))
			return FALSE;
		if (!fu_superio_device_ec_read (self, self->pm1_iobad0,
						&tmp, error))
			return FALSE;
	} while ((tmp & SIO_STATUS_EC_IBF) != 0);

	/* watch SCI events */
	return fu_superio_device_ec_write (self, self->pm1_iobad1,
					   SIO_EC_PMC_PM1DISCI, error);
}

static gboolean
fu_superio_device_ec_write_enable (FuSuperioDevice *self, GError **error)
{
	guint8 tmp = 0x0;

	/* read existing status */
	if (!fu_superio_device_ec_read_status (self, error))
		return FALSE;

	/* write enable */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_WREN, error))
		return FALSE;

	/* read status register */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_RDSR, error))
		return FALSE;

	/* wait for !BUSY */
	do {
		if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
						 SIO_EC_PMC_PM1DI, error))
			return FALSE;
		if (!fu_superio_device_ec_read (self, self->pm1_iobad0,
						&tmp, error))
			return FALSE;
	} while ((tmp & 3) != SIO_STATUS_EC_IBF);

	/* watch SCI events */
	return fu_superio_device_ec_write (self, self->pm1_iobad1,
					   SIO_EC_PMC_PM1DISCI, error);
}

static GBytes *
fu_superio_device_read_addr (FuSuperioDevice *self,
			     guint32 addr,
			     guint size,
			     GFileProgressCallback progress_cb,
			     GError **error)
{
	g_autofree guint8 *buf = NULL;

	/* setup both, just like the vendor programmer... */
	if (!fu_superio_device_ec_write_disable (self, error))
		return FALSE;
	if (!fu_superio_device_ec_read_status (self, error))
		return FALSE;

	/* high speed read */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_HS_READ, error))
		return FALSE;

	/* set address, MSB, MID, LSB, then ZERO */
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 16, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 8, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr & 0xff, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, 0x0, error))
		return FALSE;

	/* read out data */
	buf = g_malloc0 (size);
	for (guint i = 0; i < size; i++) {
		if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
						 SIO_EC_PMC_PM1DI, error))
			return FALSE;
		if (!fu_superio_device_ec_read (self, self->pm1_iobad0, &buf[i], error))
			return FALSE;

		/* update progress */
		if (progress_cb != NULL)
			progress_cb ((goffset) i, (goffset) size, self);
	}

	/* reset back? */
	if (!fu_superio_device_ec_read_status (self, error))
		return FALSE;

	/* success */
	return g_bytes_new_take (g_steal_pointer (&buf), size);
}

static void
fu_superio_device_progress_cb (goffset current, goffset total, gpointer user_data)
{
	FuDevice *device = FU_DEVICE (user_data);
	fu_device_set_progress_full (device, (gsize) current, (gsize) total);
}

static gboolean
fu_superio_device_write_addr (FuSuperioDevice *self, guint addr, GBytes *fw, GError **error)
{
	gsize size = 0;
	const guint8 *buf = g_bytes_get_data (fw, &size);

	/* sanity check */
	if ((addr & 0xff) != 0x00) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "write addr unaligned, got 0x%04x",
			     (guint) addr);
	}
	if (size % 2 != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "write length not supported, got 0x%04x",
			     (guint) size);
	}

	/* enable writes */
	if (!fu_superio_device_ec_write_enable (self, error))
		return FALSE;

	/* write DWORDs */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_WRITE_WORD, error))
		return FALSE;

	/* set address, MSB, MID, LSB, then ZERO */
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 16, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 8, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr & 0xff, error))
		return FALSE;

	/* write data two bytes at a time */
	for (guint i = 0; i < size; i += 2) {
		if (i > 0) {
			if (!fu_superio_device_ec_read_status (self, error))
				return FALSE;
			if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
							 SIO_EC_PMC_PM1DO, error))
				return FALSE;
			if (!fu_superio_device_ec_pm1do_sci (self,
							     SIO_SPI_CMD_WRITE_WORD,
							     error))
				return FALSE;
		}
		if (!fu_superio_device_ec_pm1do_smi (self, buf[i+0], error))
			return FALSE;
		if (!fu_superio_device_ec_pm1do_smi (self, buf[i+1], error))
			return FALSE;
	}

	/* reset back? */
	if (!fu_superio_device_ec_write_disable (self, error))
		return FALSE;
	return fu_superio_device_ec_read_status (self, error);
}

static gboolean
fu_superio_device_erase_addr (FuSuperioDevice *self, guint addr, GError **error)
{
	/* enable writes */
	if (!fu_superio_device_ec_write_enable (self, error))
		return FALSE;

	/* sector erase */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DO, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_sci (self, SIO_SPI_CMD_4K_SECTOR_ERASE, error))
		return FALSE;

	/* set address, MSB, MID, LSB */
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 16, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr << 8, error))
		return FALSE;
	if (!fu_superio_device_ec_pm1do_smi (self, addr & 0xff, error))
		return FALSE;

	/* watch SCI events */
	if (!fu_superio_device_ec_write (self, self->pm1_iobad1,
					 SIO_EC_PMC_PM1DISCI, error))
		return FALSE;
	return fu_superio_device_ec_read_status (self, error);
}

static GBytes *
fu_superio_device_read_firmware (FuDevice *device, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	/* get entire flash contents */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_READ);
	return fu_superio_device_read_addr (self, 0x0, self->size,
					    fu_superio_device_progress_cb,
					    error);
}

static GBytes *
fu_superio_device_prepare_firmware (FuDevice *device, GBytes *fw, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	gsize sz = 0;
	const guint8 *buf = g_bytes_get_data (fw, &sz);
	const guint8 sig1[] = { 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5 };
	const guint8 sig2[] = { 0x85, 0x12, 0x5a, 0x5a, 0xaa, 0xaa, 0x55, 0x55 };

	/* check size */
	if (sz != self->size) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "firmware size incorrect, got 0x%04x, expected 0x%04x",
			     (guint) sz, (guint) self->size);
		return FALSE;
	}

	/* find signature -- maybe look for 0xb4 too? */
	for (gsize off = 0; off < sz; off += 16) {
		if (memcmp (&buf[off], sig1, sizeof(sig1)) == 0 &&
		    memcmp (&buf[off + 8], sig2, sizeof(sig2)) == 0) {
			g_debug ("found signature at 0x%4x", (guint) off);
			return g_object_ref (fw);
		}
	}
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "did not detect signature in firmware image");
	return NULL;
}

static gboolean
fu_superio_device_check_eflash (FuSuperioDevice *self, GError **error)
{
	g_autoptr(GBytes) fw = NULL;
	const guint sigsz = 16;

	/* last 16 bytes of eeprom */
	fw = fu_superio_device_read_addr (self, self->size - sigsz,
					  sigsz, NULL, error);
	if (fw == NULL) {
		g_prefix_error (error, "failed to read signature bytes");
		return FALSE;
	}

	/* cannot flash here without keyboard programmer */
	if (!fu_common_bytes_is_empty (fw)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "eflash has been protected");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_superio_device_write_chunk (FuSuperioDevice *self, FuChunk *chk, GError **error)
{
	g_autoptr(GBytes) fw1 = NULL;
	g_autoptr(GBytes) fw2 = NULL;
	g_autoptr(GBytes) fw3 = NULL;

	/* erase page */
	if (!fu_superio_device_erase_addr (self, chk->address, error)) {
		g_prefix_error (error, "failed to erase @0x%04x", (guint) chk->address);
		return FALSE;
	}

	/* check erased */
	fw1 = fu_superio_device_read_addr (self, chk->address,
					   chk->data_sz, NULL,
					   error);
	if (fw1 == NULL) {
		g_prefix_error (error, "failed to read erased "
				"bytes @0x%04x", (guint) chk->address);
		return FALSE;
	}
	if (!fu_common_bytes_is_empty (fw1)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_READ,
				     "sector was not erased");
		return FALSE;
	}

	/* skip empty page */
	fw2 = g_bytes_new_static (chk->data, chk->data_sz);
	if (fu_common_bytes_is_empty (fw2))
		return TRUE;

	/* write page */
	if (!fu_superio_device_write_addr (self, chk->address, fw2, error)) {
		g_prefix_error (error, "failed to write @0x%04x", (guint) chk->address);
		return FALSE;
	}

	/* verify page */
	fw3 = fu_superio_device_read_addr (self, chk->address,
					   chk->data_sz, NULL,
					   error);
	if (fw3 == NULL) {
		g_prefix_error (error, "failed to read written "
				"bytes @0x%04x", (guint) chk->address);
		return FALSE;
	}
	if (!g_bytes_equal (fw2, fw3)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "failed to verify @0x%04x",
			     (guint) chk->address);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_superio_device_write_firmware (FuDevice *device, GBytes *fw, GError **error)
{
	FuSuperioDevice *self = FU_SUPERIO_DEVICE (device);
	g_autoptr(GPtrArray) chunks = NULL;

	/* check eflash is writable */
	if (!fu_superio_device_check_eflash (self, error))
		return FALSE;

	/* chunks of 1kB, skipping the final chunk */
	chunks = fu_chunk_array_new_from_bytes (fw, 0x00, 0x00, 0x400);
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	for (guint i = 0; i < chunks->len - 1; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);

		/* try this many times; the failure-to-flash case leaves you
		 * without a keyboard and future boot may completely fail */
		for (guint j = 0;; j++) {
			g_autoptr(GError) error_chk = NULL;
			if (fu_superio_device_write_chunk (self, chk, &error_chk))
				break;
			if (j > 5) {
				g_propagate_error (error, g_steal_pointer (&error_chk));
				return FALSE;
			}
			g_warning ("failure %u: %s", j, error_chk->message);
		}

		/* set progress */
		fu_device_set_progress_full (device, (gsize) i, (gsize) chunks->len);
	}

	/* success */
	fu_device_set_progress (device, 100);
	return TRUE;
}

static void
fu_superio_device_init (FuSuperioDevice *self)
{
	fu_device_set_physical_id (FU_DEVICE (self), "/dev/port");
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_ONLY_OFFLINE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_set_summary (FU_DEVICE (self), "SuperIO device");
	fu_device_add_icon (FU_DEVICE (self), "computer");
}

static void
fu_superio_device_finalize (GObject *object)
{
	G_OBJECT_CLASS (fu_superio_device_parent_class)->finalize (object);
}

static void
fu_superio_device_class_init (FuSuperioDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	object_class->finalize = fu_superio_device_finalize;
	klass_device->to_string = fu_superio_device_to_string;
	klass_device->open = fu_superio_device_open;
	klass_device->attach = fu_superio_device_attach;
	klass_device->detach = fu_superio_device_detach;
	klass_device->read_firmware = fu_superio_device_read_firmware;
	klass_device->write_firmware = fu_superio_device_write_firmware;
	klass_device->prepare_firmware = fu_superio_device_prepare_firmware;
	klass_device->probe = fu_superio_device_probe;
	klass_device->setup = fu_superio_device_setup;
	klass_device->close = fu_superio_device_close;
}

FuSuperioDevice *
fu_superio_device_new (const gchar *chipset, guint16 id, guint16 port)
{
	FuSuperioDevice *self;
	self = g_object_new (FU_TYPE_SUPERIO_DEVICE, NULL);
	self->chipset = g_strdup (chipset);
	self->id = id;
	self->port = port;
	return self;
}
