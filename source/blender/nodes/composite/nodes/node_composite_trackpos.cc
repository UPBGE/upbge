/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_index_range.hh"
#include "BLI_math_vector_types.hh"

#include "DNA_defaults.h"
#include "DNA_movieclip_types.h"
#include "DNA_tracking_types.h"

#include "BKE_context.h"
#include "BKE_lib_id.h"
#include "BKE_movieclip.h"
#include "BKE_tracking.h"

#include "RNA_access.h"
#include "RNA_prototypes.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_trackpos_cc {

NODE_STORAGE_FUNCS(NodeTrackPosData)

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

    if (tracking_object->active_track) {
      BLI_strncpy(data->track_name, tracking_object->active_track->name, sizeof(data->track_name));
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
    MovieTrackingObject *tracking_object;
    uiLayout *col;
    PointerRNA tracking_ptr;
    NodeTrackPosData *data = (NodeTrackPosData *)node->storage;

    RNA_pointer_create(&clip->id, &RNA_MovieTracking, tracking, &tracking_ptr);

    col = uiLayoutColumn(layout, false);
    uiItemPointerR(col, ptr, "tracking_object", &tracking_ptr, "objects", "", ICON_OBJECT_DATA);

    tracking_object = BKE_tracking_object_get_named(tracking, data->tracking_object);
    if (tracking_object) {
      PointerRNA object_ptr;

      RNA_pointer_create(&clip->id, &RNA_MovieTrackingObject, tracking_object, &object_ptr);

      uiItemPointerR(col, ptr, "track_name", &object_ptr, "tracks", "", ICON_ANIM_DATA);
    }
    else {
      uiItemR(layout, ptr, "track_name", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_ANIM_DATA);
    }

    uiItemR(layout, ptr, "position", UI_ITEM_R_SPLIT_EMPTY_NAME, nullptr, ICON_NONE);

    if (ELEM(node->custom1,
             CMP_NODE_TRACK_POSITION_RELATIVE_FRAME,
             CMP_NODE_TRACK_POSITION_ABSOLUTE_FRAME)) {
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
    MovieTrackingTrack *track = get_movie_tracking_track();

    if (!track) {
      execute_invalid();
      return;
    }

    const float2 current_marker_position = compute_marker_position_at_frame(track, get_frame());
    const int2 size = get_size();

    execute_position(track, current_marker_position, size);
    execute_speed(track, current_marker_position, size);
  }

  void execute_position(MovieTrackingTrack *track, float2 current_marker_position, int2 size)
  {
    const bool should_compute_x = should_compute_output("X");
    const bool should_compute_y = should_compute_output("Y");
    if (!should_compute_x && !should_compute_y) {
      return;
    }

    /* Compute the position relative to the reference marker position. Multiply by the size to get
     * the position in pixel space. */
    const float2 reference_marker_position = compute_reference_marker_position(track);
    const float2 position = (current_marker_position - reference_marker_position) * float2(size);

    if (should_compute_x) {
      Result &result = get_result("X");
      result.allocate_single_value();
      result.set_float_value(position.x);
    }

    if (should_compute_y) {
      Result &result = get_result("Y");
      result.allocate_single_value();
      result.set_float_value(position.y);
    }
  }

  void execute_speed(MovieTrackingTrack *track, float2 current_marker_position, int2 size)
  {
    if (!should_compute_output("Speed")) {
      return;
    }

    /* Compute the speed as the difference between the previous marker position and the current
     * marker position. Notice that we compute the speed from the current to the previous position,
     * not the other way around. */
    const float2 previous_marker_position = compute_temporally_neighbouring_marker_position(
        track, current_marker_position, -1);
    const float2 speed_toward_previous = previous_marker_position - current_marker_position;

    /* Compute the speed as the difference between the current marker position and the next marker
     * position. */
    const float2 next_marker_position = compute_temporally_neighbouring_marker_position(
        track, current_marker_position, 1);
    const float2 speed_toward_next = current_marker_position - next_marker_position;

    /* Encode both speeds in a 4D vector. Multiply by the size to get the speed in pixel space. */
    const float4 speed = float4(speed_toward_previous, speed_toward_next) * float4(size, size);

    Result &result = get_result("Speed");
    result.allocate_single_value();
    result.set_vector_value(speed);
  }

  void execute_invalid()
  {
    if (should_compute_output("X")) {
      Result &result = get_result("X");
      result.allocate_single_value();
      result.set_float_value(0.0f);
    }
    if (should_compute_output("Y")) {
      Result &result = get_result("Y");
      result.allocate_single_value();
      result.set_float_value(0.0f);
    }
    if (should_compute_output("Speed")) {
      Result &result = get_result("Speed");
      result.allocate_single_value();
      result.set_vector_value(float4(0.0f));
    }
  }

  /* Compute the position of the marker that is delta time away from the evaluation frame. If no
   * marker exist for that particular frame or is disabled, the current marker position is
   * returned. This is useful for computing the speed by providing small negative and positive
   * delta times. */
  float2 compute_temporally_neighbouring_marker_position(MovieTrackingTrack *track,
                                                         float2 current_marker_position,
                                                         int time_delta)
  {
    const int local_frame_number = BKE_movieclip_remap_scene_to_clip_frame(
        get_movie_clip(), get_frame() + time_delta);
    MovieTrackingMarker *marker = BKE_tracking_marker_get_exact(track, local_frame_number);

    if (marker == nullptr || marker->flag & MARKER_DISABLED) {
      return current_marker_position;
    }

    return float2(marker->pos);
  }

  /* Compute the position of the reference marker which the output position will be computed
   * relative to. For non-relative modes, this is just the zero origin or the tracking space. See
   * the get_mode() method for more information. */
  float2 compute_reference_marker_position(MovieTrackingTrack *track)
  {
    switch (get_mode()) {
      case CMP_NODE_TRACK_POSITION_RELATIVE_START:
        return compute_first_marker_position(track);
      case CMP_NODE_TRACK_POSITION_RELATIVE_FRAME:
        return compute_marker_position_at_frame(track, get_relative_frame());
      default:
        return float2(0.0f);
    }
  }

  /* Compute the position of the first non-disabled marker in the track. */
  float2 compute_first_marker_position(MovieTrackingTrack *track)
  {
    for (const int i : IndexRange(track->markersnr)) {
      MovieTrackingMarker &marker = track->markers[i];

      if (marker.flag & MARKER_DISABLED) {
        continue;
      }

      return float2(marker.pos);
    }

    return float2(0.0f);
  }

  /* Compute the marker position at the given frame, if no such marker exist, return the position
   * of the temporally nearest marker before it, if no such marker exist, return the position of
   * the temporally nearest marker after it. */
  float2 compute_marker_position_at_frame(MovieTrackingTrack *track, int frame)
  {
    const int local_frame_number = BKE_movieclip_remap_scene_to_clip_frame(get_movie_clip(),
                                                                           frame);
    MovieTrackingMarker *marker = BKE_tracking_marker_get(track, local_frame_number);
    return float2(marker->pos);
  }

  /* Get the movie tracking track corresponding to the given object and track names. If no such
   * track exist, return nullptr. */
  MovieTrackingTrack *get_movie_tracking_track()
  {
    MovieClip *movie_clip = get_movie_clip();
    if (!movie_clip) {
      return nullptr;
    }

    MovieTracking *movie_tracking = &movie_clip->tracking;

    MovieTrackingObject *movie_tracking_object = BKE_tracking_object_get_named(
        movie_tracking, node_storage(bnode()).tracking_object);
    if (!movie_tracking_object) {
      return nullptr;
    }

    return BKE_tracking_object_find_track_with_name(movie_tracking_object,
                                                    node_storage(bnode()).track_name);
  }

  /* Get the size of the movie clip at the evaluation frame. This is constant for all frames in
   * most cases. */
  int2 get_size()
  {
    MovieClipUser user = *DNA_struct_default_get(MovieClipUser);
    BKE_movieclip_user_set_frame(&user, get_frame());

    int2 size;
    BKE_movieclip_get_size(get_movie_clip(), &user, &size.x, &size.y);

    return size;
  }

  /* In the CMP_NODE_TRACK_POSITION_RELATIVE_FRAME mode, this represents the offset that will be
   * added to the current scene frame. See the get_mode() method for more information. */
  int get_relative_frame()
  {
    return bnode().custom2;
  }

  /* Get the frame where the marker will be retrieved. This is the absolute frame for the absolute
   * mode and the current scene frame otherwise. */
  int get_frame()
  {
    if (get_mode() == CMP_NODE_TRACK_POSITION_ABSOLUTE_FRAME) {
      return get_absolute_frame();
    }

    return context().get_frame_number();
  }

  /* In the CMP_NODE_TRACK_POSITION_ABSOLUTE_FRAME mode, this represents the frame where the marker
   * will be retrieved. See the get_mode() method for more information. */
  int get_absolute_frame()
  {
    return bnode().custom2;
  }

  /* CMP_NODE_TRACK_POSITION_ABSOLUTE:
   *   Returns the position and speed of the marker at the current scene frame relative to the zero
   *   origin of the tracking space.
   *
   * CMP_NODE_TRACK_POSITION_RELATIVE_START:
   *   Returns the position and speed of the marker at the current scene frame relative to the
   *   position of the first non-disabled marker in the track.
   *
   * CMP_NODE_TRACK_POSITION_RELATIVE_FRAME:
   *   Returns the position and speed of the marker at the current scene frame relative to the
   *   position of the marker at the current scene frame plus the user given relative frame.
   *
   * CMP_NODE_TRACK_POSITION_ABSOLUTE_FRAME:
   *   Returns the position and speed of the marker at the given absolute frame. */
  CMPNodeTrackPositionMode get_mode()
  {
    return static_cast<CMPNodeTrackPositionMode>(bnode().custom1);
  }

  MovieClip *get_movie_clip()
  {
    return (MovieClip *)bnode().id;
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
