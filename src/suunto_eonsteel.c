/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Linus Torvalds
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>	/* For crc32() */

#include "suunto_eonsteel.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#ifdef USBHID
#include "usbhid.h"
#endif
#include "platform.h"

#define EONSTEEL 0
#define EONCORE  1

typedef struct suunto_eonsteel_device_t {
	dc_device_t base;
	unsigned int model;
	unsigned int magic;
	unsigned short seq;
	unsigned char version[0x30];
	unsigned char fingerprint[4];
} suunto_eonsteel_device_t;

// The EON Steel implements a small filesystem
#define DIRTYPE_FILE 0x0001
#define DIRTYPE_DIR  0x0002

struct directory_entry {
	struct directory_entry *next;
	int type;
	int namelen;
	char name[1];
};

// EON Steel command numbers and other magic field values
#define CMD_INIT	0x0000
#define INIT_MAGIC	0x0001
#define INIT_SEQ	0

#define CMD_READ_STRING	0x0411

#define CMD_FILE_OPEN	0x0010
#define CMD_FILE_READ	0x0110
#define CMD_FILE_STAT	0x0710
#define CMD_FILE_CLOSE	0x0510

#define CMD_DIR_OPEN	0x0810
#define CMD_DIR_READDIR	0x0910
#define CMD_DIR_CLOSE	0x0a10

#define CMD_SET_TIME	0x0003
#define CMD_GET_TIME	0x0103
#define CMD_SET_DATE	0x0203
#define CMD_GET_DATE	0x0303

static dc_status_t suunto_eonsteel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t suunto_eonsteel_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime);
static dc_status_t suunto_eonsteel_device_close(dc_device_t *abstract);

static const dc_device_vtable_t suunto_eonsteel_device_vtable = {
	sizeof(suunto_eonsteel_device_t),
	DC_FAMILY_SUUNTO_EONSTEEL,
	suunto_eonsteel_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	suunto_eonsteel_device_foreach, /* foreach */
	suunto_eonsteel_device_timesync, /* timesync */
	suunto_eonsteel_device_close /* close */
};

static const char dive_directory[] = "0:/dives";

static void file_list_free (struct directory_entry *de)
{
	while (de) {
		struct directory_entry *next = de->next;
		free (de);
		de = next;
	}
}

static struct directory_entry *alloc_dirent(int type, int len, const char *name)
{
	struct directory_entry *res;

	res = (struct directory_entry *) malloc(offsetof(struct directory_entry, name) + len + 1);
	if (res) {
		res->next = NULL;
		res->type = type;
		res->namelen = len;
		memcpy(res->name, name, len);
		res->name[len] = 0;
	}
	return res;
}

static void put_le16(unsigned short val, unsigned char *p)
{
	p[0] = val;
	p[1] = val >> 8;
}

static void put_le32(unsigned int val, unsigned char *p)
{
	p[0] = val;
	p[1] = val >> 8;
	p[2] = val >> 16;
	p[3] = val >> 24;
}

/*
 * Get a single 64-byte packet from the dive computer. This handles packet
 * logging and any obvious packet-level errors, and returns the payload of
 * packet.
 *
 * The two first bytes of the packet are packet-level metadata: the report
 * type (always 0x3f), and then the size of the valid data in the packet.
 *
 * The maximum payload is 62 bytes.
 */
#define PACKET_SIZE 64
static int receive_usbhid_packet(dc_custom_io_t *io, suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	unsigned char buf[64];
	dc_status_t rc = DC_STATUS_SUCCESS;
	size_t transferred = 0;
	int len;

	rc = io->packet_read(io, buf, PACKET_SIZE, &transferred);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "read interrupt transfer failed");
		return -1;
	}
	if (transferred != PACKET_SIZE) {
		ERROR(eon->base.context, "incomplete read interrupt transfer (got " DC_PRINTF_SIZE ", expected %d)", transferred, PACKET_SIZE);
		return -1;
	}
	if (buf[0] != 0x3f) {
		ERROR(eon->base.context, "read interrupt transfer returns wrong report type (%d)", buf[0]);
		return -1;
	}
	len = buf[1];
	if (len > PACKET_SIZE-2) {
		ERROR(eon->base.context, "read interrupt transfer reports bad length (%d)", len);
		return -1;
	}
	if (len > size) {
		ERROR(eon->base.context, "receive_packet result buffer too small - truncating");
		len = size;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "rcv", buf+2, len);
	memcpy(buffer, buf+2, len);
	return len;
}

