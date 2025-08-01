/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

/**
 * Historically, each builtin node type was given an integer type as part of its definition. These
 * defines are redundant with idnames and shouldn't be used in new code. However in some cases they
 * are used for backwards compatibility when versioning relied on the integer type while the idname
 * changed.
 *
 * See #bNode::type_legacy for more information.
 *
 * NOTE: Values here must not be larger than #NODE_LEGACY_TYPE_GENERATION_START.
 */

/* -------------------------------------------------------------------- */
/** \name Shader Nodes
 * \{ */

// #define SH_NODE_MATERIAL  100
#define SH_NODE_RGB 101
#define SH_NODE_VALUE 102
#define SH_NODE_MIX_RGB_LEGACY 103
#define SH_NODE_VALTORGB 104
#define SH_NODE_RGBTOBW 105
#define SH_NODE_SHADERTORGB 106
// #define SH_NODE_TEXTURE       106
#define SH_NODE_NORMAL 107
// #define SH_NODE_GEOMETRY  108
#define SH_NODE_MAPPING 109
#define SH_NODE_CURVE_VEC 110
#define SH_NODE_CURVE_RGB 111
#define SH_NODE_CAMERA 114
#define SH_NODE_MATH 115
#define SH_NODE_VECTOR_MATH 116
#define SH_NODE_SQUEEZE 117
// #define SH_NODE_MATERIAL_EXT  118
#define SH_NODE_INVERT 119
#define SH_NODE_SEPRGB_LEGACY 120
#define SH_NODE_COMBRGB_LEGACY 121
#define SH_NODE_HUE_SAT 122

#define SH_NODE_OUTPUT_MATERIAL 124
#define SH_NODE_OUTPUT_WORLD 125
#define SH_NODE_OUTPUT_LIGHT 126
#define SH_NODE_FRESNEL 127
#define SH_NODE_MIX_SHADER 128
#define SH_NODE_ATTRIBUTE 129
#define SH_NODE_BACKGROUND 130
#define SH_NODE_BSDF_GLOSSY 131
#define SH_NODE_BSDF_DIFFUSE 132
#define SH_NODE_BSDF_GLOSSY_LEGACY 133
#define SH_NODE_BSDF_GLASS 134
#define SH_NODE_BSDF_TRANSLUCENT 137
#define SH_NODE_BSDF_TRANSPARENT 138
#define SH_NODE_BSDF_SHEEN 139
#define SH_NODE_EMISSION 140
#define SH_NODE_NEW_GEOMETRY 141
#define SH_NODE_LIGHT_PATH 142
#define SH_NODE_TEX_IMAGE 143
#define SH_NODE_TEX_SKY 145
#define SH_NODE_TEX_GRADIENT 146
#define SH_NODE_TEX_VORONOI 147
#define SH_NODE_TEX_MAGIC 148
#define SH_NODE_TEX_WAVE 149
#define SH_NODE_TEX_NOISE 150
#define SH_NODE_TEX_MUSGRAVE_DEPRECATED 152
#define SH_NODE_TEX_COORD 155
#define SH_NODE_ADD_SHADER 156
#define SH_NODE_TEX_ENVIRONMENT 157
// #define SH_NODE_OUTPUT_TEXTURE 158
#define SH_NODE_HOLDOUT 159
#define SH_NODE_LAYER_WEIGHT 160
#define SH_NODE_VOLUME_ABSORPTION 161
#define SH_NODE_VOLUME_SCATTER 162
#define SH_NODE_GAMMA 163
#define SH_NODE_TEX_CHECKER 164
#define SH_NODE_BRIGHTCONTRAST 165
#define SH_NODE_LIGHT_FALLOFF 166
#define SH_NODE_OBJECT_INFO 167
#define SH_NODE_PARTICLE_INFO 168
#define SH_NODE_TEX_BRICK 169
#define SH_NODE_BUMP 170
#define SH_NODE_SCRIPT 171
#define SH_NODE_AMBIENT_OCCLUSION 172
#define SH_NODE_BSDF_REFRACTION 173
#define SH_NODE_TANGENT 174
#define SH_NODE_NORMAL_MAP 175
#define SH_NODE_HAIR_INFO 176
#define SH_NODE_SUBSURFACE_SCATTERING 177
#define SH_NODE_WIREFRAME 178
#define SH_NODE_BSDF_TOON 179
#define SH_NODE_WAVELENGTH 180
#define SH_NODE_BLACKBODY 181
#define SH_NODE_VECT_TRANSFORM 182
#define SH_NODE_SEPHSV_LEGACY 183
#define SH_NODE_COMBHSV_LEGACY 184
#define SH_NODE_BSDF_HAIR 185
// #define SH_NODE_LAMP 186
#define SH_NODE_UVMAP 187
#define SH_NODE_SEPXYZ 188
#define SH_NODE_COMBXYZ 189
#define SH_NODE_OUTPUT_LINESTYLE 190
#define SH_NODE_UVALONGSTROKE 191
// #define SH_NODE_TEX_POINTDENSITY 192
#define SH_NODE_BSDF_PRINCIPLED 193
#define SH_NODE_TEX_IES 194
#define SH_NODE_EEVEE_SPECULAR 195
#define SH_NODE_BEVEL 197
#define SH_NODE_DISPLACEMENT 198
#define SH_NODE_VECTOR_DISPLACEMENT 199
#define SH_NODE_VOLUME_PRINCIPLED 200
/* 201..700 occupied by other node types, continue from 701 */
#define SH_NODE_BSDF_HAIR_PRINCIPLED 701
#define SH_NODE_MAP_RANGE 702
#define SH_NODE_CLAMP 703
#define SH_NODE_TEX_WHITE_NOISE 704
#define SH_NODE_VOLUME_INFO 705
#define SH_NODE_VERTEX_COLOR 706
#define SH_NODE_OUTPUT_AOV 707
#define SH_NODE_VECTOR_ROTATE 708
#define SH_NODE_CURVE_FLOAT 709
#define SH_NODE_POINT_INFO 710
#define SH_NODE_COMBINE_COLOR 711
#define SH_NODE_SEPARATE_COLOR 712
#define SH_NODE_MIX 713
#define SH_NODE_BSDF_RAY_PORTAL 714
#define SH_NODE_TEX_GABOR 715
#define SH_NODE_BSDF_METALLIC 716
#define SH_NODE_VOLUME_COEFFICIENTS 717

