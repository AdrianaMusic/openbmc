/*
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This file contains code to support IPMI2.0 Specificaton available @
 * http://www.intel.com/content/www/us/en/servers/ipmi/ipmi-specifications.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "bic_cpld_altera_fwupdate.h"

//#define DEBUG

/****************************/
/*      CPLD fw update      */                            
/****************************/
#define MAX_RETRY 3

#define SET_READ_DATA(buf, reg, offset) \
                     do { \
                       buf[offset++] = (reg >> 24) & 0xff; \
                       buf[offset++] = (reg >> 16) & 0xff; \
                       buf[offset++] = (reg >> 8) & 0xff; \
                       buf[offset++] = (reg >> 0) & 0xff; \
                     } while(0)\

#define SET_WRITE_DATA(buf, reg, value, offset) \
                     do { \
                       buf[offset++] = (reg >> 24) & 0xff; \
                       buf[offset++] = (reg >> 16) & 0xff; \
                       buf[offset++] = (reg >> 8)  & 0xff; \
                       buf[offset++] = (reg >> 0)  & 0xff; \
                       buf[offset++] = (value >> 0 )  & 0xFF; \
                       buf[offset++] = (value >> 8 )  & 0xFF; \
                       buf[offset++] = (value >> 16 ) & 0xFF; \
                       buf[offset++] = (value >> 24 ) & 0xFF; \
                    } while(0)\

#define GET_VALUE(buf) (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)

static int 
set_register_via_bypass(uint8_t slot_id, int reg, int val, uint8_t intf) {
  int ret;
  int retries = MAX_RETRY;
  uint8_t txbuf[32] = {0};
  uint8_t rxbuf[32] = {0};
  uint8_t txlen = 0;
  uint8_t rxlen = 0;

  if ( intf == NONE_INTF ) {
    //BIC is located on the server board
    txbuf[0] = 0x05; //bus
  } else {
    //BIC is located on the baseboard
    txbuf[0] = 0x01; //bus
  }

  txbuf[1] = 0x80; //slave
  txbuf[2] = 0x00; //read cnt
  txlen = 3;
  SET_WRITE_DATA(txbuf, reg, val, txlen);

  while ( retries > 0 ) {
    ret = bic_ipmb_send(slot_id, NETFN_APP_REQ, CMD_APP_MASTER_WRITE_READ, txbuf, txlen, rxbuf, &rxlen, intf);
    if (ret) {
      sleep(1);
      retries--;
    } else {
      break;
    }
  }

  if ( retries == 0 ) {
     printf("%s() write register fails after retry %d times. ret=%d \n", __func__, MAX_RETRY, ret);
  }
  return ret;
}

static int 
get_register_via_bypass(uint8_t slot_id, int reg, int *val, uint8_t intf) {
  int ret;
  int retries = MAX_RETRY;
  uint8_t txbuf[32] = {0};
  uint8_t rxbuf[32] = {0};
  uint8_t txlen = 0;
  uint8_t rxlen = 0;

  if ( intf == NONE_INTF ) {
    //BIC is located on the server board
    txbuf[0] = 0x05; //bus
  } else {
    //BIC is located on the baseboard
    txbuf[0] = 0x01; //bus
  }

  txbuf[1] = 0x80; //slave
  txbuf[2] = 0x04; //read cnt
  txlen = 3;
  SET_READ_DATA(txbuf, reg, txlen);

  while ( retries > 0 ) {
    ret = bic_ipmb_send(slot_id, NETFN_APP_REQ, CMD_APP_MASTER_WRITE_READ, txbuf, txlen, rxbuf, &rxlen, intf);
    if (ret) {
      sleep(1);
      retries--;
    } else {
      break;
    }
  }

  if ( retries == 0 ) {
     printf("%s() write register fails after retry %d times. ret=%d \n", __func__, MAX_RETRY, ret);
  }

  *val = GET_VALUE(rxbuf);
  return ret;
}

#define ON_CHIP_FLASH_IP_DATA_REG         0x00000000
#define ON_CHIP_FLASH_IP_CSR_STATUS_REG   0x00200020
#define ON_CHIP_FLASH_IP_CSR_CTRL_REG     0x00200024
#define ON_CHIP_FLASH_USER_VER            0x00200028
#define DUAL_BOOT_IP_BASE                 0x00200000

// status register
#define BUSY_IDLE      0x00
#define BUSY_ERASE     0x01
#define BUSY_WRITE     0x02
#define BUSY_READ      0x03

#define READ_SUCCESS   0x04
#define WRITE_SUCCESS  0x08
#define ERASE_SUCCESS  0x10

enum{
  WP_CFM0 = 0x1 << 27,
  WP_CFM1 = 0x1 << 26,
  WP_CFM2 = 0x1 << 25,
  WP_UFM0 = 0x1 << 24,
  WP_UFM1 = 0x1 << 23,
};

