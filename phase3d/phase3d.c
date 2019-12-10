/*
 * phase3d.c
 *
 */

/***************

NOTES ON SYNCHRONIZATION

There are various shared resources that require proper synchronization. 

Swap space. Free swap space is a shared resource, we don't want multiple pagers choosing the
same free space to hold a page. You'll need a mutex around the free swap space.

The clock hand is also a shared resource.

The frames are a shared resource in that we don't want multiple pagers to choose the same frame via
the clock algorithm. That's the purpose of marking a frame as "busy" in the pseudo-code below. 
Pagers ignore busy frames when running the clock algorithm.

A process's page table is a shared resource with the pager. The process changes its page table
when it quits, and a pager changes the page table when it selects one of the process's pages
in the clock algorithm. 

Normally the pagers would perform I/O concurrently, which means they would release the mutex
while performing disk I/O. I made it simpler by having the pagers hold the mutex while they perform
disk I/O.

***************/


#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <usloss.h>
#include <string.h>
#include <libuser.h>

#include "phase3.h"
#include "phase3Int.h"

#ifdef DEBUG
static int debugging3 = 1;
#else
static int debugging3 = 0;
#endif

static void debug3(char *fmt, ...)
{
    va_list ap;

    if (debugging3) {
        va_start(ap, fmt);
        USLOSS_VConsole(fmt, ap);
    }
}

int initialized = FALSE;
int numPages;
int numFrames;

// disk data
int sectorSize; // bytes
int sectorsInTrack;
int tracksInDisk;

// semaphores
int mutex;

// helper functions for semaphores, makes code cleaner
void P(int sid) {
	assert(P1_P(sid) == P1_SUCCESS);
}

void V(int sid) {
	assert(P1_V(sid) == P1_SUCCESS);
}

typedef struct f {
	int busy;
	int track, sector, onDisk; // disk properties
} Frame;

Frame *allFrames;
int maxFramesOnDisk;

typedef struct p {
	int pid, page;
} DiskPage;
DiskPage *pagesOnDisk;

/*
 *----------------------------------------------------------------------
 *
 * P3SwapInit --
 *
 *  Initializes the swap data structures.
 *
 * Results:
 *   P3_ALREADY_INITIALIZED:    this function has already been called
 *   P1_SUCCESS:                success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapInit(int pages, int frames)
{
    int result = P1_SUCCESS;
		if (initialized) return P3_ALREADY_INITIALIZED;
    // initialize the swap data structures, e.g. the pool of free blocks
		numPages = pages;
		numFrames = frames;
		initialized = TRUE;
		assert(P2_DiskSize(P3_SWAP_DISK, &sectorSize, &sectorsInTrack, &tracksInDisk) == P1_SUCCESS);
		allFrames = (Frame*) malloc(numFrames*sizeof(Frame));
		for (int i = 0; i < numFrames; i++) {
			allFrames[i].busy = FALSE;
			allFrames[i].onDisk = FALSE;
		}
		result = P1_SemCreate("mutex", 1, &mutex);
		assert(result == P1_SUCCESS);
	maxFramesOnDisk = tracksInDisk*sectorsInTrack*sectorSize/USLOSS_MmuPageSize();
	pagesOnDisk = (DiskPage*) malloc(maxFramesOnDisk*sizeof(DiskPage));
	for (int i = 0; i < maxFramesOnDisk; i++) {
		pagesOnDisk[i].page = -1;
		pagesOnDisk[i].pid = -1;
	}
    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * P3SwapShutdown --
 *
 *  Cleans up the swap data structures.
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapShutdown(void)
{
    int result = P1_SUCCESS;
		if (!initialized) return P3_NOT_INITIALIZED;
    // clean things up
		free(allFrames);
		result = P1_SemFree(mutex);
		assert(result == P1_SUCCESS);
	free(pagesOnDisk);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3SwapFreeAll --
 *
 *  Frees all swap space used by a process
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */

