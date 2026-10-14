//////////////////////////////////////////////////////////////////////////
/*
  PainterEngine Core - glTF 2.0 / GLB model parser (PX_gltf)

  Ported from cgltf 1.15 for PainterEngine:
    - all allocations go through px_memorypool (no malloc/free)
    - no stdio: models are parsed from memory, external .bin/.png files are
      supplied by the caller through PX_Gltf_FileOptions.read or by filling
      PX_Gltf_Buffer.data before PX_GltfLoadBuffers
    - identifiers follow PainterEngine naming:
        cgltf_parse            -> PX_GltfParse           (public functions)
        cgltf_data             -> PX_Gltf_Data           (structures)
        cgltf_result           -> PX_GLTF_RESULT         (enum types)
        cgltf_result_success   -> PX_GLTF_RESULT_SUCCESS (enum values)
        cgltf_size             -> px_gltf_size
    - JSON numbers are parsed by an internal routine that understands the full
      JSON grammar (sign, fraction, exponent)

  Typical usage:
    PX_Gltf_Data* model;
    if (PX_GltfParse(mp, PX_NULL, file_data, file_size, &model) == PX_GLTF_RESULT_SUCCESS)
    {
        if (PX_GltfLoadBuffers(PX_NULL, model, PX_NULL) == PX_GLTF_RESULT_SUCCESS)
        {
            ... read model->meshes[i].primitives[j] through PX_GltfAccessor* helpers ...
        }
        PX_GltfFree(model);
    }
  See documents/PX_gltf.md for the full guide.

  cgltf - a single-file glTF 2.0 parser written in C99.
  Copyright (c) 2018-2021 Johannes Kuhlmann
  https://github.com/jkuhlmann/cgltf

  jsmn - a minimalistic JSON parser
  Copyright (c) 2010 Serge A. Zaitsev
  https://github.com/zserge/jsmn

  Both are distributed under the MIT License:

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
//////////////////////////////////////////////////////////////////////////
#ifndef PX_GLTF_H
#define PX_GLTF_H
#include "PX_MemoryPool.h"

/* pointer-sized integer types (px_gltf_size / px_gltf_ssize) */
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__) || defined(__LP64__) || defined(_LP64) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
typedef px_uint64 px_gltf_size;
typedef px_int64  px_gltf_ssize;
#else
typedef px_uint   px_gltf_size;
typedef px_int    px_gltf_ssize;
#endif

typedef enum PX_GLTF_FILE_TYPE
{
	PX_GLTF_FILE_TYPE_INVALID,
	PX_GLTF_FILE_TYPE_GLTF,
	PX_GLTF_FILE_TYPE_GLB,
	PX_GLTF_FILE_TYPE_MAX_ENUM
} PX_GLTF_FILE_TYPE;

typedef enum PX_GLTF_RESULT
{
	PX_GLTF_RESULT_SUCCESS,
	PX_GLTF_RESULT_DATA_TOO_SHORT,
	PX_GLTF_RESULT_UNKNOWN_FORMAT,
	PX_GLTF_RESULT_INVALID_JSON,
	PX_GLTF_RESULT_INVALID_GLTF,
	PX_GLTF_RESULT_INVALID_OPTIONS,
	PX_GLTF_RESULT_FILE_NOT_FOUND,
	PX_GLTF_RESULT_IO_ERROR,
	PX_GLTF_RESULT_OUT_OF_MEMORY,
	PX_GLTF_RESULT_LEGACY_GLTF,
    PX_GLTF_RESULT_MAX_ENUM
} PX_GLTF_RESULT;

typedef struct PX_Gltf_MemoryOptions
{
	px_void* (*alloc_func)(px_void* user, px_gltf_size size);
	px_void (*free_func) (px_void* user, px_void* ptr);
	px_void* user_data;
} PX_Gltf_MemoryOptions;

typedef struct PX_Gltf_FileOptions
{
	PX_GLTF_RESULT(*read)(const struct PX_Gltf_MemoryOptions* memory_options, const struct PX_Gltf_FileOptions* file_options, const px_char* path, px_gltf_size* size, px_void** data);
	px_void (*release)(const struct PX_Gltf_MemoryOptions* memory_options, const struct PX_Gltf_FileOptions* file_options, px_void* data, px_gltf_size size);
	px_void* user_data;
} PX_Gltf_FileOptions;

