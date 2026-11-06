// Shared declarations for every spud shader. Mirrors src/render/gpu_types.hpp.
// Colour pipeline: textures are sampled through UNORM views and the scene is lit into a
// half-float target, in gamma space, like Skyrim's main render target.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform texture2D u_textures[];
// The same descriptor array: cube-map slots hold cube views and are only read through this alias.
layout(set = 0, binding = 0) uniform textureCube u_cubes[];
layout(set = 0, binding = 0) uniform texture3D u_volumes[]; // the volumetric lighting's volume
// And array views (the shadow maps).
layout(set = 0, binding = 0) uniform texture2DArray u_arrays[];
layout(set = 0, binding = 1) uniform sampler u_samplers[6];

#define SAMPLER_WRAP 3u
#define SAMPLER_ISOTROPIC 4u // trilinear without anisotropy (render::sampler_wrap_isotropic)
#define SHADOW_PASS_SUN 0u        // PushConstants::mode of the shadow casters (render::ShadowPass)
#define SHADOW_PASS_SPOT 1u
#define SHADOW_PASS_PARABOLOID 2u
#define SAMPLER_SHADOW 5u    // shadow-map comparison (render::sampler_shadow)

// GpuFrame::far_land (render::GpuFarLand).
struct FarLand {
    uvec4 maps;   // colour, normal, noise slots, heightfield width in samples
    vec4 map;     // world xy -> macro uv: scale, offset
    vec4 samples; // world xy of sample (0, 0), metres per sample, metres of height value 0
    vec4 ring;    // the full-resolution ring: fade start, 1 / length; morph start, 1 / length (metres)
    vec4 grass;   // fade start, fade length (clip-space units), wind amplitude
    vec4 ambient; // the weather's Ambient colour, which grass takes unscaled
    uint64_t heights; // the heightfield (TerrainHeightRef)
    uint64_t pad;
};

// GpuDoorLight.
struct DoorLight {
    vec4 position;
    vec4 normal;
    vec4 colour;
    vec4 ambient[3];
};

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer FrameRef {
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 sun_dir;
    vec4 sun_color;
    vec4 ambient_sky;
    vec4 ambient_ground;
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
    vec4 fog;
    vec4 water_color;
    vec4 params;
    uvec4 flags;
    vec4 dir_light_color; // rgb DirLightColor, w the sun's shadow factor (1 lit, 0 shadowed: sun_shadow)
    vec4 dir_ambient[3];
    vec4 amb_spec;
    vec4 ibl;
    vec4 fog_param;
    vec4 fog_near_color;
    vec4 fog_far_color;
    vec4 fog_color;
    vec4 output_clamp;
    vec4 frame_params;
    mat4 fog_view_proj;
    vec4 clock; // x the controllers' clock one frame back; y the weather transition
    vec4 effect_light; // effect lighting: the weather's Effect Lighting colour x Sunlight Scale
    vec4 clip_plane;   // View::clip_plane
    vec4 depth_params; // x near plane (m) of the reverse-Z projection; y 1 when the scene depth copy is valid
    uvec4 projected_textures; // ProjectedNoise, ProjectedDiffuse, ProjectedNormal, ProjectedNormalDetail
    vec4 projected_params;    // ProjectedUVParams3: diffuse tiling, detail tiling, 0, textured
    vec4 snow_rim;            // SnowRimLightParameters
    vec4 camera_right;        // view basis (world)
    vec4 camera_up;
    vec4 lod_fade;            // x LOD fade metric per metre, yz viewport size
    vec4 fade_spec_env;       // specular start, end, environment map start, end
    vec4 fade_decal_refr;     // decal start, end, refraction start, end
    vec4 tree;                // leaf dampening start, end (metres), wind magnitude, wave time scale
    vec4 lod_fade_types;      // LOD fade metric per metre: items, sky cell, visible when distant
    vec4 shadow_rows[9];      // the sun's shadow cascades (shadows.glsl)
    vec4 shadow_split;
    vec4 shadow_params;
    vec4 shadow_filter;
    uvec4 shadow_info;
    vec4 shadow_light_rows[16];
    vec4 shadow_light_params[4];
    vec4 shadow_light_fade;
    FarLand far_land;
    uvec4 door_info;
    DoorLight doors[4];
};

#define INSTANCE_FADE_SHIFT 2u // Instance::flags bits 2-4: the fade node's category (render::InstanceFade)

