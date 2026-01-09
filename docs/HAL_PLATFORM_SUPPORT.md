# HAL Platform Support Matrix

This document provides a comprehensive overview of Hardware Abstraction Layer (HAL) function support across all Ingenic SoC platforms, including the specific SDK functions used on each platform.

## Platform Overview

| Platform | SDK Version(s) | Notes                           |
|----------|----------------|---------------------------------|
| T10      | 3.9.0, 3.12.0  | Entry-level SoC                 |
| T20      | 3.9.0, 3.12.0  |                                 |
| T21      | 1.0.33         |                                 |
| T23      | 1.1.6          |                                 |
| T30      | 1.0.5          |                                 |
| T31      | 1.1.6          | Popular mid-range SoC           |
| C100     | 1.1.6          |                                 |
| T40      | 1.2.0          | XBurst2, Kernel 4.x             |
| T41      | 1.0.1, 1.1.0, 1.1.1, 1.2.0 | XBurst2, Kernel 4.x |
| A1       | Various        | Present in headers but not actively supported in HAL |

## Image Quality Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_brightness** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetBrightness(unsigned char)`<br>T40/T41: `IMP_ISP_Tuning_SetBrightness(IMPVI_NUM, unsigned char *)` |
| **get_brightness** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetBrightness(unsigned char *)`<br>T40/T41: `IMP_ISP_Tuning_GetBrightness(IMPVI_NUM, unsigned char *)` |
| **set_contrast** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetContrast(unsigned char)`<br>T40/T41: `IMP_ISP_Tuning_SetContrast(IMPVI_NUM, unsigned char *)` |
| **get_contrast** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetContrast(unsigned char *)`<br>T40/T41: `IMP_ISP_Tuning_GetContrast(IMPVI_NUM, unsigned char *)` |
| **set_saturation** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetSaturation(unsigned char)`<br>T40/T41: `IMP_ISP_Tuning_SetSaturation(IMPVI_NUM, unsigned char *)` |
| **get_saturation** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetSaturation(unsigned char *)`<br>T40/T41: `IMP_ISP_Tuning_GetSaturation(IMPVI_NUM, unsigned char *)` |
| **set_sharpness** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetSharpness(unsigned char)`<br>T40/T41: `IMP_ISP_Tuning_SetSharpness(IMPVI_NUM, unsigned char *)` |
| **get_sharpness** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetSharpness(unsigned char *)`<br>T40/T41: `IMP_ISP_Tuning_GetSharpness(IMPVI_NUM, unsigned char *)` |
| **set_hue** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✓* | ✓* | T23/T31/C100: `IMP_ISP_Tuning_SetHue(unsigned char)`<br>T40/T41: `IMP_ISP_Tuning_SetHue(IMPVI_NUM, unsigned char *)` |
| **get_hue** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✓* | ✓* | T23/T31/C100: `IMP_ISP_Tuning_GetHue(unsigned char *)`<br>T40/T41: `IMP_ISP_Tuning_GetHue(IMPVI_NUM, unsigned char *)` |

## Denoise Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_sinter_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetSinterStrength(unsigned char)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_SetSinterDnsAttr(IMPISPSinterDenoiseAttr *)` |
| **get_sinter_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetSinterStrength(unsigned char *)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_GetSinterDnsAttr(IMPISPSinterDenoiseAttr *)` |
| **set_temper_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetTemperStrength(unsigned char)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_SetTemperDnsAttr(IMPISPTemperDenoiseAttr *)` |
| **get_temper_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetTemperStrength(unsigned char *)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_GetTemperDnsAttr(IMPISPTemperDenoiseAttr *)` |
| **set_dpc_strength** | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | T31/C100: `IMP_ISP_Tuning_SetDPC_Strength(unsigned char)` |
| **get_dpc_strength** | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | T31/C100: `IMP_ISP_Tuning_GetDPC_Strength(unsigned char *)` |
| **set_drc_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetDRC_Strength(unsigned char)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_SetRawDRC(IMPISPDrcAttr *)` |
| **get_drc_strength** | ✓† | ✓† | ✓† | ✓ | ✓† | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetDRC_Strength(unsigned char *)`<br>T10/T20/T21/T30: `IMP_ISP_Tuning_GetRawDRC(IMPISPDrcAttr *)` |
| **set_defog_strength** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetDefog_Strength(uint8_t)` |

