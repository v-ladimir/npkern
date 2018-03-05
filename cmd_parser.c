/* stuff for receiving commands etc */

/* (c) copyright fenugrec 2016
 * GPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stypes.h"

#include <string.h>	//memcpy

#include "reg_defines/7055_7058_180nm.h"	//required for SCI stuff
#include "npk_ver.h"
#include "platf.h"

#include "eep_funcs.h"
#include "iso_cmds.h"
#include "npk_errcodes.h"
#include "crc.h"

#define MAX_INTERBYTE	10	//ms between bytes that causes a disconnect

extern void die(void);

/* concatenate the ReadECUID positive response byte
 * in front of the version string
 */
static const u8 npk_ver_string[] = SID_RECUID_PRC NPK_VER;

/* make receiving slightly easier maybe */
struct iso14230_msg {
	int	hdrlen;		//expected header length : 1 (len-in-fmt), 2(fmt + len), 3(fmt+addr), 4(fmt+addr+len)
	int	datalen;	//expected data length
	int	hi;		//index in hdr[]
	int	di;		//index in data[]
	u8	hdr[4];
	u8	data[256];	//255 data bytes + checksum
};

/** simple 8-bit sum */
uint8_t cks_u8(const uint8_t * data, unsigned int len) {
	uint8_t rv=0;

	while (len > 0) {
		len--;
		rv += data[len];
	}
	return rv;
}

/* sign-extend 24bit number to 32bits,
 * i.e. FF8000 => FFFF8000 etc
 * data stored as big (sh) endian
 */
static u32 reconst_24(const u8 *data) {
	u32 tmp;
	tmp = (data[0] << 16) | (data[1] << 8) | data[2];
	if (data[0] & 0x80) {
		//sign-extend to cover RAM
		tmp |= 0xFF << 24;
	}
	return tmp;
}

/** discard RX data until idle for a given time
 * @param idle : purge until interbyte > idle ms
 *
 * blocking, of course. Do not call from ISR
 */
static void sci_rxidle(unsigned ms) {
	u32 t0, tc, intv;

	if (ms > MCLK_MAXSPAN) ms = MCLK_MAXSPAN;
	intv = MCLK_GETTS(ms);	//# of ticks for delay

	t0 = get_mclk_ts();
	while (1) {
		tc = get_mclk_ts();
		if ((tc - t0) >= intv) return;

		if (SCI1.SSR.BYTE & 0x78) {
			/* RDRF | ORER | FER | PER :reset timer */
			t0 = get_mclk_ts();
			SCI1.SSR.BYTE &= 0x87;	//clear RDRF + error flags
		}
	}
}

/** send a whole buffer, blocking. For use by iso_sendpkt() only */
static void sci_txblock(const uint8_t *buf, uint32_t len) {
	for (; len > 0; len--) {
		while (!SCI1.SSR.BIT.TDRE) {}	//wait for empty
		SCI1.TDR = *buf;
		buf++;
		SCI1.SSR.BIT.TDRE = 0;		//start tx
	}
}

/** Send a headerless iso14230 packet
 * @param len is clipped to 0xff
 *
 * disables RX during sending to remove halfdup echo. Should be reliable since
 * we re-enable after the stop bit, so K should definitely be back up to '1' again
 *
 * this is blocking
 */
void iso_sendpkt(const uint8_t *buf, int len) {
	u8 hdr[2];
	uint8_t cks;
	if (len <= 0) return;

	if (len > 0xff) len = 0xff;

	SCI1.SCR.BIT.RE = 0;

	if (len <= 0x3F) {
		hdr[0] = (uint8_t) len;
		sci_txblock(hdr, 1);	//FMT/Len
	} else {
		hdr[0] = 0;
		hdr[1] = (uint8_t) len;
		sci_txblock(hdr, 2);	//Len
	}

	sci_txblock(buf, len);	//Payload

	cks = len;
	cks += cks_u8(buf, len);
	sci_txblock(&cks, 1);	//cks

	//ugly : wait for transmission end; this means re-enabling RX won't pick up a partial byte
	while (!SCI1.SSR.BIT.TEND) {}

	SCI1.SCR.BIT.RE = 1;
	return;
}



/* transmit negative response, 0x7F <SID> <NRC>
 * Blocking
 */