#define SH_NODE_SPRITES_ANIMATION 800 //UPBGE

/** \} */

/* -------------------------------------------------------------------- */
/** \name Composite Nodes
 * \{ */

/* output socket defines */
#define RRES_OUT_IMAGE 0
#define RRES_OUT_ALPHA 1

/* NOTE: types are needed to restore callbacks, don't change values. */
#define CMP_NODE_VIEWER 201
#define CMP_NODE_RGB 202
#define CMP_NODE_VALUE_DEPRECATED 203
#define CMP_NODE_MIX_RGB_DEPRECATED 204
#define CMP_NODE_VALTORGB_DEPRECATED 205
#define CMP_NODE_RGBTOBW 206
#define CMP_NODE_NORMAL 207
#define CMP_NODE_CURVE_VEC_DEPRECATED 208
#define CMP_NODE_CURVE_RGB 209
#define CMP_NODE_ALPHAOVER 210
#define CMP_NODE_BLUR 211
#define CMP_NODE_FILTER 212
#define CMP_NODE_MAP_VALUE_DEPRECATED 213
#define CMP_NODE_TIME 214
#define CMP_NODE_VECBLUR 215
#define CMP_NODE_SEPRGBA_LEGACY 216
#define CMP_NODE_SEPHSVA_LEGACY 217
#define CMP_NODE_SETALPHA 218
#define CMP_NODE_HUE_SAT 219
#define CMP_NODE_IMAGE 220
#define CMP_NODE_R_LAYERS 221
#define CMP_NODE_COMPOSITE_DEPRECATED 222
#define CMP_NODE_OUTPUT_FILE 223
#define CMP_NODE_TEXTURE_DEPRECATED 224
#define CMP_NODE_TRANSLATE 225
#define CMP_NODE_ZCOMBINE 226
#define CMP_NODE_COMBRGBA_LEGACY 227
#define CMP_NODE_DILATEERODE 228
#define CMP_NODE_ROTATE 229
#define CMP_NODE_SCALE 230
#define CMP_NODE_SEPYCCA_LEGACY 231
#define CMP_NODE_COMBYCCA_LEGACY 232
#define CMP_NODE_SEPYUVA_LEGACY 233
#define CMP_NODE_COMBYUVA_LEGACY 234
#define CMP_NODE_DIFF_MATTE 235
#define CMP_NODE_COLOR_SPILL 236
#define CMP_NODE_CHROMA_MATTE 237
#define CMP_NODE_CHANNEL_MATTE 238
#define CMP_NODE_FLIP 239
/* Split viewer node is now a regular split node: CMP_NODE_SPLIT. */
#define CMP_NODE_SPLITVIEWER__DEPRECATED 240
// #define CMP_NODE_INDEX_MASK  241
#define CMP_NODE_MAP_UV 242
#define CMP_NODE_ID_MASK 243
#define CMP_NODE_DEFOCUS 244
#define CMP_NODE_DISPLACE 245
#define CMP_NODE_COMBHSVA_LEGACY 246
#define CMP_NODE_MATH_DEPRECATED 247
#define CMP_NODE_LUMA_MATTE 248
#define CMP_NODE_BRIGHTCONTRAST 249
#define CMP_NODE_GAMMA 250
#define CMP_NODE_INVERT 251
#define CMP_NODE_NORMALIZE 252
#define CMP_NODE_CROP 253
#define CMP_NODE_DBLUR 254
#define CMP_NODE_BILATERALBLUR 255
#define CMP_NODE_PREMULKEY 256
#define CMP_NODE_DIST_MATTE 257
#define CMP_NODE_VIEW_LEVELS 258
#define CMP_NODE_COLOR_MATTE 259
#define CMP_NODE_COLORBALANCE 260
#define CMP_NODE_HUECORRECT 261
#define CMP_NODE_MOVIECLIP 262
#define CMP_NODE_STABILIZE2D 263
#define CMP_NODE_TRANSFORM 264
#define CMP_NODE_MOVIEDISTORTION 265
#define CMP_NODE_DOUBLEEDGEMASK 266
#define CMP_NODE_OUTPUT_MULTI_FILE__DEPRECATED \
  267 /* DEPRECATED multi file node has been merged into regular CMP_NODE_OUTPUT_FILE */
