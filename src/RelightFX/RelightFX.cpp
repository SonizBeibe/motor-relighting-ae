/*
    RelightFX.cpp

    Real-time relight for line art: a cached, OpenCV-based hybrid normal map
    (per-region distance-field relief over the whole silhouette, plus an
    analytic face ellipsoid + nose bump + eye protection when a face is
    detected) shaded against a directional light rig (Light Position ->
    Point of Interest, same convention as AE's own Light layers). See
    JULES_TASK.md for the full pipeline spec.

    NOTE: the Z axis here is a local axis for the light rig, not AE's real
    3D camera space yet -- dragging the points changes the light direction
    consistently, but it isn't camera-matrix-accurate. That's a later step.
*/

#include "RelightFX.h"

#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <delayimp.h>

// Temporary diagnostic logging while tracking down why the render produces
// no visible shading in AE. Remove once resolved.
static std::string GetPluginDirectory(); // fwd decl, defined below

static void DebugLog(const std::string& msg)
{
    std::string path = GetPluginDirectory() + "relightfx_debug.log";
    std::ofstream f(path, std::ios::app);
    if (f)
    {
        f << msg << std::endl;
    }
}

// Resolves to the folder this .aex itself lives in, so auxiliary files (the
// face cascade) can be found regardless of AE's current working directory.
static std::string GetPluginDirectory()
{
    HMODULE hModule = NULL;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&GetPluginDirectory,
            &hModule))
    {
        return std::string();
    }

    char pathBuf[MAX_PATH];
    DWORD len = GetModuleFileNameA(hModule, pathBuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        return std::string();
    }

    std::string path(pathBuf, len);
    size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

// opencv_world*.dll is delay-loaded (see DelayLoadDLLs in RelightFX.vcxproj):
// Windows' default DLL search order for an implicitly-linked dependency looks
// next to the HOST executable (AfterFX.exe), not next to the plugin that
// actually needs it, so a plain implicit link never finds a DLL sitting in
// the Plug-ins folder. This hook runs only if the normal delay-load search
// fails, and resolves the DLL explicitly from this .aex's own folder.
static FARPROC WINAPI DelayLoadFailureHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify == dliFailLoadLib)
    {
        std::string dllPath = GetPluginDirectory() + pdli->szDll;
        HMODULE h = LoadLibraryA(dllPath.c_str());
        return (FARPROC)h;
    }
    return NULL;
}

extern "C" const PfnDliHook __pfnDliFailureHook2 = DelayLoadFailureHook;

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
    // MUTABLE_RENDER_SEQUENCE_DATA_SLOWER: without it, in_data->sequence_data
    // is NULL during PF_Cmd_RENDER when SUPPORTS_THREADED_RENDERING is set,
    // which silently disabled the whole normal-map cache (Render() always
    // fell through to a plain passthrough copy). This trades a small perf
    // cost (sequence_data is per-render-thread and dropped between preview
    // spans) for the cache actually working at all.
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING | PF_OutFlag2_MUTABLE_RENDER_SEQUENCE_DATA_SLOWER;

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

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_ShadowHardness_Param_Name),
        0.0,
        100.0,
        0.0,
        100.0,
        100.0,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        SHADOW_HARDNESS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_HeightBlurRadius_Param_Name),
        0.0,
        100.0,
        0.0,
        50.0,
        20.0,
        PF_Precision_TENTHS,
        0,
        0,
        HEIGHT_BLUR_RADIUS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_HeightCurveExponent_Param_Name),
        0.1,
        2.0,
        0.1,
        1.5,
        0.6,
        PF_Precision_HUNDREDTHS,
        0,
        0,
        HEIGHT_CURVE_EXPONENT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_CoarseDetailStrength_Param_Name),
        0.0,
        500.0,
        0.0,
        200.0,
        100.0,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        COARSE_DETAIL_STRENGTH_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_FineDetailStrength_Param_Name),
        0.0,
        500.0,
        0.0,
        200.0,
        100.0,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        FINE_DETAIL_STRENGTH_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX(STR(StrID_EnableFaceDetection_Param_Name), TRUE, 0, ENABLE_FACE_DETECTION_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(
        STR(StrID_EyeProtectionStrength_Param_Name),
        0.0,
        100.0,
        0.0,
        100.0,
        100.0,
        PF_Precision_TENTHS,
        PF_ValueDisplayFlag_PERCENT,
        0,
        EYE_PROTECTION_STRENGTH_DISK_ID);

    out_data->num_params = RELIGHT_NUM_PARAMS;

    return err;
}

