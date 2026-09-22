/*
    RelightFX.cpp

    Heuristic real-time relight: builds a Sobel-based normal map from the
    layer's own luma every frame and shades it against a directional light
    rig (Light Position -> Point of Interest, same convention as AE's own
    Light layers).

    NOTE: the Z axis here is a local axis for the light rig, not AE's real
    3D camera space yet -- dragging the points changes the light direction
    consistently, but it isn't camera-matrix-accurate. That's a later step.
*/

#include "RelightFX.h"

#include <cmath>
#include <vector>

static inline float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline A_long ClampL(A_long v, A_long lo, A_long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg, "%s v%d.%d\r%s", STR(StrID_Name), MAJOR_VERSION, MINOR_VERSION, STR(StrID_Description));

    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);

    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT_3D(STR(StrID_LightPos_Param_Name), 50.0, 50.0, -100.0, LIGHT_POS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT_3D(STR(StrID_LightPoi_Param_Name), 50.0, 50.0, 0.0, LIGHT_POI_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(STR(StrID_LightColor_Param_Name), PF_MAX_CHAN8, PF_MAX_CHAN8, PF_MAX_CHAN8, LIGHT_COLOR_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_LightIntensity_Param_Name),
        RELIGHT_INTENSITY_MIN,
        RELIGHT_INTENSITY_MAX,
        RELIGHT_INTENSITY_MIN,
        100.0,
        RELIGHT_INTENSITY_DFLT,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        LIGHT_INTENSITY_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_NormalStrength_Param_Name),
        RELIGHT_STRENGTH_MIN,
        RELIGHT_STRENGTH_MAX,
        RELIGHT_STRENGTH_MIN,
        300.0,
        RELIGHT_STRENGTH_DFLT,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        NORMAL_STRENGTH_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(
        STR(StrID_Mode_Param_Name), 2, RELIGHT_MODE_ANIME, STR(StrID_Mode_Choices), MODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX(STR(StrID_PreviewNormals_Param_Name), FALSE, 0, PREVIEW_NORMALS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER(STR(StrID_HeightMap_Param_Name), PF_LayerDefault_NONE, HEIGHTMAP_DISK_ID);

    out_data->num_params = RELIGHT_NUM_PARAMS;

    return err;
}

// Single-threaded 3x3 Sobel pass over a height source's luma. Needs neighbor
// pixels, so it reads the world directly instead of going through PF's
// per-pixel iterate suite (which only ever sees one pixel at a time).
//
// outSizeP dictates how big gx/gy are (matches the layer being shaded);
// heightSrcP is what's actually sampled for luma -- either the layer's own
// artwork (heuristic edge-based relief) or a separately painted height-map
// layer (smooth, art-directed relief). Coordinates are clamped to
// heightSrcP's own bounds, so a mismatched size just clamps to its edge
// instead of crashing -- lining the two layers up is on the user.
static void ComputeGradients(PF_EffectWorld* outSizeP, PF_EffectWorld* heightSrcP, std::vector<float>& gx, std::vector<float>& gy)
{
    const A_long width = outSizeP->width;
    const A_long height = outSizeP->height;
    const size_t count = (size_t)width * (size_t)height;

    gx.assign(count, 0.0f);
    gy.assign(count, 0.0f);

    auto luma_at = [&](A_long x, A_long y) -> float {
        x = ClampL(x, 0, heightSrcP->width - 1);
        y = ClampL(y, 0, heightSrcP->height - 1);
        PF_Pixel8* p =
            (PF_Pixel8*)((char*)heightSrcP->data + (size_t)y * heightSrcP->rowbytes + (size_t)x * sizeof(PF_Pixel8));
        return 0.299f * p->red + 0.587f * p->green + 0.114f * p->blue;
    };

    for (A_long y = 0; y < height; ++y)
    {
        for (A_long x = 0; x < width; ++x)
        {
            float tl = luma_at(x - 1, y - 1), tc = luma_at(x, y - 1), tr = luma_at(x + 1, y - 1);
            float ml = luma_at(x - 1, y), mr = luma_at(x + 1, y);
            float bl = luma_at(x - 1, y + 1), bc = luma_at(x, y + 1), br = luma_at(x + 1, y + 1);

            gx[(size_t)y * width + x] = (tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl);
            gy[(size_t)y * width + x] = (bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr);
        }
    }
}

struct CombineRefcon
{
    const float* gx;
    const float* gy;
    A_long width;
    float normal_strength;
    A_long mode;
    float intensityF;
    PF_Pixel8 light_color;
    float Lx, Ly, Lz; // unit vector from the surface toward the light
    bool preview_normals;
};

static PF_Err CombineFunc8(void* refcon, A_long xL, A_long yL, PF_Pixel8* inP, PF_Pixel8* outP)
{
    CombineRefcon* rc = reinterpret_cast<CombineRefcon*>(refcon);
    if (!rc)
    {
        *outP = *inP;
        return PF_Err_NONE;
    }

    const size_t idx = (size_t)yL * (size_t)rc->width + (size_t)xL;
    float gx = rc->gx[idx];
    float gy = rc->gy[idx];

    if (rc->mode == RELIGHT_MODE_ANIME)
    {
        // Soft-clip strong ink edges so thick outlines don't read as huge
        // fake relief; smooth shading regions pass through mostly linearly.
        const float kSoftness = 0.02f;
        gx = gx / (1.0f + fabsf(gx) * kSoftness);
        gy = gy / (1.0f + fabsf(gy) * kSoftness);
    }

    const float k = rc->normal_strength / 255.0f;
    float nx = -gx * k;
    float ny = -gy * k;
    float nz = 1.0f;
    const float invLen = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
    nx *= invLen;
    ny *= invLen;
    nz *= invLen;

    if (rc->preview_normals)
    {
        // Grayscale reading of the normal map: white = flat (nz close to 1),
        // dark = steep slope, i.e. exactly where Sobel found an edge to
        // treat as relief. Easier to sanity-check than tangent-space RGB.
        const A_u_char gray = (A_u_char)ClampF(nz * 255.0f, 0.0f, 255.0f);
        outP->alpha = inP->alpha;
        outP->red = gray;
        outP->green = gray;
        outP->blue = gray;
        return PF_Err_NONE;
    }

    float diffuse = nx * rc->Lx + ny * rc->Ly + nz * rc->Lz;
    diffuse = ClampF(diffuse, 0.0f, 1.0f);

    float shadowFactor = ClampF(1.0f - rc->intensityF * (1.0f - diffuse), 0.0f, 1.0f);

    const float shadedR = inP->red * shadowFactor;
    const float shadedG = inP->green * shadowFactor;
    const float shadedB = inP->blue * shadowFactor;

    const float highlightAmt = ClampF(diffuse * rc->intensityF, 0.0f, 1.0f);
    const float lightR = rc->light_color.red * highlightAmt;
    const float lightG = rc->light_color.green * highlightAmt;
    const float lightB = rc->light_color.blue * highlightAmt;

    auto screen = [](float a, float b) { return 255.0f - ((255.0f - a) * (255.0f - b) / 255.0f); };

    outP->alpha = inP->alpha;
    outP->red = (A_u_char)ClampF(screen(shadedR, lightR), 0.0f, 255.0f);
    outP->green = (A_u_char)ClampF(screen(shadedG, lightG), 0.0f, 255.0f);
    outP->blue = (A_u_char)ClampF(screen(shadedB, lightB), 0.0f, 255.0f);

    return PF_Err_NONE;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    PF_EffectWorld* srcP = &params[RELIGHT_INPUT]->u.ld;

    if (PF_WORLD_IS_DEEP(output))
    {
        // 16bpc path not implemented yet; keep the effect harmless on deep worlds.
        ERR(suites.WorldTransformSuite1()->copy(in_data->effect_ref, srcP, output, NULL, NULL));
        return err;
    }

    PF_ParamDef heightmap_checkout;
    AEFX_CLR_STRUCT(heightmap_checkout);
    ERR(PF_CHECKOUT_PARAM(
        in_data, RELIGHT_HEIGHTMAP, in_data->current_time, in_data->time_step, in_data->time_scale, &heightmap_checkout));

    PF_EffectWorld* heightSrcP = (!err && heightmap_checkout.u.ld.data) ? &heightmap_checkout.u.ld : srcP;

    std::vector<float> gx, gy;
    ComputeGradients(srcP, heightSrcP, gx, gy);

    ERR2(PF_CHECKIN_PARAM(in_data, &heightmap_checkout)); // always check in, even if no layer was assigned

    const PF_FpLong lx = params[RELIGHT_LIGHT_POS]->u.point3d_d.x_value;
    const PF_FpLong ly = params[RELIGHT_LIGHT_POS]->u.point3d_d.y_value;
    const PF_FpLong lz = params[RELIGHT_LIGHT_POS]->u.point3d_d.z_value;

    const PF_FpLong tx = params[RELIGHT_LIGHT_POI]->u.point3d_d.x_value;
    const PF_FpLong ty = params[RELIGHT_LIGHT_POI]->u.point3d_d.y_value;
    const PF_FpLong tz = params[RELIGHT_LIGHT_POI]->u.point3d_d.z_value;

    PF_FpLong dx = tx - lx, dy = ty - ly, dz = tz - lz;
    PF_FpLong dlen = sqrt(dx * dx + dy * dy + dz * dz);
    if (dlen < 1e-6)
    {
        dx = 0.0;
        dy = 0.0;
        dz = 1.0;
        dlen = 1.0;
    }
    dx /= dlen;
    dy /= dlen;
    dz /= dlen;

    CombineRefcon rc;
    rc.gx = gx.data();
    rc.gy = gy.data();
    rc.width = srcP->width;
    rc.normal_strength = (float)params[RELIGHT_NORMAL_STRENGTH]->u.fs_d.value;
    rc.mode = params[RELIGHT_MODE]->u.pd.value;
    rc.intensityF = (float)(params[RELIGHT_LIGHT_INTENSITY]->u.fs_d.value / 100.0);
    rc.light_color = params[RELIGHT_LIGHT_COLOR]->u.cd.value;
    rc.Lx = (float)-dx;
    rc.Ly = (float)-dy;
    rc.Lz = (float)-dz;
    rc.preview_normals = params[RELIGHT_PREVIEW_NORMALS]->u.bd.value != 0;

    const A_long linesL = output->extent_hint.bottom - output->extent_hint.top;

    ERR(suites.Iterate8Suite2()->iterate(
        in_data,
        0,
        linesL,
        srcP,
        NULL,
        (void*)&rc,
        CombineFunc8,
        output));

    return err;
}

extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "RelightFX",
        "ADBE RelightFX",
        "Anime Relight",
        AE_RESERVED_INFO,
        "EffectMain",
        "https://www.adobe.com");

    return result;
}

PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;

    try
    {
        switch (cmd)
        {
        case PF_Cmd_ABOUT:
            err = About(in_data, out_data, params, output);
            break;

        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;
        }
    }
    catch (PF_Err& thrown_err)
    {
        err = thrown_err;
    }
    return err;
}