static int fill_ble_buffer(dc_custom_io_t *io, suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	int state = 0;
	int bytes = 0;
	unsigned int crc;

	for (;;) {
		unsigned char packet[32];
		dc_status_t rc = DC_STATUS_SUCCESS;
		size_t transferred = 0;
		int i;

		rc = io->packet_read(io, packet, sizeof(packet), &transferred);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR(eon->base.context, "BLE GATT read transfer failed");
			return -1;
		}
		for (i = 0; i < transferred; i++) {
			unsigned char c = packet[i];

			if (c == 0x7e) {
				if (state == 1)
					goto done;
				if (state == 2) {
					ERROR(eon->base.context, "BLE GATT stream has escaped 7e character");
					return -1;
				}
				/* Initial 7e character - good */
				state = 1;
				continue;
			}

			if (!state) {
				ERROR(eon->base.context, "BLE GATT stream did not start with 7e");
				return -1;
			}

			if (c == 0x7d) {
				if (state == 2) {
					ERROR(eon->base.context, "BLE GATT stream has escaped 7d character");
					return -1;
				}
				state = 2;
				continue;
			}

			if (state == 2) {
				c ^= 0x20;
				state = 1;
			}
			if (bytes < size)
				buffer[bytes] = c;
			bytes++;
		}
	}
done:
	if (bytes < 4) {
		ERROR(eon->base.context, "did not receive BLE CRC32 data");
		return -1;
	}
	if (bytes > size) {
		ERROR(eon->base.context, "BLE GATT stream too long (%d bytes, buffer is %d)", bytes, size);
		return -1;
	}

	/* Remove and check CRC */
	bytes -= 4;
	crc = crc32(0, buffer, bytes);
	if (crc != array_uint32_le(buffer + bytes)) {
		ERROR(eon->base.context, "incorrect BLE CRC32 data");
		return -1;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "rcv", buffer, bytes);
	return bytes;
}

#define HDRSIZE 12
#define MAXDATA 2048
#define CRCSIZE 4

static struct {
	unsigned int len, offset;
	unsigned char buffer[HDRSIZE + MAXDATA + CRCSIZE];
} ble_data;

static void fill_ble_data(dc_custom_io_t *io, suunto_eonsteel_device_t *eon)
{
	int received;

	received = fill_ble_buffer(io, eon, ble_data.buffer, sizeof(ble_data.buffer));
	if (received < 0)
		received = 0;
	ble_data.offset = 0;
	ble_data.len = received;
}

static int receive_ble_packet(dc_custom_io_t *io, suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	int maxsize;

	if (ble_data.offset >= ble_data.len)
		return 0;
	maxsize = ble_data.len - ble_data.offset;
	if (size > maxsize)
		size = maxsize;
	memcpy(buffer, ble_data.buffer + ble_data.offset, size);
	ble_data.offset += size;
	return size;
}

static int receive_packet(dc_custom_io_t *io, suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	if (io->packet_size < 64)
		return receive_ble_packet(io, eon, buffer, size);
	return receive_usbhid_packet(io, eon, buffer, size);
}

static int add_hdlc(unsigned char *dst, unsigned char val)
{
	int chars = 1;
	switch (val) {
	case 0x7e: case 0x7d:
		*dst++ = 0x7d;
		val ^= 0x20;
		chars++;
		/* fallthrough */
	default:
		*dst = val;
	}
	return chars;
}

static int hdlc_reencode(unsigned char *dst, unsigned char *src, int len)
{
	unsigned int crc = crc32(0, src, len);
	int result = 0, i;

	*dst++ = 0x7e; result++;
	for (i = 0; i < len; i++) {
		int chars = add_hdlc(dst, src[i]);
		dst += chars;
		result += chars;
	}
	for (i = 0; i < 4; i++) {
		int chars = add_hdlc(dst, crc & 255);
		dst += chars;
		result += chars;
		crc >>= 8;
	}
	*dst++ = 0x7e; result++;
	return result;
}