int
P3SwapFreeAll(int pid)
{
    int result = P1_SUCCESS;
		if (!initialized) return P3_NOT_INITIALIZED;

    /*****************

    P(mutex)
    free all swap space used by the process
    V(mutex)

    *****************/
	P(mutex);
	USLOSS_PTE *table;
	result = P3PageTableGet(pid, &table);
	if (table) {
		for (int i = 0; i < numPages; i++) {
			if (table[i].incore) {
				table[i].incore = 0;
				allFrames[table[i].frame].busy = FALSE;
				for (int j = 0; j < maxFramesOnDisk; j++) {
					if (pagesOnDisk[j].pid == pid && pagesOnDisk[j].page == i) {
						pagesOnDisk[j].pid = -1;
						pagesOnDisk[j].page = -1;
					}
				}
			}
		}
	}
	V(mutex);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * P3SwapOut --
 *
 * Uses the clock algorithm to select a frame to replace, writing the page that is in the frame out 
 * to swap if it is dirty. The page table of the page’s process is modified so that the page no 
 * longer maps to the frame. The frame that was selected is returned in *frame. 
 *
 * Results:
 *   P3_NOT_INITIALIZED:    P3SwapInit has not been called
 *   P1_SUCCESS:            success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapOut(int *frame) 
{
    int result = P1_SUCCESS;
		if (!initialized) return P3_NOT_INITIALIZED;

    /*****************

    NOTE: in the pseudo-code below I used the notation frames[x] to indicate frame x. You 
    may or may not have an actual array with this name. As with all my pseudo-code feel free
    to ignore it.


    static int hand = -1;    // start with frame 0
    P(mutex)
    loop
        hand = (hand + 1) % # of frames
        if frames[hand] is not busy
            if frames[hand] hasn't been referenced (USLOSS_MmuGetAccess)
                target = hand
                break
            else
                clear reference bit (USLOSS_MmuSetAccess)
    if frame[target] is dirty (USLOSS_MmuGetAccess)
        write page to its location on the swap disk (P3FrameMap,P2_DiskWrite,P3FrameUnmap)
        clear dirty bit (USLOSS_MmuSetAccess)
    update page table of process to indicate page is no longer in a frame
    mark frames[target] as busy
    V(mutex)
    *frame = target

    *****************/
	static int hand = -1;
	int accessed;
	int writeIndex = -1;
	P(mutex);
	while (1) {
		hand = (hand + 1) % numFrames;
		if (!allFrames[hand].busy) {
			result = USLOSS_MmuGetAccess(hand, &accessed);
			assert(result == USLOSS_MMU_OK);
			if (accessed & 1) {
				*frame = hand;
				break;
			}
			else {
				result = USLOSS_MmuSetAccess(hand, accessed & -2);
				assert(result == USLOSS_MMU_OK);
			}
		}
	}
	result = USLOSS_MmuGetAccess(*frame, &accessed);
	assert(result == USLOSS_MMU_OK);
	if ((accessed >> 1) & 1) {
		void *page;
		result = P3FrameMap(*frame, &page);
		assert(result == P1_SUCCESS);
		int i;
		for (i = 0; i < maxFramesOnDisk; i++) {
			if (pagesOnDisk[i].pid == -1) break;
		}
		int track = USLOSS_MmuPageSize()*i/(sectorsInTrack*sectorSize);
		int first = (USLOSS_MmuPageSize()*i - track*sectorsInTrack*sectorSize)/sectorSize;
		int sectors = USLOSS_MmuPageSize()/sectorSize;
		USLOSS_Console("attempting write %x track %d first %d sectors %d\n", page, track, first, sectors);
		char *tmpBuffer = (char*) malloc(USLOSS_MmuPageSize()*sizeof(char));
		for (int i = 0; i < USLOSS_MmuPageSize(); i++) tmpBuffer[i] = ((char*) page)[i];
		result = P2_DiskWrite(P3_SWAP_DISK, track, first, sectors, tmpBuffer);
		free(tmpBuffer);
		if (result != P1_SUCCESS) {
			USLOSS_Console("write failed\n");
			V(mutex);
			return P3_OUT_OF_SWAP;
		}
		writeIndex = i;
		USLOSS_Console("written %d\n", writeIndex);

		result = P3FrameUnmap(*frame);
		assert(result == P1_SUCCESS);
		result = USLOSS_MmuSetAccess(*frame, accessed & (~(2)));
		assert(result == USLOSS_MMU_OK);
	}
	USLOSS_PTE *table;
	USLOSS_Console("pid %d\n", P1_GetPid());
	result = P3PageTableGet(P1_GetPid(), &table);
	assert(result == P1_SUCCESS);
	if (table) {
		for (int i = 0; i < numPages; i++) {
			USLOSS_Console("%d %d\n", table[i].incore, table[i].frame);
			if (table[i].incore && table[i].frame == *frame) {
				table[i].incore = 0;
				if (writeIndex != -1) {
					USLOSS_Console("INSIDE HERE\n");
					pagesOnDisk[writeIndex].pid = P1_GetPid();
					pagesOnDisk[writeIndex].page = i;
				}
				break;
			}
		}
	}

	USLOSS_Console("frame chosen %d\n", *frame);
	allFrames[*frame].busy = TRUE;
	V(mutex);
    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * P3SwapIn --
 *
 *  Opposite of P3FrameMap. The frame is unmapped.
 *
 * Results:
 *   P3_NOT_INITIALIZED:     P3SwapInit has not been called
 *   P1_INVALID_PID:         pid is invalid      
 *   P1_INVALID_PAGE:        page is invalid         
 *   P1_INVALID_FRAME:       frame is invalid
 *   P3_EMPTY_PAGE:          page is not in swap
 *   P1_OUT_OF_SWAP:         there is no more swap space
 *   P1_SUCCESS:             success
 *
 *----------------------------------------------------------------------
 */
int
P3SwapIn(int pid, int page, int frame)
{
    int result = P1_SUCCESS;
		if (!initialized) return P3_NOT_INITIALIZED;
	if (pid < 0 || pid >= P1_MAXPROC) return P1_INVALID_PID;
	if (page < 0 || page >= numPages) return P3_INVALID_PAGE;
	if (frame < 0 || frame >= numFrames) return P3_INVALID_FRAME;

    /*****************

    P(mutex)
    if page is on swap disk
        read page from swap disk into frame (P3FrameMap,P2_DiskRead,P3FrameUnmap)
    else
        allocate space for the page on the swap disk
        if no more space
            result = P3_OUT_OF_SWAP
        else
            result = P3_EMPTY_PAGE
    mark frame as not busy
    V(mutex)

    *****************/
	P(mutex);
	int isSpace = FALSE, diskIndex = -1;
	for (int i = 0; i < maxFramesOnDisk; i++) {
		USLOSS_Console("status %d\n", pagesOnDisk[i].page);
		if (pagesOnDisk[i].pid == pid && pagesOnDisk[i].page == page) diskIndex = i;
		else if (pagesOnDisk[i].pid == -1) isSpace = TRUE;
	}
	if (diskIndex != -1) {
		void *addr;
		result = P3FrameMap(frame, &addr);
		assert(result == P1_SUCCESS);

		int track = USLOSS_MmuPageSize()*diskIndex/(sectorsInTrack*sectorSize);		
		int first = (USLOSS_MmuPageSize()*diskIndex - track*sectorsInTrack*sectorSize)/sectorSize;
		int sectors = USLOSS_MmuPageSize()/sectorSize;
		char *tmpBuffer = (char*) malloc(USLOSS_MmuPageSize()*sizeof(char));
		for (int i = 0; i < USLOSS_MmuPageSize(); i++) tmpBuffer[i] = ((char*) addr)[i];
		USLOSS_Console("addr %x contents %d %d\n", addr, *((char*) addr), *((char*) (addr+1)));
		result = P2_DiskRead(P3_SWAP_DISK, track, first, sectors, tmpBuffer);
		free(tmpBuffer);
		if (result != P1_SUCCESS) {
			USLOSS_Console("read failed %d\n", result);
			USLOSS_Console("track %d first %d sectors %d page %d\n", track, first, sectors, page);
			V(mutex);
			return P3_OUT_OF_SWAP;
		}
		result = P3FrameUnmap(frame);
		assert(result == P1_SUCCESS);
	} else {
		if (!isSpace) result = P3_OUT_OF_SWAP;
		else result = P3_EMPTY_PAGE;
	}
	allFrames[frame].busy = FALSE;
	V(mutex);
    return result;
}
