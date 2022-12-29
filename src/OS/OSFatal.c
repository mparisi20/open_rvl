#include "OSFatal.h"
#include "OS.h"
#include "OSAddress.h"
#include "OSArena.h"
#include "OSAudioSystem.h"
#include "OSCache.h"
#include "OSContext.h"
#include "OSError.h"
#include "OSFont.h"
#include "OSInterrupt.h"
#include "OSReset.h"
#include "OSThread.h"
#include "OSTime.h"

#include <BASE/PPCArch.h>
#include <EXI/EXIBios.h>
#include <GX/GXFrameBuf.h>
#include <GX/GXMisc.h>
#include <VI/vi.h>

#include <string.h>

#define CLAMP_F32(min, max, x) ((x > max) ? max : (x < min ? min : x))

/**
 * Framebuffer formatted as 32-bits per two pixels
 * Pixels are written as brightness/chroma pairs
 */
#define FATAL_FB_W 640
#define FATAL_FB_H 480
#define FATAL_FB_SIZE (FATAL_FB_W * FATAL_FB_H * 2)

typedef struct OSFatalParam {
    GXColor textColor; // at 0x0
    GXColor bgColor;   // at 0x4
    const char* msg;   // at 0x8
} OSFatalParam;

static OSContext FatalContext;
static OSFatalParam FatalParam;

static void Halt(void);

static void ScreenClear(u8* fb, u16 width, u16 height, GXColor yuv) {
    int x, y;

    for (y = 0; y < height; y += 1) {
        for (x = 0; x < width; x += 2) {
            fb[0] = yuv.r;
            fb[1] = yuv.g;

            fb[2] = yuv.r;
            fb[3] = yuv.b;

            fb += sizeof(u32);
        }
    }
}

static void ScreenReport(void* fb, u16 width, u16 height, GXColor msgYUV, s32 x,
                         s32 y, u32 lf, const char* msg) {
    u32 texel[70];
    u32 charWidth;
    u8* tmp;
    u8 bright;
    s32 xPos;
    u32 idx;
    u32 idx2;
    u32 idx3;
    u32 i;
    u32 j;

new_line:
    if (height - 24 < y) {
        return;
    }

    tmp = (u8*)fb + ((x + (y * width)) * 2);
    xPos = x;

    while (*msg != '\0') {
        if (*msg == '\n') {
            msg++;
            y += lf;
            goto new_line;
        } else if (width - 48 < xPos) {
            y += lf;
            goto new_line;
        }

        for (i = 0; i < 24; i++) {
            idx = (i & 7) + ((i / 8) * 24);
            texel[idx] = 0;
            texel[idx + 8] = 0;
            texel[idx + 16] = 0;
        }

        msg = OSGetFontTexel(msg, texel, 0, 6, &charWidth);

        for (i = 0; i < 24; i++) {
            idx = (i & 7) + ((i / 8) * 24);

            for (j = 0; j < 24; j++) {
                idx2 = idx + ((j / 8) * 8);
                bright = (texel[idx2] >> ((7 - (j & 7)) * 4)) & 0x0F;

                if (bright != 0) {
                    // Y component/brightness
                    bright = (((msgYUV.r * (bright * 0xEF)) / 255) / 15) + 16;
                    idx3 = (j + (width * i));
                    tmp[idx3 * 2] = bright;

                    // Order chroma depending on pixel pair
                    if ((xPos + j) % 2 != 0) {
                        tmp[(idx3 * 2) - 1] = msgYUV.g;
                        tmp[(idx3 * 2) + 1] = msgYUV.b;
                    } else {
                        tmp[(idx3 * 2) - 1] = msgYUV.b;
                        tmp[(idx3 * 2) + 1] = msgYUV.g;
                    }
                }
            }
        }

        tmp += charWidth * 2;
        xPos += charWidth;
    }
}

static void ConfigureVideo(u16 width, u16 height) {
    GXRenderModeObj rmode;

    rmode.fbWidth = width;
    rmode.efbHeight = FATAL_FB_H;
    rmode.xfbHeight = height;

    rmode.viXOrigin = 40;
    rmode.viWidth = FATAL_FB_W;
    rmode.viHeight = height;

    switch (VIGetTvFormat()) {
    case VI_TV_FMT_NTSC:
    case VI_TV_FMT_MPAL:
        if (OS_VI_VICLK & 1) {
            // Progressive mode
            rmode.tvInfo = GX_RM_TV_INFO(VI_TV_FMT_NTSC, GX_TV_MODE_PROG);
            rmode.viYOrigin = 0;
            rmode.xfbMode = GX_XFB_MODE_SF;
        } else {
            // Non-progressive mode
            rmode.tvInfo = GX_RM_TV_INFO(VI_TV_FMT_NTSC, GX_TV_MODE_INT);
            rmode.viYOrigin = 0;
            rmode.xfbMode = GX_XFB_MODE_DF;
        }
        break;
    case VI_TV_FMT_EURGB60:
        rmode.tvInfo = GX_RM_TV_INFO(VI_TV_FMT_EURGB60, GX_TV_MODE_INT);
        rmode.viYOrigin = 0;
        rmode.xfbMode = GX_XFB_MODE_DF;
        break;
    case VI_TV_FMT_PAL:
        rmode.tvInfo = GX_RM_TV_INFO(VI_TV_FMT_PAL, GX_TV_MODE_INT);
        rmode.viYOrigin = 47;
        rmode.xfbMode = GX_XFB_MODE_DF;
        break;
    }

    VIConfigure(&rmode);
    VIConfigurePan(0, 0, FATAL_FB_W, FATAL_FB_H);
}