void tx_7F(u8 sid, u8 nrc) {
	u8 buf[3];
	buf[0]=0x7F;
	buf[1]=sid;
	buf[2]=nrc;
	iso_sendpkt(buf, 3);
}


void iso_clearmsg(struct iso14230_msg *msg) {
	msg->hdrlen = 0;
	msg->datalen = 0;
	msg->hi = 0;
	msg->di = 0;
}
enum iso_prc { ISO_PRC_ERROR, ISO_PRC_NEEDMORE, ISO_PRC_DONE };
/** Add newly-received byte to msg;
 *
 * @return ISO_PRC_ERROR if bad header, bad checksum, or overrun (caller's fault)
 *	ISO_PRC_NEEDMORE if ok but msg not complete
 *	ISO_PRC_DONE when msg complete + good checksum
 *
 * Note : the *msg->hi, ->di, ->hdrlen, ->datalen memberes must be set to 0 before parsing a new message
 */

enum iso_prc iso_parserx(struct iso14230_msg *msg, u8 newbyte) {
	u8 dl;

	// 1) new msg ?
	if (msg->hi == 0) {
		msg->hdrlen = 1;	//at least 1 byte (FMT)

		//parse FMT byte
		if ((newbyte & 0xC0) == 0x40) {
			//CARB mode, not supported
			return ISO_PRC_ERROR;
		}
		if (newbyte & 0x80) {
			//addresses supplied
			msg->hdrlen += 2;
		}

		dl = newbyte & 0x3f;
		if (dl == 0) {
			/* Additional length byte present */
			msg->hdrlen += 1;
		} else {
			/* len-in-fmt : we can set length already */
			msg->datalen = dl;
		}
	}

	// 2) add to header if required
	if (msg->hi != msg->hdrlen) {
		msg->hdr[msg->hi] = newbyte;
		msg->hi += 1;
		// fetch LEN byte if applicable
		if ((msg->datalen == 0) && (msg->hi == msg->hdrlen)) {
			msg->datalen = newbyte;
		}
		return ISO_PRC_NEEDMORE;
	}

	// ) here, header is complete. Add to data
	msg->data[msg->di] = newbyte;
	msg->di += 1;

	// +1 because we need checksum byte too
	if (msg->di != (msg->datalen + 1)) {
		return ISO_PRC_NEEDMORE;
	}

	// ) data now complete. valide cks
	u8 cks = cks_u8(msg->hdr, msg->hdrlen);
	cks += cks_u8(msg->data, msg->datalen);
	if (cks == msg->data[msg->datalen]) {
		return ISO_PRC_DONE;
	}
	return ISO_PRC_ERROR;
}


/* Command state machine */
static enum t_cmdsm {
	CM_IDLE,		//not initted, only accepts the "startComm" request
	CM_READY,		//initted, accepts all commands

} cmstate;

/* flash state machine */
static enum t_flashsm {
	FL_IDLE,
	FL_READY,	//after doing init.
} flashstate;

/* initialize command parser state machine;
 * updates SCI1 settings : 62500 bps
 * beware the FER error flag, it disables further RX. So when changing BRR, if the host sends a byte
 * FER will be set, etc.
 */

void cmd_init(u8 brrdiv) {
	cmstate = CM_IDLE;
	flashstate = FL_IDLE;
	SCI1.SCR.BYTE &= 0xCF;	//disable TX + RX
	SCI1.BRR = brrdiv;		// speed = (div + 1) * 625k
	SCI1.SSR.BYTE &= 0x87;	//clear RDRF + error flags
	SCI1.SCR.BYTE |= 0x30;	//enable TX+RX , no RX interrupts for now
	return;
}

static void cmd_startcomm(void) {
	// KW : noaddr;  len-in-fmt or lenbyte
	static const u8 txbuf[3] = {0xC1, 0x67, 0x8F};
	iso_sendpkt(txbuf, 3);
	flashstate = FL_IDLE;
}

/* dump command processor, called from cmd_loop.
 * args[0] : address space (0: EEPROM 93cxx, 1: ROM)
 * args[1,2] : # of 32-byte blocks
 * args[3,4] : (address / 32)
 *
 * EEPROM addresses are interpreted as the flattened memory, i.e. 93C66 set as 256 * 16bit will
 * actually be read as a 512 * 8bit array, so block #0 is bytes 0 to 31 == words 0 to 15.
 *
 * ex.: "00 00 02 00 01" dumps 64 bytes @ EEPROM 0x20 (== address 0x10 in 93C66)
 * ex.: "01 80 00 00 00" dumps 1MB of ROM@ 0x0
 *
 */
