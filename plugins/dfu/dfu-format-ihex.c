/*
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-common.h"

#include "dfu-element.h"
#include "dfu-firmware.h"
#include "dfu-format-ihex.h"
#include "dfu-image.h"

#include "fwupd-error.h"

/**
 * dfu_firmware_detect_ihex: (skip)
 * @bytes: data to parse
 *
 * Attempts to sniff the data and work out the firmware format
 *
 * Returns: a #DfuFirmwareFormat, e.g. %DFU_FIRMWARE_FORMAT_RAW
 **/
DfuFirmwareFormat
dfu_firmware_detect_ihex (GBytes *bytes)
{
	guint8 *data;
	gsize len;
	data = (guint8 *) g_bytes_get_data (bytes, &len);
	if (len < 12)
		return DFU_FIRMWARE_FORMAT_UNKNOWN;

	/* match the first char */
	if (data[0] == ':')
		return DFU_FIRMWARE_FORMAT_INTEL_HEX;

	/* look for the EOF line */
	if (g_strstr_len ((const gchar *) data, (gssize) len, ":000000") != NULL)
		return DFU_FIRMWARE_FORMAT_INTEL_HEX;

	/* failed */
	return DFU_FIRMWARE_FORMAT_UNKNOWN;
}

#define	DFU_INHX32_RECORD_TYPE_DATA		0x00
#define	DFU_INHX32_RECORD_TYPE_EOF		0x01
#define	DFU_INHX32_RECORD_TYPE_EXTENDED_SEGMENT	0x02
#define	DFU_INHX32_RECORD_TYPE_START_SEGMENT	0x03
#define	DFU_INHX32_RECORD_TYPE_EXTENDED_LINEAR	0x04
#define	DFU_INHX32_RECORD_TYPE_START_LINEAR	0x05
#define	DFU_INHX32_RECORD_TYPE_SIGNATURE	0xfd

static const gchar *
dfu_firmware_ihex_record_type_to_string (guint8 record_type)
{
	if (record_type == DFU_INHX32_RECORD_TYPE_DATA)
		return "DATA";
	if (record_type == DFU_INHX32_RECORD_TYPE_EOF)
		return "EOF";
	if (record_type == DFU_INHX32_RECORD_TYPE_EXTENDED_SEGMENT)
		return "EXTENDED_SEGMENT";
	if (record_type == DFU_INHX32_RECORD_TYPE_START_SEGMENT)
		return "START_SEGMENT";
	if (record_type == DFU_INHX32_RECORD_TYPE_EXTENDED_LINEAR)
		return "EXTENDED_LINEAR";
	if (record_type == DFU_INHX32_RECORD_TYPE_START_LINEAR)
		return "ADDR32";
	if (record_type == DFU_INHX32_RECORD_TYPE_SIGNATURE)
		return "SIGNATURE";
	return NULL;
}

/**
 * dfu_firmware_from_ihex: (skip)
 * @firmware: a #DfuFirmware
 * @bytes: data to parse
 * @flags: some #DfuFirmwareParseFlags
 * @error: a #GError, or %NULL
 *
 * Unpacks into a firmware object from raw data.
 *
 * Returns: %TRUE for success
 **/