typedef struct PX_Gltf_Options
{
	PX_GLTF_FILE_TYPE type; /* invalid == auto detect */
	px_gltf_size json_token_count; /* 0 == auto */
	PX_Gltf_MemoryOptions memory;
	PX_Gltf_FileOptions file;
} PX_Gltf_Options;

typedef enum PX_GLTF_BUFFER_VIEW_TYPE
{
	PX_GLTF_BUFFER_VIEW_TYPE_INVALID,
	PX_GLTF_BUFFER_VIEW_TYPE_INDICES,
	PX_GLTF_BUFFER_VIEW_TYPE_VERTICES,
	PX_GLTF_BUFFER_VIEW_TYPE_MAX_ENUM
} PX_GLTF_BUFFER_VIEW_TYPE;

typedef enum PX_GLTF_ATTRIBUTE_TYPE
{
	PX_GLTF_ATTRIBUTE_TYPE_INVALID,
	PX_GLTF_ATTRIBUTE_TYPE_POSITION,
	PX_GLTF_ATTRIBUTE_TYPE_NORMAL,
	PX_GLTF_ATTRIBUTE_TYPE_TANGENT,
	PX_GLTF_ATTRIBUTE_TYPE_TEXCOORD,
	PX_GLTF_ATTRIBUTE_TYPE_COLOR,
	PX_GLTF_ATTRIBUTE_TYPE_JOINTS,
	PX_GLTF_ATTRIBUTE_TYPE_WEIGHTS,
	PX_GLTF_ATTRIBUTE_TYPE_CUSTOM,
	PX_GLTF_ATTRIBUTE_TYPE_MAX_ENUM
} PX_GLTF_ATTRIBUTE_TYPE;

typedef enum PX_GLTF_COMPONENT_TYPE
{
	PX_GLTF_COMPONENT_TYPE_INVALID,
	PX_GLTF_COMPONENT_TYPE_R_8, /* BYTE */
	PX_GLTF_COMPONENT_TYPE_R_8U, /* UNSIGNED_BYTE */
	PX_GLTF_COMPONENT_TYPE_R_16, /* SHORT */
	PX_GLTF_COMPONENT_TYPE_R_16U, /* UNSIGNED_SHORT */
	PX_GLTF_COMPONENT_TYPE_R_32U, /* UNSIGNED_INT */
	PX_GLTF_COMPONENT_TYPE_R_32F, /* FLOAT */
    PX_GLTF_COMPONENT_TYPE_MAX_ENUM
} PX_GLTF_COMPONENT_TYPE;

typedef enum PX_GLTF_TYPE
{
	PX_GLTF_TYPE_INVALID,
	PX_GLTF_TYPE_SCALAR,
	PX_GLTF_TYPE_VEC2,
	PX_GLTF_TYPE_VEC3,
	PX_GLTF_TYPE_VEC4,
	PX_GLTF_TYPE_MAT2,
	PX_GLTF_TYPE_MAT3,
	PX_GLTF_TYPE_MAT4,
	PX_GLTF_TYPE_MAX_ENUM
} PX_GLTF_TYPE;

typedef enum PX_GLTF_PRIMITIVE_TYPE
{
	PX_GLTF_PRIMITIVE_TYPE_INVALID,
	PX_GLTF_PRIMITIVE_TYPE_POINTS,
	PX_GLTF_PRIMITIVE_TYPE_LINES,
	PX_GLTF_PRIMITIVE_TYPE_LINE_LOOP,
	PX_GLTF_PRIMITIVE_TYPE_LINE_STRIP,
	PX_GLTF_PRIMITIVE_TYPE_TRIANGLES,
	PX_GLTF_PRIMITIVE_TYPE_TRIANGLE_STRIP,
	PX_GLTF_PRIMITIVE_TYPE_TRIANGLE_FAN,
	PX_GLTF_PRIMITIVE_TYPE_MAX_ENUM
} PX_GLTF_PRIMITIVE_TYPE;

typedef enum PX_GLTF_ALPHA_MODE
{
	PX_GLTF_ALPHA_MODE_OPAQUE,
	PX_GLTF_ALPHA_MODE_MASK,
	PX_GLTF_ALPHA_MODE_BLEND,
	PX_GLTF_ALPHA_MODE_MAX_ENUM
} PX_GLTF_ALPHA_MODE;