#define CMP_NODE_MASK 268
#define CMP_NODE_KEYINGSCREEN 269
#define CMP_NODE_KEYING 270
#define CMP_NODE_TRACKPOS 271
#define CMP_NODE_INPAINT 272
#define CMP_NODE_DESPECKLE 273
#define CMP_NODE_ANTIALIASING 274
#define CMP_NODE_KUWAHARA 275
#define CMP_NODE_SPLIT 276

#define CMP_NODE_GLARE 301
#define CMP_NODE_TONEMAP 302
#define CMP_NODE_LENSDIST 303
#define CMP_NODE_SUNBEAMS 304

#define CMP_NODE_COLORCORRECTION 312
#define CMP_NODE_MASK_BOX 313
#define CMP_NODE_MASK_ELLIPSE 314
#define CMP_NODE_BOKEHIMAGE 315
#define CMP_NODE_BOKEHBLUR 316
#define CMP_NODE_SWITCH 317
#define CMP_NODE_PIXELATE 318

#define CMP_NODE_MAP_RANGE_DEPRECATED 319
#define CMP_NODE_PLANETRACKDEFORM 320
#define CMP_NODE_CORNERPIN 321
#define CMP_NODE_SWITCH_VIEW 322
#define CMP_NODE_CRYPTOMATTE_LEGACY 323
#define CMP_NODE_DENOISE 324
#define CMP_NODE_EXPOSURE 325
#define CMP_NODE_CRYPTOMATTE 326
#define CMP_NODE_POSTERIZE 327
#define CMP_NODE_CONVERT_COLOR_SPACE 328
#define CMP_NODE_SCENE_TIME 329
#define CMP_NODE_SEPARATE_XYZ_DEPRECATED 330
#define CMP_NODE_COMBINE_XYZ_DEPRECATED 331
#define CMP_NODE_COMBINE_COLOR 332
#define CMP_NODE_SEPARATE_COLOR 333
#define CMP_NODE_IMAGE_INFO 334

