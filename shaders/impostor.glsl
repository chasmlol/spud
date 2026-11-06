// osh::TreeInstance and render::GpuImpostor.
struct Tree {
    vec3 position;
    float scale;
    vec4 rotation;
    uint impostor;
    uint form_id;
};
struct Impostor {
    uint colour;
    uint normal;
    uint frames;
    float lateral;
    vec3 centre;
    float radius;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TreeRef { Tree t[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ImpostorRef { Impostor i[]; };