typedef enum PX_GLTF_ANIMATION_PATH_TYPE {
	PX_GLTF_ANIMATION_PATH_TYPE_INVALID,
	PX_GLTF_ANIMATION_PATH_TYPE_TRANSLATION,
	PX_GLTF_ANIMATION_PATH_TYPE_ROTATION,
	PX_GLTF_ANIMATION_PATH_TYPE_SCALE,
	PX_GLTF_ANIMATION_PATH_TYPE_WEIGHTS,
	PX_GLTF_ANIMATION_PATH_TYPE_MAX_ENUM
} PX_GLTF_ANIMATION_PATH_TYPE;

typedef enum PX_GLTF_INTERPOLATION_TYPE {
	PX_GLTF_INTERPOLATION_TYPE_LINEAR,
	PX_GLTF_INTERPOLATION_TYPE_STEP,
	PX_GLTF_INTERPOLATION_TYPE_CUBIC_SPLINE,
	PX_GLTF_INTERPOLATION_TYPE_MAX_ENUM
} PX_GLTF_INTERPOLATION_TYPE;

typedef enum PX_GLTF_CAMERA_TYPE {
	PX_GLTF_CAMERA_TYPE_INVALID,
	PX_GLTF_CAMERA_TYPE_PERSPECTIVE,
	PX_GLTF_CAMERA_TYPE_ORTHOGRAPHIC,
	PX_GLTF_CAMERA_TYPE_MAX_ENUM
} PX_GLTF_CAMERA_TYPE;

typedef enum PX_GLTF_LIGHT_TYPE {
	PX_GLTF_LIGHT_TYPE_INVALID,
	PX_GLTF_LIGHT_TYPE_DIRECTIONAL,
	PX_GLTF_LIGHT_TYPE_POINT,
	PX_GLTF_LIGHT_TYPE_SPOT,
	PX_GLTF_LIGHT_TYPE_MAX_ENUM
} PX_GLTF_LIGHT_TYPE;

typedef enum PX_GLTF_DATA_FREE_METHOD {
	PX_GLTF_DATA_FREE_METHOD_NONE,
	PX_GLTF_DATA_FREE_METHOD_FILE_RELEASE,
	PX_GLTF_DATA_FREE_METHOD_MEMORY_FREE,
	PX_GLTF_DATA_FREE_METHOD_MAX_ENUM
} PX_GLTF_DATA_FREE_METHOD;

typedef struct PX_Gltf_Extras {
	px_gltf_size start_offset; /* this field is deprecated and will be removed in the future; use data instead */
	px_gltf_size end_offset; /* this field is deprecated and will be removed in the future; use data instead */

	px_char* data;
} PX_Gltf_Extras;

typedef struct PX_Gltf_Extension {
	px_char* name;
	px_char* data;
} PX_Gltf_Extension;

