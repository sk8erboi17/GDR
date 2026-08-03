#include "grd/gpu.h"

#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <string.h>

grd_gpu_capabilities grd_gpu_detect(void)
{
    grd_gpu_capabilities capabilities;
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.integrated_renderer = true;
    (void)snprintf(
        capabilities.adapter_name,
        sizeof(capabilities.adapter_name),
        "GPU integrata / renderer SDL3"
    );

    char name[sizeof(capabilities.adapter_name)] = {0};
    capabilities.metal = grd_metal_available(name, sizeof(name));
    if (capabilities.metal) {
        (void)snprintf(
            capabilities.adapter_name,
            sizeof(capabilities.adapter_name),
            "%s",
            name
        );
    }
    capabilities.cuda = grd_cuda_available(name, sizeof(name));
    if (capabilities.cuda && !capabilities.metal) {
        (void)snprintf(
            capabilities.adapter_name,
            sizeof(capabilities.adapter_name),
            "%s",
            name
        );
    }
    capabilities.videotoolbox_h264 =
        avcodec_find_encoder_by_name("h264_videotoolbox") != NULL;
    capabilities.nvenc = avcodec_find_encoder_by_name("h264_nvenc") != NULL;
    capabilities.nvdec =
        avcodec_find_decoder_by_name("h264_cuvid") != NULL ||
        avcodec_find_decoder_by_name("h264_nvdec") != NULL;
    if (!capabilities.metal && capabilities.nvenc &&
        strcmp(capabilities.adapter_name, "GPU integrata / renderer SDL3") == 0) {
        (void)snprintf(
            capabilities.adapter_name,
            sizeof(capabilities.adapter_name),
            "NVIDIA NVENC/NVDEC runtime"
        );
    }
    return capabilities;
}

grd_pipeline_kind grd_gpu_select(
    const grd_gpu_capabilities *capabilities,
    grd_gpu_preference preference
)
{
    if (capabilities == NULL) {
        return GRD_PIPELINE_SOFTWARE;
    }
    if (preference == GRD_GPU_SOFTWARE) {
        return GRD_PIPELINE_SOFTWARE;
    }
    if ((preference == GRD_GPU_AUTOMATIC || preference == GRD_GPU_METAL) &&
        capabilities->metal && capabilities->videotoolbox_h264) {
        return GRD_PIPELINE_METAL_VIDEOTOOLBOX;
    }
    /* FFmpeg can expose NVENC/NVDEC through the installed driver even when
     * GRD was built without nvcc. Do not require our optional CUDA color
     * conversion module to select the hardware codec. */
    if ((preference == GRD_GPU_AUTOMATIC || preference == GRD_GPU_CUDA) &&
        capabilities->nvenc) {
        return GRD_PIPELINE_CUDA_NVENC;
    }
    if (preference == GRD_GPU_CUDA && capabilities->cuda) {
        return GRD_PIPELINE_CUDA_SOFTWARE;
    }
    if (capabilities->integrated_renderer) {
        return GRD_PIPELINE_INTEGRATED_SOFTWARE;
    }
    return GRD_PIPELINE_SOFTWARE;
}

const char *grd_pipeline_name(grd_pipeline_kind pipeline)
{
    switch (pipeline) {
    case GRD_PIPELINE_METAL_VIDEOTOOLBOX: return "Metal + VideoToolbox";
    case GRD_PIPELINE_CUDA_NVENC: return "CUDA + NVENC/NVDEC";
    case GRD_PIPELINE_CUDA_SOFTWARE: return "CUDA + H.264 software";
    case GRD_PIPELINE_INTEGRATED_SOFTWARE: return "GPU integrata + H.264 software";
    case GRD_PIPELINE_SOFTWARE: return "H.264 software";
    default: return "Pipeline sconosciuta";
    }
}
