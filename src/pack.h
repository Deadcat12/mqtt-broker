#ifndef PACK_H 
#define PACK_H

#include <stdio.h>
#include <stdint.h>

//bytes -> uint8
uint8_t unpack_u8(const uint8_t **);
//bytes -> uint16
uint16_t unpack_u16(const uint16_t **);
//bytes -> uint32
uint32_t unpack_u32(const uint32_t **);
//read a defines len of bytes
uint8_t unpack_bytes(const uint8_t **, size_t, const uint8_t *);
uint16_t unpack_string16(uint8_t **buf, uint8_t **dest);
//uint -> bytes into string
void pack_u8(uint8_t **, uint8_t);
void pack_u16(uint8_t **, uint16_t);
void pack_u32(uint8_t **, uint32_t);
void pack_bytes(uint8_t **, uint8_t *);



#endif
