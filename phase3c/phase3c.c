/*
 * phase3c.c
 *
 */

#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <usloss.h>
#include <string.h>
#include <libuser.h>

#include "phase3.h"
#include "phase3Int.h"

#ifdef DEBUG
int debugging3 = 1;
#else
int debugging3 = 0;
#endif

static int Pager(void*);
void debug3(char *fmt, ...)
{
    va_list ap;

    if (debugging3) {
        va_start(ap, fmt);
        USLOSS_VConsole(fmt, ap);
    }
}

/*
 * Checks psr to make sure OS is in kernel mode, halting USLOSS if not. Mode bit
 * is the LSB.
 */
void checkIfIsKernel(){ 
    if ((USLOSS_PsrGet() & 1) != 1) {
        USLOSS_Console("The OS must be in kernel mode!\n");
        USLOSS_IllegalInstruction();
    }
}

// helper functions for semaphores, makes code cleaner
void P(int sid) {
	assert(P1_P(sid) == P1_SUCCESS);
}

void V(int sid) {
	assert(P1_V(sid) == P1_SUCCESS);
}

int frameInitialized = FALSE;
int pageInitialized = FALSE;
int numFrames;
int numPages;

typedef struct f {
	int used;
	USLOSS_PTE *page;
} Frame;

Frame *frameTable;

/*
 *----------------------------------------------------------------------
 *
 * P3FrameInit --
 *
 *  Initializes the frame data structures.
 *
 * Results:
 *   P3_ALREADY_INITIALIZED:    this function has already been called
 *   P1_SUCCESS:                success
 *
 *----------------------------------------------------------------------
 */