gboolean
dfu_firmware_from_ihex (DfuFirmware *firmware,
			GBytes *bytes,
			DfuFirmwareParseFlags flags,
			GError **error)
{
	const gchar *data;
	gboolean got_eof = FALSE;
	gsize sz = 0;
	guint32 abs_addr = 0x0;
	guint32 addr_last = 0x0;
	guint32 base_addr = 0x0;
	guint32 seg_addr = 0x0;
	g_auto(GStrv) lines = NULL;
	g_autoptr(DfuElement) element = NULL;
	g_autoptr(DfuImage) image = NULL;
	g_autoptr(GBytes) contents = NULL;
	g_autoptr(GString) buf = g_string_new (NULL);
	g_autoptr(GString) buf_signature = g_string_new (NULL);

	g_return_val_if_fail (bytes != NULL, FALSE);

	/* create element */
	image = dfu_image_new ();
	dfu_image_set_name (image, "ihex");
	element = dfu_element_new ();

	/* parse records */
	data = g_bytes_get_data (bytes, &sz);
	lines = dfu_utils_strnsplit (data, sz, "\n", -1);
	for (guint ln = 0; lines[ln] != NULL; ln++) {
		const gchar *line = lines[ln];
		gsize linesz;
		guint32 addr;
		guint8 byte_cnt;
		guint8 record_type;
		guint line_end;

		/* ignore comments */
		if (g_str_has_prefix (line, ";"))
			continue;

		/* ignore blank lines */
		g_strdelimit (lines[ln], "\r\x1a", '\0');
		linesz = strlen (line);
		if (linesz == 0)
			continue;

		/* check starting token */
		if (line[0] != ':') {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "invalid starting token on line %u: %s",
				     ln + 1, line);
			return FALSE;
		}

		/* check there's enough data for the smallest possible record */
		if (linesz < 11) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "line %u is incomplete, length %u",
				     ln + 1, (guint) linesz);
			return FALSE;
		}

		/* length, 16-bit address, type */
		byte_cnt = dfu_utils_buffer_parse_uint8 (line + 1);
		addr = dfu_utils_buffer_parse_uint16 (line + 3);
		record_type = dfu_utils_buffer_parse_uint8 (line + 7);
		g_debug ("%s:", dfu_firmware_ihex_record_type_to_string (record_type));
		g_debug ("  addr_start:\t0x%04x", addr);
		g_debug ("  length:\t0x%02x", byte_cnt);
		addr += seg_addr;
		addr += abs_addr;
		g_debug ("  addr:\t0x%08x", addr);

		/* position of checksum */
		line_end = 9 + byte_cnt * 2;
		if (line_end > (guint) linesz) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "line %u malformed, length: %u",
				     ln + 1, line_end);
			return FALSE;
		}

		/* verify checksum */
		if ((flags & DFU_FIRMWARE_PARSE_FLAG_NO_CRC_TEST) == 0) {
			guint8 checksum = 0;
			for (guint i = 1; i < line_end + 2; i += 2) {
				guint8 data_tmp = dfu_utils_buffer_parse_uint8 (line + i);
				checksum += data_tmp;
			}
			if (checksum != 0)  {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "line %u has invalid checksum (0x%02x)",
					     ln + 1, checksum);
				return FALSE;
			}
		}

		/* process different record types */
		switch (record_type) {
		case DFU_INHX32_RECORD_TYPE_DATA:
			/* base address for element */
			if (base_addr == 0x0)
				base_addr = addr;

			/* does not make sense */
			if (addr < addr_last) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "invalid address 0x%x, last was 0x%x",
					     (guint) addr,
					     (guint) addr_last);
				return FALSE;
			}

			/* parse bytes from line */
			g_debug ("writing data 0x%08x", (guint32) addr);
			for (guint i = 9; i < line_end; i += 2) {
				/* any holes in the hex record */
				guint32 len_hole = addr - addr_last;
				guint8 data_tmp;
				if (addr_last > 0 && len_hole > 0x100000) {
					g_set_error (error,
						     FWUPD_ERROR,
						     FWUPD_ERROR_INVALID_FILE,
						     "hole of 0x%x bytes too large to fill",
						     (guint) len_hole);
					return FALSE;
				}
				if (addr_last > 0x0 && len_hole > 1) {
					g_debug ("filling address 0x%08x to 0x%08x",
						 addr_last + 1, addr_last + len_hole - 1);
					for (guint j = 1; j < len_hole; j++) {
						/* although 0xff might be clearer,
						 * we can't write 0xffff to pic14 */
						g_string_append_c (buf, 0x00);
					}
				}
				/* write into buf */
				data_tmp = dfu_utils_buffer_parse_uint8 (line + i);
				g_string_append_c (buf, (gchar) data_tmp);
				addr_last = addr++;
			}
			break;
		case DFU_INHX32_RECORD_TYPE_EOF:
			if (got_eof) {
				g_set_error_literal (error,
						     FWUPD_ERROR,
						     FWUPD_ERROR_INVALID_FILE,
						     "duplicate EOF, perhaps "
						     "corrupt file");
				return FALSE;
			}
			got_eof = TRUE;
			break;
		case DFU_INHX32_RECORD_TYPE_EXTENDED_LINEAR:
			abs_addr = dfu_utils_buffer_parse_uint16 (line + 9) << 16;
			g_debug ("  abs_addr:\t0x%02x", abs_addr);
			break;
		case DFU_INHX32_RECORD_TYPE_START_LINEAR:
			abs_addr = dfu_utils_buffer_parse_uint32 (line + 9);
			g_debug ("  abs_addr:\t0x%08x", abs_addr);
			break;
		case DFU_INHX32_RECORD_TYPE_EXTENDED_SEGMENT:
			/* segment base address, so ~1Mb addressable */
			seg_addr = dfu_utils_buffer_parse_uint16 (line + 9) * 16;
			g_debug ("  seg_addr:\t0x%08x", seg_addr);
			break;
		case DFU_INHX32_RECORD_TYPE_START_SEGMENT:
			/* initial content of the CS:IP registers */
			seg_addr = dfu_utils_buffer_parse_uint32 (line + 9);
			g_debug ("  seg_addr:\t0x%02x", seg_addr);
			break;
		case DFU_INHX32_RECORD_TYPE_SIGNATURE:
			for (guint i = 9; i < line_end; i += 2) {
				guint8 tmp_c = dfu_utils_buffer_parse_uint8 (line + i);
				g_string_append_c (buf_signature, tmp_c);
			}
			break;
		default:
			/* vendors sneak in nonstandard sections past the EOF */
			if (got_eof)
				break;
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "invalid ihex record type %i",
				     record_type);
			return FALSE;
		}
	}

	/* no EOF */
	if (!got_eof) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "no EOF, perhaps truncated file");
		return FALSE;
	}

	/* add single image */
	contents = g_bytes_new (buf->str, buf->len);
	dfu_element_set_contents (element, contents);
	dfu_element_set_address (element, base_addr);
	dfu_image_add_element (image, element);
	dfu_firmware_add_image (firmware, image);
	dfu_firmware_set_format (firmware, DFU_FIRMWARE_FORMAT_INTEL_HEX);

	/* add optional signature */
	if (buf_signature->len > 0) {
		g_autoptr(DfuElement) element_sig = dfu_element_new ();
		g_autoptr(DfuImage) image_sig = dfu_image_new ();
		g_autoptr(GBytes) data_sig = g_bytes_new_static (buf_signature->str, buf_signature->len);
		dfu_element_set_contents (element_sig, data_sig);
		dfu_image_add_element (image_sig, element_sig);
		dfu_image_set_name (image_sig, "signature");
		dfu_firmware_add_image (firmware, image_sig);
	}
	return TRUE;
}

