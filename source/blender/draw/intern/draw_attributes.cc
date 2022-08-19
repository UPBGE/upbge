/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. All rights reserved. */

#include "draw_attributes.h"

/* Return true if the given DRW_AttributeRequest is already in the requests. */
static bool drw_attributes_has_request(const DRW_Attributes *requests, DRW_AttributeRequest req)
{
  for (int i = 0; i < requests->num_requests; i++) {
    const DRW_AttributeRequest src_req = requests->requests[i];
    if (src_req.domain != req.domain) {
      continue;
    }
    if (src_req.layer_index != req.layer_index) {
      continue;
    }
    if (src_req.cd_type != req.cd_type) {
      continue;
    }
    return true;
  }
  return false;
}

static void drw_attributes_merge_requests(const DRW_Attributes *src_requests,
                                          DRW_Attributes *dst_requests)
{
  for (int i = 0; i < src_requests->num_requests; i++) {
    if (dst_requests->num_requests == GPU_MAX_ATTR) {
      return;
    }

    if (drw_attributes_has_request(dst_requests, src_requests->requests[i])) {
      continue;
    }

    dst_requests->requests[dst_requests->num_requests] = src_requests->requests[i];
    dst_requests->num_requests += 1;
  }
}

void drw_attributes_clear(DRW_Attributes *attributes)
{
  memset(attributes, 0, sizeof(DRW_Attributes));
}

void drw_attributes_merge(DRW_Attributes *dst,
                          const DRW_Attributes *src,
                          ThreadMutex *render_mutex)
{
  BLI_mutex_lock(render_mutex);
  drw_attributes_merge_requests(src, dst);
  BLI_mutex_unlock(render_mutex);
}

bool drw_attributes_overlap(const DRW_Attributes *a, const DRW_Attributes *b)
{
  for (int i = 0; i < b->num_requests; i++) {
    if (!drw_attributes_has_request(a, b->requests[i])) {
      return false;
    }
  }

  return true;
}

DRW_AttributeRequest *drw_attributes_add_request(DRW_Attributes *attrs,
                                                 const char *name,
                                                 const eCustomDataType type,
                                                 const int layer_index,
                                                 const eAttrDomain domain)
{
  if (attrs->num_requests >= GPU_MAX_ATTR) {
    return nullptr;
  }

  DRW_AttributeRequest *req = &attrs->requests[attrs->num_requests];
  req->cd_type = type;
  BLI_strncpy(req->attribute_name, name, sizeof(req->attribute_name));
  req->layer_index = layer_index;
  req->domain = domain;
  attrs->num_requests += 1;
  return req;
}

bool drw_custom_data_match_attribute(const CustomData *custom_data,
                                     const char *name,
                                     int *r_layer_index,
                                     eCustomDataType *r_type)
{
  const eCustomDataType possible_attribute_types[8] = {
      CD_PROP_BOOL,
      CD_PROP_INT8,
      CD_PROP_INT32,
      CD_PROP_FLOAT,
      CD_PROP_FLOAT2,
      CD_PROP_FLOAT3,
      CD_PROP_COLOR,
      CD_PROP_BYTE_COLOR,
  };

  for (int i = 0; i < ARRAY_SIZE(possible_attribute_types); i++) {
    const eCustomDataType attr_type = possible_attribute_types[i];
    int layer_index = CustomData_get_named_layer(custom_data, attr_type, name);
    if (layer_index == -1) {
      continue;
    }

    *r_layer_index = layer_index;
    *r_type = attr_type;
    return true;
  }

  return false;
}