typedef struct PX_Gltf_Buffer
{
	px_char* name;
	px_gltf_size size;
	px_char* uri;
	px_void* data; /* loaded by PX_GltfLoadBuffers */
	PX_GLTF_DATA_FREE_METHOD data_free_method;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Buffer;

typedef enum PX_GLTF_MESHOPT_COMPRESSION_MODE {
	PX_GLTF_MESHOPT_COMPRESSION_MODE_INVALID,
	PX_GLTF_MESHOPT_COMPRESSION_MODE_ATTRIBUTES,
	PX_GLTF_MESHOPT_COMPRESSION_MODE_TRIANGLES,
	PX_GLTF_MESHOPT_COMPRESSION_MODE_INDICES,
	PX_GLTF_MESHOPT_COMPRESSION_MODE_MAX_ENUM
} PX_GLTF_MESHOPT_COMPRESSION_MODE;

typedef enum PX_GLTF_MESHOPT_COMPRESSION_FILTER {
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_NONE,
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_OCTAHEDRAL,
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_QUATERNION,
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_EXPONENTIAL,
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_COLOR,
	PX_GLTF_MESHOPT_COMPRESSION_FILTER_MAX_ENUM
} PX_GLTF_MESHOPT_COMPRESSION_FILTER;

typedef struct PX_Gltf_MeshoptCompression
{
	PX_Gltf_Buffer* buffer;
	px_gltf_size offset;
	px_gltf_size size;
	px_gltf_size stride;
	px_gltf_size count;
	PX_GLTF_MESHOPT_COMPRESSION_MODE mode;
	PX_GLTF_MESHOPT_COMPRESSION_FILTER filter;
	px_bool is_khr;
} PX_Gltf_MeshoptCompression;

typedef struct PX_Gltf_BufferView
{
	px_char *name;
	PX_Gltf_Buffer* buffer;
	px_gltf_size offset;
	px_gltf_size size;
	px_gltf_size stride; /* 0 == automatically determined by accessor */
	PX_GLTF_BUFFER_VIEW_TYPE type;
	px_void* data; /* overrides buffer->data if present, filled by extensions */
	px_bool has_meshopt_compression;
	PX_Gltf_MeshoptCompression meshopt_compression;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_BufferView;

typedef struct PX_Gltf_AccessorSparse
{
	px_gltf_size count;
	PX_Gltf_BufferView* indices_buffer_view;
	px_gltf_size indices_byte_offset;
	PX_GLTF_COMPONENT_TYPE indices_component_type;
	PX_Gltf_BufferView* values_buffer_view;
	px_gltf_size values_byte_offset;
} PX_Gltf_AccessorSparse;

typedef struct PX_Gltf_Accessor
{
	px_char* name;
	PX_GLTF_COMPONENT_TYPE component_type;
	px_bool normalized;
	PX_GLTF_TYPE type;
	px_gltf_size offset;
	px_gltf_size count;
	px_gltf_size stride;
	PX_Gltf_BufferView* buffer_view;
	px_bool has_min;
	px_float min[16];
	px_bool has_max;
	px_float max[16];
	px_bool is_sparse;
	PX_Gltf_AccessorSparse sparse;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Accessor;

typedef struct PX_Gltf_Attribute
{
	px_char* name;
	PX_GLTF_ATTRIBUTE_TYPE type;
	px_int index;
	PX_Gltf_Accessor* data;
} PX_Gltf_Attribute;

typedef struct PX_Gltf_Image
{
	px_char* name;
	px_char* uri;
	PX_Gltf_BufferView* buffer_view;
	px_char* mime_type;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Image;

typedef enum PX_GLTF_FILTER_TYPE {
    PX_GLTF_FILTER_TYPE_UNDEFINED = 0,
    PX_GLTF_FILTER_TYPE_NEAREST = 9728,
    PX_GLTF_FILTER_TYPE_LINEAR = 9729,
    PX_GLTF_FILTER_TYPE_NEAREST_MIPMAP_NEAREST = 9984,
    PX_GLTF_FILTER_TYPE_LINEAR_MIPMAP_NEAREST = 9985,
    PX_GLTF_FILTER_TYPE_NEAREST_MIPMAP_LINEAR = 9986,
    PX_GLTF_FILTER_TYPE_LINEAR_MIPMAP_LINEAR = 9987
} PX_GLTF_FILTER_TYPE;

typedef enum PX_GLTF_WRAP_MODE {
    PX_GLTF_WRAP_MODE_CLAMP_TO_EDGE = 33071,
    PX_GLTF_WRAP_MODE_MIRRORED_REPEAT = 33648,
    PX_GLTF_WRAP_MODE_REPEAT = 10497
} PX_GLTF_WRAP_MODE;

typedef struct PX_Gltf_Sampler
{
	px_char* name;
	PX_GLTF_FILTER_TYPE mag_filter;
	PX_GLTF_FILTER_TYPE min_filter;
	PX_GLTF_WRAP_MODE wrap_s;
	PX_GLTF_WRAP_MODE wrap_t;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Sampler;

typedef struct PX_Gltf_Texture
{
	px_char* name;
	PX_Gltf_Image* image;
	PX_Gltf_Sampler* sampler;
	px_bool has_basisu;
	PX_Gltf_Image* basisu_image;
	px_bool has_webp;
	PX_Gltf_Image* webp_image;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Texture;

typedef struct PX_Gltf_TextureTransform
{
	px_float offset[2];
	px_float rotation;
	px_float scale[2];
	px_bool has_texcoord;
	px_int texcoord;
} PX_Gltf_TextureTransform;

typedef struct PX_Gltf_TextureView
{
	PX_Gltf_Texture* texture;
	px_int texcoord;
	px_float scale; /* equivalent to strength for occlusion_texture */
	px_bool has_transform;
	PX_Gltf_TextureTransform transform;
} PX_Gltf_TextureView;

typedef struct PX_Gltf_PbrMetallicRoughness
{
	PX_Gltf_TextureView base_color_texture;
	PX_Gltf_TextureView metallic_roughness_texture;

	px_float base_color_factor[4];
	px_float metallic_factor;
	px_float roughness_factor;
} PX_Gltf_PbrMetallicRoughness;

typedef struct PX_Gltf_PbrSpecularGlossiness
{
	PX_Gltf_TextureView diffuse_texture;
	PX_Gltf_TextureView specular_glossiness_texture;

	px_float diffuse_factor[4];
	px_float specular_factor[3];
	px_float glossiness_factor;
} PX_Gltf_PbrSpecularGlossiness;

typedef struct PX_Gltf_Clearcoat
{
	PX_Gltf_TextureView clearcoat_texture;
	PX_Gltf_TextureView clearcoat_roughness_texture;
	PX_Gltf_TextureView clearcoat_normal_texture;

	px_float clearcoat_factor;
	px_float clearcoat_roughness_factor;
} PX_Gltf_Clearcoat;

typedef struct PX_Gltf_Transmission
{
	PX_Gltf_TextureView transmission_texture;
	px_float transmission_factor;
} PX_Gltf_Transmission;

typedef struct PX_Gltf_Ior
{
	px_float ior;
} PX_Gltf_Ior;

typedef struct PX_Gltf_Specular
{
	PX_Gltf_TextureView specular_texture;
	PX_Gltf_TextureView specular_color_texture;
	px_float specular_color_factor[3];
	px_float specular_factor;
} PX_Gltf_Specular;

typedef struct PX_Gltf_Volume
{
	PX_Gltf_TextureView thickness_texture;
	px_float thickness_factor;
	px_float attenuation_color[3];
	px_float attenuation_distance;
} PX_Gltf_Volume;

typedef struct PX_Gltf_Sheen
{
	PX_Gltf_TextureView sheen_color_texture;
	px_float sheen_color_factor[3];
	PX_Gltf_TextureView sheen_roughness_texture;
	px_float sheen_roughness_factor;
} PX_Gltf_Sheen;

typedef struct PX_Gltf_EmissiveStrength
{
	px_float emissive_strength;
} PX_Gltf_EmissiveStrength;

typedef struct PX_Gltf_Iridescence
{
	px_float iridescence_factor;
	PX_Gltf_TextureView iridescence_texture;
	px_float iridescence_ior;
	px_float iridescence_thickness_min;
	px_float iridescence_thickness_max;
	PX_Gltf_TextureView iridescence_thickness_texture;
} PX_Gltf_Iridescence;

typedef struct PX_Gltf_DiffuseTransmission
{
	PX_Gltf_TextureView diffuse_transmission_texture;
	px_float diffuse_transmission_factor;
	px_float diffuse_transmission_color_factor[3];
	PX_Gltf_TextureView diffuse_transmission_color_texture;
} PX_Gltf_DiffuseTransmission;

typedef struct PX_Gltf_Anisotropy
{
	px_float anisotropy_strength;
	px_float anisotropy_rotation;
	PX_Gltf_TextureView anisotropy_texture;
} PX_Gltf_Anisotropy;

typedef struct PX_Gltf_Dispersion
{
	px_float dispersion;
} PX_Gltf_Dispersion;

typedef struct PX_Gltf_Material
{
	px_char* name;
	px_bool has_pbr_metallic_roughness;
	px_bool has_pbr_specular_glossiness;
	px_bool has_clearcoat;
	px_bool has_transmission;
	px_bool has_volume;
	px_bool has_ior;
	px_bool has_specular;
	px_bool has_sheen;
	px_bool has_emissive_strength;
	px_bool has_iridescence;
	px_bool has_diffuse_transmission;
	px_bool has_anisotropy;
	px_bool has_dispersion;
	PX_Gltf_PbrMetallicRoughness pbr_metallic_roughness;
	PX_Gltf_PbrSpecularGlossiness pbr_specular_glossiness;
	PX_Gltf_Clearcoat clearcoat;
	PX_Gltf_Ior ior;
	PX_Gltf_Specular specular;
	PX_Gltf_Sheen sheen;
	PX_Gltf_Transmission transmission;
	PX_Gltf_Volume volume;
	PX_Gltf_EmissiveStrength emissive_strength;
	PX_Gltf_Iridescence iridescence;
	PX_Gltf_DiffuseTransmission diffuse_transmission;
	PX_Gltf_Anisotropy anisotropy;
	PX_Gltf_Dispersion dispersion;
	PX_Gltf_TextureView normal_texture;
	PX_Gltf_TextureView occlusion_texture;
	PX_Gltf_TextureView emissive_texture;
	px_float emissive_factor[3];
	PX_GLTF_ALPHA_MODE alpha_mode;
	px_float alpha_cutoff;
	px_bool double_sided;
	px_bool unlit;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Material;

typedef struct PX_Gltf_MaterialMapping
{
	px_gltf_size variant;
	PX_Gltf_Material* material;
	PX_Gltf_Extras extras;
} PX_Gltf_MaterialMapping;

typedef struct PX_Gltf_MorphTarget {
	PX_Gltf_Attribute* attributes;
	px_gltf_size attributes_count;
} PX_Gltf_MorphTarget;

typedef struct PX_Gltf_DracoMeshCompression {
	PX_Gltf_BufferView* buffer_view;
	PX_Gltf_Attribute* attributes;
	px_gltf_size attributes_count;
} PX_Gltf_DracoMeshCompression;

typedef struct PX_Gltf_MeshGpuInstancing {
	PX_Gltf_Attribute* attributes;
	px_gltf_size attributes_count;
} PX_Gltf_MeshGpuInstancing;

typedef struct PX_Gltf_Primitive {
	PX_GLTF_PRIMITIVE_TYPE type;
	PX_Gltf_Accessor* indices;
	PX_Gltf_Material* material;
	PX_Gltf_Attribute* attributes;
	px_gltf_size attributes_count;
	PX_Gltf_MorphTarget* targets;
	px_gltf_size targets_count;
	PX_Gltf_Extras extras;
	px_bool has_draco_mesh_compression;
	PX_Gltf_DracoMeshCompression draco_mesh_compression;
	PX_Gltf_MaterialMapping* mappings;
	px_gltf_size mappings_count;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Primitive;

typedef struct PX_Gltf_Mesh {
	px_char* name;
	PX_Gltf_Primitive* primitives;
	px_gltf_size primitives_count;
	px_float* weights;
	px_gltf_size weights_count;
	px_char** target_names;
	px_gltf_size target_names_count;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Mesh;

typedef struct PX_Gltf_Node PX_Gltf_Node;

typedef struct PX_Gltf_Skin {
	px_char* name;
	PX_Gltf_Node** joints;
	px_gltf_size joints_count;
	PX_Gltf_Node* skeleton;
	PX_Gltf_Accessor* inverse_bind_matrices;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Skin;

typedef struct PX_Gltf_CameraPerspective {
	px_bool has_aspect_ratio;
	px_float aspect_ratio;
	px_float yfov;
	px_bool has_zfar;
	px_float zfar;
	px_float znear;
	PX_Gltf_Extras extras;
} PX_Gltf_CameraPerspective;

typedef struct PX_Gltf_CameraOrthographic {
	px_float xmag;
	px_float ymag;
	px_float zfar;
	px_float znear;
	PX_Gltf_Extras extras;
} PX_Gltf_CameraOrthographic;

typedef struct PX_Gltf_Camera {
	px_char* name;
	PX_GLTF_CAMERA_TYPE type;
	union {
		PX_Gltf_CameraPerspective perspective;
		PX_Gltf_CameraOrthographic orthographic;
	} data;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Camera;

typedef struct PX_Gltf_Light {
	px_char* name;
	px_float color[3];
	px_float intensity;
	PX_GLTF_LIGHT_TYPE type;
	px_float range;
	px_float spot_inner_cone_angle;
	px_float spot_outer_cone_angle;
	PX_Gltf_Extras extras;
} PX_Gltf_Light;

struct PX_Gltf_Node {
	px_char* name;
	PX_Gltf_Node* parent;
	PX_Gltf_Node** children;
	px_gltf_size children_count;
	PX_Gltf_Skin* skin;
	PX_Gltf_Mesh* mesh;
	PX_Gltf_Camera* camera;
	PX_Gltf_Light* light;
	px_float* weights;
	px_gltf_size weights_count;
	px_bool has_translation;
	px_bool has_rotation;
	px_bool has_scale;
	px_bool has_matrix;
	px_float translation[3];
	px_float rotation[4];
	px_float scale[3];
	px_float matrix[16];
	PX_Gltf_Extras extras;
	px_bool has_mesh_gpu_instancing;
	PX_Gltf_MeshGpuInstancing mesh_gpu_instancing;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
};

typedef struct PX_Gltf_Scene {
	px_char* name;
	PX_Gltf_Node** nodes;
	px_gltf_size nodes_count;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Scene;

typedef struct PX_Gltf_AnimationSampler {
	PX_Gltf_Accessor* input;
	PX_Gltf_Accessor* output;
	PX_GLTF_INTERPOLATION_TYPE interpolation;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_AnimationSampler;

typedef struct PX_Gltf_AnimationChannel {
	PX_Gltf_AnimationSampler* sampler;
	PX_Gltf_Node* target_node;
	PX_GLTF_ANIMATION_PATH_TYPE target_path;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_AnimationChannel;

typedef struct PX_Gltf_Animation {
	px_char* name;
	PX_Gltf_AnimationSampler* samplers;
	px_gltf_size samplers_count;
	PX_Gltf_AnimationChannel* channels;
	px_gltf_size channels_count;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Animation;

typedef struct PX_Gltf_MaterialVariant
{
	px_char* name;
	PX_Gltf_Extras extras;
} PX_Gltf_MaterialVariant;

typedef struct PX_Gltf_Asset {
	px_char* copyright;
	px_char* generator;
	px_char* version;
	px_char* min_version;
	PX_Gltf_Extras extras;
	px_gltf_size extensions_count;
	PX_Gltf_Extension* extensions;
} PX_Gltf_Asset;

typedef struct PX_Gltf_Data
{
	PX_GLTF_FILE_TYPE file_type;
	px_void* file_data;
	px_gltf_size file_size;

	PX_Gltf_Asset asset;

	PX_Gltf_Mesh* meshes;
	px_gltf_size meshes_count;

	PX_Gltf_Material* materials;
	px_gltf_size materials_count;

	PX_Gltf_Accessor* accessors;
	px_gltf_size accessors_count;

	PX_Gltf_BufferView* buffer_views;
	px_gltf_size buffer_views_count;

	PX_Gltf_Buffer* buffers;
	px_gltf_size buffers_count;

	PX_Gltf_Image* images;
	px_gltf_size images_count;

	PX_Gltf_Texture* textures;
	px_gltf_size textures_count;

	PX_Gltf_Sampler* samplers;
	px_gltf_size samplers_count;

	PX_Gltf_Skin* skins;
	px_gltf_size skins_count;

	PX_Gltf_Camera* cameras;
	px_gltf_size cameras_count;

	PX_Gltf_Light* lights;
	px_gltf_size lights_count;

	PX_Gltf_Node* nodes;
	px_gltf_size nodes_count;

	PX_Gltf_Scene* scenes;
	px_gltf_size scenes_count;

	PX_Gltf_Scene* scene;

	PX_Gltf_Animation* animations;
	px_gltf_size animations_count;

	PX_Gltf_MaterialVariant* variants;
	px_gltf_size variants_count;

	PX_Gltf_Extras extras;

	px_gltf_size data_extensions_count;
	PX_Gltf_Extension* data_extensions;

	px_char** extensions_used;
	px_gltf_size extensions_used_count;

	px_char** extensions_required;
	px_gltf_size extensions_required_count;

	const px_char* json;
	px_gltf_size json_size;

	const px_void* bin;
	px_gltf_size bin_size;

	PX_Gltf_MemoryOptions memory;
	PX_Gltf_FileOptions file;
} PX_Gltf_Data;

/* parse glTF (JSON) or GLB data held in memory. mp is used for all allocations unless options->memory provides alloc_func/free_func. options may be PX_NULL. */
PX_GLTF_RESULT PX_GltfParse(px_memorypool* mp, const PX_Gltf_Options* options, const px_void* data, px_gltf_size size, PX_Gltf_Data** out_data);

/* resolve buffers: GLB BIN chunk, data: URIs (base64) and, through options->file.read (or data->file.read), external files. options may be PX_NULL. */
PX_GLTF_RESULT PX_GltfLoadBuffers(const PX_Gltf_Options* options, PX_Gltf_Data* data, const px_char* gltf_path);

/* decode base64 text into a buffer of exactly `size` bytes allocated from mp (or options->memory). free with MP_Free(mp, ptr) / options->memory.free_func. */
PX_GLTF_RESULT PX_GltfLoadBufferBase64(px_memorypool* mp, const PX_Gltf_Options* options, px_gltf_size size, const px_char* base64, px_void** out_data);

px_gltf_size PX_GltfDecodeString(px_char* string);
px_gltf_size PX_GltfDecodeUri(px_char* uri);

PX_GLTF_RESULT PX_GltfValidate(PX_Gltf_Data* data);

px_void PX_GltfFree(PX_Gltf_Data* data);

px_void PX_GltfNodeTransformLocal(const PX_Gltf_Node* node, px_float* out_matrix);
px_void PX_GltfNodeTransformWorld(const PX_Gltf_Node* node, px_float* out_matrix);

const px_byte* PX_GltfBufferViewData(const PX_Gltf_BufferView* view);

const PX_Gltf_Accessor* PX_GltfFindAccessor(const PX_Gltf_Primitive* prim, PX_GLTF_ATTRIBUTE_TYPE type, px_int index);

px_bool PX_GltfAccessorReadFloat(const PX_Gltf_Accessor* accessor, px_gltf_size index, px_float* out, px_gltf_size element_size);
px_bool PX_GltfAccessorReadUint(const PX_Gltf_Accessor* accessor, px_gltf_size index, px_uint* out, px_gltf_size element_size);
px_gltf_size PX_GltfAccessorReadIndex(const PX_Gltf_Accessor* accessor, px_gltf_size index);

px_gltf_size PX_GltfNumComponents(PX_GLTF_TYPE type);
px_gltf_size PX_GltfComponentSize(PX_GLTF_COMPONENT_TYPE component_type);
px_gltf_size PX_GltfCalcSize(PX_GLTF_TYPE type, PX_GLTF_COMPONENT_TYPE component_type);

px_gltf_size PX_GltfAccessorUnpackFloats(const PX_Gltf_Accessor* accessor, px_float* out, px_gltf_size float_count);
px_gltf_size PX_GltfAccessorUnpackIndices(const PX_Gltf_Accessor* accessor, px_void* out, px_gltf_size out_component_size, px_gltf_size index_count);

/* this function is deprecated and will be removed in the future; use PX_Gltf_Extras::data instead */
PX_GLTF_RESULT PX_GltfCopyExtrasJson(const PX_Gltf_Data* data, const PX_Gltf_Extras* extras, px_char* dest, px_gltf_size* dest_size);

px_gltf_size PX_GltfMeshIndex(const PX_Gltf_Data* data, const PX_Gltf_Mesh* object);
px_gltf_size PX_GltfMaterialIndex(const PX_Gltf_Data* data, const PX_Gltf_Material* object);
px_gltf_size PX_GltfAccessorIndex(const PX_Gltf_Data* data, const PX_Gltf_Accessor* object);
px_gltf_size PX_GltfBufferViewIndex(const PX_Gltf_Data* data, const PX_Gltf_BufferView* object);
px_gltf_size PX_GltfBufferIndex(const PX_Gltf_Data* data, const PX_Gltf_Buffer* object);
px_gltf_size PX_GltfImageIndex(const PX_Gltf_Data* data, const PX_Gltf_Image* object);
px_gltf_size PX_GltfTextureIndex(const PX_Gltf_Data* data, const PX_Gltf_Texture* object);
px_gltf_size PX_GltfSamplerIndex(const PX_Gltf_Data* data, const PX_Gltf_Sampler* object);
px_gltf_size PX_GltfSkinIndex(const PX_Gltf_Data* data, const PX_Gltf_Skin* object);
px_gltf_size PX_GltfCameraIndex(const PX_Gltf_Data* data, const PX_Gltf_Camera* object);
px_gltf_size PX_GltfLightIndex(const PX_Gltf_Data* data, const PX_Gltf_Light* object);
px_gltf_size PX_GltfNodeIndex(const PX_Gltf_Data* data, const PX_Gltf_Node* object);
px_gltf_size PX_GltfSceneIndex(const PX_Gltf_Data* data, const PX_Gltf_Scene* object);
px_gltf_size PX_GltfAnimationIndex(const PX_Gltf_Data* data, const PX_Gltf_Animation* object);
px_gltf_size PX_GltfAnimationSamplerIndex(const PX_Gltf_Animation* animation, const PX_Gltf_AnimationSampler* object);
px_gltf_size PX_GltfAnimationChannelIndex(const PX_Gltf_Animation* animation, const PX_Gltf_AnimationChannel* object);

#endif
