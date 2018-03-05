#ifndef _ISO_CMDS_H
#define _ISO_CMDS_H
/** Custom iso14230 SID commands implemented by the kernel */

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


#define SID_RECUID	0x1A	/* readECUID , in this case kernel ID */
#define SID_RECUID_PRC	"\x5A"	/* positive response code, to be concatenated to version string */

#define SID_RMBA 0x23	/* ReadMemByAddress. format : <SID_RMBA> <AH> <AM> <AL> <SIZ>  , siz <= 251. */
				/* response : <SID + 0x40> <D0>....<Dn> <AH> <AM> <AL> */

#define SID_WMBA 0x3D	/* WriteMemByAddress (RAM only !) . format : <SID_WMBA> <AH> <AM> <AL> <SIZ> <DATA> , siz <= 250. */
				/* response : <SID + 0x40> <AH> <AM> <AL> */

#define SID_TP	0x3E	/* TesterPresent; not required but available. */

#define SID_DUMP 0xBD	/* format : 0xBD <AS> <BH BL> <AH AL>  ; AS=0 for EEPROM, =1 for ROM, =3 for 95xxx EEPROM */
	#define SID_DUMP_EEPROM	0
	#define SID_DUMP_ROM 1

/* 95xxx EEPROM read/write using kernel's own functions */

#define SID_EEPROM 0xBB 
							/* format <SID_EEPROM> <RD16> <AH AL>
									  <SID_EEPROM> <WR16> <AH AL> <DH DL> */
	#define SID_EE_RD16		0 // use ROM built-in ee_read16()
	#define SID_EE_WR16		1 // use ROM built-in ee_write16()
	
							/* format <SID_EEPROM> <RD128> <AH AL>
									  <SID_EEPROM> <WR128> <AH AL> <D127> ... <D0> */
	#define SID_EE_RD128	3 // use kernel's ee_readBlock(), only 95xxx EEPROMs on SCI2 are supported
	#define SID_EE_WR128	4 // use kernel's ee_writeBlock(), only 95xxx EEPROMs on SCI2 are supported

							/* format <SID_EEPROM> <RD32> <AH AL>
									  <SID_EEPROM> <WR32> <AH AL> <D3 D2 D1 D0> */
	#define SID_EE_RD32		5 // use kernel's ee_read32() for 95xxx EEPROMs on SCI2
	#define SID_EE_WR32		6 // use kernel's ee_write32() for 95xxx EEPROMs on SCI2

	
/* SID_FLASH and subcommands */
#define SID_FLASH 0xBC	/* low-level reflash commands; only available after successful RequestDownload */
	#define SIDFL_UNPROTECT 0x55	//enable erase / write. format : <SID_FLASH> <SIDFL_UNPROTECT> <~SIDFL_UNPROTECT>
	#define SIDFL_EB	0x01	//erase block. format : <SID_FLASH> <SIDFL_EB> <BLOCK #>
	#define SIDFL_WB	0x02	//write n-byte block. format : <SID_FLASH> <SIDFL_WB> <A2> <A1> <A0> <D0>...<D(SIDFL_WB_DLEN -1)> <CRC>
						// Address is <A2 A1 A0>;   CRC is calculated on address + data.
	#define SIDFL_WB_DLEN	128	//bytes per block

/* SID_CONF and subcommands */
#define SID_CONF 0xBE /* set & configure kernel */
	#define SID_CONF_SETSPEED 0x01	/* set comm speed (BRR divisor reg) : <SID_CONF> <SID_CONF_SETSPEED> <new divisor> */
			//this requires a new StartComm request at the new speed
	#define SID_CONF_SETEEPR 0x02	/* set eeprom_read() function address <SID_CONF> <SID_CONF_SETEEPR> <AH> <AM> <AL> */
	#define SID_CONF_SETEEPW 0x05	/* set eeprom_write() function address <SID_CONF> <SID_CONF_SETEEPW> <AH> <AM> <AL> */
	
	#define SID_CONF_CKS1	0x03	//verify if 4*<CRCH:CRCL> hash is valid for 4*256B chunks of the ROM (starting at <CNH:CNL> * 1024)
								//<SID_CONF> <SID_CONF_CKS1> <CNH> <CNL> <CRC0H> <CRC0L> ...<CRC3H> <CRC3L>
		#define ROMCRC_NUMCHUNKS 4
		#define ROMCRC_CHUNKSIZE 256
	#define SID_CONF_R16 0x04		/* for debugging : do a 16bit access read at given adress in RAM (top byte 0xFF)
									* <SID_CONF> <SID_CONF_R16> <A2> <A1> <A0> */


#define SID_FLREQ 0x34	/* RequestDownload */

#define SID_RESET 0x11	/* restart ECU */

#endif