typedef enum{
  Sector_CFM0,
  Sector_CFM1,
  Sector_CFM2,
  Sector_UFM0,
  Sector_UFM1,
  Sector_NotSet,
} SectorType_t;

static int
read_register(uint8_t slot_id, int address, uint8_t intf) {
  int ret = 0;
  int data = 0;

  ret = get_register_via_bypass(slot_id, address, &data, intf);
  if ( ret < 0 ) {
    printf("%s() Cannot read 0x%x", __func__, address);
  }

  return data;
}

static int
write_register(uint8_t slot_id, int address, int data, uint8_t intf) {
  return set_register_via_bypass(slot_id, address, data, intf);
}

static int
write_flash_data(uint8_t slot_id, int address, int data, uint8_t intf) {
  return write_register(slot_id, ON_CHIP_FLASH_IP_DATA_REG + address, data, intf);
}

static int 
Max10_get_status(uint8_t slot_id, uint8_t intf) {
  return read_register(slot_id, ON_CHIP_FLASH_IP_CSR_STATUS_REG, intf);
}

static int
Max10_protect_sectors(uint8_t slot_id, uint8_t intf) {
  int value = 0;

  value = read_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, intf);
  value |= (0x1F<<23);

  return write_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, value, intf);
}

static int
Max10_unprotect_sector(uint8_t slot_id, SectorType_t secType, uint8_t intf) {

  int value = 0;
  value = read_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, intf);

#ifdef DEBUG
  printf("%s() 0x%x\n", __func__, value);
#endif

  switch(secType) {
    case Sector_CFM0:
      value = value & (~WP_CFM0);
      break;
    case Sector_CFM1:
      value = value & (~WP_CFM1);
      break;
    case Sector_CFM2:
      value = value & (~WP_CFM2);
      break;
    case Sector_UFM0:
      value = value & (~WP_UFM0);
      break;
    case Sector_UFM1:
      value = value & (~WP_UFM1);
      break;
    case Sector_NotSet:
      value = 0xFFFFFFFF;
      break;
  }

#ifdef DEBUG
  write_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, value, intf);
  value = read_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, intf);
  printf("%s() 0x%x\n", __func__, value);
  return 0;
#else
  return write_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, value, intf);
#endif
}

static int
Max10_erase_sector(uint8_t slot_id, SectorType_t secType, uint8_t intf) {
  int ret = 0;
  int value = 0;
  int done = 0;

  //Control register bit 20~22
  enum{
    Sector_erase_CFM0   = 0b101 << 20,
    Sector_erase_CFM1   = 0b100 << 20,
    Sector_erase_CFM2   = 0b011 << 20,
    Sector_erase_UFM0   = 0b010 << 20,
    Sector_erase_UFM1   = 0b001 << 20,
    Sector_erase_NotSet = 0b111 << 20,
  };

  value = read_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, intf);
  value &= ~(0x7<<20);  // clear bit 20~22.

#ifdef DEBUG
  printf("%s() 0x%x\n", __func__, value);
#endif

  switch(secType)
  {
    case Sector_CFM0:
      value |= Sector_erase_CFM0;
      break;
    case Sector_CFM1:
      value |= Sector_erase_CFM1;
      break;
    case Sector_CFM2:
      value |= Sector_erase_CFM2;
      break;
    case Sector_UFM0:
      value |= Sector_erase_UFM0;
      break;
    case Sector_UFM1:
      value |= Sector_erase_UFM1;
      break;
    case Sector_NotSet:
      value |= Sector_erase_NotSet;
      break;
  }

#ifdef DEBUG
  printf("%s() w 0x%x\n", __func__, value);
#endif

  write_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, value, intf);

#ifdef DEBUG
  printf("%s() r 0x%x\n", __func__, read_register(slot_id, ON_CHIP_FLASH_IP_CSR_CTRL_REG, intf));
#endif
 
  while ( done == 0 ) {
    value = read_register(slot_id, ON_CHIP_FLASH_IP_CSR_STATUS_REG, intf);
#ifdef DEBUG
    printf("%s() busy 0x%x\n", __func__, value);
#endif

    if( value & BUSY_ERASE) {
      usleep(500*1000);
      continue;
    }

    if ( value & ERASE_SUCCESS ) {
      printf("Erase sector SUCCESS. \n");
      done = 1;
    } else {
      printf("Failed to erase the sector. Is the dev read only?\n");
      ret = -1;
      sleep(1);
    }
  }

  return ret;
}

static int
is_valid_cpld_image(uint8_t signed_byte, uint8_t intf) {
  int ret = -1;

  switch (intf) {
    case BB_BIC_INTF:
        return (signed_byte == BICBB)?0:-1;
      break;
    case NONE_INTF:
        return (signed_byte == BICDL)?0:-1;
      break;
  }

  return ret;
}