// Intel IPP RGBToYCbCr algorithm (+0.5f?)
static GXColor RGB2YUV(GXColor rgb) {
    GXColor yuv;

    const f32 y =
        (0.257f * rgb.r + 0.504f * rgb.g + 0.098f * rgb.b + 16.0f) + 0.5f;
    const f32 cb =
        (-0.148f * rgb.r - 0.291f * rgb.g + 0.439f * rgb.b + 128.0f) + 0.5f;
    const f32 cr =
        (0.439f * rgb.r - 0.368f * rgb.g - 0.071f * rgb.b + 128.0f) + 0.5f;

    yuv.r = CLAMP_F32(16.0f, 235.0f, y);
    yuv.g = CLAMP_F32(16.0f, 240.0f, cb);
    yuv.b = CLAMP_F32(16.0f, 240.0f, cr);
    yuv.a = 0;

    return yuv;
}

void OSFatal(GXColor textColor, GXColor bgColor, const char* msg) {
    s32 retraceCount;
    s64 start;
    OSBootInfo* bootInfo;

    bootInfo = (OSBootInfo*)OSPhysicalToCached(OS_PHYS_BOOT_INFO);

    OSDisableInterrupts();

    OSDisableScheduler();
    OSClearContext(&FatalContext);
    OSSetCurrentContext(&FatalContext);
    __OSStopAudioSystem();

    VIInit();
    __OSUnmaskInterrupts(0x80);
    VISetBlack(TRUE);
    VIFlush();
    VISetPreRetraceCallback(NULL);
    VISetPostRetraceCallback(NULL);

    OSEnableInterrupts();

    retraceCount = VIGetRetraceCount();
    while (VIGetRetraceCount() - retraceCount < 1) {
        ;
    }

    start = OSGetTime();
    do {
        if (__OSCallShutdownFunctions(0, 0)) {
            break;
        }
    } while (OSGetTime() - start < OS_MSEC_TO_TICKS(1000));

    OSDisableInterrupts();
    __OSCallShutdownFunctions(1, 0);

    EXISetExiCallback(EXI_CHAN_0, NULL);
    EXISetExiCallback(EXI_CHAN_2, NULL);
    while (!EXILock(EXI_CHAN_0, 1, NULL)) {
        EXISync(EXI_CHAN_0);
        EXIDeselect(EXI_CHAN_0);
        EXIUnlock(EXI_CHAN_0);
    }
    EXIUnlock(EXI_CHAN_0);
    while ((EXI_CD006800[EXI_CHAN_0].WORD_0xC & 1) == 1) {
        ;
    }

    __OSSetExceptionHandler(OS_ERR_DECREMENTER, OSDefaultExceptionHandler);

    GXAbortFrame();

    OSSetArenaLo((void*)0x81400000);
    OSSetArenaHi(bootInfo->fstStart);

    FatalParam.textColor = textColor;
    FatalParam.bgColor = bgColor;
    FatalParam.msg = msg;
    OSSwitchFiber(Halt, OSGetArenaHi());
}

static void Halt(void) {
    OSFontData* msgFont;
    const char* msg;
    s32 retraceCount;
    size_t msgLen;
    void* msgBuf;
    void* fb;
    OSFatalParam* params;

    OSEnableInterrupts();

    params = &FatalParam;
    msg = params->msg;

    msgLen = strlen(msg) + 1;
    msgBuf = OSAllocFromMEM1ArenaLo(msgLen, 32);
    params->msg = memmove(msgBuf, msg, msgLen);

    msgFont = (OSFontData*)OSAllocFromMEM1ArenaLo(0xA1004, 32);
    OSLoadFont(msgFont, OSGetArenaLo());

    fb = OSAllocFromMEM1ArenaLo(FATAL_FB_SIZE, 32);
    ScreenClear(fb, FATAL_FB_W, FATAL_FB_H, RGB2YUV(params->bgColor));
    VISetNextFrameBuffer(fb);
    ConfigureVideo(FATAL_FB_W, FATAL_FB_H);
    VIFlush();

    retraceCount = VIGetRetraceCount();
    while (VIGetRetraceCount() - retraceCount < 2) {
        ;
    }

    ScreenReport(fb, FATAL_FB_W, FATAL_FB_H, RGB2YUV(params->textColor), 48,
                 100, msgFont->lineFeed, params->msg);
    DCFlushRange(fb, FATAL_FB_SIZE);
    VISetBlack(FALSE);
    VIFlush();

    retraceCount = VIGetRetraceCount();
    while (VIGetRetraceCount() - retraceCount < 1) {
        ;
    }

    OSDisableInterrupts();
    OSReport("%s\n", params->msg);
    PPCHalt();
}