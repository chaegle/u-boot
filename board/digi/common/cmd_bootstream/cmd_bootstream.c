/*
 *  Copyright (C) 2011 by Digi International Inc.
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version2  as published by
 *  the Free Software Foundation.
*/

/*
 * Bootstream support for Freescale platforms
 */

#include <common.h>
#include <asm/io.h>
#if defined(CONFIG_CMD_BOOTSTREAM) && defined(CONFIG_CMD_NAND)
#include <div64.h>
#include <command.h>
#include <nand.h>
#include <jffs2/load_kernel.h>
#include <linux/mtd/nand.h>
#include <net.h>                /* DHCP */
#include "cmd_bootstream.h"
#include "BootControlBlocks.h"

extern uint32_t mx28_nand_mark_byte_offset(void);
extern uint32_t mx28_nand_mark_bit_offset(void);

const struct mtd_config default_mtd_config = {
	.chip_count = 1,
	.chip_0_offset = 0,
	.chip_0_size = 0,
	.chip_1_offset = 0,
	.chip_1_size = 0,
	.search_exponent = 2,
	.data_setup_time = 80,
	.data_hold_time = 60,
	.address_setup_time = 25,
	.data_sample_time = 6,
	.row_address_size = 3,
	.column_address_size = 2,
	.read_command_code1 = 0x00,
	.read_command_code2 = 0x30,
	.boot_stream_major_version = 1,
	.boot_stream_minor_version = 0,
	.boot_stream_sub_version = 0,
	.ncb_version = 3,
	.boot_stream_1_address = 0,
	.boot_stream_2_address = 0,
	.flags = 0,
};

void dump_buffer(unsigned char *buf, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if ((i % 16) == 0)
			printf("\n");
		if (i == 0 || (i % 8) == 0)
			printf("  ");
		printf("%02x ", buf[i]);
	}
}