int
P3FrameInit(int pages, int frames)
{
	checkIfIsKernel();
    int result = P1_SUCCESS;
	if (frameInitialized) return P3_ALREADY_INITIALIZED;
    // initialize the frame data structures, e.g. the pool of free frames
	numFrames = frames;
	numPages = pages;
	frameTable = (Frame*) malloc(numFrames*sizeof(Frame));
	for (int i = 0; i < numFrames; i++) {
		frameTable[i].used = FALSE;
		frameTable[i].page = NULL;
	}
    // set P3_vmStats.freeFrames
	P3_vmStats.freeFrames = frames;
	
	frameInitialized = TRUE;
    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * P3FrameShutdown --
 *
 *  Cleans up the frame data structures.
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3FrameInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3FrameShutdown(void)
{
	checkIfIsKernel();
	if (!frameInitialized) return P3_NOT_INITIALIZED;
    int result = P1_SUCCESS;

    // clean things up
	free(frameTable);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3FrameFreeAll --
 *
 *  Frees all frames used by a process
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3FrameInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */

int
P3FrameFreeAll(int pid)
{
	checkIfIsKernel();
	if (!frameInitialized) return P3_NOT_INITIALIZED;

    int result = P1_SUCCESS;
    // free all frames in use by the process (P3PageTableGet)
	USLOSS_PTE *table;
	result = P3PageTableGet(pid, &table);
	if (result != P1_SUCCESS) return result;
	if (table) {
		for (int i = 0; i < numPages; i++) {
			if (table[i].incore) {
				table[i].incore = 0;
				frameTable[table[i].frame].used = FALSE;
			}
		}
	}
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3FrameMap --
 *
 *  Maps a frame to an unused page and returns a pointer to it.
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3FrameInit has not been called
 *   P3_OUT_OF_PAGES:       process has no free pages
 *   P1_INVALID_FRAME       the frame number is invalid
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3FrameMap(int frame, void **ptr) 
{
	checkIfIsKernel();
	if (!frameInitialized) return P3_NOT_INITIALIZED;
	if (frame < 0 || frame >= numFrames) return P3_INVALID_FRAME;

    int result = P1_SUCCESS;
	
    // get the page table for the process (P3PageTableGet)
	USLOSS_PTE *table;
    result = P3PageTableGet(P1_GetPid(), &table);
	assert(table != NULL);

	// find an unused page
	int i;
	for (i = 0; i < numPages; i++) {
		if (!table[i].incore) break;
	}
	if (i == numPages) return P3_OUT_OF_PAGES;

    // update the page's PTE to map the page to the frame
	table[i].incore = 1;
	table[i].read = 1;
	table[i].write = 1;
	table[i].frame = frame;
	frameTable[frame].used = TRUE;
	frameTable[frame].page = table + i;
	int np;
	*ptr = USLOSS_MmuRegion(&np) + i*USLOSS_MmuPageSize();
    // update the page table in the MMU (USLOSS_MmuSetPageTable)
	result = USLOSS_MmuSetPageTable(table);
	assert(result == USLOSS_MMU_OK);
    return P1_SUCCESS;
}
/*
 *----------------------------------------------------------------------
 *
 * P3FrameUnmap --
 *
 *  Opposite of P3FrameMap. The frame is unmapped.
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3FrameInit has not been called
 *   P3_FRAME_NOT_MAPPED:   process didn’t map frame via P3FrameMap
 *   P1_INVALID_FRAME       the frame number is invalid
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3FrameUnmap(int frame) 
{
	checkIfIsKernel();

	if (!frameInitialized) return P3_NOT_INITIALIZED;
	if (frame < 0 || frame >= numFrames) return P3_INVALID_FRAME;
	if (!frameTable[frame].used) return P3_FRAME_NOT_MAPPED;

	int result = P1_SUCCESS;

	// get the page table for the process (P3PageTableGet)
	USLOSS_PTE *table;
    result = P3PageTableGet(P1_GetPid(), &table);
	assert(table != NULL);
    // verify that the process mapped the frame
	int i;
	for (i = 0; i < numPages; i++) {
		if (table[i].incore && table[i].frame == frame) break;
	}
	if (i == numPages) return P3_FRAME_NOT_MAPPED;

    // update page's PTE to remove the mapping
	table[i].incore = 0;
	frameTable[frame].used = FALSE;
    // update the page table in the MMU (USLOSS_MmuSetPageTable)
	result = USLOSS_MmuSetPageTable(table);
	assert(result == USLOSS_MMU_OK);
 
    return result;
}

// information about a fault. Add to this as necessary.

typedef struct Fault {
    PID         pid;
    int         offset;
    int         cause;
    SID         wait;
    // other stuff goes here
} Fault;

int queueSize = 500;
Fault queue[500];
int queueStart;
int queueEnd;

int numPagers;

// semaphores
int *pagerIsRunning;
int faultHappened;
int faultWaits[500];
/*
 *----------------------------------------------------------------------
 *
 * FaultHandler --
 *
 *  Page fault interrupt handler
 *
 *----------------------------------------------------------------------
 */

static void
FaultHandler(int type, void *arg)
{
	  // fill in other fields in fault
    // add to queue of pending faults
		int thisIndex = queueEnd;
		queueEnd = (queueEnd + 1) % queueSize;
    queue[thisIndex].offset = (int) arg;
		queue[thisIndex].pid = P1_GetPid();
		queue[thisIndex].cause = USLOSS_MmuGetCause();
		// let pagers know there is a pending fault
		V(faultHappened);
		
    // wait for fault to be handled
		P(queue[thisIndex].wait);
}

/*
 *----------------------------------------------------------------------
 *
 * P3PagerInit --6
 *
 *  Initializes the pagers.
 *
 * Results:
 *   P3_ALREADY_INITIALIZED: this function has already been called
 *   P3_INVALID_NUM_PAGERS:  the number of pagers is invalid
 *   P1_SUCCESS:             success
 *
 *----------------------------------------------------------------------
 */
int
P3PagerInit(int pages, int frames, int pagers)
{
	checkIfIsKernel();
	if (pageInitialized) return P3_ALREADY_INITIALIZED;
    int     result = P1_SUCCESS;

    USLOSS_IntVec[USLOSS_MMU_INT] = FaultHandler;
	
	if (pagers <= 0 || pagers > P3_MAX_PAGERS) return P3_INVALID_NUM_PAGERS;

    // initialize the pager data structures
	numPagers = pagers;
	queueStart = 0;
	queueEnd = 0;
	pagerIsRunning = (int*) malloc(numPagers*sizeof(int));
	for (int i = 0; i < numPagers; i++) {
		char name[5];
		sprintf(name, "%d.", i);
		result = P1_SemCreate(name, 0, pagerIsRunning + i);
		assert(result == P1_SUCCESS);
	}
	result = P1_SemCreate("fault", 0, &faultHappened);
	assert(result == P1_SUCCESS);
	for (int i = 0; i < queueSize; i++) {
		char name[5];
		sprintf(name, "%d*", i);
		result = P1_SemCreate(name, 0, faultWaits + i);
		assert(result == P1_SUCCESS);
	}
    // fork off the pagers and wait for them to start running
	for (int i = 0; i < numPagers; i++) {
		char name[5];
		sprintf(name, "%d", i);
		int pid;
		result = P1_Fork(name, Pager, (void*) i, USLOSS_MIN_STACK, P3_PAGER_PRIORITY, 0, &pid);
		assert(result == P1_SUCCESS);
		P(pagerIsRunning[i]);
	}
	pageInitialized = TRUE;
	return result;
}

int pagerShutdown = FALSE;

/*
 *----------------------------------------------------------------------
 *
 * P3PagerShutdown --
 *
 *  Kills the pagers and cleans up.
 *
 * Results:
 *   P3_NOT_INITIALIZED:     P3PagerInit has not been called
 *   P1_SUCCESS:             success
 *
 *----------------------------------------------------------------------
 */
int
P3PagerShutdown(void)
{
	checkIfIsKernel();
    int result = P1_SUCCESS;
	
    // cause the pagers to quit
	pagerShutdown = TRUE;
    // clean up the pager data structures
	for (int i = 0; i < numPagers; i++) assert(P1_SemFree(pagerIsRunning[i]) == P1_SUCCESS);
	for (int i = 0; i < numPagers; i++) V(faultHappened);
	result = P1_SemFree(faultHappened);
	assert(result == P1_SUCCESS);
	for (int i = 0; i < queueSize; i++) assert(P1_SemFree(faultWaits[i]) == P1_SUCCESS);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Pager --
 *
 *  Handles page faults
 *
 *----------------------------------------------------------------------
 */

static int
Pager(void *arg)
{

    //notify P3PagerInit that we are running
	V(pagerIsRunning[(int) arg]);
	/*
    loop until P3PagerShutdown is called
        wait for a fault
        if it's an access fault kill the faulting process
        if there are free frames
            frame = a free frame
        else
            P3SwapOut(&frame);
        rc = P3SwapIn(pid, page, frame)
        if rc == P3_EMPTY_PAGE
            P3FrameMap(frame, &addr)
            zero-out frame at addr
            P3FrameUnmap(frame);
        else if rc == P3_OUT_OF_SWAP
            kill the faulting process
        update PTE in faulting process's page table to map page to frame
        unblock faulting process

    **********************************/
	int rc;
	while (!pagerShutdown) {
		P(faultHappened);
		if (pagerShutdown) break;
		Fault fault = queue[queueStart];
		queueStart = (queueStart + 1) % queueSize;
		if (fault.cause == USLOSS_MMU_ACCESS) {
			P2_Terminate(0);
		}
		int frame;
		for (frame = 0; frame < numFrames; frame++) {
			if (!frameTable[frame].used) break;
		}
		if (frame == numFrames) assert(P3SwapOut(&frame) == P1_SUCCESS);
		int page = fault.offset/USLOSS_MmuPageSize();
		rc = P3SwapIn(fault.pid, page, frame);
		
		void *addr;
		if (rc == P3_EMPTY_PAGE) {
			rc = P3FrameMap(frame, &addr);
			assert(rc == P1_SUCCESS);
			for (int i = 0; i < USLOSS_MmuPageSize(); i++) {
					*((char*)addr + i) = 0;
			}
			rc = P3FrameUnmap(frame);
			assert(rc == P1_SUCCESS);

		} else if (rc == P3_OUT_OF_SWAP) {
			P2_Terminate(0);
			continue;
		}
		// get the page table for the process (P3PageTableGet)
		USLOSS_PTE *table;
    	rc = P3PageTableGet(fault.pid, &table);
		assert(table != NULL);
		frameTable[frame].used = TRUE;
		table[page].incore = 1;
		table[page].read = 1;
		table[page].write = 1;
		table[page].frame = frame;
		V(fault.wait);
	}
    return 0;
}