// The fade node's LOD metric: the bound centre's distance times its
// category's scale; 0 for nodes that never fade.
float lod_fade_metric(vec3 centre, vec3 camera, uint instance_flags, vec4 objects, vec4 types) {
    const uint category = (instance_flags >> INSTANCE_FADE_SHIFT) & 7u;
    const float scale = category == 0u ? objects.x
                      : category == 1u ? types.x
                      : category == 2u ? types.y
                      : category == 3u ? types.z
                                       : 0.0;
    return length(centre - camera) * scale;
}

// full up to the start, linear to 0 at the end.
float distance_fade(float metric, float start, float end) {
    return metric <= start ? 1.0 : clamp((metric - end) / (start - end), 0.0, 1.0);
}

// GpuTextureSlot
#define TEX_DIFFUSE 0
#define TEX_NORMAL 1
#define TEX_SPECULAR 2
#define TEX_HEIGHT 3
#define TEX_ENV 4
#define TEX_ENV_MASK 5
#define TEX_GLOW 6
#define TEX_INNER 7
#define TEX_BACK 8
#define TEX_RIM_SOFT 9

struct Material {
    uint technique;
    uint flags;
    uint alpha_func;
    float alpha_threshold;
    uint tex[12];
    uint smp;
    uint effect_flags;
    uint palette;
    uint uv_anim; // index into this frame's MaterialAnim[], 0xFFFFFFFF = not animated
    vec4 uv;
    vec4 specular;
    vec4 emit;
    vec4 effect;
    vec4 tint;
    vec4 layer;
    vec4 sparkle;
    vec4 extra;
    vec4 xform[3];
    vec4 base_color;
    vec4 falloff;
    vec4 projected;       // ProjectedUVParams
    vec4 projected_color; // ProjectedUVParams2 (multi pass: normal dampener, 1 / material UV scale)
    vec4 projection[3];   // TextureProj rows (world): projected UV = (P . row 0, P . row 1), row 2 = "up"
    vec4 billboard[3];    // the NiBillboardNode's frame in mesh space (xform is then below it)
    uint billboard_mode;  // 0xFFFFFFFF = none
    uint pad1[3];
};

// Skyrim effect technique
#define EFX_VC (1u << 0)
#define EFX_TEXTURE (1u << 6)
#define EFX_FALLOFF (1u << 8)
#define EFX_ADDITIVE (1u << 10)
#define EFX_MULTIPLY (1u << 11)
#define EFX_WEAPON_BLOOD (1u << 14)
#define EFX_LIGHTING (1u << 16)
#define EFX_PROJECTED (1u << 17) // membranes only here: 10.4.7
#define EFX_SOFT (1u << 18)
#define EFX_PALETTE_COLOR (1u << 19)
#define EFX_PALETTE_ALPHA (1u << 20)
#define EFX_IGNORE_ALPHA (1u << 21)
#define EFX_PREMULT (1u << 22)
#define EFX_BASE_ALPHA (1u << 23)
#define EFX_FAR_PLANE (1u << 24)
#define EFX_OPAQUE (1u << 26)

#define DEPTH_SLOT 65534u

// Skyrim lighting technique
#define TECH_VC (1u << 0)
#define TECH_SKINNED (1u << 1)
#define TECH_MSN (1u << 2)
#define TECH_SPECULAR (1u << 9)
#define TECH_SOFT (1u << 10)
#define TECH_RIM (1u << 11)
#define TECH_BACK (1u << 12)
#define TECH_PROJECTED_UV (1u << 15)
#define TECH_BASE_SNOW (1u << 19)
#define TECH_ANISO (1u << 16)
#define TECH_AMBIENT_SPECULAR (1u << 17)
#define TECH_ALPHA_TEST (1u << 20)
#define TECH_SNOW (1u << 21)
#define TYPE_DEFAULT 0u
#define TYPE_ENVMAP 1u
#define TYPE_GLOWMAP 2u
#define TYPE_PARALLAX 3u
#define TYPE_FACE 4u
#define TYPE_FACE_TINT 5u
#define TYPE_HAIR 6u
#define TYPE_LANDSCAPE 8u
#define TYPE_LOD_LAND 9u
#define TYPE_MULTILAYER 11u
#define TYPE_TREE 12u
#define TYPE_LOD_OBJECTS 13u
#define TYPE_SPARKLE 14u
#define TYPE_LOD_OBJECTS_HD 15u
#define TYPE_EYE 16u
#define TYPE_LOD_LAND_NOISE 18u
#define TYPE_LANDSCAPE_LOD_BLEND 19u

