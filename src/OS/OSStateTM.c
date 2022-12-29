#include "OSStateTM.h"
#include "OS.h"
#include "OSCache.h"
#include "OSError.h"
#include "OSGlobals.h"
#include "OSInterrupt.h"

#include <IPC/ipcclt.h>

static u8 StmEhInBuf[32] ALIGN(32);
static u8 StmEhOutBuf[32] ALIGN(32);

static u8 StmImInBuf[32] ALIGN(32);
static u8 StmImOutBuf[32] ALIGN(32);

static u8 StmVdInBuf[32] ALIGN(32);
static u8 StmVdOutBuf[32] ALIGN(32);

static OSStateCallback ResetCallback;
static OSStateCallback PowerCallback;

static BOOL StmVdInUse;

static BOOL StmEhRegistered;
static BOOL StmEhDesc;

static BOOL StmImDesc;

static BOOL StmReady;
static BOOL ResetDown;

static s32 AccessVIDimRegs(void);
static s32 __OSVIDimReplyHandler(IPCResult result, void* arg1);
static void __OSRegisterStateEvent(void);
static void __OSDefaultResetCallback(void);
static void __OSDefaultPowerCallback(void);
static s32 __OSStateEventHandler(IPCResult, void*);
static void LockUp(void);

OSStateCallback OSSetResetCallback(OSStateCallback callback) {
    BOOL enabled;
    OSStateCallback old;

    enabled = OSDisableInterrupts();

    old = ResetCallback;
    ResetCallback = callback;

    if (!StmEhRegistered) {
        __OSRegisterStateEvent();
    }

    OSRestoreInterrupts(enabled);
    return old;
}

OSStateCallback OSSetPowerCallback(OSStateCallback callback) {
    BOOL enabled;
    OSStateCallback old;

    enabled = OSDisableInterrupts();

    old = PowerCallback;
    PowerCallback = callback;

    if (!StmEhRegistered) {
        __OSRegisterStateEvent();
    }

    OSRestoreInterrupts(enabled);
    return old;
}

BOOL __OSInitSTM(void) {
    BOOL success;

    PowerCallback = __OSDefaultPowerCallback;
    ResetCallback = __OSDefaultResetCallback;
    ResetDown = FALSE;

    if (StmReady) {
        return TRUE;
    }

    StmVdInUse = FALSE;
    StmImDesc = IOS_Open("/dev/stm/immediate", IPC_OPEN_NONE);
    if (StmImDesc < 0) {
        StmReady = FALSE;
        success = FALSE;
        goto exit;
    }

    StmEhDesc = IOS_Open("/dev/stm/eventhook", IPC_OPEN_NONE);
    if (StmEhDesc < 0) {
        StmReady = FALSE;
        success = FALSE;
        goto exit;
    }

    __OSRegisterStateEvent();
    StmReady = TRUE;
    success = TRUE;

exit:
    return success;
}

void __OSShutdownToSBY(void) {
#define in_args ((u32*)StmImInBuf)

    OS_VI_DCR = 0;

#line 275
    OSAssert(StmReady,
             "Error: The firmware doesn't support shutdown feature.\n");

    in_args[0] = 0;
    IOS_Ioctl(StmImDesc, IPC_IOCTL_STM_SHUTDOWN_TO_SBY, StmImInBuf,
              sizeof(StmImInBuf), StmImOutBuf, sizeof(StmImOutBuf));
    LockUp();

#undef in_args
}

void __OSHotReset(void) {
    OS_VI_DCR = 0;

#line 340
    OSAssert(StmReady, "Error: The firmware doesn't support reboot feature.\n");

    IOS_Ioctl(StmImDesc, IPC_IOCTL_STM_HOT_RESET, StmImInBuf,
              sizeof(StmImInBuf), StmImOutBuf, sizeof(StmImOutBuf));
    LockUp();
}

BOOL __OSGetResetButtonStateRaw(void) {
    return (!(OS_PI_INTSR & 0x10000)) ? TRUE : FALSE;
}