// Runs the full Steps 1-4 pipeline (segmentation, per-region distance
// transform, height shaping, Sobel normal extraction, face-aware
// enhancement). This is the expensive part that gets cached -- see Render().
//
// outSizeP dictates the output dimensions and supplies alpha/silhouette
// (always the actual artwork being shaded); heightSrcP is what's sampled for
// luma -- either that same artwork (heuristic edge-based relief) or a
// separately painted height-map layer (smooth, art-directed relief) when one
// is assigned. Luma coordinates are clamped to heightSrcP's own bounds, so a
// mismatched size just clamps to its edge instead of crashing -- lining the
// two layers up is on the user.
static void ComputeNormalMapWithOpenCV(
    PF_InData* in_data,
    PF_EffectWorld* outSizeP,
    PF_EffectWorld* heightSrcP,
    float blurRadius,
    float heightExponent,
    float coarseStrength,
    float fineStrength,
    float normalStrength,
    bool enableFaceDetection,
    cv::CascadeClassifier* face_cascade,
    std::vector<float>& out_normals,
    std::vector<float>& out_eye_mask)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    const int width = outSizeP->width;
    const int height = outSizeP->height;
    const size_t count = (size_t)width * (size_t)height;

    out_normals.assign(count * 3, 0.0f);
    out_eye_mask.assign(count, 0.0f);

    cv::Mat luma(height, width, CV_32FC1);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const int hx = ClampL(x, 0, heightSrcP->width - 1);
            const int hy = ClampL(y, 0, heightSrcP->height - 1);
            PF_Pixel8* p =
                (PF_Pixel8*)((char*)heightSrcP->data + (size_t)hy * heightSrcP->rowbytes + (size_t)hx * sizeof(PF_Pixel8));
            float l = 0.299f * p->red + 0.587f * p->green + 0.114f * p->blue;
            luma.at<float>(y, x) = l / 255.0f;
        }
    }

    // Step 1: Topological segmentation
    cv::Mat blurred_luma;
    cv::GaussianBlur(luma, blurred_luma, cv::Size(0, 0), 2.0);

    cv::Mat ink_mask(height, width, CV_8UC1);
    cv::Mat silhouette_mask(height, width, CV_8UC1);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            // Silhouette/alpha always comes from the actual artwork being
            // shaded, not the (possibly differently-sized) height map.
            PF_Pixel8* p = (PF_Pixel8*)((char*)outSizeP->data + (size_t)y * outSizeP->rowbytes + (size_t)x * sizeof(PF_Pixel8));
            float b_luma = blurred_luma.at<float>(y, x);

            // True / 0 where there's solid dark ink, False / 255 elsewhere. Combine with alpha.
            if (b_luma < 0.35f && p->alpha > 128)
            {
                ink_mask.at<uchar>(y, x) = 0;
            }
            else if (p->alpha <= 128)
            {
                ink_mask.at<uchar>(y, x) = 0; // Outline is also a wall
            }
            else
            {
                ink_mask.at<uchar>(y, x) = 255;
            }

            if (p->alpha > 128)
            {
                silhouette_mask.at<uchar>(y, x) = 255;
            }
            else
            {
                silhouette_mask.at<uchar>(y, x) = 0;
            }
        }
    }

    // Step 2: Distance field per enclosed region (Fine)
    cv::Mat labels, stats, centroids;
    int num_labels = cv::connectedComponentsWithStats(ink_mask, labels, stats, centroids, 8, CV_32S);

    cv::Mat dist_fine;
    cv::distanceTransform(ink_mask, dist_fine, cv::DIST_L2, 5);

    cv::Mat normalized_dist_fine = cv::Mat::zeros(height, width, CV_32FC1);
    std::vector<float> max_dist_per_label(num_labels, 0.0f);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int label = labels.at<int>(y, x);
            float d = dist_fine.at<float>(y, x);
            if (d > max_dist_per_label[label])
            {
                max_dist_per_label[label] = d;
            }
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int label = labels.at<int>(y, x);
            float max_d = max_dist_per_label[label];
            if (max_d > 0.0f)
            {
                normalized_dist_fine.at<float>(y, x) = std::min(1.0f, dist_fine.at<float>(y, x) / max_d);
            }
        }
    }

    // Coarse silhouette distance transform
    cv::Mat dist_coarse;
    cv::distanceTransform(silhouette_mask, dist_coarse, cv::DIST_L2, 5);
    int num_labels_coarse = cv::connectedComponentsWithStats(silhouette_mask, labels, stats, centroids, 8, CV_32S);

    cv::Mat normalized_dist_coarse = cv::Mat::zeros(height, width, CV_32FC1);
    std::vector<float> max_dist_per_label_coarse(num_labels_coarse, 0.0f);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int label = labels.at<int>(y, x);
            float d = dist_coarse.at<float>(y, x);
            if (d > max_dist_per_label_coarse[label])
            {
                max_dist_per_label_coarse[label] = d;
            }
        }
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int label = labels.at<int>(y, x);
            float max_d = max_dist_per_label_coarse[label];
            if (max_d > 0.0f)
            {
                normalized_dist_coarse.at<float>(y, x) = std::min(1.0f, dist_coarse.at<float>(y, x) / max_d);
            }
        }
    }

    // Step 3: Height shaping
    cv::Mat height_fine = cv::Mat::zeros(height, width, CV_32FC1);
    cv::Mat height_coarse = cv::Mat::zeros(height, width, CV_32FC1);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            height_fine.at<float>(y, x) = std::pow(normalized_dist_fine.at<float>(y, x), heightExponent);
            height_coarse.at<float>(y, x) = std::pow(normalized_dist_coarse.at<float>(y, x), heightExponent);
        }
    }

    cv::Mat blurred_height_fine, blurred_height_coarse;
    cv::GaussianBlur(height_fine, blurred_height_fine, cv::Size(0, 0), blurRadius);
    cv::GaussianBlur(height_coarse, blurred_height_coarse, cv::Size(0, 0), blurRadius);

    // Step 4: Normal extraction (Sobel) + face-aware enhancement
    cv::Mat dx_fine, dy_fine, dx_coarse, dy_coarse;
    cv::Sobel(blurred_height_fine, dx_fine, CV_32F, 1, 0, 3);
    cv::Sobel(blurred_height_fine, dy_fine, CV_32F, 0, 1, 3);
    cv::Sobel(blurred_height_coarse, dx_coarse, CV_32F, 1, 0, 3);
    cv::Sobel(blurred_height_coarse, dy_coarse, CV_32F, 0, 1, 3);

    cv::Mat dx_combined = cv::Mat::zeros(height, width, CV_32FC1);
    cv::Mat dy_combined = cv::Mat::zeros(height, width, CV_32FC1);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            dx_combined.at<float>(y, x) =
                (dx_fine.at<float>(y, x) * fineStrength + dx_coarse.at<float>(y, x) * coarseStrength) * normalStrength;
            dy_combined.at<float>(y, x) =
                (dy_fine.at<float>(y, x) * fineStrength + dy_coarse.at<float>(y, x) * coarseStrength) * normalStrength;
        }
    }

    std::vector<cv::Rect> faces;
    if (enableFaceDetection && face_cascade && !face_cascade->empty())
    {
        cv::Mat luma_8u;
        luma.convertTo(luma_8u, CV_8UC1, 255.0);
        cv::equalizeHist(luma_8u, luma_8u);
        face_cascade->detectMultiScale(luma_8u, faces, 1.02, 2, 0, cv::Size(24, 24));
    }

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            float dx = dx_combined.at<float>(y, x);
            float dy = dy_combined.at<float>(y, x);

            float nx = -dx;
            float ny = -dy;
            float nz = 1.0f;

            // Apply face ellipsoid and nose bump if within a face
            bool inside_face = false;
            float face_nx = 0, face_ny = 0, face_nz = 1.0f;
            float blend_factor = 0.0f;

            for (const auto& face : faces)
            {
                float cx = face.x + face.width * 0.5f;
                float cy = face.y + face.height * 0.5f;
                float rx = face.width * 0.5f;
                float ry = face.height * 0.5f;

                float u = (x - cx) / rx;
                float v = (y - cy) / ry;
                float d_sq = u * u + v * v;

                if (d_sq <= 1.3225f) // Inside 115% of radius
                {
                    inside_face = true;

                    // Nose bump
                    float nose_cx = cx + face.width * 0.02f;
                    float nose_cy = cy + face.height * 0.12f;
                    float nose_rx = face.width * 0.1f;
                    float nose_ry = face.height * 0.1f;

                    float nu = (x - nose_cx) / nose_rx;
                    float nv = (y - nose_cy) / nose_ry;
                    float nd_sq = nu * nu + nv * nv;

                    // Nose bump perturbation, scaled down so it reads as a small
                    // highlight breaking into the shadow rather than dominating
                    // the whole face ellipsoid.
                    const float kNoseStrength = 0.55f;
                    float bump_u = 0, bump_v = 0;
                    if (nd_sq <= 1.0f)
                    {
                        bump_u = nu * kNoseStrength;
                        bump_v = nv * kNoseStrength;
                    }

                    // Main face ellipsoid
                    float w = 0.0f;
                    if (d_sq <= 1.0f)
                    {
                        w = std::sqrt(std::max(0.0f, 1.0f - d_sq));
                    }

                    face_nx = u + bump_u + dx_fine.at<float>(y, x) * fineStrength * 0.1f;
                    face_ny = v + bump_v + dy_fine.at<float>(y, x) * fineStrength * 0.1f;
                    face_nz = w;

                    // Blend factor (75% to 115%)
                    float dist = std::sqrt(d_sq);
                    if (dist < 0.75f)
                    {
                        blend_factor = 1.0f;
                    }
                    else
                    {
                        blend_factor = 1.0f - (dist - 0.75f) / 0.40f;
                    }

                    // Eye falloff
                    float left_eye_cx = cx - face.width * 0.115f;
                    float right_eye_cx = cx + face.width * 0.115f;
                    float eye_cy = cy - face.height * 0.025f;
                    float eye_radius = face.width * 0.1f;

                    float dist_left_eye = std::hypot(x - left_eye_cx, y - eye_cy);
                    float dist_right_eye = std::hypot(x - right_eye_cx, y - eye_cy);

                    if (dist_left_eye < eye_radius)
                    {
                        float f = 1.0f - (dist_left_eye / eye_radius);
                        out_eye_mask[y * width + x] = std::max(out_eye_mask[y * width + x], f);
                    }
                    if (dist_right_eye < eye_radius)
                    {
                        float f = 1.0f - (dist_right_eye / eye_radius);
                        out_eye_mask[y * width + x] = std::max(out_eye_mask[y * width + x], f);
                    }

                    break;
                }
            }

            if (inside_face)
            {
                nx = nx * (1.0f - blend_factor) + face_nx * blend_factor;
                ny = ny * (1.0f - blend_factor) + face_ny * blend_factor;
                nz = nz * (1.0f - blend_factor) + face_nz * blend_factor;
            }

            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len > 1e-6f)
            {
                nx /= len;
                ny /= len;
                nz /= len;
            }

            int idx = (y * width + x) * 3;
            out_normals[idx + 0] = nx;
            out_normals[idx + 1] = ny;
            out_normals[idx + 2] = nz;
        }
    }
}