static void
dfu_firmware_ihex_emit_chunk (GString *str,
			      guint16 address,
			      guint8 record_type,
			      const guint8 *data,
			      gsize sz)
{
	guint8 checksum = 0x00;
	g_string_append_printf (str, ":%02X%04X%02X",
				(guint) sz,
				(guint) address,
				(guint) record_type);
	for (gsize j = 0; j < sz; j++)
		g_string_append_printf (str, "%02X", data[j]);
	checksum = (guint8) sz;
	checksum += (guint8) ((address & 0xff00) >> 8);
	checksum += (guint8) (address & 0xff);
	checksum += record_type;
	for (gsize j = 0; j < sz; j++)
		checksum += data[j];
	g_string_append_printf (str, "%02X\n", (guint) (((~checksum) + 0x01) & 0xff));
}

static void
dfu_firmware_to_ihex_bytes (GString *str, guint8 record_type,
			    guint32 address, GBytes *contents)
{
	const guint8 *data;
	const guint chunk_size = 16;
	gsize len;
	guint32 address_offset_last = 0x0;

	/* get number of chunks */
	data = g_bytes_get_data (contents, &len);
	for (gsize i = 0; i < len; i += chunk_size) {
		guint32 address_tmp = address + i;
		guint32 address_offset = (address_tmp >> 16) & 0xffff;
		gsize chunk_len = MIN (len - i, 16);

		/* need to offset */
		if (address_offset != address_offset_last) {
			guint8 buf[2];
			fu_common_write_uint16 (buf, address_offset, G_BIG_ENDIAN);
			dfu_firmware_ihex_emit_chunk (str, 0x0,
						      DFU_INHX32_RECORD_TYPE_EXTENDED_LINEAR,
						      buf, 2);
			address_offset_last = address_offset;
		}
		address_tmp &= 0xffff;
		dfu_firmware_ihex_emit_chunk (str, address_tmp,
					      record_type, data + i, chunk_len);
	}
}

static gboolean
dfu_firmware_to_ihex_element (DfuElement *element, GString *str,
			      guint8 record_type, GError **error)
{
	GBytes *contents = dfu_element_get_contents (element);
	dfu_firmware_to_ihex_bytes (str, record_type,
				    dfu_element_get_address (element),
				    contents);
	return TRUE;
}

static gboolean
dfu_firmware_to_ihex_image (DfuImage *image, GString *str, GError **error)
{
	GPtrArray *elements;
	guint8 record_type = DFU_INHX32_RECORD_TYPE_DATA;

	if (g_strcmp0 (dfu_image_get_name (image), "signature") == 0)
		record_type = DFU_INHX32_RECORD_TYPE_SIGNATURE;
	elements = dfu_image_get_elements (image);
	for (guint i = 0; i < elements->len; i++) {
		DfuElement *element = g_ptr_array_index (elements, i);
		if (!dfu_firmware_to_ihex_element (element,
						   str,
						   record_type,
						   error))
			return FALSE;
	}
	return TRUE;
}

/**
 * dfu_firmware_to_ihex: (skip)
 * @firmware: a #DfuFirmware
 * @error: a #GError, or %NULL
 *
 * Packs a IHEX firmware
 *
 * Returns: (transfer full): the packed data
 **/
GBytes *
dfu_firmware_to_ihex (DfuFirmware *firmware, GError **error)
{
	GPtrArray *images;
	g_autoptr(GString) str = NULL;

	/* write all the element data */
	str = g_string_new ("");
	images = dfu_firmware_get_images (firmware);
	for (guint i = 0; i < images->len; i++) {
		DfuImage *image = g_ptr_array_index (images, i);
		if (!dfu_firmware_to_ihex_image (image, str, error))
			return NULL;
	}

	/* add EOF */
	dfu_firmware_ihex_emit_chunk (str, 0x0, DFU_INHX32_RECORD_TYPE_EOF, NULL, 0);
	return g_bytes_new (str->str, str->len);
}