int 
update_bic_cpld_altera(uint8_t slot_id, char *image, uint8_t intf, uint8_t force) {
#define STATUS_BIT_MASK  0x1F
#define CFM_START_ADDR 0x64000
#define CFM_END_ADDR   0xbffff
#define MAX10_RPD_SIZE 0x5C000

  int fd= 0;
  uint8_t *rpd_file = NULL;
  int ret = 0;
  int offset = 0;
  int address = 0;
  int receivedHex[4];
  int byte = 0;
  int data = 0;
  int status = 0;
  int retries = MAX_RETRY;
  int total = CFM_END_ADDR - CFM_START_ADDR;
  int percent = 0;
  uint8_t done = 0;
  struct stat finfo;
  int rpd_filesize = 0;
  int read_bytes = 0;

  SectorType_t secType = Sector_CFM0;

  printf("OnChip Flash Status = 0x%X., slot_id 0x%x, sectype 0x%x, intf: 0x%x, ", Max10_get_status(slot_id, intf), slot_id, secType, intf);

  //step 0 - Open the file
  if ((fd = open(image, O_RDONLY)) < 0) {
    printf("Fail to open file: %s.\n", image);
    ret = -1;
    goto error_exit;
  }

  fstat(fd, &finfo);
  rpd_filesize = finfo.st_size;
  rpd_file = malloc(rpd_filesize);
  if( rpd_file == 0 ) {
    printf("Failed to allocate memory\n");
    ret = -1;
    goto error_exit;
  }

  read_bytes = read(fd, rpd_file, rpd_filesize);
  printf("read %d bytes.\n", read_bytes);

  if ( force == 0 ) {
    //it's an old image
    if ( rpd_filesize == MAX10_RPD_SIZE ) {
      printf("image is not a valid CPLD image for this component.\n");
      ret = -1;
      goto error_exit;
    } else if ( (MAX10_RPD_SIZE + 1) == rpd_filesize ) {
      if ( is_valid_cpld_image((rpd_file[MAX10_RPD_SIZE]&0xf), intf) < 0 ) {
        printf("image is not a valid CPLD image for this component.\n");
        ret = -1;
        goto error_exit;
      }
    }
  }

  //step 1 - UnprotectSector
  ret = Max10_unprotect_sector(slot_id, secType, intf);
  if ( ret < 0 ) {
    printf("Failed to unprotect the sector.\n");
    goto error_exit;
  }

  //step 2 - Erase a sector
  ret = Max10_erase_sector(slot_id, secType, intf);
  if ( ret < 0 ) {
    printf("Failed to erase the sector.\n");
    goto error_exit;
  }
 
  //step 3 - Set Erase to `None`
  ret = Max10_erase_sector(slot_id, Sector_NotSet, intf);
  if ( ret < 0 ) {
    printf("Failed to set None.\n");
    goto error_exit;
  }

  //step 4 - Start program
  offset = 0;
  for (address = CFM_START_ADDR; address < CFM_END_ADDR; address += 4) {
    receivedHex[0] = rpd_file[offset + 0];
    receivedHex[1] = rpd_file[offset + 1];
    receivedHex[2] = rpd_file[offset + 2];
    receivedHex[3] = rpd_file[offset + 3];
    for (byte=0; byte < 4; byte++) {
      receivedHex[byte] = (((receivedHex[byte] & 0xaa)>>1)|((receivedHex[byte] & 0x55)<<1));
      receivedHex[byte] = (((receivedHex[byte] & 0xcc)>>2)|((receivedHex[byte] & 0x33)<<2));
      receivedHex[byte] = (((receivedHex[byte] & 0xf0)>>4)|((receivedHex[byte] & 0x0f)<<4));
    }

    data = (receivedHex[0]<<24)|(receivedHex[1]<<16)|(receivedHex[2]<<8)|(receivedHex[3]);
    write_flash_data(slot_id, address, data, intf);

    usleep(50);
    retries = MAX_RETRY;
    do {
      status = Max10_get_status(slot_id, intf);
      status &= STATUS_BIT_MASK;
      if( (status & WRITE_SUCCESS) == WRITE_SUCCESS) {
        done = 1;
      }

      if ( status == 0x0 ) {
        printf("retry...\n");
        retries--;
      }
    } while ( done == 0 && retries > 0);

    if ( retries == 0 ) {
      ret = -1;
      break;
    }

    // show percentage
    {
      static int last_percent = 0;
      percent = ((address - CFM_START_ADDR)*100)/total;
      if(last_percent != percent) {
        last_percent = percent;
        printf(" Progress  %d %%.  addr: 0x%X \n", percent, address);
      }
    }

    offset += 4;

  }

  if ( ret < 0 ) {
    printf("Failed to send the data.\n");
    goto error_exit;
  }

  //step 5 - protect sectors
  ret = Max10_protect_sectors(slot_id, intf);
  if ( ret < 0 ) {
    printf("Failed to erase the sector.\n");
    goto error_exit;
  }

error_exit:
  if ( fd > 0 ) close(fd);
  if ( rpd_file != NULL ) free(rpd_file);

  return ret;
}