struct CombineRefcon
{
    const float* normals;
    const float* eye_mask;
    A_long width;
    A_long mode;
    float intensityF;
    float shadow_hardness;
    float eye_protection_strength;
    PF_Pixel8 light_color;
    float Lx, Ly, Lz; // unit vector from the surface toward the light
    bool preview_normals;
};

static float smoothstep(float edge0, float edge1, float x)
{
    float t = ClampF((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static PF_Err CombineFunc8(void* refcon, A_long xL, A_long yL, PF_Pixel8* inP, PF_Pixel8* outP)
{
    CombineRefcon* rc = reinterpret_cast<CombineRefcon*>(refcon);
    if (!rc)
    {
        *outP = *inP;
        return PF_Err_NONE;
    }

    const size_t idx = ((size_t)yL * (size_t)rc->width + (size_t)xL) * 3;
    float nx = rc->normals[idx + 0];
    float ny = rc->normals[idx + 1];
    float nz = rc->normals[idx + 2];

    if (rc->preview_normals)
    {
        const A_u_char gray = (A_u_char)ClampF(nz * 255.0f, 0.0f, 255.0f);
        outP->alpha = inP->alpha;
        outP->red = gray;
        outP->green = gray;
        outP->blue = gray;
        return PF_Err_NONE;
    }

    float dot = nx * rc->Lx + ny * rc->Ly + nz * rc->Lz;
    dot = ClampF(dot, 0.0f, 1.0f);

    float threshold = 0.5f;
    float eps = (100.0f - rc->shadow_hardness) / 100.0f * 0.5f;
    if (eps < 0.001f) eps = 0.001f;

    float shadowFactor = smoothstep(threshold - eps, threshold + eps, dot);
    shadowFactor = ClampF(1.0f - rc->intensityF * (1.0f - shadowFactor), 0.0f, 1.0f);

    float shadedR = inP->red * shadowFactor;
    float shadedG = inP->green * shadowFactor;
    float shadedB = inP->blue * shadowFactor;

    const float highlightAmt = ClampF(dot * rc->intensityF, 0.0f, 1.0f);
    const float lightR = rc->light_color.red * highlightAmt;
    const float lightG = rc->light_color.green * highlightAmt;
    const float lightB = rc->light_color.blue * highlightAmt;

    auto screen = [](float a, float b) { return 255.0f - ((255.0f - a) * (255.0f - b) / 255.0f); };

    float compR = ClampF(screen(shadedR, lightR), 0.0f, 255.0f);
    float compG = ClampF(screen(shadedG, lightG), 0.0f, 255.0f);
    float compB = ClampF(screen(shadedB, lightB), 0.0f, 255.0f);

    float eye_mask_val = rc->eye_mask[yL * rc->width + xL] * rc->eye_protection_strength;
    compR = compR * (1.0f - eye_mask_val) + inP->red * eye_mask_val;
    compG = compG * (1.0f - eye_mask_val) + inP->green * eye_mask_val;
    compB = compB * (1.0f - eye_mask_val) + inP->blue * eye_mask_val;

    // Preserve ink lines. Use Luma to detect ink lines
    float l = 0.299f * inP->red + 0.587f * inP->green + 0.114f * inP->blue;
    if (l < 89.0f && inP->alpha > 128)
    {
        // Dark ink, redraw original pixel
        compR = inP->red;
        compG = inP->green;
        compB = inP->blue;
    }

    outP->alpha = inP->alpha;
    outP->red = (A_u_char)compR;
    outP->green = (A_u_char)compG;
    outP->blue = (A_u_char)compB;

    return PF_Err_NONE;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    DebugLog("=== Render() called ===");

    PF_EffectWorld* srcP = &params[RELIGHT_INPUT]->u.ld;

    if (PF_WORLD_IS_DEEP(output))
    {
        DebugLog("PF_WORLD_IS_DEEP -> early return (16bpc passthrough)");
        // 16bpc path not implemented yet; keep the effect harmless on deep worlds.
        ERR(suites.WorldTransformSuite1()->copy(in_data->effect_ref, srcP, output, NULL, NULL));
        return err;
    }

    float blurRadius = (float)params[RELIGHT_HEIGHT_BLUR_RADIUS]->u.fs_d.value;
    float heightExponent = (float)params[RELIGHT_HEIGHT_CURVE_EXPONENT]->u.fs_d.value;
    float coarseStrength = (float)params[RELIGHT_COARSE_DETAIL_STRENGTH]->u.fs_d.value / 100.0f;
    float fineStrength = (float)params[RELIGHT_FINE_DETAIL_STRENGTH]->u.fs_d.value / 100.0f;
    float normalStrength = (float)params[RELIGHT_NORMAL_STRENGTH]->u.fs_d.value / 100.0f;
    bool enableFaceDetection = params[RELIGHT_ENABLE_FACE_DETECTION]->u.bd.value != 0;

    PF_ParamDef heightmap_checkout;
    AEFX_CLR_STRUCT(heightmap_checkout);
    ERR(PF_CHECKOUT_PARAM(
        in_data, RELIGHT_HEIGHTMAP, in_data->current_time, in_data->time_step, in_data->time_scale, &heightmap_checkout));
    PF_EffectWorld* heightSrcP = (!err && heightmap_checkout.u.ld.data) ? &heightmap_checkout.u.ld : srcP;
    DebugLog(
        "after heightmap checkout: err=" + std::to_string(err) + " srcP=" + std::to_string(srcP->width) + "x" +
        std::to_string(srcP->height) + " heightSrcP==srcP? " + std::to_string(heightSrcP == srcP));

    size_t param_hash = 0;
    // Simple hash to invalidate cache based purely on input buffer + params
    // Current time is intentionally NOT used, so identical art caches properly.
    param_hash ^= std::hash<float>()(blurRadius) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<float>()(heightExponent) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<float>()(coarseStrength) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<float>()(fineStrength) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<float>()(normalStrength) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<bool>()(enableFaceDetection) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);
    param_hash ^= std::hash<bool>()(heightSrcP != srcP) + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);

    // Fast checksum of input pixels (using a stride for performance). Hash
    // whichever buffer actually feeds the height pipeline (the optional
    // height-map layer when assigned, otherwise the artwork itself), so an
    // edit to either one correctly invalidates the cache.
    size_t pixel_hash = 0;
    const int stride = 16;
    for (int y = 0; y < heightSrcP->height; y += stride)
    {
        for (int x = 0; x < heightSrcP->width; x += stride)
        {
            PF_Pixel8* p =
                (PF_Pixel8*)((char*)heightSrcP->data + (size_t)y * heightSrcP->rowbytes + (size_t)x * sizeof(PF_Pixel8));
            size_t val = (p->alpha << 24) | (p->red << 16) | (p->green << 8) | p->blue;
            pixel_hash ^= std::hash<size_t>()(val) + 0x9e3779b9 + (pixel_hash << 6) + (pixel_hash >> 2);
        }
    }
    param_hash ^= pixel_hash + 0x9e3779b9 + (param_hash << 6) + (param_hash >> 2);

    DebugLog("in_data->sequence_data is " + std::string(in_data->sequence_data ? "non-null" : "NULL"));

    RelightSeqData* seq_data = NULL;
    if (in_data->sequence_data)
    {
        seq_data = (RelightSeqData*)suites.HandleSuite1()->host_lock_handle(in_data->sequence_data);
    }

    DebugLog("seq_data is " + std::string(seq_data ? "non-null" : "NULL"));

    if (seq_data)
    {
        DebugLog(
            "seq_data->width=" + std::to_string(seq_data->width) + " height=" + std::to_string(seq_data->height) +
            " param_hash=" + std::to_string(seq_data->param_hash) + " new_hash=" + std::to_string(param_hash) +
            " cache=" + std::string(seq_data->normal_eye_cache ? "set" : "NULL") + " cascade_loaded=" +
            std::to_string(seq_data->cascade_loaded));

        if (seq_data->width != srcP->width || seq_data->height != srcP->height || seq_data->param_hash != param_hash || seq_data->normal_eye_cache == NULL)
        {
            DebugLog("Recomputing normal map cache...");
            if (seq_data->normal_eye_cache)
            {
                suites.HandleSuite1()->host_dispose_handle(seq_data->normal_eye_cache);
                seq_data->normal_eye_cache = NULL;
            }

            std::vector<float> normals;
            std::vector<float> eye_mask;
            ComputeNormalMapWithOpenCV(
                in_data,
                srcP,
                heightSrcP,
                blurRadius,
                heightExponent,
                coarseStrength,
                fineStrength,
                normalStrength,
                enableFaceDetection,
                seq_data->face_cascade,
                normals,
                eye_mask);
            DebugLog(
                "ComputeNormalMapWithOpenCV returned. normals.size()=" + std::to_string(normals.size()) +
                " eye_mask.size()=" + std::to_string(eye_mask.size()));

            size_t bytes = normals.size() * sizeof(float) + eye_mask.size() * sizeof(float);
            DebugLog("Allocating handle of " + std::to_string(bytes) + " bytes...");
            seq_data->normal_eye_cache = suites.HandleSuite1()->host_new_handle(bytes);
            DebugLog(
                std::string("host_new_handle returned ") + (seq_data->normal_eye_cache ? "non-null" : "NULL"));
            if (seq_data->normal_eye_cache)
            {
                float* ptr = (float*)suites.HandleSuite1()->host_lock_handle(seq_data->normal_eye_cache);
                memcpy(ptr, normals.data(), normals.size() * sizeof(float));
                memcpy(ptr + normals.size(), eye_mask.data(), eye_mask.size() * sizeof(float));
                suites.HandleSuite1()->host_unlock_handle(seq_data->normal_eye_cache);
            }

            seq_data->width = srcP->width;
            seq_data->height = srcP->height;
            seq_data->param_hash = param_hash;
        }
        else
        {
            DebugLog("Cache hit, reusing existing normal map.");
        }
    }

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

    float* cache_ptr = NULL;
    if (seq_data && seq_data->normal_eye_cache)
    {
        cache_ptr = (float*)suites.HandleSuite1()->host_lock_handle(seq_data->normal_eye_cache);
    }

    DebugLog(std::string("cache_ptr is ") + (cache_ptr ? "VALID -> will shade" : "NULL -> will passthrough-copy"));

    if (cache_ptr)
    {
        CombineRefcon rc;
        rc.normals = cache_ptr;
        rc.eye_mask = cache_ptr + (size_t)srcP->width * (size_t)srcP->height * 3;
        rc.width = srcP->width;
        rc.mode = params[RELIGHT_MODE]->u.pd.value;
        rc.intensityF = (float)(params[RELIGHT_LIGHT_INTENSITY]->u.fs_d.value / 100.0);
        rc.shadow_hardness = (float)params[RELIGHT_SHADOW_HARDNESS]->u.fs_d.value;
        rc.eye_protection_strength = (float)params[RELIGHT_EYE_PROTECTION_STRENGTH]->u.fs_d.value / 100.0f;
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

        suites.HandleSuite1()->host_unlock_handle(seq_data->normal_eye_cache);
    }
    else
    {
        ERR(suites.WorldTransformSuite1()->copy(in_data->effect_ref, srcP, output, NULL, NULL));
    }

    if (in_data->sequence_data)
    {
        suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
    }

    ERR2(PF_CHECKIN_PARAM(in_data, &heightmap_checkout)); // always check in, even if no layer was assigned

    DebugLog("Render() returning err=" + std::to_string(err));

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

        case PF_Cmd_SEQUENCE_SETUP:
        case PF_Cmd_SEQUENCE_RESETUP:
        {
            DebugLog(std::string("=== ") + (cmd == PF_Cmd_SEQUENCE_SETUP ? "SEQUENCE_SETUP" : "SEQUENCE_RESETUP") + " called ===");
            AEGP_SuiteHandler suites(in_data->pica_basicP);
            PF_Handle seq_handle = suites.HandleSuite1()->host_new_handle(sizeof(RelightSeqData));
            if (seq_handle)
            {
                RelightSeqData* seq_data = (RelightSeqData*)suites.HandleSuite1()->host_lock_handle(seq_handle);
                if (seq_data)
                {
                    seq_data->normal_eye_cache = NULL;
                    seq_data->width = 0;
                    seq_data->height = 0;
                    seq_data->param_hash = 0;
                    seq_data->cascade_loaded = false;
                    seq_data->face_cascade = new cv::CascadeClassifier();

                    std::string cascade_path = GetPluginDirectory() + "lbpcascade_animeface.xml";
                    DebugLog("Loading cascade from: " + cascade_path);
                    if (seq_data->face_cascade->load(cascade_path))
                    {
                        seq_data->cascade_loaded = true;
                    }
                    DebugLog("cascade_loaded=" + std::to_string(seq_data->cascade_loaded));

                    suites.HandleSuite1()->host_unlock_handle(seq_handle);
                    out_data->sequence_data = seq_handle;
                    DebugLog("out_data->sequence_data set successfully.");
                }
                else
                {
                    DebugLog("SEQUENCE_SETUP: host_lock_handle(seq_handle) returned NULL!");
                }
            }
            else
            {
                DebugLog("SEQUENCE_SETUP: host_new_handle(sizeof(RelightSeqData)) returned NULL!");
                err = PF_Err_OUT_OF_MEMORY;
            }
            break;
        }

        case PF_Cmd_SEQUENCE_FLATTEN:
        case PF_Cmd_SEQUENCE_SETDOWN:
        {
            AEGP_SuiteHandler suites(in_data->pica_basicP);
            if (in_data->sequence_data)
            {
                RelightSeqData* seq_data = (RelightSeqData*)suites.HandleSuite1()->host_lock_handle(in_data->sequence_data);
                if (seq_data)
                {
                    if (seq_data->normal_eye_cache)
                    {
                        suites.HandleSuite1()->host_dispose_handle(seq_data->normal_eye_cache);
                        seq_data->normal_eye_cache = NULL;
                    }

                    if (seq_data->face_cascade)
                    {
                        delete seq_data->face_cascade;
                        seq_data->face_cascade = NULL;
                    }

                    suites.HandleSuite1()->host_unlock_handle(in_data->sequence_data);
                }
                suites.HandleSuite1()->host_dispose_handle(in_data->sequence_data);
                out_data->sequence_data = NULL;
            }
            break;
        }

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;
        }
    }
    catch (PF_Err& thrown_err)
    {
        err = thrown_err;
    }
    catch (const std::exception&)
    {
        // Covers cv::Exception (OpenCV) and any other std::exception thrown
        // from the new pipeline -- letting one of these escape uncaught
        // across the plugin boundary is what previously made AE report a
        // generic "couldn't find main entry point" instead of a real error.
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    catch (...)
    {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
