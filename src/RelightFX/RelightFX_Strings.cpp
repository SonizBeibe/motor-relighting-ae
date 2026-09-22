#include "RelightFX.h"

typedef struct
{
    A_u_long index;
    A_char str[256];
} TableString;

TableString g_strs[StrID_NUMTYPES] = {
    StrID_NONE,
    "",
    StrID_Name,
    "RelightFX",
    StrID_Description,
    "Real-time normal-map relight for line-art layers.\rCopyright 2026.",
    StrID_LightPos_Param_Name,
    "Light Position",
    StrID_LightPoi_Param_Name,
    "Point of Interest",
    StrID_LightColor_Param_Name,
    "Light Color",
    StrID_LightIntensity_Param_Name,
    "Intensity",
    StrID_NormalStrength_Param_Name,
    "Normal Strength",
    StrID_Mode_Param_Name,
    "Mode",
    StrID_Mode_Choices,
    "Anime|Fotorealismo",
    StrID_PreviewNormals_Param_Name,
    "Preview Normal Map",
    StrID_HeightMap_Param_Name,
    "Height Map (optional)",
};

char* GetStringPtr(int strNum)
{
    return g_strs[strNum].str;
}