// Pipeline specialisation of the lighting shader (render/pipelines.hpp, MeshVariant): the shader types, technique
// bits and material flags the pipeline's materials can have. Unspecialised pipelines allow everything; the others let
// the compiler drop the code, and the registers, of every feature their materials never use. A material is only drawn
// with a pipeline that covers it, so the result is the same.
layout(constant_id = 0) const uint k_spec_types = 0xFFFFFFFFu;     // bit t: shader type t
layout(constant_id = 1) const uint k_spec_technique = 0xFFFFFFFFu; // technique feature bits (0-23)
layout(constant_id = 2) const uint k_spec_flags = 0xFFFFFFFFu;     // material flags (MAT_*)
bool is_type(uint type, uint t) { return ((k_spec_types >> t) & 1u) != 0u && type == t; }

#define EFFECT_FALLOFF (1u << 0)
#define EFFECT_PALETTE_COLOR (1u << 1)
#define EFFECT_PALETTE_ALPHA (1u << 2)
#define EFFECT_MEMBRANE (1u << 3)

// osm::MaterialFlag
#define MAT_ALPHA_TEST (1u << 0)
#define MAT_ALPHA_BLEND (1u << 1)
#define MAT_TWO_SIDED (1u << 2)
#define MAT_VERTEX_COLORS (1u << 4)
#define MAT_VERTEX_ALPHA (1u << 5)
#define MAT_TREE_ANIM (1u << 8)
#define MAT_GLOW (1u << 9)
#define MAT_DECAL (1u << 3)
#define MAT_NO_FADE (1u << 27)
#define MAT_ENV_MAP (1u << 10)        // flags1.7 Environment_Mapping: the environment fade applies
#define MAT_NO_DECAL_FADE (1u << 24)  // flags2.3 No_Fade or flags2.9: no decal fade
#define MAT_EXTERNAL_EMITTANCE (1u << 26)
#define MAT_RECEIVE_SHADOWS (1u << 23) // takes the shadow mask
#define MAT_BILLBOARD (1u << 25) // under an NiBillboardNode (Material::billboard_mode)
#define MAT_REFRACTION_CLAMP (1u << 28)
#define MAT_FIRE_REFRACTION (1u << 29)
#define MAT_REFRACTION (1u << 30)
#define MAT_MODEL_SPACE_NORMALS (1u << 11)
#define MAT_EFFECT (1u << 31)

struct Instance {
    vec4 r0;
    vec4 r1;
    vec4 r2;
    uvec4 lights; // point-light list: 8 u16 indices into the frame's lights, 0xFFFF = end
    vec4 sphere;  // world bound sphere (metres)
    uint form_id;
    float leaf_amplitude;
    float leaf_frequency;
    uint flags;   // INSTANCE_*
};
#define INSTANCE_LEAF_ANIM (1u << 0)
#define INSTANCE_EMITTANCE (1u << 1) // flags bits 8-31: the emittance colour, unorm8 r, g, b

struct TerrainQuad {
    uint base;
    uint count;
    uint layers[8];
    uvec4 lights; // as Instance::lights
};

// A placed point light this frame.
struct PointLight {
    vec4 position; // xyz camera-relative, w radius (metres)
    vec4 color;    // rgb PointLightColor, w = 1 lit, 0 culled, 2 + n the shadow light n
};
// The values a material's controllers animate this frame; they replace the Material fields.
struct MaterialAnim {
    vec4 uv;
    vec4 specular;
    vec4 emit;
    vec4 effect;
    vec4 base_color;
    vec4 falloff;
    vec4 extra;
    float alpha_threshold;
    float pad[3];
};

// Entry i of a light list.
uint light_index(uvec4 list, uint i) { return (list[i >> 1u] >> ((i & 1u) * 16u)) & 0xFFFFu; }

layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MaterialRef { Material m[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer InstanceTableRef { Instance v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer DrawInstanceRef { uvec2 v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer QuadRef { TerrainQuad q[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer PointLightRef { PointLight l[]; };
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer MaterialAnimRef { MaterialAnim a[]; };
struct LandTexture {
    uint diffuse;
    uint normal;
    float specular_power;
    float is_snow;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer LandTexRef { LandTexture t[]; };

struct TerrainVertex {
    vec3 pos;
    float tangent_x; // (T.x + 1) / 2, read undecoded like the game
    uint normal;     // unorm8x4: record normal + 128, w = T.y
    uint binormal;   // unorm8x4: B, w = T.z
    uint color;      // unorm8x4
    uint weight0;    // unorm8x4: texture slots 0-3
    uint weight1;    // unorm8x4: slots 4-5, then column and row in the quadrant
    uint quad;
};
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainVertexRef { TerrainVertex v[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainCellRef { uint base[]; };
layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer TerrainHeightRef { uint w[]; };

// One sky draw (render::SkyDraw as the renderer packs it).
struct SkyDraw {
    mat4 world;
    vec4 color[3];
    vec4 uv_offset;
    uvec4 info;  // x technique, y texture 0, z texture 1
    vec4 params; // x PParams.x, y depth, z PParams.y
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer SkyDrawRef { SkyDraw d[]; };

// One precipitation emitter (GpuPrecip in src/render/gpu_types.hpp).
struct Precip {
    vec4 box;       // x box size, y 1 for snow, zw c0.xy: the snow corners' scale
    vec4 shape;     // size x, y, centre offset span, initial rotation range (radians)
    vec4 scroll;    // xyz, w rotation
    vec4 forward;   // xyz camera forward x box / 2, w amount
    vec4 velocity;  // xyz V / 15
    vec4 instance[10];
    uvec4 info;     // x seed, y texture, z subtextures x, w subtextures y
};

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer FlagRef { uint value; };

// The image-space constants of a frame (GpuPost in src/render/gpu_types.hpp).
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer PostRef {
    SkyDrawRef sky_draws;
    uvec2 pad0;
    vec4 fog_param;
    vec4 fog_near;  // rgb, w g_FramebufferRangeRcp
    vec4 fog_far;
    vec4 camera;    // x near, y far (the game's, units), z units per metre, w spud's near plane (m)
    vec4 sao_proj;  // g_ProjInfos
    vec4 sao_info;  // g_SSAOInfos
    vec4 sao_screen; // g_ScreenInfos
    vec4 sao_info2; // g_SSAOInfos2
    vec4 sao_screen2; // x fDOFMaxDepthParticipation
    vec4 misc;      // x g_ContrastParams, y 1 / fGamma, z g_FramebufferRangeRcp
    vec4 bloom;     // threshold, scale, 1 / 1.2, 1.2
    vec4 adapt;     // Param.z, Param.w, 1 to jump, the value to hold at (0: none)
    vec4 tone;      // Param
    vec4 cinematic;
    vec4 tint;
    vec4 fade;
    vec4 dof1;
    vec4 dof2;
    vec4 dof3;
    vec4 gauss[8];  // x: the bloom weights by distance from the centre
    vec4 radial0;   // Params
    vec4 radial1;   // Center
    vec4 dv0;       // doubleVisParams
    vec4 dv1;       // blurParams
    vec4 vl0;
    vec4 vl1;
    vec4 vl2;
    vec4 vl3;
    uvec4 post_slots; // x the camera-Z pyramid's last level
    FlagRef snow_flag; // the composite marks a snow pixel here
    FlagRef lens_flag; // lens_down marks a pixel over 1 here
    vec4 sparkle[4];   // SparklesParameters1..4
    vec4 sss[2];       // g_SSSDarkColor, g_SSSParameters
    vec4 ssr;          // fReflectionMarchingRadius, fReflectionRayThickness; the composite's SSRParams.x, .y
    vec4 water_fog;  // the distant blur's underwater fog: rgb colour, a its alpha (0: above water)
    vec4 water_fog2; // x far, y near
    Precip precip[2];
    mat4 precip_occlusion;
    vec4 portal[5];    // GpuPost::portal
};

// Water: one form's constants, the game's PerMaterial buffer c[14] and what its vertex shader
// reads beyond the frame's fog; one drawn surface; the frame's water state.
struct WaterMaterial {
    vec4 c[14];
    vec4 uv_offset[2]; // the noise layers' scroll: VS cb1[3], cb1[4]
    vec4 uv_scale;     // xyz the noise layers' tile size in units
    uvec4 noise;       // xyz the noise textures
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WaterMaterialRef {
    WaterMaterial m[];
};
struct WaterInstance {
    vec4 origin;   // xyz world (metres), w size (metres)
    vec4 flow;     // x the flow grid, yz the flow offset
    vec4 flow_cell;
    uvec4 info;    // x material
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WaterInstanceRef {
    WaterInstance i[];
};
struct WaterVertex {
    vec4 position;
    vec2 uv;
    uint material;
    uint vertex_uv;
    vec4 plane;
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WaterVertexRef {
    WaterVertex v[];
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer WaterRef {
    mat4 previous_view_proj;
    vec4 camera_z;        // x near in units over the reverse-Z depth, y metres per unit, zw tan of the half fov
    vec4 screen;          // xy 1 / the target's size, z 250 / (far - near), w unused
    vec4 camera_forward;  // xyz the view's forward axis, w the game's near plane
    vec4 sun_dir_range;   // xyz sun direction, w 1 / the framebuffer's range
    vec4 sun_color;       // rgb, a its multiplier
    vec4 eye;             // xyz the camera's position, units
    vec4 params;          // x the blend's distance factor (0: off), y cb12[42].w
    vec4 default_normal;  // the surface's normal in view space (SSR's DefaultNormal)
    vec4 quad_rect;       // the wading quad, world units: min xy, max xy; 0 without one
    vec4 ripple;          // xy the window's shift this step, z the stamp's half width (0: none)
    vec4 ripple_segments[8]; // the stamps this step, in window coordinates; ripple.w how many
    vec4 light_positions[6]; // The game's light pass: from the camera and radius, in units
    vec4 light_colors[6];
    vec4 light_count;
    uvec4 slots;          // x refraction, y the scene's depth, z the reflection cube, w the displacement
    uvec4 slots2;         // x reflections blurred, y raw, z flow map, w flow normals
    uvec4 slots3;         // x the water's last image, y the water target's mask
    WaterMaterialRef materials;
    WaterInstanceRef instances;
    WaterVertexRef placed;
};

// World particles.
struct Particle {
    vec4 position;    // xyz world metres, w rotation
    vec4 size_system; // x half extent, y the system's index as float bits
    vec4 color;
    vec4 uv;          // u offset, u size, v offset, v size
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ParticleArrayRef {
    Particle p[];
};
struct ParticleSystem {
    uvec4 info;       // x texture, y technique, z palette
    vec4 base_color;
    vec4 uv;          // offset, scale
    vec4 params;      // x soft falloff depth, y base colour scale, z lighting influence
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ParticleSystemRef {
    ParticleSystem s[];
};
layout(buffer_reference, scalar, buffer_reference_align = 16) readonly buffer ParticlesRef {
    ParticleArrayRef particles;
    ParticleSystemRef systems;
};

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    DrawInstanceRef draw_instances;
    InstanceTableRef instances;
    MaterialRef materials;
    QuadRef quads;
    LandTexRef land_textures;
    TerrainVertexRef terrain_vertices;
    TerrainCellRef terrain_cells;
    PointLightRef lights;
    MaterialAnimRef material_anims;
    PostRef post;
    WaterRef water;
    ParticlesRef particles;
    uint mode;
    uint pad;
    uvec4 pass; // image-space passes: source slots x, y, z; w the target's width | height << 16
} pc;

// The game's normal target: a spheremap of the view-space normal (x right, y up, z forward).
// The SSR mask of SV_Target2: a step at 0.1 of the specular mask, as SSRParams.x and.y are both 0.1.
float ssr_mask(float spec_mask) {
    float t = clamp((spec_mask - (0.1 - 0.00001)) / 0.00001, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// On snow the sun's specular is this rim term: Ng the interpolated vertex normal.
float snow_rim_light(vec4 R, vec3 Ng, vec3 N, vec3 V) {
    if (R.w <= 0.00001) {
        return 0.0;
    }
    return R.x * pow(1.0 - clamp(dot(Ng, V), 0.0, 1.0), R.y) * pow(1.0 - clamp(dot(N, V), 0.0, 1.0), R.z);
}

vec4 encode_normal(vec3 n) {
    FrameRef f = pc.frame;
    vec3 v = vec3(dot(n, f.camera_right.xyz), dot(n, f.camera_up.xyz),
                  dot(n, cross(f.camera_up.xyz, f.camera_right.xyz)));
    return vec4(v.xy / max(sqrt(8.0 - 8.0 * v.z), 0.001) + 0.5, 0.0, 0.0);
}

// A material with this frame's animated values applied.
Material load_material(uint index) {
    Material m = pc.materials.m[index];
    if (m.uv_anim != 0xFFFFFFFFu) {
        MaterialAnim a = pc.material_anims.a[m.uv_anim];
        m.uv = a.uv;
        m.specular = a.specular;
        m.emit = a.emit;
        m.effect = a.effect;
        m.base_color = a.base_color;
        m.falloff = a.falloff;
        m.extra = a.extra;
        m.alpha_threshold = a.alpha_threshold;
    }
    return m;
}

// Debug views (frame.flags.x)
#define VIEW_MATERIAL 4u // r: shader type (+100 effect, 200 landscape), g: material flags, b: 1 alpha tested
#define VIEW_FORM 5u     // the reference's FormID in 11-bit pieces (r low, g middle, b high; landscape all ones),
                         // a: distance from the camera (metres)
#define VIEW_TECHNIQUE 6u // the game technique id in 11-bit pieces (r low, g middle, b high: type << 2)
#define VIEW_SHADOW 8u    // the shadow mask, 1 lit: r the sun's, g and b the first shadow lights'
#define VIEW_UV 7u        // fract(UV): r + g / 1024 = u * 256, b + a / 1024 = v * 256 (exact in the HDR target)
#define VIEW_LIT 0u
#define VIEW_ALBEDO 1u
#define VIEW_NORMALS 2u
#define VIEW_LIGHTING 3u

vec4 tex(uint id, uint smp, vec2 uv) {
    return texture(sampler2D(u_textures[nonuniformEXT(id)], u_samplers[smp]), uv);
}

vec4 tex_cube(uint id, uint smp, vec3 dir) {
    return texture(samplerCube(u_cubes[nonuniformEXT(id)], u_samplers[smp]), dir);
}

vec4 tex_grad(uint id, uint smp, vec2 uv, vec2 dx, vec2 dy) {
    return textureGrad(sampler2D(u_textures[nonuniformEXT(id)], u_samplers[smp]), uv, dx, dy);
}

vec3 sky_color(vec3 dir, bool with_sun) {
    FrameRef f = pc.frame;
    vec3 c;
    if (dir.z >= 0.0) {
        float t = pow(clamp(dir.z, 0.0, 1.0), 0.5);
        c = mix(f.sky_horizon.rgb, f.sky_zenith.rgb, t);
    } else {
        float t = clamp(-dir.z * 6.0, 0.0, 1.0);
        c = mix(f.sky_horizon.rgb, f.sky_ground.rgb, t);
    }
    float s = max(dot(dir, f.sun_dir.xyz), 0.0);
    c += f.sun_color.rgb * (pow(s, 8.0) * 0.18 + pow(s, 64.0) * 0.25);
    if (with_sun) {
        c += f.sun_color.rgb * smoothstep(0.9994, 0.9997, s) * f.sun_color.w;
    }
    return c;
}

vec3 fog_color(vec3 dir) {
    FrameRef f = pc.frame;
    float s = max(dot(dir, f.sun_dir.xyz), 0.0);
    return f.sky_horizon.rgb + f.sun_color.rgb * pow(s, 8.0) * 0.18;
}

float fog_amount(float dist) {
    vec4 f = pc.frame.fog;
    float t = clamp((dist - f.x) / max(f.y - f.x, 1.0), 0.0, 1.0);
    return pow(t, f.w) * f.z;
}

vec3 apply_fog(vec3 c, vec3 rel) {
    float d = length(rel);
    return mix(c, fog_color(rel / max(d, 1e-4)), fog_amount(d));
}

vec3 lighting(vec3 n) {
    FrameRef f = pc.frame;
    if (f.flags.y != 0u) {
        return vec3(1.0);
    }
    float ndl = max(dot(n, f.sun_dir.xyz), 0.0);
    vec3 amb = mix(f.ambient_ground.rgb, f.ambient_sky.rgb, n.z * 0.5 + 0.5);
    return f.sun_color.rgb * ndl + amb;
}

// NiAlphaProperty test functions; returns true if the fragment survives.
bool alpha_pass(float a, float ref, uint func) {
    switch (func) {
    case 0u: return true;
    case 1u: return a < ref;
    case 2u: return a == ref;
    case 3u: return a <= ref;
    case 4u: return a > ref;
    case 5u: return a != ref;
    case 6u: return a >= ref;
    default: return false;
    }
}

vec3 debug_view(vec3 lit, vec3 albedo, vec3 n, vec3 light) {
    uint v = pc.frame.flags.x;
    if (v == VIEW_ALBEDO) return albedo;
    if (v == VIEW_NORMALS) return n * 0.5 + 0.5;
    if (v == VIEW_LIGHTING) return light;
    return lit;
}