int write_firmware(struct mtd_info *mtd,
		   struct mtd_config *cfg,
		   struct mtd_bootblock *bootblock,
		   unsigned long bs_start_address,
		   unsigned int boot_stream_size_in_bytes,
		   unsigned int pre_padding)
{
	int startpage, start, size;
	int i, r, chunk;
	loff_t ofs, end;
	int chip = 0;
	unsigned long read_addr;
	size_t nbytes = 0;
	char *readbuf = NULL;

	readbuf = malloc(mtd->writesize);
	if (NULL == readbuf)
		return -1;

	if (pre_padding) {
		/*
		 * rewind bs_start_address pre_padding bytes and fill it with
		 * zeros.
		 */
		if (bs_start_address - pre_padding < PHYS_SDRAM) {
			printf("pre-padding required! "
			       "Use a $loadaddr of at least 0x%08x\n",
			       PHYS_SDRAM + pre_padding);
			goto _error;
		}
		bs_start_address -= pre_padding;
		memset((u8 *)bs_start_address, 0, pre_padding);
	}
	//----------------------------------------------------------------------
	// Loop over the two boot streams.
	//----------------------------------------------------------------------

	for (i = 0; i < 2; i++) {
		/* Set start address where bootstream is in RAM */
		read_addr = bs_start_address;

		//--------------------------------------------------------------
		// Figure out where to put the current boot stream.
		//--------------------------------------------------------------

		if (i == 0) {
			startpage = bootblock->fcb.FCB_Block.m_u32Firmware1_startingPage;
			size      = bootblock->fcb.FCB_Block.m_u32PagesInFirmware1;
			end       = bootblock->fcb.FCB_Block.m_u32Firmware2_startingPage;
		} else {
			startpage = bootblock->fcb.FCB_Block.m_u32Firmware2_startingPage;
			size      = bootblock->fcb.FCB_Block.m_u32PagesInFirmware2;
			end       = lldiv(mtd->size, mtd->writesize);
		}

		//--------------------------------------------------------------
		// Compute the byte addresses corresponding to the page
		// addresses.
		//--------------------------------------------------------------

		start = startpage * mtd->writesize;
		size  = size      * mtd->writesize;
		end   = end       * mtd->writesize;

		if (cfg->flags & F_VERBOSE)
			printf("mtd: Writting firmware image #%d @%d: 0x%08x - 0x%08x\n", i,
					chip, start, start + size);

		//--------------------------------------------------------------
		// Loop over pages as we write them.
		//--------------------------------------------------------------

		ofs = start;
		while (ofs < end && size > 0) {

			//------------------------------------------------------
			// Check if the current block is bad.
			//------------------------------------------------------

			while (nand_block_isbad(mtd, ofs) == 1) {
				if (cfg->flags & F_VERBOSE)
					fprintf(stdout, "mtd: Skipping bad block at 0x%llx\n", ofs);
				ofs += mtd->erasesize;
			}

			chunk = size;

			//------------------------------------------------------
			// Check if we've entered a new block and, if so, erase
			// it before beginning to write it.
			//------------------------------------------------------

			if (llmod(ofs, mtd->erasesize) == 0) {
				if (cfg->flags & F_VERBOSE) {
					fprintf(stdout, "erasing block at 0x%llx\n", ofs);
				}
				if (!(cfg->flags & F_DRYRUN)) {
					r = nand_erase(mtd, ofs, mtd->erasesize);
					if (r < 0) {
						fprintf(stderr, "mtd: Failed to erase block @0x%llx\n", ofs);
						ofs += mtd->erasesize;
						continue;
					}
				}
			}

			if (chunk > mtd->writesize)
				chunk = mtd->writesize;

			//------------------------------------------------------
			// Write the current chunk to the medium.
			//------------------------------------------------------

			if (cfg->flags & F_VERBOSE) {
				fprintf(stdout, "Writing bootstream file from 0x%lx to offset 0x%llx\n", read_addr, ofs);
			}
			if (!(cfg->flags & F_DRYRUN)) {
				r = nand_write_skip_bad(mtd, ofs, (size_t *)&chunk, &nbytes, mtd->size, (unsigned char *)read_addr, WITH_WR_VERIFY);
				if (r || nbytes != chunk) {
					fprintf(stderr, "mtd: Failed to write BS @0x%llx (%d)\n", ofs, r);
				}
			}
			//------------------------------------------------------
			// Verify the written data
			//------------------------------------------------------
			r = nand_read_skip_bad(mtd, ofs, (size_t*)&chunk, &nbytes, mtd->size, (unsigned char *)readbuf);
			if (r || nbytes != chunk) {
				fprintf(stderr, "mtd: Failed to read BS @0x%llx (%d)\n", ofs, r);
				goto _error;
			}
			if (memcmp((void *)read_addr, readbuf, chunk)) {
				fprintf(stderr, "mtd: Verification error @0x%llx\n", ofs);
				goto _error;
			}

			ofs += mtd->writesize;
			read_addr += mtd->writesize;
			size -= chunk;
		}
		if (cfg->flags & F_VERBOSE)
			printf("mtd: Verified firmware image #%d @%d: 0x%08x - 0x%08x\n", i,
					chip, start, start + size);

		/*
		 * Write one safe guard page:
		 * The Image_len of uboot is bigger then the real size of
		 * uboot by 1K. The ROM will get all 0xff error in this case.
		 * So we write one more page for safe guard.
		 */


		//--------------------------------------------------------------
		// Check if we ran out of room.
		//--------------------------------------------------------------

		if (ofs >= end) {
			fprintf(stderr, "mtd: Failed to write BS#%d\n", i);
			goto _error;
		}
	}

	free(readbuf);
	return 0;
_error:
	free(readbuf);
	return -1;
}

