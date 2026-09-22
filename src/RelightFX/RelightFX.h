/*
    RelightFX.h
*/

#pragma once

#ifndef RELIGHTFX_H
    #define RELIGHTFX_H

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned short u_int16;
typedef unsigned long u_long;
typedef short int int16;
    #define PF_TABLE_BITS 12
    #define PF_TABLE_SZ_16 4096

    #define PF_DEEP_COLOR_AWARE 1

    #include "AEConfig.h"

    #ifdef AE_OS_WIN
typedef unsigned short PixelType;
        #include <Windows.h>
    #endif

    #include "entry.h"
    #include "AE_Effect.h"
    #include "AE_EffectCB.h"
    #include "AE_Macros.h"
    #include "Param_Utils.h"
    #include "AE_EffectCBSuites.h"
    #include "String_Utils.h"
    #include "AE_GeneralPlug.h"
    #include "AEFX_ChannelDepthTpl.h"
    #include "AEGP_SuiteHandler.h"

    #include "RelightFX_Strings.h"

/* Versioning information */

    #define MAJOR_VERSION 1
    #define MINOR_VERSION 0
    #define BUG_VERSION 0
    #define STAGE_VERSION PF_Stage_DEVELOP
    #define BUILD_VERSION 1

/* Parameter ranges / defaults */

    #define RELIGHT_INTENSITY_MIN 0.0
    #define RELIGHT_INTENSITY_MAX 300.0
    #define RELIGHT_INTENSITY_DFLT 100.0

    #define RELIGHT_STRENGTH_MIN 0.0
    #define RELIGHT_STRENGTH_MAX 500.0
    #define RELIGHT_STRENGTH_DFLT 150.0

enum
{
    RELIGHT_MODE_ANIME = 1,
    RELIGHT_MODE_PHOTOREAL = 2
};

enum
{
    RELIGHT_INPUT = 0,
    RELIGHT_LIGHT_POS,
    RELIGHT_LIGHT_POI,
    RELIGHT_LIGHT_COLOR,
    RELIGHT_LIGHT_INTENSITY,
    RELIGHT_NORMAL_STRENGTH,
    RELIGHT_MODE,
    RELIGHT_PREVIEW_NORMALS,
    RELIGHT_HEIGHTMAP,
    RELIGHT_NUM_PARAMS
};

enum
{
    LIGHT_POS_DISK_ID = 1,
    LIGHT_POI_DISK_ID,
    LIGHT_COLOR_DISK_ID,
    LIGHT_INTENSITY_DISK_ID,
    NORMAL_STRENGTH_DISK_ID,
    MODE_DISK_ID,
    PREVIEW_NORMALS_DISK_ID,
    HEIGHTMAP_DISK_ID,
};

extern "C"
{

    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif // RELIGHTFX_H