static int send_cmd(suunto_eonsteel_device_t *eon,
	unsigned short cmd,
	unsigned int len,
	const unsigned char *buffer)
{
	unsigned char buf[64];
	unsigned short seq = eon->seq;
	unsigned int magic = eon->magic;
	dc_custom_io_t *io = _dc_context_custom_io(eon->base.context);
	dc_status_t rc = DC_STATUS_SUCCESS;
	size_t transferred = 0;

	// Two-byte packet header, followed by 12 bytes of extended header
	if (len > sizeof(buf)-2-12) {
		ERROR(eon->base.context, "send command with too much long");
		return -1;
	}

	memset(buf, 0, sizeof(buf));

	buf[0] = 0x3f;
	buf[1] = len + 12;

	// 2-byte LE command word
	put_le16(cmd, buf+2);

	// 4-byte LE magic value (starts at 1)
	put_le32(magic, buf+4);

	// 2-byte LE sequence number;
	put_le16(seq, buf+8);

	// 4-byte LE length
	put_le32(len, buf+10);

	// .. followed by actual data
	if (len) {
		memcpy(buf+14, buffer, len);
	}

	// BLE GATT protocol?
	if (io->packet_size < 64) {
		int hdlc_len;
		unsigned char hdlc[2+2*(62+4)]; /* start/stop + escaping*(maxbuf+crc32) */
		unsigned char *ptr;

		hdlc_len = hdlc_reencode(hdlc, buf+2, buf[1]);

		ptr = hdlc;
		do {
			int len = hdlc_len;

			if (len > io->packet_size)
				len = io->packet_size;
			rc = io->packet_write(io, ptr, len, &transferred);
			if (rc != DC_STATUS_SUCCESS)
				break;
			ptr += len;
			hdlc_len -= len;
		} while (hdlc_len);
	} else {
		rc = io->packet_write(io, buf, sizeof(buf), &transferred);
	}
	if (rc != DC_STATUS_SUCCESS) {
		ERROR(eon->base.context, "write interrupt transfer failed");
		return -1;
	}

	// dump every outgoing packet?
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "cmd", buf+2, len+12);
	return 0;
}

struct eon_hdr {
	unsigned short cmd;
	unsigned int magic;
	unsigned short seq;
	unsigned int len;
};

static int receive_header(suunto_eonsteel_device_t *eon, struct eon_hdr *hdr, unsigned char *buffer, int size)
{
	int ret;
	unsigned char header[64];
	dc_custom_io_t *io = _dc_context_custom_io(eon->base.context);

	if (io->packet_size < 64)
		fill_ble_data(io, eon);
	ret = receive_packet(io, eon, header, sizeof(header));
	if (ret < 0)
		return -1;
	if (ret < 12) {
		ERROR(eon->base.context, "short reply packet (%d)", ret);
		return -1;
	}

	/* Unpack the 12-byte header */
	hdr->cmd = array_uint16_le(header);
	hdr->magic = array_uint32_le(header+2);
	hdr->seq = array_uint16_le(header+6);
	hdr->len = array_uint32_le(header+8);

	ret -= 12;
	if (ret > size) {
		ERROR(eon->base.context, "receive_header result data buffer too small (%d vs %d)", ret, size);
		return -1;
	}
	memcpy(buffer, header+12, ret);
	return ret;
}

static int receive_data(suunto_eonsteel_device_t *eon, unsigned char *buffer, int size)
{
	int ret = 0;
	dc_custom_io_t *io = _dc_context_custom_io(eon->base.context);

	while (size > 0) {
		int len;

		len = receive_packet(io, eon, buffer + ret, size);
		if (len < 0)
			return -1;

		size -= len;
		ret += len;

		/* Was it not a full packet of data? We're done, regardless of expectations */
		if (len < PACKET_SIZE-2)
			break;
	}

	return ret;
}

/*
 * Send a command, receive a reply
 *
 * This carefully checks the data fields in the reply for a match
 * against the command, and then only returns the actual reply
 * data itself.
 *
 * Also note that "receive_data()" itself will have removed the
 * per-packet handshake bytes, so unlike "send_cmd()", this does
 * not see the two initial 0x3f 0x?? bytes, and this the offsets
 * for the cmd/magic/seq/len are off by two compared to the
 * send_cmd() side. The offsets are the same in the actual raw
 * packet.
 */
