/*
 * Bluetooth H4 framing and the kernel-control endpoint for the phone's WCN6750
 * (see bsd/dev/hakios.bt.c).  iOS's bluetoothd speaks H4 over a PF_SYSTEM
 * stream socket; the driver owns the UART and passes whole H4 packets between
 * the two.  QCA in-band sleep bytes are transport control, not H4 packets.
 */
#ifndef _SYS_HAKIOS_BLUETOOTH_H_
#define _SYS_HAKIOS_BLUETOOTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAKIOS_BT_H4_MAX 4096u

struct hakios_bt_h4 {
	uint8_t bytes[HAKIOS_BT_H4_MAX];
	uint32_t used, wanted, header;
	int complete;
};

/*
 * 0: incomplete, 1: packet ready, 2: QCA IBS byte, -1: bad type,
 * -2: packet exceeds the fixed bound.  The transport must reset on framing
 * errors.  Complete bytes remain valid until the next call.
 */
static inline int
hakios_bt_h4_push(struct hakios_bt_h4 *p, uint8_t byte)
{
	if (p->complete) {
		p->used = p->wanted = p->header = 0;
		p->complete = 0;
	}
	if (p->used == 0) {
		if (byte >= 0xfc && byte <= 0xfe) {
			return 2;
		}
		switch (byte) {
		case 1:
		case 3:
			p->header = 4;
			break;
		case 2:
		case 5:
			p->header = 5;
			break;
		case 4:
			p->header = 3;
			break;
		default:
			return -1;
		}
		p->wanted = p->header;
	}
	if (p->used >= HAKIOS_BT_H4_MAX) {
		return -2;
	}
	p->bytes[p->used++] = byte;
	if (p->used == p->header) {
		uint32_t length;

		switch (p->bytes[0]) {
		case 1:
		case 3:
			length = p->bytes[3];
			break;
		case 4:
			length = p->bytes[2];
			break;
		default:
			length = p->bytes[3] | (uint32_t)p->bytes[4] << 8;
			if (p->bytes[0] == 5) {
				length &= 0x3fff;
			}
			break;
		}
		if (length > HAKIOS_BT_H4_MAX - p->header) {
			p->used = p->wanted = p->header = 0;
			return -2;
		}
		p->wanted = p->header + length;
	}
	if (p->used == p->wanted) {
		p->complete = 1;
		return 1;
	}
	return 0;
}

/* HCI Command Complete: verify framing/opcode before reading status. */
static inline int
hakios_bt_cc_status(const uint8_t *event, size_t length, uint16_t opcode)
{
	if (length < 7 || event[0] != 4 || event[1] != 0x0e ||
	    (size_t)event[2] + 3 != length ||
	    event[4] != (uint8_t)opcode || event[5] != (uint8_t)(opcode >> 8)) {
		return -1;
	}
	return event[6];
}

/* QCA firmware-download acknowledgement: unified Command Complete on WCN6750. */
static inline int
hakios_bt_qca_tlv_ack(const uint8_t *event, size_t length)
{
	return (length == 8 && hakios_bt_cc_status(event, length, 0xfc00) == 0 &&
	    event[7] == 0x1e) ||
	    (length == 6 && event[0] == 4 && event[1] == 0xff && event[2] == 3 &&
	    event[3] == 0 && event[4] == 4 && event[5] == 0);
}

/* boot-arg bt=1: the phone's Bluetooth; bt_step=N: the first N steps */
void hakios_bt_init(void);

/* iokit/Kernel/hakios.bluetooth.cpp */
int HakiosBluetoothPublish(const uint8_t address[6], const uint8_t commands[64]);
void HakiosBluetoothSetReady(bool ready);

#endif /* _SYS_HAKIOS_BLUETOOTH_H_ */
