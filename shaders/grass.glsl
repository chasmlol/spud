// engine::GrassInstance and render::GpuGrassForm.
struct GrassBlade {
    uint w[7]; // 14 half floats
    uint packed; // form, block x + 128, block y + 128
};
struct GrassForm {
    uint material;
    float wave_period;
    float uniform_scale;
    float pad;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer GrassRef { GrassBlade b[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer GrassFormRef { GrassForm f[]; };
