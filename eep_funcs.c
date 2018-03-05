#include "stypes.h"

#include "reg_defines/7055_7058_180nm.h" //required for SCI stuff

#include "eep_funcs.h" // EEPROM functions prototype list

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


/* built-in EEPROM read & write functions in stock ROM */
/* this assumes the compiler ABI matches the stock ROM, i.e. input args in r4, r5 */
static void (*builtin_eep_read16)(uint16_t addr, uint16_t *dest) = 0;
static void (*builtin_eep_write16)(uint16_t addr, uint16_t *dest) = 0;


void eep_read16(uint16_t addr, uint16_t *dest) {
	if (builtin_eep_read16 == 0) return;
	builtin_eep_read16(addr, dest);
	return;
}

void eep_write16(uint16_t addr, uint16_t *dest) {
	if (builtin_eep_write16 == 0) return;
	builtin_eep_write16(addr, dest);
	return;
}

//set the address of the ROM's eeprom_read function
void eep_setrdptr(void *newaddr) {
	builtin_eep_read16 = newaddr;
	return;
}

//set the address of the ROM's eeprom_write function
void eep_setwrptr(void *newaddr) {
	builtin_eep_write16 = newaddr;
	return;
}


/* below are the kernel built-in EEPROM routines interacting with 95xxx ICS connected to SCI2 CPU interface */

//initialize the SCI2 to act as ST95xxx SPI EEPROM interface
void eep_initSCI(void) {
	SCI2.SCR.BYTE = 0; // Disable TX / RX, interrupts, setup CLK pin
	SCI2.SMR.BYTE = 0x80; // SYNC mode
	
	SCI2.BRR = 1;  // 2.5 Mbit 
	SCI2.SDCR.BIT.DIR = 1; // MSB first
	
	PFC.PCIOR.BIT.B4 = 1; // PC4 as output (A)
	PFC.PCCR.BIT.PC4MD = 0; // PC4 as GPIO (C)
	PFC.PCCR.BIT.PC3MD = 1; // PC3 as RXD2
	PFC.PCCR.BIT.PC2MD = 1; // PC2 as TXD2
	PFC.PBCRH.BIT.PB15MD = 2; // PB15 as SCK (2)
	PFC.PBIR.BIT.B15 = 0; // not invert SCK (6)
	
	// below must be done at once to avoid data 1-bit shifting during reception
	SCI2.SCR.BYTE |= (1<<4) | (1<<5); //	SCI2.SCR.BIT.TE = 1; SCI2.SCR.BIT.RE = 1;
}

// reads 128-byte block from 95xxx EEPROM on SCI2
void eep_readBlock(uint16_t a, uint8_t * d) {
	uint8_t cmd[] = {0x03, (uint8_t) (a >> 8) & 0xFF , (uint8_t) a & 0xFF};
	
	eep_initSCI();
	
	PC.DR.BIT.B4 = 0; // CS low
	
	uint8_t i;
	for (i = 0; i < sizeof(cmd); i++)
	{
		eep_spiEx(cmd[i]);
	}
	
	for (i = 0; i < 128; i++) {
		*d++ = eep_spiEx(0xFF);
	}
	
	PC.DR.BIT.B4 = 1; // CS high
}

// writes 128-byte block to 95xxx EEPROM on SCI2
void eep_writeBlock(uint16_t a, uint8_t * d) {
	uint8_t cmd[3];
	uint8_t p, c;
	
	cmd[0] = 0x02;
	
	eep_initSCI();
	
	for (p = 0; p < 4; p++) {
		eep_writeEnable();
		
		cmd[1] = (uint8_t) (a >> 8) & 0xFF;
		cmd[2] = (uint8_t) a & 0xFF;
		
		PC.DR.BIT.B4 = 0; // CS low
		for (c = 0; c < sizeof(cmd); c++) {
			eep_spiEx(cmd[c]);
		}
		
		for (c = 0; c < 32; c++) {
			eep_spiEx(*d++);
		}
		a += 32;
		PC.DR.BIT.B4 = 1; // CS high
		while (eep_getStatus() & 1);
	}
}

//reads DWORD value from 95xxx EEPROM on SCI2
void eep_read32(uint16_t a, uint32_t * d) {
	uint8_t cmd[] = {0x03, (uint8_t) (a >> 8) & 0xFF , (uint8_t) a & 0xFF};
	uint8_t i;
	uint8_t r[4];
	
	eep_initSCI();
	
	PC.DR.BIT.B4 = 0; // CS low
	for (i = 0; i < sizeof(cmd); i++) {
		eep_spiEx(cmd[i]);
	} 
	for (i = 0; i < 4; i++) {
		r[i] = eep_spiEx(0xFF);
	}
	PC.DR.BIT.B4 = 1; // CS high
	
	*d = 	((uint32_t) (r[0] << 24) & 0xFF000000) |
			((uint32_t) (r[1] << 16) & 0xFF0000) |
			((uint32_t) (r[2] << 8) & 0xFF00) |
			((uint32_t) r[3] & 0xFF);
}

//writes DWORD value to 95xxx EEPROM on SCI2
void eep_write32(uint16_t a, uint32_t * d) {
	uint8_t cmd[] = {0x02,	(uint8_t) (a >> 8) & 0xFF , (uint8_t) a & 0xFF,
							(uint8_t) (*d >> 24) & 0xFF, (uint8_t) (*d >> 16) & 0xFF,
							(uint8_t) (*d >> 8) & 0xFF, (uint8_t) (*d) & 0xFF};
	uint8_t i;
	
	eep_initSCI();
	
	eep_writeEnable();
	
	PC.DR.BIT.B4 = 0; // CS low
	for (i = 0; i < sizeof(cmd); i++) {
		eep_spiEx(cmd[i]);
	}
	PC.DR.BIT.B4 = 1; // CS high
	while (eep_getStatus() & 1);
}



// sends & receives single byte via SPI
uint8_t eep_spiEx(uint8_t b) {
	uint8_t r;
	
	while (SCI2.SSR.BIT.TDRE != 1);
	SCI2.TDR = b;
	SCI2.SSR.BIT.TDRE = 0;
	while (SCI2.SSR.BIT.RDRF != 1) {
			SCI2.SSR.BYTE &= 0xC7;
	}
	r = SCI2.RDR;
	SCI2.SSR.BIT.RDRF = 0;
	return r;
}

// reads EEPROM status register
uint8_t eep_getStatus(void) {
	uint8_t p[] = {0x05, 0xFF};
	
	PC.DR.BIT.B4 = 0; // CS low
	
	uint8_t i, s;
	for (i = 0; i < sizeof(p); i++) {
		s = eep_spiEx(p[i]);
	}
	PC.DR.BIT.B4 = 1; // CS high
	
	return s;
}

// sends "write enable" EEPROM command via SPI
void eep_writeEnable(void) {
	PC.DR.BIT.B4 = 0; // CS low
	eep_spiEx(0x06);
	PC.DR.BIT.B4 = 1; // CS high
}