int v1_rom_mtd_init(struct mtd_info *mtd,
		    struct mtd_config *cfg,
		    struct mtd_bootblock *bootblock,
		    unsigned int boot_stream_size_in_bytes,
		    uint64_t part_size)
{
	unsigned int  stride_size_in_bytes;
	unsigned int  search_area_size_in_bytes;
#ifdef CONFIG_USE_NAND_DBBT
	unsigned int  search_area_size_in_pages;
#endif
	unsigned int  max_boot_stream_size_in_bytes;
	unsigned int  boot_stream_size_in_pages;
	unsigned int  boot_stream1_pos;
	unsigned int  boot_stream2_pos;
	BCB_ROM_BootBlockStruct_t  *fcb;
	BCB_ROM_BootBlockStruct_t  *dbbt;

	//----------------------------------------------------------------------
	// Compute the geometry of a search area.
	//----------------------------------------------------------------------

	stride_size_in_bytes = PAGES_PER_STRIDE * mtd->writesize;
	search_area_size_in_bytes = (1 << cfg->search_exponent) * stride_size_in_bytes;
#ifdef CONFIG_USE_NAND_DBBT
	search_area_size_in_pages = (1 << cfg->search_exponent) * PAGES_PER_STRIDE;
#endif
	//----------------------------------------------------------------------
	// Check if the target MTD is too small to even contain the necessary
	// search areas.
	//
	// Recall that the boot area for the i.MX28 appears at the beginning of
	// the first chip and contains two search areas: one each for the FCB
	// and DBBT.
	//----------------------------------------------------------------------

	if ((search_area_size_in_bytes * 2) > mtd->size) {
		fprintf(stderr, "mtd: mtd size too small\n");
		return -1;
	}

	//----------------------------------------------------------------------
	// Figure out how large a boot stream the target MTD could possibly
	// hold.
	//
	// The boot area will contain both search areas and two copies of the
	// boot stream.
	//----------------------------------------------------------------------

	max_boot_stream_size_in_bytes =

		lldiv(part_size - search_area_size_in_bytes * 2,
		//--------------------------------------------//
					2);

	//----------------------------------------------------------------------
	// Figure out how large the boot stream is.
	//----------------------------------------------------------------------

	boot_stream_size_in_pages =

		(boot_stream_size_in_bytes + (mtd->writesize - 1)) /
		//---------------------------------------------------//
				mtd->writesize;

	if (cfg->flags & F_VERBOSE) {
		printf("mtd: max_boot_stream_size_in_bytes = %d\n", max_boot_stream_size_in_bytes);
		printf("mtd: boot_stream_size_in_bytes = %d\n", boot_stream_size_in_bytes);
	}

	//----------------------------------------------------------------------
	// Check if the boot stream will fit.
	//----------------------------------------------------------------------

	if (boot_stream_size_in_bytes >= max_boot_stream_size_in_bytes) {
		fprintf(stderr, "mtd: bootstream too large\n");
		return -1;
	}

	//----------------------------------------------------------------------
	// Compute the positions of the boot stream copies.
	//----------------------------------------------------------------------

	boot_stream1_pos = 2 * search_area_size_in_bytes;
	boot_stream2_pos = boot_stream1_pos + max_boot_stream_size_in_bytes;

	if (cfg->flags & F_VERBOSE) {
		printf("mtd: #1 0x%08x - 0x%08x (0x%08x)\n",
				boot_stream1_pos, boot_stream1_pos + max_boot_stream_size_in_bytes,
				boot_stream1_pos + boot_stream_size_in_bytes);
		printf("mtd: #2 0x%08x - 0x%08x (0x%08x)\n",
				boot_stream2_pos, boot_stream2_pos + max_boot_stream_size_in_bytes,
				boot_stream2_pos + boot_stream_size_in_bytes);
	}

	//----------------------------------------------------------------------
	// Fill in the FCB.
	//----------------------------------------------------------------------

	fcb = &(bootblock->fcb);
	memset(fcb, 0, sizeof(*fcb));

	fcb->m_u32FingerPrint                        = FCB_FINGERPRINT;
	fcb->m_u32Version                            = FCB_VERSION_1;

	fcb->FCB_Block.m_NANDTiming.m_u8DataSetup    = cfg->data_setup_time;
	fcb->FCB_Block.m_NANDTiming.m_u8DataHold     = cfg->data_hold_time;
	fcb->FCB_Block.m_NANDTiming.m_u8AddressSetup = cfg->address_setup_time;
	fcb->FCB_Block.m_NANDTiming.m_u8DSAMPLE_TIME = cfg->data_sample_time;

	fcb->FCB_Block.m_u32PageDataSize             = mtd->writesize;
	fcb->FCB_Block.m_u32TotalPageSize            = mtd->writesize + mtd->oobsize;
	fcb->FCB_Block.m_u32SectorsPerBlock          = mtd->erasesize / mtd->writesize;

	if (mtd->writesize == 2048) {
                fcb->FCB_Block.m_u32NumEccBlocksPerPage      = mtd->writesize / 512 - 1;
                fcb->FCB_Block.m_u32MetadataBytes            = 10;
                fcb->FCB_Block.m_u32EccBlock0Size            = 512;
                fcb->FCB_Block.m_u32EccBlockNSize            = 512;
#if defined(CONFIG_MX28)
		fcb->FCB_Block.m_u32EccBlock0EccType         = ROM_BCH_Ecc_8bit;
		fcb->FCB_Block.m_u32EccBlockNEccType         = ROM_BCH_Ecc_8bit;
#elif defined(CONFIG_MX6UL)
		fcb->FCB_Block.m_u32EccBlock0EccType         = ROM_BCH_Ecc_4bit;
		fcb->FCB_Block.m_u32EccBlockNEccType         = ROM_BCH_Ecc_4bit;
#endif

	} else if (mtd->writesize == 4096) {
		fcb->FCB_Block.m_u32NumEccBlocksPerPage      = (mtd->writesize / 512) - 1;
		fcb->FCB_Block.m_u32MetadataBytes            = 10;
		fcb->FCB_Block.m_u32EccBlock0Size            = 512;
		fcb->FCB_Block.m_u32EccBlockNSize            = 512;
		if (mtd->oobsize == 218) {
			fcb->FCB_Block.m_u32EccBlock0EccType = ROM_BCH_Ecc_16bit;
			fcb->FCB_Block.m_u32EccBlockNEccType = ROM_BCH_Ecc_16bit;
		} else if ((mtd->oobsize == 128)){
			fcb->FCB_Block.m_u32EccBlock0EccType = ROM_BCH_Ecc_8bit;
			fcb->FCB_Block.m_u32EccBlockNEccType = ROM_BCH_Ecc_8bit;
		}
	} else {
		fprintf(stderr, "Illegal page size %d\n", mtd->writesize);
	}

	fcb->FCB_Block.m_u32BootPatch                  = 0; // Normal boot.

	fcb->FCB_Block.m_u32Firmware1_startingPage   = boot_stream1_pos / mtd->writesize;
	fcb->FCB_Block.m_u32Firmware2_startingPage   = boot_stream2_pos / mtd->writesize;
	fcb->FCB_Block.m_u32PagesInFirmware1         = boot_stream_size_in_pages;
	fcb->FCB_Block.m_u32PagesInFirmware2         = boot_stream_size_in_pages;
#ifdef CONFIG_USE_NAND_DBBT
	fcb->FCB_Block.m_u32DBBTSearchAreaStartAddress = search_area_size_in_pages;
#else
	fcb->FCB_Block.m_u32DBBTSearchAreaStartAddress = 0;
#endif
	fcb->FCB_Block.m_u32BadBlockMarkerByte         = mx28_nand_mark_byte_offset();
	fcb->FCB_Block.m_u32BadBlockMarkerStartBit     = mx28_nand_mark_bit_offset();
	fcb->FCB_Block.m_u32BBMarkerPhysicalOffset     = mtd->writesize;

	//----------------------------------------------------------------------
	// Fill in the DBBT.
	//----------------------------------------------------------------------

	dbbt = &(bootblock->dbbt28);
	memset(dbbt, 0, sizeof(*dbbt));

	dbbt->m_u32FingerPrint                = DBBT_FINGERPRINT2;
	dbbt->m_u32Version                    = 1;

	dbbt->DBBT_Block.v2.m_u32NumberBB        = 0;
	dbbt->DBBT_Block.v2.m_u32Number2KPagesBB = 0;
	return 0;

}

