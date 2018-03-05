#ifndef _EEP_FUNCS_H
#define _EEP_FUNCS_H

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

//call ROM's eeprom_read function. Does nothing if eep_setrdptr() hasn't been called yet
void eep_read16(uint16_t addr, uint16_t *dest);

//call ROM's eeprom_write function. Does nothing if eep_setwrptr() hasn't been called yet
void eep_write16(uint16_t addr, uint16_t *dest);

//set the address of the ROM's eeprom_read function
void eep_setrdptr(void *newaddr);

//set the address of the ROM's eeprom_read function
void eep_setwrptr(void *newaddr);

//initialize the SCI2 to act as ST95xxx SPI EEPROM interface
void eep_initSCI(void);

//reads 128-byte block from 95xxx EEPROM on SCI2
void eep_readBlock(uint16_t a, uint8_t * d);

//writes 128-byte block to 95xxx EEPROM on SCI2
void eep_writeBlock(uint16_t a, uint8_t * d);

//reads DWORD value from 95xxx EEPROM on SCI2
void eep_read32(uint16_t a, uint32_t * d);

//writes DWORD value to 95xxx EEPROM on SCI2
void eep_write32(uint16_t a, uint32_t * d);

// sends single byte via SPI
uint8_t eep_spiEx(uint8_t b);

// reads EEPROM status register
uint8_t eep_getStatus(void);

// sends "write enable" EEPROM command via SPI
void eep_writeEnable(void);

#endif
