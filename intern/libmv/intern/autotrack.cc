/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2014 Blender Foundation. All rights reserved. */

#include "intern/autotrack.h"
#include "intern/tracksN.h"
#include "intern/utildefines.h"
#include "libmv/autotrack/autotrack.h"

using libmv::TrackRegionOptions;
using libmv::TrackRegionResult;
using mv::AutoTrack;
using mv::FrameAccessor;
using mv::Marker;

libmv_AutoTrack* libmv_autoTrackNew(libmv_FrameAccessor* frame_accessor) {
  return (libmv_AutoTrack*)LIBMV_OBJECT_NEW(AutoTrack,
                                            (FrameAccessor*)frame_accessor);
}

void libmv_autoTrackDestroy(libmv_AutoTrack* libmv_autotrack) {
  LIBMV_OBJECT_DELETE(libmv_autotrack, AutoTrack);
}

void libmv_autoTrackSetOptions(libmv_AutoTrack* libmv_autotrack,
                               const libmv_AutoTrackOptions* options) {
  AutoTrack* autotrack = ((AutoTrack*)libmv_autotrack);
  libmv_configureTrackRegionOptions(options->track_region,
                                    &autotrack->options.track_region);

  autotrack->options.search_region.min(0) = options->search_region.min[0];
  autotrack->options.search_region.min(1) = options->search_region.min[1];
  autotrack->options.search_region.max(0) = options->search_region.max[0];
  autotrack->options.search_region.max(1) = options->search_region.max[1];
}

int libmv_autoTrackMarker(libmv_AutoTrack* libmv_autotrack,
                          const libmv_TrackRegionOptions* libmv_options,
                          libmv_Marker* libmv_tracked_marker,
                          libmv_TrackRegionResult* libmv_result) {
  Marker tracked_marker;
  TrackRegionOptions options;
  TrackRegionResult result;
  libmv_apiMarkerToMarker(*libmv_tracked_marker, &tracked_marker);
  libmv_configureTrackRegionOptions(*libmv_options, &options);
  bool ok = (((AutoTrack*)libmv_autotrack)
                 ->TrackMarker(&tracked_marker, &result, &options));
  libmv_markerToApiMarker(tracked_marker, libmv_tracked_marker);
  libmv_regionTrackergetResult(result, libmv_result);
  return ok && result.is_usable();
}

void libmv_autoTrackAddMarker(libmv_AutoTrack* libmv_autotrack,
                              const libmv_Marker* libmv_marker) {
  Marker marker;
  libmv_apiMarkerToMarker(*libmv_marker, &marker);
  ((AutoTrack*)libmv_autotrack)->AddMarker(marker);
}

void libmv_autoTrackSetMarkers(libmv_AutoTrack* libmv_autotrack,
                               const libmv_Marker* libmv_marker,
                               size_t num_markers) {
  if (num_markers == 0) {
    // Early output.
    return;
  }
  libmv::vector<Marker> markers;
  markers.resize(num_markers);
  for (size_t i = 0; i < num_markers; ++i) {
    libmv_apiMarkerToMarker(libmv_marker[i], &markers[i]);
  }
  ((AutoTrack*)libmv_autotrack)->SetMarkers(&markers);
}

int libmv_autoTrackGetMarker(libmv_AutoTrack* libmv_autotrack,
                             int clip,
                             int frame,
                             int track,
                             libmv_Marker* libmv_marker) {
  Marker marker;
  int ok =
      ((AutoTrack*)libmv_autotrack)->GetMarker(clip, frame, track, &marker);
  if (ok) {
    libmv_markerToApiMarker(marker, libmv_marker);
  }
  return ok;
}