static void cmd_dump(struct iso14230_msg *msg) {
	u32 addr;
	u32 len;
	u8 space;
	u8 *args = &msg->data[1];	//skip SID byte

	if (msg->datalen != 6) {
		tx_7F(SID_DUMP, 0x12);
		return;
	}

	space = args[0];
	len = 32 * ((args[1] << 8) | args[2]);
	addr = 32 * ((args[3] << 8) | args[4]);
	switch (space) {
	case SID_DUMP_EEPROM:
		/* dump eeprom stuff */
		addr /= 2;	/* modify address to fit with eeprom 256*16bit org */
		len &= ~1;	/* align to 16bits */
		while (len) {
			u16 pbuf[17];
			u8 *pstart;	//start of ISO packet
			u16 *ebuf=&pbuf[1];	//cheat : form an ISO packet with the pos resp code in pbuf[0]

			int pktlen;
			int ecur;

			pstart = (u8 *)(pbuf) + 1;
			*pstart = SID_DUMP + 0x40;

			pktlen = len;
			if (pktlen > 32) pktlen = 32;

			for (ecur = 0; ecur < (pktlen / 2); ecur += 1) {
				eep_read16((uint8_t) addr + ecur, (uint16_t *)&ebuf[ecur]);
			}
			iso_sendpkt(pstart, pktlen + 1);

			len -= pktlen;
			addr += (pktlen / 2);	//work in eeprom addresses
		}
		break;
	case SID_DUMP_ROM:
		/* dump from ROM */
		while (len) {
			u8 buf[33];

			int pktlen;

			buf[0] = SID_DUMP + 0x40;
			pktlen = len;
			if (pktlen > 32) pktlen = 32;
			memcpy(&buf[1], (void *) addr, pktlen);
			iso_sendpkt(buf, pktlen + 1);
			len -= pktlen;
			addr += pktlen;
		}
		break;
	default:
		tx_7F(SID_DUMP, 0x12);
		break;
	}	//switch (space)

	return;
}


/* SID 34 : prepare for reflashing */
static void cmd_flash_init(void) {
	u8 txbuf[2];
	u8 errval;

	if (!platf_flash_init(&errval)) {
		tx_7F(SID_FLREQ, errval);
		return;
	}

	txbuf[0] = 0x74;
	iso_sendpkt(txbuf, 1);
	flashstate = FL_READY;
	return;
}

/* "one's complement" checksum; if adding causes a carry, add 1 to sum. Slightly better than simple 8bit sum
 */
u8 cks_add8(u8 *data, unsigned len) {
	u16 sum = 0;
	for (; len; len--, data++) {
		sum += *data;
		if (sum & 0x100) sum += 1;
		sum = (u8) sum;
	}
	return sum;
}

/* compare given CRC with calculated value.
 * data is the first byte after SID_CONF_CKS1
 */
int cmd_romcrc(const u8 *data) {
	unsigned idx;
	// <CNH> <CNL> <CRC0H> <CRC0L> ...<CRC3H> <CRC3L>
	u16 chunkno = (*(data+0) << 8) | *(data+1);
	for (idx = 0; idx < ROMCRC_NUMCHUNKS; idx++) {
		u16 crc;
		data += 2;
		u16 test_crc = (*(data+0) << 8) | *(data+1);
		u32 start = chunkno * ROMCRC_CHUNKSIZE;
		crc = crc16((const u8 *)start, ROMCRC_CHUNKSIZE);
		if (crc != test_crc) {
			return -1;
		}
		chunkno += 1;
	}

	return 0;
}