//------------------------------------------------------------------------------
// This function writes the search areas for a given BCB. It will write *two*
// search areas for a given BCB. If there are multiple chips, it will write one
// search area on each chip. If there is one chip, it will write two search
// areas on the first chip.
//
// md         A pointer to the current struct mtd_data.
// bcb_name   A pointer to a human-readable string that indicates what kind of
//            BCB we're writing. This string will only be used in log messages.
// ofs1       If there is one chips, the index of the
// ofs2
// ofs_mchip  If there are multiple chips, the index of the search area to write
//            on both chips.
// end        The number of consecutive search areas to be written.
// size       The size of the BCB data to be written.
// ecc        Indicates whether or not to use hardware ECC.
//------------------------------------------------------------------------------

int mtd_commit_bcb(struct mtd_info *mtd,
		   struct mtd_config *cfg,
		   struct mtd_bootblock *bootblock,
		   char *bcb_name,
		   loff_t ofs1, loff_t ofs2, loff_t ofs_mchip,
		   loff_t end, size_t size)
{
	int chip;
	loff_t end_index, search_area_indices[2], o;
	int err = 0, r = -1;
	int i;
	int j;
	unsigned stride_size_in_bytes;
	unsigned search_area_size_in_strides;
	unsigned search_area_size_in_bytes;
	size_t nbytes = 0;
	unsigned char *readbuf = NULL;
	unsigned count;

	readbuf = malloc(mtd->writesize);
	if (NULL == readbuf)
		return -1;

	//----------------------------------------------------------------------
	// Compute some important facts about geometry.
	//----------------------------------------------------------------------
#ifdef MX_USE_SINGLE_PAGE_STRIDE	/* mx23, mx28 */
		stride_size_in_bytes        = mtd->erasesize;
		search_area_size_in_strides = 4;
		search_area_size_in_bytes   = search_area_size_in_strides * stride_size_in_bytes;
		count = 2; //write two copy on mx23/28
#else
		stride_size_in_bytes        = PAGES_PER_STRIDE * mtd->writesize;
		search_area_size_in_strides = 1 << cfg->search_exponent;
		search_area_size_in_bytes   = search_area_size_in_strides * stride_size_in_bytes;
		count = 1; //only write one copy
#endif
	/* NOTE (hpalacio): For i.MX28 we are not defining MX_USE_SINGLE_PAGE_STRIDE and
	 * so we are getting into the 'else' above. The calculated figures result the same
	 * except for the 'count' field. Having the 'count' field to a value of 2 simply
	 * loops to write twice the four FCB or DBBT copies.
	 */

	//----------------------------------------------------------------------
	// Check whether there are multiple chips and set up the two search area
	// indices accordingly.
	//----------------------------------------------------------------------

	if (cfg->chip_count > 1)
		search_area_indices[0] = search_area_indices[1] = ofs_mchip;
	else {
		search_area_indices[0] = ofs1;
		search_area_indices[1] = ofs2;
	}

	//----------------------------------------------------------------------
	// Loop over search areas for this BCB.
	//----------------------------------------------------------------------

	for (i = 0; !err && i < count; i++) {

		//--------------------------------------------------------------
		// Compute the search area index that marks the end of the
		// writing on this chip.
		//--------------------------------------------------------------

		end_index = search_area_indices[i] + end;

		//--------------------------------------------------------------
		// Figure out which chip we're writing.
		//--------------------------------------------------------------

		chip = (cfg->chip_count > 1) ? i : 0;

		//--------------------------------------------------------------
		// Loop over consecutive search areas to write.
		//--------------------------------------------------------------

		for (; search_area_indices[i] < end_index; search_area_indices[i]++) {

			//------------------------------------------------------
			// Compute the byte offset of the beginning of this
			// search area.
			//------------------------------------------------------

			o = search_area_indices[i] * search_area_size_in_bytes;

			//------------------------------------------------------
			// Loop over strides in this search area.
			//------------------------------------------------------

			for (j = 0; j < search_area_size_in_strides; j++, o += stride_size_in_bytes) {

				//----------------------------------------------
				// If we're crossing into a new block, erase it
				// first.
				//----------------------------------------------

				if (llmod(o, mtd->erasesize) == 0) {
					if (cfg->flags & F_VERBOSE) {
						fprintf(stdout, "erasing block at 0x%llx\n", o);
					}
					if (!(cfg->flags & F_DRYRUN)) {
						r = nand_erase(mtd, o, mtd->erasesize);
						if (r < 0) {
							fprintf(stderr, "mtd: Failed to erase block @0x%llx\n", o);
							err++;
							continue;
						}
					}
				}

				//----------------------------------------------
				// Write & verify the page.
				//----------------------------------------------

				if (cfg->flags & F_VERBOSE)
					fprintf(stdout, "mtd: Writing %s%d @%d:0x%llx(%x)\n",
								bcb_name, j, chip, o, size);

				if (!(cfg->flags & F_DRYRUN)) {
					if (size == mtd->writesize + mtd->oobsize) {
						/* We're going to write a raw page (data+oob) */
						mtd_oob_ops_t ops = {
							.datbuf = bootblock->buf,
							.len = mtd->writesize,
							.mode = MTD_OPS_RAW,
						};

						r = mtd_write_oob(mtd, o, &ops);
						if (r) {
							fprintf(stderr, "mtd: Failed to write %s @%d: 0x%llx (%d)\n",
								bcb_name, chip, o, r);
							err ++;
						}
						//------------------------------------------------------
						// Verify the written data
						//------------------------------------------------------
						ops.datbuf = (u8 *)readbuf;
						r = mtd_read_oob(mtd, o, &ops);
						if (r) {
							fprintf(stderr, "mtd: Failed to read @0x%llx (%d)\n", o, r);
							err++;
							goto _error;
						}
						if (memcmp(bootblock->buf, readbuf, mtd->writesize)) {
							fprintf(stderr, "mtd: Verification error @0x%llx\n", o);
							err++;
							goto _error;
						}

						if (cfg->flags & F_VERBOSE)
							fprintf(stdout, "mtd: Verified %s%d @%d:0x%llx(%x)\n",
										bcb_name, j, chip, o, size);
					}
					else {
						r = nand_write_skip_bad(mtd, o, &size, &nbytes, mtd->size, bootblock->buf, WITH_WR_VERIFY);
						if (r || nbytes != size) {
							fprintf(stderr, "mtd: Failed to write %s @%d: 0x%llx (%d)\n",
								bcb_name, chip, o, r);
							err ++;
						}
						//------------------------------------------------------
						// Verify the written data
						//------------------------------------------------------
						r = nand_read_skip_bad(mtd, o, &size, &nbytes, mtd->size, readbuf);
						if (r || nbytes != size) {
							fprintf(stderr, "mtd: Failed to read @0x%llx (%d)\n", o, r);
							err++;
							goto _error;
						}
						if (memcmp(bootblock->buf, readbuf, size)) {
							fprintf(stderr, "mtd: Verification error @0x%llx\n", o);
							err++;
							goto _error;
						}
						if (cfg->flags & F_VERBOSE)
							fprintf(stdout, "mtd: Verified %s%d @%d:0x%llx(%x)\n",
										bcb_name, j, chip, o, size);
					}
				}
			}

		}

	}

	if (cfg->flags & F_VERBOSE)
		printf("%s(%s): status %d\n", __func__, bcb_name, err);

_error:
	free(readbuf);
	return err;
}