## Image Enhancement Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_backlight_comp** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetBacklightComp(unsigned char)` |
| **set_highlight_depress** | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T21/T23/T30/T31/C100: `IMP_ISP_Tuning_SetHiLightDepress(unsigned char)` |

## Flip/Mirror Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_hflip** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓‡ | ✓‡ | T10-T31/C100: `IMP_ISP_Tuning_SetISPHflip(IMPISPTuningOpsMode)`<br>T40: Combined via helper using `IMP_ISP_Tuning_SetISPHVFLIP(IMPVI_NUM, IMPISPHVFLIP)`<br>T41: Combined via helper using `IMP_ISP_Tuning_SetFLIP(IMPVI_NUM, IMPISPFLIP)` |
| **set_vflip** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓‡ | ✓‡ | T10-T31/C100: `IMP_ISP_Tuning_SetISPVflip(IMPISPTuningOpsMode)`<br>T40/T41: Combined HVFLIP API (see above) |

## Running Mode & ISP Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_running_mode** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetISPRunningMode(IMPISPRunningMode)`<br>T40/T41: `IMP_ISP_Tuning_SetISPRunningMode(IMPVI_NUM, IMPISPRunningMode)` |
| **get_running_mode** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetISPRunningMode(IMPISPRunningMode *)`<br>T40/T41: `IMP_ISP_Tuning_GetISPRunningMode(IMPVI_NUM, IMPISPRunningMode *)` |
| **set_isp_bypass** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓** | T10-T31/C100: `IMP_ISP_Tuning_SetISPBypass(IMPISPTuningOpsMode)`<br>T40: `IMP_ISP_Tuning_SetISPBypass(IMPVI_NUM, IMPISPTuningOpsMode *)`<br>T41: `IMP_ISP_SetISPBypass(IMPVI_NUM, IMPISPTuningOpsMode *)` |
| **set_anti_flicker** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_SetAntiFlickerAttr(IMPISPAntiflickerAttr)`<br>T40/T41: `IMP_ISP_Tuning_SetAntiFlickerAttr(IMPVI_NUM, IMPISPAntiflickerAttr *)` |
| **get_anti_flicker** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_GetAntiFlickerAttr(IMPISPAntiflickerAttr *)`<br>T40/T41: `IMP_ISP_Tuning_GetAntiFlickerAttr(IMPVI_NUM, IMPISPAntiflickerAttr *)` |

## Auto Exposure (AE) Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_ae_compensation** | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10/T20/T23/T30/T31/C100: `IMP_ISP_Tuning_SetAeComp(int)` |
| **get_ae_compensation** | ✓ | ✓ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10/T20/T23/T30/T31/C100: `IMP_ISP_Tuning_GetAeComp(int *)` |
| **set_ae_it_max** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetAe_IT_MAX(unsigned int)` |
| **get_ae_it_max** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetAE_IT_MAX(unsigned int *)` |
| **set_ae_min** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_SetAeMin(unsigned int, unsigned int, unsigned int, unsigned int)` |
| **get_ae_min** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetAeMin(IMPISPAEMin *)` |
| **get_ae_attr** | ✗ | ✗ | ✗ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T23/T31/C100: `IMP_ISP_Tuning_GetAeAttr(IMPISPAEAttr *)` |
| **get_ae_luma** | ✗ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | T21/T23/T31/C100: `IMP_ISP_Tuning_GetAeLuma(int *)` |

## Gain Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_max_again** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_SetMaxAgain(unsigned char)` |
| **get_max_again** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetMaxAgain(unsigned char *)` |
| **set_max_dgain** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_SetMaxDgain(unsigned char)` |
| **get_max_dgain** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetMaxDgain(unsigned char *)` |
| **get_total_gain** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetTotalGain(uint32_t *)`<br>T40/T41: `IMP_ISP_Tuning_GetAeExprInfo(IMPVI_NUM, IMPISPAEExprInfo *)` → `TotalGainDb` |