static int send_receive(suunto_eonsteel_device_t *eon,
	unsigned short cmd,
	unsigned int len_out, const unsigned char *out,
	unsigned int len_in, unsigned char *in)
{
	int len, actual;
	struct eon_hdr hdr;

	if (send_cmd(eon, cmd, len_out, out) < 0)
		return -1;

	/* Get the header and the first part of the data */
	len = receive_header(eon, &hdr, in, len_in);
	if (len < 0)
		return -1;

	/* Verify the header data */
	if (hdr.cmd != cmd) {
		ERROR(eon->base.context, "command reply doesn't match command");
		return -1;
	}
	if (hdr.magic != eon->magic + 5) {
		ERROR(eon->base.context, "command reply doesn't match magic (got %08x, expected %08x)", hdr.magic, eon->magic + 5);
		return -1;
	}
	if (hdr.seq != eon->seq) {
		ERROR(eon->base.context, "command reply doesn't match sequence number");
		return -1;
	}
	actual = hdr.len;
	if (actual < len) {
		ERROR(eon->base.context, "command reply length mismatch (got %d, claimed %d)", len, actual);
		return -1;
	}
	if (actual > len_in) {
		ERROR(eon->base.context, "command reply too big for result buffer - truncating");
		actual = len_in;
	}

	/* Get the rest of the data */
	len += receive_data(eon, in + len, actual - len);
	if (len != actual) {
		ERROR(eon->base.context, "command reply returned unexpected amoutn of data (got %d, expected %d)", len, actual);
		return -1;
	}

	// Successful command - increment sequence number
	eon->seq++;
	return len;
}

static int read_file(suunto_eonsteel_device_t *eon, const char *filename, dc_buffer_t *buf)
{
	unsigned char result[2560];
	unsigned char cmdbuf[64];
	unsigned int size, offset;
	int rc, len;

	memset(cmdbuf, 0, sizeof(cmdbuf));
	len = strlen(filename) + 1;
	if (len + 4 > sizeof(cmdbuf)) {
		ERROR(eon->base.context, "too long filename: %s", filename);
		return -1;
	}
	memcpy(cmdbuf+4, filename, len);
	rc = send_receive(eon, CMD_FILE_OPEN,
		len+4, cmdbuf,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "unable to look up %s", filename);
		return -1;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "lookup", result, rc);

	rc = send_receive(eon, CMD_FILE_STAT,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "unable to stat %s", filename);
		return -1;
	}
	HEXDUMP (eon->base.context, DC_LOGLEVEL_DEBUG, "stat", result, rc);

	size = array_uint32_le(result+4);
	offset = 0;

	while (size > 0) {
		unsigned int ask, got, at;

		ask = size;
		if (ask > 1024)
			ask = 1024;
		put_le32(1234, cmdbuf+0);	// Not file offset, after all
		put_le32(ask, cmdbuf+4);	// Size of read
		rc = send_receive(eon, CMD_FILE_READ,
			8, cmdbuf,
			sizeof(result), result);
		if (rc < 0) {
			ERROR(eon->base.context, "unable to read %s", filename);
			return -1;
		}
		if (rc < 8) {
			ERROR(eon->base.context, "got short read reply for %s", filename);
			return -1;
		}

		// Not file offset, just stays unmodified.
		at = array_uint32_le(result);
		if (at != 1234) {
			ERROR(eon->base.context, "read of %s returned different offset than asked for (%d vs %d)", filename, at, offset);
			return -1;
		}

		// Number of bytes actually read
		got = array_uint32_le(result+4);
		if (!got)
			break;
		if (rc < 8 + got) {
			ERROR(eon->base.context, "odd read size reply for offset %d of file %s", offset, filename);
			return -1;
		}

		if (got > size)
			got = size;
		if (!dc_buffer_append (buf, result + 8, got)) {
			ERROR (eon->base.context, "Insufficient buffer space available.");
			return -1;
		}
		offset += got;
		size -= got;
	}

	rc = send_receive(eon, CMD_FILE_CLOSE,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "cmd CMD_FILE_CLOSE failed");
		return -1;
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "close", result, rc);

	return offset;
}

/*
 * NOTE! This will create the list of dirent's in reverse order,
 * with the last dirent first. That's intentional: for dives,
 * we will want to look up the last dive first.
 */
static struct directory_entry *add_dirent(struct directory_entry *new, struct directory_entry *list)
{
	struct directory_entry **pp = &list, *p;

	/* Skip any entries that are later than the new one */
	while ((p = *pp) != NULL && strcmp(p->name, new->name) > 0)
		pp = &p->next;