int v1_rom_mtd_commit_structures(struct mtd_info *mtd,
				 struct mtd_config *cfg,
				 struct mtd_bootblock *bootblock,
				 unsigned long bs_start_address,
				 unsigned int boot_stream_size_in_bytes)
{
	int size;
//	unsigned int search_area_size_in_bytes, stride_size_in_bytes;
	int r;

	//----------------------------------------------------------------------
	// Compute some important facts about geometry.
	//----------------------------------------------------------------------

//	stride_size_in_bytes = PAGES_PER_STRIDE * mtd->writesize;
//	search_area_size_in_bytes = (1 << cfg->search_exponent) * stride_size_in_bytes;

	//----------------------------------------------------------------------
	// Construct the ECC decorations and such for the FCB.
	//----------------------------------------------------------------------

	size = mtd->writesize + mtd->oobsize;

	if (cfg->flags & F_VERBOSE) {
		if (bootblock->ncb_version != cfg->ncb_version)
			printf("NCB versions differ, %d is used.\n", cfg->ncb_version);
	}

	r = fcb_encrypt(&bootblock->fcb, bootblock->buf, size, 1);
	if (r < 0)
		goto _error;;

	//----------------------------------------------------------------------
	// Write the FCB search area.
	//----------------------------------------------------------------------
	mtd_commit_bcb(mtd, cfg, bootblock, "FCB", 0, 0, 0, 1, size);

	//----------------------------------------------------------------------
	// Write the DBBT search area.
	//----------------------------------------------------------------------

	memset(bootblock->buf, 0, size);
	memcpy(bootblock->buf, &(bootblock->dbbt28), sizeof(bootblock->dbbt28));

	mtd_commit_bcb(mtd, cfg, bootblock, "DBBT", 1, 1, 1, 1, mtd->writesize);

	/* Write the firmware copies */
	write_firmware(mtd, cfg, bootblock, bs_start_address,
		       boot_stream_size_in_bytes, 0);

	return 0;

_error:
	return -1;
}