/* handle low-level reflash commands */
static void cmd_flash_utils(struct iso14230_msg *msg) {
	u8 subcommand;
	u8 txbuf[10];
	u32 tmp;

	u32 rv = 0x10;

	if (flashstate != FL_READY) {
		rv = 0x22;
		goto exit_bad;
	}

	if (msg->datalen <= 1) {
		rv = 0x12;
		goto exit_bad;
	}

	subcommand = msg->data[1];

	switch(subcommand) {
	case SIDFL_EB:
		//format : <SID_FLASH> <SIDFL_EB> <BLOCKNO>
		if (msg->datalen != 3) {
			rv = 0x12;
			goto exit_bad;
		}
		rv = platf_flash_eb(msg->data[2]);
		if (rv) {
			rv = (rv & 0xFF) | 0x80;	//make sure it's a valid extented NRC
			goto exit_bad;
		}
		break;
	case SIDFL_WB:
		//format : <SID_FLASH> <SIDFL_WB> <A2> <A1> <A0> <D0>...<D127> <CRC>
		if (msg->datalen != (SIDFL_WB_DLEN + 6)) {
			rv = 0x12;
			goto exit_bad;
		}

		if (cks_add8(&msg->data[2], (SIDFL_WB_DLEN + 3)) != msg->data[SIDFL_WB_DLEN + 5]) {
			rv = 0x77;	//crcerror
			goto exit_bad;
		}

		tmp = (msg->data[2] << 16) | (msg->data[3] << 8) | msg->data[4];
		rv = platf_flash_wb(tmp, (u32) &msg->data[5], SIDFL_WB_DLEN);
		if (rv) {
			rv = (rv & 0xFF) | 0x80;	//make sure it's a valid extented NRC
			goto exit_bad;
		}
		break;
	case SIDFL_UNPROTECT:
		//format : <SID_FLASH> <SIDFL_UNPROTECT> <~SIDFL_UNPROTECT>
		if (msg->datalen != 3) {
			rv = 0x12;
			goto exit_bad;
		}
		if (msg->data[2] != (u8) ~SIDFL_UNPROTECT) {
			rv = 0x35;	//InvalidKey
			goto exit_bad;
		}

		platf_flash_unprotect();
		break;
	default:
		rv = 0x12;
		goto exit_bad;
		break;
	}

	txbuf[0] = SID_FLASH + 0x40;
	iso_sendpkt(txbuf, 1);	//positive resp
	return;

exit_bad:
	tx_7F(SID_FLASH, rv);
	return;
}

/* EEPROM manipulations */
static void cmd_ee(struct iso14230_msg *msg) {
	
	uint8_t action;
	uint16_t addr, data;
	uint32_t dwdata;
	uint8_t repl[134];
		
	if ((msg->datalen != 132) && (msg->datalen != 4) && (msg->datalen != 6) && (msg->datalen != 8))  {
		tx_7F(SID_EEPROM, 0x12);
		return;
	}
	
	action = msg->data[1];
	addr = ((msg->data[2] << 8) & 0xFF00) | msg->data[3];
	repl[0] = SID_EEPROM + 0x40;
			
	switch (action) {
	
	case SID_EE_RD16:
		eep_read16(addr, &data); 
		repl[1] = (uint8_t) (data >> 8) & 0xFF;
		repl[2] = (uint8_t) data & 0xFF;
		iso_sendpkt(repl, 3);
		break;
		
	case SID_EE_WR16:
		if(msg->datalen == 6) {
			data = msg->data[5] | ((msg->data[4] << 8) & 0xFF00);
			eep_write16(addr, &data);
			iso_sendpkt(repl, 1);
		} else {
			tx_7F(SID_EEPROM, 0x12);
		}
		break;
		
	case SID_EE_RD32:
		eep_read32(addr, &dwdata); 
		repl[1] = (uint8_t) (dwdata >> 24) & 0xFF;
		repl[2] = (uint8_t) (dwdata >> 16) & 0xFF;
		repl[3] = (uint8_t) (dwdata >> 8) & 0xFF;
		repl[4] = (uint8_t) dwdata & 0xFF;
		iso_sendpkt(repl, 5);
		break;
		
	case SID_EE_WR32:
		if ((msg->datalen == 8) && (((addr & 0xFFE0) == addr) || ((addr & (~0xFFE0)) >= 4))) {
			dwdata =	((uint32_t) (msg->data[4] << 24) & 0xFF000000) |
						((uint32_t) (msg->data[5] << 16) & 0xFF0000) | 
						((uint32_t) (msg->data[6] << 8) & 0xFF00) | 
						((uint32_t) (msg->data[7]) & 0xFF); 
			eep_write32(addr, &dwdata);
			iso_sendpkt(repl, 1);
		} else {
			tx_7F(SID_EEPROM, 0x12);
		}
		break;
	
	case SID_EE_RD128:
		eep_readBlock(addr, &repl[1]); 
		iso_sendpkt(repl, 129);
		break;
	
	case SID_EE_WR128:
		if ((addr % 32 == 0) && (msg->datalen == 132)) {
			eep_writeBlock(addr, &msg->data[4]);
			iso_sendpkt(repl, 1);
		} else {
			tx_7F(SID_EEPROM, 0x12);
		}
		break;
	default:
		tx_7F(SID_EEPROM, 0x12);
		break;
	} // switch action
}