	/* Add the new one to that location and return the new list pointer */
	new->next = p;
	*pp = new;
	return list;
}

static struct directory_entry *parse_dirent(suunto_eonsteel_device_t *eon, int nr, const unsigned char *p, int len, struct directory_entry *old)
{
	while (len > 8) {
		unsigned int type = array_uint32_le(p);
		unsigned int namelen = array_uint32_le(p+4);
		const unsigned char *name = p+8;
		struct directory_entry *entry;

		if (namelen + 8 + 1 > len || name[namelen] != 0) {
			ERROR(eon->base.context, "corrupt dirent entry: len=%d namelen=%d name='%s'", len, namelen, name);
			break;
		}
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "dir entry", p, 8);

		p += 8 + namelen + 1;
		len -= 8 + namelen + 1;
		entry = alloc_dirent(type, namelen, (const char *) name);
		if (!entry) {
			ERROR(eon->base.context, "out of memory");
			break;
		}
		old = add_dirent(entry, old);
	}
	return old;
}

static int get_file_list(suunto_eonsteel_device_t *eon, struct directory_entry **res)
{
	struct directory_entry *de = NULL;
	unsigned char cmd[64];
	unsigned char result[2048];
	int rc, cmdlen;

	*res = NULL;
	put_le32(0, cmd);
	memcpy(cmd + 4, dive_directory, sizeof(dive_directory));
	cmdlen = 4 + sizeof(dive_directory);
	rc = send_receive(eon, CMD_DIR_OPEN,
		cmdlen, cmd,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "cmd DIR_LOOKUP failed");
		return -1;
	}
	HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "DIR_LOOKUP", result, rc);

	for (;;) {
		unsigned int nr, last;

		rc = send_receive(eon, CMD_DIR_READDIR,
			0, NULL,
			sizeof(result), result);
		if (rc < 0) {
			ERROR(eon->base.context, "readdir failed");
			return -1;
		}
		if (rc < 8) {
			ERROR(eon->base.context, "short readdir result");
			return -1;
		}
		nr = array_uint32_le(result);
		last = array_uint32_le(result+4);
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "dir packet", result, 8);

		de = parse_dirent(eon, nr, result+8, rc-8, de);
		if (last)
			break;
	}

	rc = send_receive(eon, CMD_DIR_CLOSE,
		0, NULL,
		sizeof(result), result);
	if (rc < 0) {
		ERROR(eon->base.context, "dir close failed");
	}

	*res = de;
	return 0;
}

static int initialize_eonsteel(suunto_eonsteel_device_t *eon)
{
	const unsigned char init[] = {0x02, 0x00, 0x2a, 0x00};
	struct eon_hdr hdr;

	if (send_cmd(eon, CMD_INIT, sizeof(init), init)) {
		ERROR(eon->base.context, "Failed to send initialization command");
		return -1;
	}
	if (receive_header(eon, &hdr, eon->version, sizeof(eon->version)) < 0) {
		ERROR(eon->base.context, "Failed to receive initial reply");
		return -1;
	}

	// Don't ask
	eon->magic = (hdr.magic & 0xffff0000) | 0x0005;
	// Increment the sequence number for every command sent
	eon->seq++;
	return 0;
}

dc_status_t
suunto_eonsteel_device_open(dc_device_t **out, dc_context_t *context, const char *name, unsigned int model)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eonsteel_device_t *eon = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	eon = (suunto_eonsteel_device_t *) dc_device_allocate(context, &suunto_eonsteel_device_vtable);
	if (!eon)
		return DC_STATUS_NOMEMORY;

	// Set up the magic handshake fields
	eon->model = model;
	eon->magic = INIT_MAGIC;
	eon->seq = INIT_SEQ;
	memset (eon->version, 0, sizeof (eon->version));
	memset (eon->fingerprint, 0, sizeof (eon->fingerprint));

	dc_custom_io_t *io = _dc_context_custom_io(eon->base.context);
	if (io && io->packet_open)
		status = io->packet_open(io, context, name);
	else {
		/* We really need some way to specify USB ID's in the descriptor */
		unsigned int vendor_id = 0x1493;
		unsigned int device_id = model ? 0x0033 : 0x0030;
#ifdef USBHID
		status = dc_usbhid_custom_io(context, vendor_id, device_id);
#else
		status = DC_STATUS_UNSUPPORTED;
#endif
	}

	if (status != DC_STATUS_SUCCESS) {
		ERROR(context, "unable to open device");
		goto error_free;
	}

	if (initialize_eonsteel(eon) < 0) {
		ERROR(context, "unable to initialize device");
		status = DC_STATUS_IO;
		goto error_close;
	}

	*out = (dc_device_t *) eon;

	return DC_STATUS_SUCCESS;

