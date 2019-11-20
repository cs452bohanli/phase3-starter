/*
 * phase3b.c
 *
 */

#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <usloss.h>
#include <string.h>
#include <libuser.h>

#include "phase3Int.h"

void
P3PageFaultHandler(int type, void *arg)
{
    /*******************

    if the cause is USLOSS_MMU_FAULT (USLOSS_MmuGetCause)
        if the process does not have a page table  (P3PageTableGet)
            print error message
            USLOSS_Halt(1)
        else
            determine which page suffered the fault (USLOSS_MmuPageSize)
            update the page's PTE to map page x to frame x
            set the PTE to be read-write and incore
            update the page table in the MMU (USLOSS_MmuSetPageTable)
    else
        print error message
        USLOSS_Halt(1)
    *********************/
	int rc;
	if (USLOSS_MmuGetCause() == USLOSS_MMU_FAULT) {
		USLOSS_PTE *table;
		rc = P3PageTableGet(P1_GetPid(), &table);
		if (rc != P1_SUCCESS || table == NULL) {
			USLOSS_Console("Error: process has no page table\n");
			USLOSS_Halt(1);
		}
		else {
			int page = ((int) arg) / USLOSS_MmuPageSize();
			table[page].incore = table[page].read = table[page].write = 1;
			table[page].frame = page;
			assert(USLOSS_MmuSetPageTable(table) == USLOSS_MMU_OK);
		}
	}
	else {
		USLOSS_Console("Error: page fault not caused my mmu fault\n");
		USLOSS_Halt(1);
	}
}

USLOSS_PTE *
P3PageTableAllocateEmpty(int pages)
{
    USLOSS_PTE  *table = (USLOSS_PTE*) malloc(pages*sizeof(USLOSS_PTE));
	if (table) {
		for (int i = 0; i < pages; i++){
			table[i].incore = 0;
		}
	}
    // allocate and initialize an empty page table
    return table;
}