/* ReadMemByAddress */
void cmd_rmba(struct iso14230_msg *msg) {
	//format : <SID_RMBA> <AH> <AM> <AL> <SIZ>
	/* response : <SID + 0x40> <D0>....<Dn> <AH> <AM> <AL> */

	u32 addr;
	u8 buf[255];
	int siz;

	if (msg->datalen != 5) goto bad12;
	siz = msg->data[4];

	if ((siz == 0) || (siz > 251)) goto bad12;

	addr = reconst_24(&msg->data[1]);

	buf[0] = SID_RMBA + 0x40;
	memcpy(buf + 1, (void *) addr, siz);

	siz += 1;
	buf[siz++] = msg->data[1];
	buf[siz++] = msg->data[2];
	buf[siz++] = msg->data[3];

	iso_sendpkt(buf, siz);
	return;

bad12:
	tx_7F(SID_RMBA, 0x12);
	return;
}


/* WriteMemByAddr - RAM only */
static void cmd_wmba(struct iso14230_msg *msg) {
	/* WriteMemByAddress (RAM only !) . format : <SID_WMBA> <AH> <AM> <AL> <SIZ> <DATA> , siz <= 250. */
	/* response : <SID + 0x40> <AH> <AM> <AL> */
	u8 rv = 0x12;
	u32 addr;
	u8 siz;
	u8 *src;

	if (msg->datalen < 6) goto badexit;
	siz = msg->data[4];

	if (	(siz == 0) ||
		(siz > 250) ||
		(msg->datalen != (siz + 5))) goto badexit;

	addr = reconst_24(&msg->data[1]);

	// bounds check, restrict to RAM
	if (	(addr < RAM_MIN) ||
		(addr > RAM_MAX)) {
		rv = 0x42;
		goto badexit;
	}

	/* write */
	src = &msg->data[5];
	memcpy((void *) addr, src, siz);

	msg->data[0] = SID_WMBA + 0x40;	//cheat !
	iso_sendpkt(msg->data, 4);
	return;

badexit:
	tx_7F(SID_WMBA, rv);
	return;
}

/* set & configure kernel */
static void cmd_conf(struct iso14230_msg *msg) {
	u8 resp[4];
	u32 tmp;

	resp[0] = SID_CONF + 0x40;
	if (msg->datalen < 3) goto bad12;

	switch (msg->data[1]) {
	case SID_CONF_SETSPEED:
		/* set comm speed (BRR divisor reg) : <SID_CONF> <SID_CONF_SETSPEED> <new divisor> */
		iso_sendpkt(resp, 1);
		cmd_init(msg->data[2]);
		sci_rxidle(25);
		return;
		break;
	case SID_CONF_SETEEPR:
		/* set eeprom_read() function address <SID_CONF> <SID_CONF_SETEEPR> <AH> <AM> <AL> */
		if (msg->datalen != 5) goto bad12;
		tmp = (msg->data[2] << 16) | (msg->data[3] << 8) | msg->data[4];
		eep_setrdptr((void *) tmp);
		iso_sendpkt(resp, 1);
		return;
		break;
	case SID_CONF_SETEEPW:
		/* set eeprom_write() function address <SID_CONF> <SID_CONF_SETEEPW> <AH> <AM> <AL> */
		if (msg->datalen != 5) goto bad12;
		tmp = (msg->data[2] << 16) | (msg->data[3] << 8) | msg->data[4];
		eep_setwrptr((void *) tmp);
		iso_sendpkt(resp, 1);
		return;
		break;
	case SID_CONF_CKS1:
		//<SID_CONF> <SID_CONF_CKS1> <CNH> <CNL> <CRC0H> <CRC0L> ...<CRC3H> <CRC3L>
		if (msg->datalen != 12) {
			goto bad12;
		}
		if (cmd_romcrc(&msg->data[2])) {
			tx_7F(SID_CONF, SID_CONF_CKS1_BADCKS);
			return;
		}	
		iso_sendpkt(resp, 1);
		return;
		break;
#ifdef DIAG_U16READ
	case SID_CONF_R16:
		{
		u16 val;
		//<SID_CONF> <SID_CONF_R16> <A2> <A1> <A0>
		tmp = reconst_24(&msg->data[2]);
		tmp &= ~1;	//clr lower bit of course
		val = *(const u16 *) tmp;
		resp[1] = val >> 8;
		resp[2] = val & 0xFF;
		iso_sendpkt(resp,3);
		return;
		break;
		}
#endif
	default:
		goto bad12;
		break;
	}

bad12:
	tx_7F(SID_CONF, 0x12);
	return;
}


