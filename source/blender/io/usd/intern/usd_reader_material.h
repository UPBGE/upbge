/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2021 NVIDIA Corporation. All rights reserved. */
#pragma once

#include "usd.h"

#include <pxr/usd/usdShade/material.h>

struct Main;
struct Material;
struct bNode;
struct bNodeTree;

namespace blender::io::usd {

/* Helper struct used when arranging nodes in columns, keeping track the
 * occupancy information for a given column.  I.e., for column n,
 * column_offsets[n] is the y-offset (from top to bottom) of the occupied
 * region in that column. */
struct NodePlacementContext {
  float origx;
  float origy;
  std::vector<float> column_offsets;
  const float horizontal_step;
  const float vertical_step;

  NodePlacementContext(float in_origx,
                       float in_origy,
                       float in_horizontal_step = 300.0f,
                       float in_vertical_step = 300.0f)
      : origx(in_origx),
        origy(in_origy),
        column_offsets(64, 0.0f),
        horizontal_step(in_horizontal_step),
        vertical_step(in_vertical_step)
  {
  }
};

/* Converts USD materials to Blender representation. */

/**
 * By default, the #USDMaterialReader creates a Blender material with
 * the same name as the USD material.  If the USD material has a
 * #UsdPreviewSurface source, the Blender material's viewport display
 * color, roughness and metallic properties are set to the corresponding
 * #UsdPreoviewSurface inputs.
 *
 * If the Import USD Preview option is enabled, the current implementation
 * converts #UsdPreviewSurface to Blender nodes as follows:
 *
 * - #UsdPreviewSurface -> Principled BSDF
 * - #UsdUVTexture -> Texture Image + Normal Map
 * - UsdPrimvarReader_float2 -> UV Map
 *
 * Limitations: arbitrary primvar readers or UsdTransform2d not yet
 * supported. For #UsdUVTexture, only the file, st and #sourceColorSpace
 * inputs are handled.
 *
 * TODO(makowalski):  Investigate adding support for converting additional
 * shaders and inputs.  Supporting certain types of inputs, such as texture
 * scale and bias, will probably require creating Blender Group nodes with
 * the corresponding inputs.
 */
class USDMaterialReader {
 protected:
  USDImportParams params_;

  Main *bmain_;

 public:
  USDMaterialReader(const USDImportParams &params, Main *bmain);

  Material *add_material(const pxr::UsdShadeMaterial &usd_material) const;

 protected:
  /** Create the Principled BSDF shader node network. */
  void import_usd_preview(Material *mtl, const pxr::UsdShadeShader &usd_shader) const;

  void set_principled_node_inputs(bNode *principled_node,
                                  bNodeTree *ntree,
                                  const pxr::UsdShadeShader &usd_shader) const;

  /** Convert the given USD shader input to an input on the given Blender node. */
  void set_node_input(const pxr::UsdShadeInput &usd_input,
                      bNode *dest_node,
                      const char *dest_socket_name,
                      bNodeTree *ntree,
                      int column,
                      NodePlacementContext *r_ctx) const;

  /**
   * Follow the connected source of the USD input to create corresponding inputs
   * for the given Blender node.
   */
  void follow_connection(const pxr::UsdShadeInput &usd_input,
                         bNode *dest_node,
                         const char *dest_socket_name,
                         bNodeTree *ntree,
                         int column,
                         NodePlacementContext *r_ctx) const;

  void convert_usd_uv_texture(const pxr::UsdShadeShader &usd_shader,
                              const pxr::TfToken &usd_source_name,
                              bNode *dest_node,
                              const char *dest_socket_name,
                              bNodeTree *ntree,
                              int column,
                              NodePlacementContext *r_ctx) const;

  /**
   * Load the texture image node's texture from the path given by the USD shader's
   * file input value.
   */
  void load_tex_image(const pxr::UsdShadeShader &usd_shader, bNode *tex_image) const;

  /**
   * This function creates a Blender UV Map node, under the simplifying assumption that
   * UsdPrimvarReader_float2 shaders output UV coordinates.
   * TODO(makowalski): investigate supporting conversion to other Blender node types
   * (e.g., Attribute Nodes) if needed.
   */
  void convert_usd_primvar_reader_float2(const pxr::UsdShadeShader &usd_shader,
                                         const pxr::TfToken &usd_source_name,
                                         bNode *dest_node,
                                         const char *dest_socket_name,
                                         bNodeTree *ntree,
                                         int column,
                                         NodePlacementContext *r_ctx) const;
};

}  // namespace blender::io::usd
