/**********************************************************************
 * Copyright (c) 2020-2023
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#ifndef _BITMAP_H_
#define _BITMAP_H_

#include <linux/bitmap.h>
#include <linux/types.h>
#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/kernel.h>

#define SMALL_LENGTH 1024
#define LARGE_LENGTH 4096

int bitmap_allocator_init(u64 size);
size_t bitmap_allocate(u64 length, void* args);
void bitmap_kill(void);

//inline int wftl_test_bit(unsigned int nr, char *addr);
//inline void wftl_set_bit(unsigned int nr, char *addr);
//inline void wftl_clear_bit(unsigned int nr, char *addr);
//inline int wftl_test_and_set_bit(unsigned int nr, char *addr);
//inline int wftl_test_and_clear_bit(unsigned int nr, char *addr);
//inline void wftl_change_bit(unsigned int nr, char *addr);
//inline uint64_t find_next_zero_bit(char *bitmap, uint64_t nbits, uint64_t sidx);

#endif