/* channel toggles */
#define CMP_CHAN_RGB 1
#define CMP_CHAN_A 2

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture Nodes
 * \{ */

#define TEX_NODE_OUTPUT 401
#define TEX_NODE_CHECKER 402
#define TEX_NODE_TEXTURE 403
#define TEX_NODE_BRICKS 404
#define TEX_NODE_MATH 405
#define TEX_NODE_MIX_RGB 406
#define TEX_NODE_RGBTOBW 407
#define TEX_NODE_VALTORGB 408
#define TEX_NODE_IMAGE 409
#define TEX_NODE_CURVE_RGB 410
#define TEX_NODE_INVERT 411
#define TEX_NODE_HUE_SAT 412
#define TEX_NODE_CURVE_TIME 413
#define TEX_NODE_ROTATE 414
#define TEX_NODE_VIEWER 415
#define TEX_NODE_TRANSLATE 416
#define TEX_NODE_COORD 417
#define TEX_NODE_DISTANCE 418
#define TEX_NODE_COMPOSE_LEGACY 419
#define TEX_NODE_DECOMPOSE_LEGACY 420
#define TEX_NODE_VALTONOR 421
#define TEX_NODE_SCALE 422
#define TEX_NODE_AT 423
#define TEX_NODE_COMBINE_COLOR 424
#define TEX_NODE_SEPARATE_COLOR 425

/* 501-599 reserved. Use like this: TEX_NODE_PROC + TEX_CLOUDS, etc */
#define TEX_NODE_PROC 500
#define TEX_NODE_PROC_MAX 600

/** \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Nodes
 * \{ */

#define GEO_NODE_TRIANGULATE 1000
#define GEO_NODE_TRANSFORM_GEOMETRY 1002
#define GEO_NODE_MESH_BOOLEAN 1003
#define GEO_NODE_OBJECT_INFO 1007
#define GEO_NODE_JOIN_GEOMETRY 1010
#define GEO_NODE_COLLECTION_INFO 1023
#define GEO_NODE_IS_VIEWPORT 1024
#define GEO_NODE_SUBDIVIDE_MESH 1029
#define GEO_NODE_MESH_PRIMITIVE_CUBE 1032
#define GEO_NODE_MESH_PRIMITIVE_CIRCLE 1033
#define GEO_NODE_MESH_PRIMITIVE_UV_SPHERE 1034
#define GEO_NODE_MESH_PRIMITIVE_CYLINDER 1035
#define GEO_NODE_MESH_PRIMITIVE_ICO_SPHERE 1036
#define GEO_NODE_MESH_PRIMITIVE_CONE 1037
#define GEO_NODE_MESH_PRIMITIVE_LINE 1038
#define GEO_NODE_MESH_PRIMITIVE_GRID 1039
#define GEO_NODE_BOUNDING_BOX 1042
#define GEO_NODE_SWITCH 1043
#define GEO_NODE_CURVE_TO_MESH 1045
#define GEO_NODE_RESAMPLE_CURVE 1047
#define GEO_NODE_INPUT_MATERIAL 1050
#define GEO_NODE_REPLACE_MATERIAL 1051
#define GEO_NODE_CURVE_LENGTH 1054
#define GEO_NODE_CONVEX_HULL 1056
#define GEO_NODE_SEPARATE_COMPONENTS 1059
#define GEO_NODE_CURVE_PRIMITIVE_STAR 1062
#define GEO_NODE_CURVE_PRIMITIVE_SPIRAL 1063
#define GEO_NODE_CURVE_PRIMITIVE_QUADRATIC_BEZIER 1064
#define GEO_NODE_CURVE_PRIMITIVE_BEZIER_SEGMENT 1065
#define GEO_NODE_CURVE_PRIMITIVE_CIRCLE 1066
#define GEO_NODE_VIEWER 1067
#define GEO_NODE_CURVE_PRIMITIVE_LINE 1068
#define GEO_NODE_CURVE_PRIMITIVE_QUADRILATERAL 1070
#define GEO_NODE_TRIM_CURVE 1071
#define GEO_NODE_FILL_CURVE 1075
#define GEO_NODE_INPUT_POSITION 1076
#define GEO_NODE_SET_POSITION 1077
#define GEO_NODE_INPUT_INDEX 1078
#define GEO_NODE_INPUT_NORMAL 1079
#define GEO_NODE_CAPTURE_ATTRIBUTE 1080
#define GEO_NODE_MATERIAL_SELECTION 1081
#define GEO_NODE_SET_MATERIAL 1082
#define GEO_NODE_REALIZE_INSTANCES 1083
#define GEO_NODE_ATTRIBUTE_STATISTIC 1084
#define GEO_NODE_SAMPLE_CURVE 1085
#define GEO_NODE_INPUT_TANGENT 1086
#define GEO_NODE_STRING_JOIN 1087
#define GEO_NODE_CURVE_SPLINE_PARAMETER 1088
#define GEO_NODE_FILLET_CURVE 1089
#define GEO_NODE_DISTRIBUTE_POINTS_ON_FACES 1090
#define GEO_NODE_STRING_TO_CURVES 1091
#define GEO_NODE_INSTANCE_ON_POINTS 1092
#define GEO_NODE_MESH_TO_POINTS 1093
#define GEO_NODE_POINTS_TO_VERTICES 1094
#define GEO_NODE_REVERSE_CURVE 1095
#define GEO_NODE_PROXIMITY 1096
#define GEO_NODE_SUBDIVIDE_CURVE 1097
#define GEO_NODE_INPUT_SPLINE_LENGTH 1098
#define GEO_NODE_CURVE_SPLINE_TYPE 1099
#define GEO_NODE_CURVE_SET_HANDLE_TYPE 1100
#define GEO_NODE_POINTS_TO_VOLUME 1101
#define GEO_NODE_CURVE_HANDLE_TYPE_SELECTION 1102
#define GEO_NODE_DELETE_GEOMETRY 1103
#define GEO_NODE_SEPARATE_GEOMETRY 1104
#define GEO_NODE_INPUT_RADIUS 1105
#define GEO_NODE_INPUT_CURVE_TILT 1106
#define GEO_NODE_INPUT_CURVE_HANDLES 1107
#define GEO_NODE_INPUT_FACE_SMOOTH 1108
#define GEO_NODE_INPUT_SPLINE_RESOLUTION 1109
#define GEO_NODE_INPUT_SPLINE_CYCLIC 1110
#define GEO_NODE_SET_CURVE_RADIUS 1111
#define GEO_NODE_SET_CURVE_TILT 1112
#define GEO_NODE_SET_CURVE_HANDLES 1113
#define GEO_NODE_SET_SHADE_SMOOTH 1114
#define GEO_NODE_SET_SPLINE_RESOLUTION 1115
#define GEO_NODE_SET_SPLINE_CYCLIC 1116
#define GEO_NODE_SET_POINT_RADIUS 1117
#define GEO_NODE_INPUT_MATERIAL_INDEX 1118
#define GEO_NODE_SET_MATERIAL_INDEX 1119
#define GEO_NODE_TRANSLATE_INSTANCES 1120
#define GEO_NODE_SCALE_INSTANCES 1121
#define GEO_NODE_ROTATE_INSTANCES 1122
#define GEO_NODE_SPLIT_EDGES 1123
#define GEO_NODE_MESH_TO_CURVE 1124
#define GEO_NODE_TRANSFER_ATTRIBUTE_DEPRECATED 1125
#define GEO_NODE_SUBDIVISION_SURFACE 1126
#define GEO_NODE_CURVE_ENDPOINT_SELECTION 1127
#define GEO_NODE_RAYCAST 1128
#define GEO_NODE_CURVE_TO_POINTS 1130
#define GEO_NODE_INSTANCES_TO_POINTS 1131
#define GEO_NODE_IMAGE_TEXTURE 1132
#define GEO_NODE_VOLUME_TO_MESH 1133
#define GEO_NODE_INPUT_ID 1134
#define GEO_NODE_SET_ID 1135
#define GEO_NODE_ATTRIBUTE_DOMAIN_SIZE 1136
#define GEO_NODE_DUAL_MESH 1137
#define GEO_NODE_INPUT_MESH_EDGE_VERTICES 1138
#define GEO_NODE_INPUT_MESH_FACE_AREA 1139
#define GEO_NODE_INPUT_MESH_FACE_NEIGHBORS 1140
#define GEO_NODE_INPUT_MESH_VERTEX_NEIGHBORS 1141
#define GEO_NODE_GEOMETRY_TO_INSTANCE 1142
#define GEO_NODE_INPUT_MESH_EDGE_NEIGHBORS 1143
#define GEO_NODE_INPUT_MESH_ISLAND 1144
#define GEO_NODE_INPUT_SCENE_TIME 1145
#define GEO_NODE_ACCUMULATE_FIELD 1146
#define GEO_NODE_INPUT_MESH_EDGE_ANGLE 1147
#define GEO_NODE_EVALUATE_AT_INDEX 1148
#define GEO_NODE_CURVE_PRIMITIVE_ARC 1149
#define GEO_NODE_FLIP_FACES 1150
#define GEO_NODE_SCALE_ELEMENTS 1151
#define GEO_NODE_EXTRUDE_MESH 1152
#define GEO_NODE_MERGE_BY_DISTANCE 1153
#define GEO_NODE_DUPLICATE_ELEMENTS 1154
#define GEO_NODE_INPUT_MESH_FACE_IS_PLANAR 1155
#define GEO_NODE_STORE_NAMED_ATTRIBUTE 1156
#define GEO_NODE_INPUT_NAMED_ATTRIBUTE 1157
#define GEO_NODE_REMOVE_ATTRIBUTE 1158
#define GEO_NODE_INPUT_INSTANCE_ROTATION 1159
#define GEO_NODE_INPUT_INSTANCE_SCALE 1160
#define GEO_NODE_VOLUME_CUBE 1161
#define GEO_NODE_POINTS 1162
#define GEO_NODE_EVALUATE_ON_DOMAIN 1163
#define GEO_NODE_MESH_TO_VOLUME 1164
#define GEO_NODE_UV_UNWRAP 1165
#define GEO_NODE_UV_PACK_ISLANDS 1166
#define GEO_NODE_DEFORM_CURVES_ON_SURFACE 1167
#define GEO_NODE_INPUT_SHORTEST_EDGE_PATHS 1168
#define GEO_NODE_EDGE_PATHS_TO_CURVES 1169
#define GEO_NODE_EDGE_PATHS_TO_SELECTION 1170
#define GEO_NODE_MESH_FACE_GROUP_BOUNDARIES 1171
#define GEO_NODE_DISTRIBUTE_POINTS_IN_VOLUME 1172
#define GEO_NODE_SELF_OBJECT 1173
#define GEO_NODE_SAMPLE_INDEX 1174
#define GEO_NODE_SAMPLE_NEAREST 1175
#define GEO_NODE_SAMPLE_NEAREST_SURFACE 1176
#define GEO_NODE_OFFSET_POINT_IN_CURVE 1177
#define GEO_NODE_CURVE_TOPOLOGY_CURVE_OF_POINT 1178
#define GEO_NODE_CURVE_TOPOLOGY_POINTS_OF_CURVE 1179
#define GEO_NODE_MESH_TOPOLOGY_OFFSET_CORNER_IN_FACE 1180
#define GEO_NODE_MESH_TOPOLOGY_CORNERS_OF_FACE 1181
#define GEO_NODE_MESH_TOPOLOGY_CORNERS_OF_VERTEX 1182
#define GEO_NODE_MESH_TOPOLOGY_EDGES_OF_CORNER 1183
#define GEO_NODE_MESH_TOPOLOGY_EDGES_OF_VERTEX 1184
#define GEO_NODE_MESH_TOPOLOGY_FACE_OF_CORNER 1185
#define GEO_NODE_MESH_TOPOLOGY_VERTEX_OF_CORNER 1186
#define GEO_NODE_SAMPLE_UV_SURFACE 1187
#define GEO_NODE_SET_CURVE_NORMAL 1188
#define GEO_NODE_IMAGE_INFO 1189
#define GEO_NODE_BLUR_ATTRIBUTE 1190
#define GEO_NODE_IMAGE 1191
#define GEO_NODE_INTERPOLATE_CURVES 1192
#define GEO_NODE_EDGES_TO_FACE_GROUPS 1193
// #define GEO_NODE_POINTS_TO_SDF_VOLUME 1194
// #define GEO_NODE_MESH_TO_SDF_VOLUME 1195
// #define GEO_NODE_SDF_VOLUME_SPHERE 1196
// #define GEO_NODE_MEAN_FILTER_SDF_VOLUME 1197
// #define GEO_NODE_OFFSET_SDF_VOLUME 1198
#define GEO_NODE_INDEX_OF_NEAREST 1199
/* Function nodes use the range starting at 1200. */
#define GEO_NODE_SIMULATION_INPUT 2100
#define GEO_NODE_SIMULATION_OUTPUT 2101
// #define GEO_NODE_INPUT_SIGNED_DISTANCE 2102
// #define GEO_NODE_SAMPLE_VOLUME 2103
#define GEO_NODE_MESH_TOPOLOGY_CORNERS_OF_EDGE 2104
/* Leaving out two indices to avoid crashes with files that were created during the development of
 * the repeat zone. */
#define GEO_NODE_REPEAT_INPUT 2107
#define GEO_NODE_REPEAT_OUTPUT 2108
#define GEO_NODE_TOOL_SELECTION 2109
#define GEO_NODE_TOOL_SET_SELECTION 2110
#define GEO_NODE_TOOL_3D_CURSOR 2111
#define GEO_NODE_TOOL_FACE_SET 2112
#define GEO_NODE_TOOL_SET_FACE_SET 2113
#define GEO_NODE_POINTS_TO_CURVES 2114
#define GEO_NODE_INPUT_EDGE_SMOOTH 2115
#define GEO_NODE_SPLIT_TO_INSTANCES 2116
#define GEO_NODE_INPUT_NAMED_LAYER_SELECTION 2117
#define GEO_NODE_INDEX_SWITCH 2118
#define GEO_NODE_INPUT_ACTIVE_CAMERA 2119
#define GEO_NODE_BAKE 2120
#define GEO_NODE_GET_NAMED_GRID 2121
#define GEO_NODE_STORE_NAMED_GRID 2122
#define GEO_NODE_SORT_ELEMENTS 2123
#define GEO_NODE_MENU_SWITCH 2124
#define GEO_NODE_SAMPLE_GRID 2125
#define GEO_NODE_MESH_TO_DENSITY_GRID 2126
#define GEO_NODE_MESH_TO_SDF_GRID 2127
#define GEO_NODE_POINTS_TO_SDF_GRID 2128
#define GEO_NODE_GRID_TO_MESH 2129
#define GEO_NODE_DISTRIBUTE_POINTS_IN_GRID 2130
#define GEO_NODE_SDF_GRID_BOOLEAN 2131
#define GEO_NODE_TOOL_VIEWPORT_TRANSFORM 2132
#define GEO_NODE_TOOL_MOUSE_POSITION 2133
#define GEO_NODE_SAMPLE_GRID_INDEX 2134
#define GEO_NODE_TOOL_ACTIVE_ELEMENT 2135
#define GEO_NODE_SET_INSTANCE_TRANSFORM 2136
#define GEO_NODE_INPUT_INSTANCE_TRANSFORM 2137
#define GEO_NODE_IMPORT_STL 2138
#define GEO_NODE_IMPORT_OBJ 2139
#define GEO_NODE_SET_GEOMETRY_NAME 2140
#define GEO_NODE_GIZMO_LINEAR 2141
#define GEO_NODE_GIZMO_DIAL 2142
#define GEO_NODE_GIZMO_TRANSFORM 2143
#define GEO_NODE_CURVES_TO_GREASE_PENCIL 2144
#define GEO_NODE_GREASE_PENCIL_TO_CURVES 2145
#define GEO_NODE_IMPORT_PLY 2146
#define GEO_NODE_WARNING 2147
#define GEO_NODE_FOREACH_GEOMETRY_ELEMENT_INPUT 2148
#define GEO_NODE_FOREACH_GEOMETRY_ELEMENT_OUTPUT 2149
#define GEO_NODE_MERGE_LAYERS 2150
#define GEO_NODE_INPUT_COLLECTION 2151
#define GEO_NODE_INPUT_OBJECT 2152
#define NODE_COMBINE_BUNDLE 2153
#define NODE_SEPARATE_BUNDLE 2154
#define NODE_CLOSURE_OUTPUT 2155
#define NODE_EVALUATE_CLOSURE 2156
#define NODE_CLOSURE_INPUT 2157

/** \} */

/* -------------------------------------------------------------------- */
/** \name Function Nodes
 * \{ */

#define FN_NODE_BOOLEAN_MATH 1200
#define FN_NODE_COMPARE 1202
#define FN_NODE_LEGACY_RANDOM_FLOAT 1206
#define FN_NODE_INPUT_VECTOR 1207
#define FN_NODE_INPUT_STRING 1208
#define FN_NODE_FLOAT_TO_INT 1209
#define FN_NODE_VALUE_TO_STRING 1210
#define FN_NODE_STRING_LENGTH 1211
#define FN_NODE_SLICE_STRING 1212
#define FN_NODE_INPUT_SPECIAL_CHARACTERS 1213
#define FN_NODE_RANDOM_VALUE 1214
#define FN_NODE_ROTATE_EULER 1215
#define FN_NODE_ALIGN_EULER_TO_VECTOR 1216
#define FN_NODE_INPUT_COLOR 1217
#define FN_NODE_REPLACE_STRING 1218
#define FN_NODE_INPUT_BOOL 1219
#define FN_NODE_INPUT_INT 1220
#define FN_NODE_SEPARATE_COLOR 1221
#define FN_NODE_COMBINE_COLOR 1222
#define FN_NODE_AXIS_ANGLE_TO_ROTATION 1223
#define FN_NODE_EULER_TO_ROTATION 1224
#define FN_NODE_QUATERNION_TO_ROTATION 1225
#define FN_NODE_ROTATION_TO_AXIS_ANGLE 1226
#define FN_NODE_ROTATION_TO_EULER 1227
#define FN_NODE_ROTATION_TO_QUATERNION 1228
#define FN_NODE_ROTATE_VECTOR 1229
#define FN_NODE_ROTATE_ROTATION 1230
#define FN_NODE_INVERT_ROTATION 1231
#define FN_NODE_TRANSFORM_POINT 1232
#define FN_NODE_TRANSFORM_DIRECTION 1233
#define FN_NODE_MATRIX_MULTIPLY 1234
#define FN_NODE_COMBINE_TRANSFORM 1235
#define FN_NODE_SEPARATE_TRANSFORM 1236
#define FN_NODE_INVERT_MATRIX 1237
#define FN_NODE_TRANSPOSE_MATRIX 1238
#define FN_NODE_PROJECT_POINT 1239
#define FN_NODE_ALIGN_ROTATION_TO_VECTOR 1240
#define FN_NODE_COMBINE_MATRIX 1241
#define FN_NODE_SEPARATE_MATRIX 1242
#define FN_NODE_INPUT_ROTATION 1243
#define FN_NODE_AXES_TO_ROTATION 1244
#define FN_NODE_HASH_VALUE 1245
#define FN_NODE_INTEGER_MATH 1246
#define FN_NODE_MATRIX_DETERMINANT 1247
#define FN_NODE_FIND_IN_STRING 1248

/** \} */
