/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "DNA_movieclip_types.h"
#include "DNA_tracking_types.h"

#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_tracking.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_trackpos_cc {

static void cmp_node_trackpos_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>(N_("X"));
  b.add_output<decl::Float>(N_("Y"));
  b.add_output<decl::Vector>(N_("Speed")).subtype(PROP_VELOCITY);
}

static void init(const bContext *C, PointerRNA *ptr)
{
  bNode *node = (bNode *)ptr->data;

  NodeTrackPosData *data = MEM_cnew<NodeTrackPosData>(__func__);
  node->storage = data;

  const Scene *scene = CTX_data_scene(C);
  if (scene->clip) {
    MovieClip *clip = scene->clip;
    MovieTracking *tracking = &clip->tracking;

    node->id = &clip->id;
    id_us_plus(&clip->id);

    const MovieTrackingObject *tracking_object = BKE_tracking_object_get_active(tracking);
    BLI_strncpy(data->tracking_object, tracking_object->name, sizeof(data->tracking_object));

    const MovieTrackingTrack *active_track = BKE_tracking_track_get_active(tracking);
    if (active_track) {
      BLI_strncpy(data->track_name, active_track->name, sizeof(data->track_name));
    }
  }
}

static void node_composit_buts_trackpos(uiLayout *layout, bContext *C, PointerRNA *ptr)
{
  bNode *node = (bNode *)ptr->data;

  uiTemplateID(layout,
               C,
               ptr,
               "clip",
               nullptr,
               "CLIP_OT_open",
               nullptr,
               UI_TEMPLATE_ID_FILTER_ALL,
               false,
               nullptr);

  if (node->id) {
    MovieClip *clip = (MovieClip *)node->id;
    MovieTracking *tracking = &clip->tracking;
    MovieTrackingObject *object;
    uiLayout *col;
    PointerRNA tracking_ptr;
    NodeTrackPosData *data = (NodeTrackPosData *)node->storage;

    RNA_pointer_create(&clip->id, &RNA_MovieTracking, tracking, &tracking_ptr);

    col = uiLayoutColumn(layout, false);
    uiItemPointerR(col, ptr, "tracking_object", &tracking_ptr, "objects", "", ICON_OBJECT_DATA);

    object = BKE_tracking_object_get_named(tracking, data->tracking_object);
    if (object) {
      PointerRNA object_ptr;

      RNA_pointer_create(&clip->id, &RNA_MovieTrackingObject, object, &object_ptr);

      uiItemPointerR(col, ptr, "track_name", &object_ptr, "tracks", "", ICON_ANIM_DATA);
    }
    else {
      uiItemR(layout, ptr, "track_name", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_ANIM_DATA);
    }

    uiItemR(layout, ptr, "position", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);

    if (ELEM(node->custom1, CMP_TRACKPOS_RELATIVE_FRAME, CMP_TRACKPOS_ABSOLUTE_FRAME)) {
      uiItemR(layout, ptr, "frame_relative", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);
    }
  }
}

using namespace blender::realtime_compositor;

class TrackPositionOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    get_result("X").allocate_invalid();
    get_result("Y").allocate_invalid();
    get_result("Speed").allocate_invalid();
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new TrackPositionOperation(context, node);
}

}  // namespace blender::nodes::node_composite_trackpos_cc

void register_node_type_cmp_trackpos()
{
  namespace file_ns = blender::nodes::node_composite_trackpos_cc;

  static bNodeType ntype;

  cmp_node_type_base(&ntype, CMP_NODE_TRACKPOS, "Track Position", NODE_CLASS_INPUT);
  ntype.declare = file_ns::cmp_node_trackpos_declare;
  ntype.draw_buttons = file_ns::node_composit_buts_trackpos;
  ntype.initfunc_api = file_ns::init;
  node_type_storage(
      &ntype, "NodeTrackPosData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  nodeRegisterType(&ntype);
}
