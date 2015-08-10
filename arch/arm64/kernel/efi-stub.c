/*
 * Copyright (C) 2013, 2014 Linaro Ltd;  <roy.franz@linaro.org>
 *
 * This file implements the EFI boot stub for the arm64 kernel.
 * Adapted from ARM version by Mark Salter <msalter@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/efi.h>
#include <asm/efi.h>
#include <asm/sections.h>

efi_status_t __init handle_kernel_image(efi_system_table_t *sys_table_arg,
					unsigned long *image_addr,
					unsigned long *image_size,
					unsigned long *reserve_addr,
					unsigned long *reserve_size,
					unsigned long dram_base,
					efi_loaded_image_t *image)
{
	efi_status_t status;
	unsigned long kernel_size, kernel_memsize = 0;
	unsigned long nr_pages;

	/* Relocate the image, if required. */
	kernel_size = _edata - _text;
	if (*image_addr != (dram_base + TEXT_OFFSET)) {
		kernel_memsize = kernel_size + (_end - _edata);

		/*
		 * First, try a straight allocation at the preferred offset.
		 * This will work around the issue where, if dram_base == 0x0,
		 * efi_low_alloc() refuses to allocate at 0x0 (to prevent the
		 * address of the allocation to be mistaken for a FAIL return
		 * value or a NULL pointer). It will also ensure that, on
		 * platforms where the [dram_base, dram_base + TEXT_OFFSET)
		 * interval is partially occupied by the firmware (like on APM
		 * Mustang), we can still place the kernel at the address
		 * 'dram_base + TEXT_OFFSET'.
		 */
		*reserve_addr = dram_base + TEXT_OFFSET;
		nr_pages = round_up(kernel_memsize, SZ_64K) / EFI_PAGE_SIZE;
		status = efi_call_early(allocate_pages, EFI_ALLOCATE_ADDRESS,
					EFI_LOADER_DATA, nr_pages,
					(efi_physical_addr_t *)reserve_addr);
		if (status == EFI_SUCCESS) {
			memcpy((void *)*reserve_addr, (void *)*image_addr,
				kernel_size);
			*image_addr = *reserve_addr;
			*reserve_size = round_up(kernel_memsize, EFI_PAGE_SIZE);
		} else {
			status = efi_low_alloc(sys_table_arg,
						kernel_memsize + TEXT_OFFSET,
						SZ_2M, reserve_addr);
			if (status == EFI_SUCCESS) {
				memcpy((void *)*reserve_addr + TEXT_OFFSET,
					(void *)*image_addr, kernel_size);
				*image_addr = *reserve_addr + TEXT_OFFSET;
				*reserve_size = round_up(kernel_memsize +
							 TEXT_OFFSET,
							 EFI_PAGE_SIZE);
			}
		}
		if (status != EFI_SUCCESS) {
			pr_efi_err(sys_table_arg, "Failed to relocate kernel\n");
			return status;
		}
	}
	return EFI_SUCCESS;
}