error_close:
	suunto_eonsteel_device_close((dc_device_t *) eon);
error_free:
	free(eon);
	return status;
}

static int count_dir_entries(struct directory_entry *de)
{
	int count = 0;
	while (de) {
		count++;
		de = de->next;
	}
	return count;
}

static dc_status_t
suunto_eonsteel_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_eonsteel_device_t *device = (suunto_eonsteel_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_device_foreach(dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	int skip = 0, rc;
	struct directory_entry *de;
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;
	dc_buffer_t *file;
	char pathname[64];
	unsigned int time;
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = eon->model;
	devinfo.firmware = array_uint32_be (eon->version + 0x20);
	devinfo.serial = array_convert_str2num(eon->version + 0x10, 16);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	if (get_file_list(eon, &de) < 0)
		return DC_STATUS_IO;

	if (de == NULL) {
		return DC_STATUS_SUCCESS;
	}

	file = dc_buffer_new (16384);
	if (file == NULL) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		file_list_free (de);
		return DC_STATUS_NOMEMORY;
	}
	progress.maximum = count_dir_entries(de);
	progress.current = 0;
	device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

	while (de) {
		int len;
		struct directory_entry *next = de->next;
		unsigned char buf[4];
		const unsigned char *data = NULL;
		unsigned int size = 0;

		if (device_is_cancelled(abstract))
			skip = 1;

		switch (de->type) {
		case DIRTYPE_DIR:
			/* Ignore subdirectories in the dive directory */
			break;
		case DIRTYPE_FILE:
			if (skip)
				break;
			if (sscanf(de->name, "%x.LOG", &time) != 1)
				break;
			len = snprintf(pathname, sizeof(pathname), "%s/%s", dive_directory, de->name);
			if (len >= sizeof(pathname))
				break;

			// Reset the membuffer, put the 4-byte length at the head.
			dc_buffer_clear(file);
			put_le32(time, buf);
			dc_buffer_append(file, buf, 4);

			// Then read the filename into the rest of the buffer
			rc = read_file(eon, pathname, file);
			if (rc < 0)
				break;

			data = dc_buffer_get_data(file);
			size = dc_buffer_get_size(file);

			if (memcmp (data, eon->fingerprint, sizeof (eon->fingerprint)) == 0) {
				skip = 1;
				break;
			}

			if (callback && !callback(data, size, data, sizeof(eon->fingerprint), userdata))
				skip = 1;
		}
		progress.current++;
		device_event_emit(abstract, DC_EVENT_PROGRESS, &progress);

		free(de);
		de = next;
	}
	dc_buffer_free(file);

	return device_is_cancelled(abstract) ? DC_STATUS_CANCELLED : DC_STATUS_SUCCESS;
}

static dc_status_t suunto_eonsteel_device_timesync(dc_device_t *abstract, const dc_datetime_t *datetime)
{
	suunto_eonsteel_device_t *eon = (suunto_eonsteel_device_t *) abstract;
	unsigned char result[64], cmd[8];
	unsigned int year, month, day;
	unsigned int hour, min, msec;
	int rc;

	year = datetime->year;
	month = datetime->month;
	day = datetime->day;
	hour = datetime->hour;
	min = datetime->minute;
	msec = datetime->second * 1000;

	cmd[0] = year & 0xFF;
	cmd[1] = year >> 8;
	cmd[2] = month;
	cmd[3] = day;
	cmd[4] = hour;
	cmd[5] = min;
	cmd[6] = msec & 0xFF;
	cmd[7] = msec >> 8;

	rc = send_receive(eon, CMD_SET_TIME, sizeof(cmd), cmd, sizeof(result), result);
	if (rc < 0) {
		return DC_STATUS_IO;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_device_close(dc_device_t *abstract)
{
	dc_custom_io_t *io = _dc_context_custom_io(abstract->context);

	return io->packet_close(io);
}
