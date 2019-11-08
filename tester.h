#ifndef _TESTER_H_
#define _TESTER_H_

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <phase1.h>

static char *states[] = {"Free", "Run", "Ready", "Quit", "Block", "Join"};

static char *errors[] = {
    "Success",
    "Too many processes/contexts",
    "Invalid stack",
    "Invalid priority",
    "Invalid tag",
    "Process has no children w/ matching tag",
    "No children w/ matching tag have quit",
    "Too many semaphores",
    "Name is NULL",
    "Duplicate name",
    "Invalid SID",
    "Processes are blocked on the semaphore",
    "Invalid PID/CID",
    "Invalid state",
    "Invalid device type",
    "Invalid device unit",
    "Wait aborted",
    "Child has quit",
    "Name is too long",
    "Context is in-use"
};

static int numCodes = sizeof(errors) / sizeof(char *);

static char *
ErrorCodeToString(int code)
{
    code = -code;
    if ((code < 0) || (code >= numCodes)) {
        return "Invalid error code";
    } else {
        return errors[code];
    }
}




static void
DumpProcesses(void)
{
    USLOSS_Console("%20s %3s %8s %3s %3s %8s %3s %3s %s\n", "Name", "PID", "State", "Pri", "Tag", "CPU", "SID", "Par", "Children");
    for (int i = 0; i < P1_MAXPROC; i++) {
        P1_ProcInfo info;
        memset(&info, '\0', sizeof(info));
        int rc = P1_GetProcInfo(i, &info);
        if ((rc == P1_SUCCESS) && (info.state != P1_STATE_FREE)) {
            USLOSS_Console("%20s %3d %8s %3d %3d %8d %3d %3d ", info.name, i, states[info.state], info.priority, info.tag, info.cpu, info.sid, info.parent);
            for (int j = 0; j < info.numChildren; j++) {
              USLOSS_Console("%d ", info.children[j]);
            }
            USLOSS_Console("\n");
        }
    }
}

static char *
MakeName(char *prefix, int suffix)
{
    static char name[P1_MAXNAME+1];
    snprintf(name, sizeof(name), "%s%d", prefix, suffix);
    return name;
}

static void
DeleteDisk(int disk) 
{
    char path[30];
    snprintf(path, sizeof(path), "disk%d", disk);
    unlink(path);
}

static void
DeleteAllDisks(void)
{
    for (int i = 0; i < USLOSS_DISK_UNITS; i++) {
        DeleteDisk(i);
    }
}

#define PASSED() { \
    passed = TRUE; \
}

#define FAILED(val, expected) { \
    USLOSS_Console("%s:%d: %d != %d.\n", __FUNCTION__, __LINE__, (val), (expected)); \
    USLOSS_Console("TEST FAILED.\n"); \
    passed = FALSE; \
    USLOSS_Halt(0); \
}

#define TEST(val, expected) { \
    if ((val) != (expected)) { \
        FAILED((val), (expected)); \
    } \
}

#endif