/* command parser; infinite loop waiting for commands.
 * not sure if it's worth the trouble to make this async,
 * what other tasks could run in background ? reflash shit ?
 *
 * This receives valid iso14230 packets; message splitting is by pkt length
 */
void cmd_loop(void) {
	u8 rxbyte;
	u8 txbuf[63];

	struct iso14230_msg msg;

	//u32 t_last, t_cur;	//timestamps

	iso_clearmsg(&msg);

	while (1) {
		enum iso_prc prv;

		/* in case of errors (ORER | FER | PER), reset state mach. */
		if (SCI1.SSR.BYTE & 0x38) {

			cmstate = CM_IDLE;
			flashstate = FL_IDLE;
			iso_clearmsg(&msg);
			sci_rxidle(MAX_INTERBYTE);
			continue;
		}

		if (!SCI1.SSR.BIT.RDRF) continue;

		rxbyte = SCI1.RDR;
		SCI1.SSR.BIT.RDRF = 0;

		//t_cur = get_mclk_ts();	/* XXX TODO : filter out interrupted messages with t>5ms interbyte ? */

		/* got a byte; parse according to state */
		prv = iso_parserx(&msg, rxbyte);

		if (prv == ISO_PRC_NEEDMORE) {
			continue;
		}
		if (prv != ISO_PRC_DONE) {
			iso_clearmsg(&msg);
			sci_rxidle(MAX_INTERBYTE);
			continue;
		}
		/* here, we have a complete iso frame */

		switch (cmstate) {
		case CM_IDLE:
			/* accept only startcomm requests */
			if (msg.data[0] == 0x81) {
				cmd_startcomm();
				cmstate = CM_READY;
			}
			iso_clearmsg(&msg);
			break;

		case CM_READY:
			switch (msg.data[0]) {
			case 0x81:
				cmd_startcomm();
				iso_clearmsg(&msg);
				break;
			case SID_RECUID:
				iso_sendpkt(npk_ver_string, sizeof(npk_ver_string));
				iso_clearmsg(&msg);
				break;
			case SID_CONF:
				cmd_conf(&msg);
				iso_clearmsg(&msg);
				break;
			case SID_RESET:
				/* ECUReset */
				txbuf[0] = msg.data[0] + 0x40;
				iso_sendpkt(txbuf, 1);
				die();
				break;
			case SID_RMBA:
				cmd_rmba(&msg);
				iso_clearmsg(&msg);
				break;
			case SID_WMBA:
				cmd_wmba(&msg);
				iso_clearmsg(&msg);
				break;
			case SID_DUMP:
				cmd_dump(&msg);
				iso_clearmsg(&msg);
				break;
			case SID_FLASH:
				cmd_flash_utils(&msg);
				iso_clearmsg(&msg);
				break;
			case SID_TP:
				txbuf[0] = msg.data[0] + 0x40;
				iso_sendpkt(txbuf, 1);
				iso_clearmsg(&msg);
				break;
			case SID_FLREQ:
				cmd_flash_init();
				iso_clearmsg(&msg);
				break;
			case SID_EEPROM:
				cmd_ee(&msg);
				iso_clearmsg(&msg);
				break;
			default:
				tx_7F(msg.data[0], 0x11);
				iso_clearmsg(&msg);
				break;
			}	//switch (SID)
			break;
		default :
			//invalid state, or nothing special to do
			break;
		}	//switch (cmstate)
	}	//while 1

	die();
}