int v6_rom_mtd_commit_structures(struct mtd_info *mtd,
				 struct mtd_config *cfg,
				 struct mtd_bootblock *bootblock,
				 unsigned long bs_start_address,
				 unsigned int boot_stream_size_in_bytes)
{
	int size, r;

	/* [1] Write the FCB search area. */
	size =  mtd->writesize + mtd->oobsize;

	r = fcb_encrypt(&bootblock->fcb, bootblock->buf, size, 3);
	if (r < 0)
		return r;
	mtd_commit_bcb(mtd, cfg, bootblock, "FCB", 0, 0, 0, 1, size);

	/* [2] Write the DBBT search area. */
	memset(bootblock->buf, 0, size);
	memcpy(bootblock->buf, &(bootblock->dbbt28), sizeof(bootblock->dbbt28));

	mtd_commit_bcb(mtd, cfg, bootblock, "DBBT", 1, 1, 1, 1, mtd->writesize);

	/* [3] Write the two boot streams using a 1K padding. */
	return write_firmware(mtd, cfg, bootblock, bs_start_address,
			      boot_stream_size_in_bytes, 1024);
}

int write_bootstream(struct part_info *part,
		     unsigned long bs_start_address,
		     int bs_size)
{
	/* TODO: considering chip = 0 */
	int chip = 0;
	struct mtd_info *mtd = &nand_info[chip];
	int r = -1;
	struct mtd_config cfg;
	struct mtd_bootblock bootblock;

	/* copy defaults */
	memcpy(&cfg, &default_mtd_config, sizeof(cfg));

	/* flags (TODO: parse command arguments?) */
	//cfg.flags |= F_VERBOSE;
	//cfg.flags |= F_DRYRUN;

	/* alloc buffer */
	bootblock.buf = malloc(mtd->writesize + mtd->oobsize);
	if (NULL == bootblock.buf) {
		fprintf(stderr, "mtd: unable to allocate page buffer\n");
		return -1;
	}

	printf("Writing bootstream...");
#if defined(CONFIG_MX28) || defined(CONFIG_MX6UL)
	r = v1_rom_mtd_init(mtd, &cfg, &bootblock, bs_size, part->size);
#endif
	if (r < 0) {
		printf("mtd_init failed!\n");
	}
	else {
#if defined(CONFIG_MX28)
		r = v1_rom_mtd_commit_structures(mtd, &cfg, &bootblock, bs_start_address, bs_size);
#elif defined(CONFIG_MX6UL)
		r = v6_rom_mtd_commit_structures(mtd, &cfg, &bootblock, bs_start_address, bs_size);
#endif
	}

	/* free buffer */
	free(bootblock.buf);
	if (r)
		printf("FAILED\n");
	else
		printf("OK\n");

	return r;
}

#endif	/* CONFIG_CMD_BOOTSTREAM && CONFIG_CMD_NAND */