## Exposure Value (EV) Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **get_ev** | ✗ | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ | ✓* | ✓* | T20/T23/T31/C100: `IMP_ISP_Tuning_GetEVAttr(IMPISPEVAttr *)` → `ev`<br>T40/T41: `IMP_ISP_Tuning_GetAeExprInfo(IMPVI_NUM, IMPISPAEExprInfo *)` → `ExposureValue` |
| **get_ev_attr** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetEVAttr(IMPISPEVAttr *)` |

## Gamma Curve Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_gamma** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_SetGamma(IMPISPGamma *)`<br>T40/T41: `IMP_ISP_Tuning_SetGammaAttr(IMPVI_NUM, IMPISPGammaAttr *)` with `IMP_ISP_GAMMA_CURVE_USER` |
| **get_gamma** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_GetGamma(IMPISPGamma *)`<br>T40/T41: `IMP_ISP_Tuning_GetGammaAttr(IMPVI_NUM, IMPISPGammaAttr *)` |

## White Balance (AWB) Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_wb** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_SetWB(IMPISPWB *)` |
| **set_awb_weight** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_SetAwbWeight(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_SetAwbWeight(IMPVI_NUM, IMPISPWeight *)` |
| **get_awb_weight** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T10-T31/C100: `IMP_ISP_Tuning_GetAwbWeight(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_GetAwbWeight(IMPVI_NUM, IMPISPWeight *)` |
| **get_awb_zone** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetAwbZone(IMPISPAWBZone *)` |
| **get_awb_weighted_gains** | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓* | ✓* | T20-T31/C100: `IMP_ISP_Tuning_GetAwbHist(IMPISPAWBHist *)` → `r_gain`/`b_gain`<br>T40/T41: `IMP_ISP_Tuning_GetAwbGlobalStatistics(IMPVI_NUM, IMPISPAWBGlobalStatisInfo *)` → `rgain`/`bgain` |
| **get_awb_color_temp** | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✗ | ✗ | T31/C100: `IMP_ISP_Tuning_GetAWBCt(unsigned int *)` |

## Auto Exposure (AE) Zone Weight and ROI Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_ae_weight** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_SetAeWeight(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_SetAeWeight(IMPVI_NUM, IMPISPAEWeightAttr *)` with `weight_enable=ENABLE` |
| **get_ae_weight** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_GetAeWeight(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_GetAeWeight(IMPVI_NUM, IMPISPAEWeightAttr *)` |
| **set_ae_roi** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_AE_SetROI(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_SetAeWeight(IMPVI_NUM, IMPISPAEWeightAttr *)` with `roi_enable=ENABLE` |
| **get_ae_roi** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_AE_GetROI(IMPISPWeight *)`<br>T40/T41: `IMP_ISP_Tuning_GetAeWeight(IMPVI_NUM, IMPISPAEWeightAttr *)` |
| **get_ae_zone** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetAeZone(IMPISPZone *)` |

## Auto Exposure (AE) Histogram Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_ae_hist** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_SetAeHist(IMPISPAEHist *)` |
| **get_ae_hist** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetAeHist(IMPISPAEHist *)` |
| **get_ae_hist_origin** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | T10-T31/C100: `IMP_ISP_Tuning_GetAeHist_Origin(IMPISPAEHistOrigin *)` |

## Sensor FPS Control Functions

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_sensor_fps** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_SetSensorFPS(uint32_t, uint32_t)`<br>T40: `IMP_ISP_Tuning_SetSensorFPS(IMPVI_NUM, uint32_t *, uint32_t *)`<br>T41: `IMP_ISP_Tuning_SetSensorFPS(IMPVI_NUM, IMPISPSensorFps *)` |
| **get_sensor_fps** | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓† | ✓† | T10-T31/C100: `IMP_ISP_Tuning_GetSensorFPS(uint32_t *, uint32_t *)`<br>T40: `IMP_ISP_Tuning_GetSensorFPS(IMPVI_NUM, uint32_t *, uint32_t *)`<br>T41: `IMP_ISP_Tuning_GetSensorFPS(IMPVI_NUM, IMPISPSensorFps *)` |

## Auto Zoom Function

| HAL Function | T10 | T20 | T21 | T23 | T30 | T31 | C100 | T40 | T41 | SDK Function(s) Used |
|--------------|-----|-----|-----|-----|-----|-----|------|-----|-----|---------------------|
| **set_auto_zoom** | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✓ | ✓* | ✓* | T31/C100: `IMP_ISP_Tuning_SetAutoZoom(IMPISPAutoZoom *)`<br>T40/T41: `IMP_ISP_Tuning_SetAutoZoom(IMPVI_NUM, IMPISPAutoZoom *)` |

## Legend

| Symbol | Meaning |
|--------|---------|
| **✓** | Fully supported with native SDK function |
| **✗** | Not supported (returns -1 or no-op) |
| **\*** | API variation: requires IMPVI_NUM parameter |
| **\*\*** | API variation: uses different function name |
| **†** | API variation: uses different parameter structure |
| **‡** | Combined HV flip API (separate H/V calls merged into single HVFLIP API) |

## Key Platform-Specific Variations

### T40/T41 API Differences

The T40 and T41 platforms (XBurst2 architecture with Kernel 4.x) have significant API differences:

1. **IMPVI_NUM Parameter**: Most ISP tuning functions require `IMPVI_MAIN` or `IMPVI` as first parameter
2. **Pointer Parameters**: Many functions take pointer to value instead of direct value (e.g., `unsigned char *` instead of `unsigned char`)
3. **Combined HVFLIP**: Separate H/V flip functions replaced with combined flip API
   - T40: Uses `IMP_ISP_Tuning_SetISPHVFLIP(IMPVI_NUM, IMPISPHVFLIP)`
   - T41: Uses `IMP_ISP_Tuning_SetFLIP(IMPVI_NUM, IMPISPFLIP)`
4. **T41 ISP Bypass**: Uses `IMP_ISP_SetISPBypass` instead of `IMP_ISP_Tuning_SetISPBypass`
5. **No Manual WB**: `IMP_ISP_Tuning_SetWB()` not available
6. **No Max Gain Control**: `set_max_again()`/`set_max_dgain()` not available
7. **No Sinter/Temper**: Direct sinter/temper strength control not available
8. **Sensor FPS Struct**: T41 uses `IMPISPSensorFps` struct instead of separate num/den pointers

### Denoise API Differences

#### Simple Value APIs (T23/T31/C100)
```c
IMP_ISP_Tuning_SetSinterStrength(unsigned char val);
IMP_ISP_Tuning_SetTemperStrength(unsigned char val);
IMP_ISP_Tuning_SetDRC_Strength(unsigned char val);
```

#### Struct-based APIs (T10/T20/T21/T30)
```c
IMP_ISP_Tuning_SetSinterDnsAttr(IMPISPSinterDenoiseAttr *attr);
IMP_ISP_Tuning_SetTemperDnsAttr(IMPISPTemperDenoiseAttr *attr);
IMP_ISP_Tuning_SetRawDRC(IMPISPDrcAttr *attr);
```

The HAL layer normalizes these differences by wrapping struct-based calls and filling the appropriate struct fields.

### Anti-Flicker API Differences

#### Enum-based (T10-T31/C100)
```c
typedef enum {
    IMPISP_ANTIFLICKER_DISABLE,
    IMPISP_ANTIFLICKER_50HZ,
    IMPISP_ANTIFLICKER_60HZ
} IMPISPAntiflickerAttr;
```

#### Struct-based (T40/T41)
```c
typedef struct {
    IMPISPAntiflickerMode mode;  // DISABLE_MODE or NORMAL_MODE
    uint32_t freq;               // 50 or 60
} IMPISPAntiflickerAttr;
```

### Platform-Specific Limitations

| Platform | Missing Features |
|----------|-----------------|
| **T10**  | get_ev, hue control, various advanced ISP features |
| **T20**  | get_ev (T10 SDK only), hue control, defog, backlight comp |
| **T21**  | AE compensation, most advanced ISP features |
| **T23**  | Some advanced features present in T31 |
| **T30**  | Similar to T20/T21 limitations |
| **T31**  | Most complete feature set for older generation |
| **T40**  | No manual WB, no max gain control, no sinter/temper direct control, no AE compensation |
| **T41**  | No manual WB, no max gain control, no sinter/temper direct control, no AE compensation |

### Notes on AWB Gains

On some platforms (particularly T23/T31), the AWB histogram gains from `IMP_ISP_Tuning_GetAwbHist()` may return 0. This is a known SDK limitation where the ISP histogram statistics may not be populated, or the fields represent histogram weights rather than applied gains. The EV (exposure) metric is more reliable for sensor response data on these platforms.

### A1 Platform Support

The A1 platform headers are present in the `include/` directory but are not currently handled in the HAL preprocessor conditionals. This suggests the A1 platform may not be actively supported in this codebase.

## Implementation Files

- **HAL Interface**: `src/imp_hal.hpp`
- **HAL Implementation**: `src/imp_hal.cpp`
- **Platform Capabilities**: `hal::caps()` function in `imp_hal.cpp`

## Related Documentation

- [Platform Support Overview](PLATFORM_SUPPORT.md)
- [IMP Control Capabilities](LIBIMP_CONTROL_CAPABILITIES.md)
- [Technical Reference](TECHNICAL_REFERENCE.md)