s32 __OSSetVIForceDimming(u32 arg0, u32 arg1, u32 arg2) {
#define in_args ((u32*)StmVdInBuf)

    BOOL enabled;

    if (!StmReady) {
        return -10;
    }

    enabled = OSDisableInterrupts();

    if (StmVdInUse) {
        OSRestoreInterrupts(enabled);
        return 0;
    }

    StmVdInUse = TRUE;
    OSRestoreInterrupts(enabled);

    // Screen brightness
    in_args[0] = arg1 << 3 | arg2 | arg0 << 7;

    in_args[1] = 0;
    in_args[2] = 0;
    in_args[3] = 0;
    in_args[4] = 0;

    in_args[5] = 0xFFFFFFFF;
    in_args[6] = 0xFFFF0000;

    in_args[7] = 0;

    return AccessVIDimRegs();

#undef in_args
}

s32 __OSSetIdleLEDMode(u32 mode) {
#define in_args ((u32*)StmImInBuf)

    if (!StmReady) {
        return -6;
    }

    in_args[0] = mode;

    return IOS_Ioctl(StmImDesc, IPC_IOCTL_STM_SET_IDLE_LED_MODE, StmImInBuf,
                     sizeof(StmImInBuf), StmImOutBuf, sizeof(StmImOutBuf));

#undef in_args
}

s32 __OSUnRegisterStateEvent(void) {
    IPCResult result;

    if (!StmEhRegistered) {
        return 0;
    }

    if (!StmReady) {
        return -6;
    }

    result = IOS_Ioctl(StmImDesc, IPC_IOCTL_STM_UNREG_EVENT, StmImInBuf,
                       sizeof(StmImInBuf), StmImOutBuf, sizeof(StmImOutBuf));
    if (result == IPC_RESULT_OK) {
        StmEhRegistered = FALSE;
    }

    return result;
}

static s32 AccessVIDimRegs(void) {
    const IPCResult result = IOS_IoctlAsync(
        StmImDesc, IPC_IOCTL_STM_SET_VI_DIM, StmVdInBuf, sizeof(StmVdInBuf),
        StmVdOutBuf, sizeof(StmVdOutBuf), __OSVIDimReplyHandler, NULL);
    return result != IPC_RESULT_OK ? result : 1;
}

static s32 __OSVIDimReplyHandler(IPCResult result, void* arg) {
#pragma unused(result)
#pragma unused(arg)

    StmVdInUse = FALSE;
    return 0;
}

static void __OSRegisterStateEvent(void) {
    BOOL enabled = OSDisableInterrupts();

    if (IOS_IoctlAsync(StmEhDesc, IPC_IOCTL_STM_REG_EVENT, StmEhInBuf,
                       sizeof(StmEhInBuf), StmEhOutBuf, sizeof(StmEhOutBuf),
                       __OSStateEventHandler, NULL) == IPC_RESULT_OK) {
        StmEhRegistered = TRUE;
    } else {
        StmEhRegistered = FALSE;
    }

    OSRestoreInterrupts(enabled);
}

static void __OSDefaultResetCallback(void) {}

static void __OSDefaultPowerCallback(void) {}

static s32 __OSStateEventHandler(IPCResult result, void* arg) {
#pragma unused(result)
#pragma unused(arg)

#define out_args ((u32*)StmEhOutBuf)

    BOOL enabled;
    OSStateCallback callback;

#line 748
    OSAssert(result == IPC_RESULT_OK, "Error on STM state event handler\n");

    StmEhRegistered = FALSE;

    if (out_args[0] == 0x20000) {
        if (__OSGetResetButtonStateRaw()) {
            enabled = OSDisableInterrupts();

            callback = ResetCallback;
            ResetDown = TRUE;
            ResetCallback = __OSDefaultResetCallback;
            callback();

            OSRestoreInterrupts(enabled);
        }
        __OSRegisterStateEvent();
    }

    if (out_args[0] == 0x800) {
        enabled = OSDisableInterrupts();

        callback = PowerCallback;
        PowerCallback = __OSDefaultPowerCallback;
        callback();

        OSRestoreInterrupts(enabled);
    }

    return 0;

#undef out_args
}

static void LockUp(void) {
    OSDisableInterrupts();
    ICFlashInvalidate();

    while (TRUE) {
        ;
    }
}