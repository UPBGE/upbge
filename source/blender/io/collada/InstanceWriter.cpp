/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */

#include <sstream>
#include <string>

#include "COLLADASWInstanceMaterial.h"

#include "BKE_customdata.h"
#include "BKE_material.h"

#include "DNA_mesh_types.h"

#include "InstanceWriter.h"
#include "collada_internal.h"
#include "collada_utils.h"

void InstanceWriter::add_material_bindings(COLLADASW::BindMaterial &bind_material,
                                           Object *ob,
                                           bool active_uv_only)
{
  for (int a = 0; a < ob->totcol; a++) {
    Material *ma = BKE_object_material_get(ob, a + 1);

    COLLADASW::InstanceMaterialList &iml = bind_material.getInstanceMaterialList();

    if (ma) {
      std::string matid(get_material_id(ma));
      matid = translate_id(matid);
      std::ostringstream ostr;
      ostr << matid;
      COLLADASW::InstanceMaterial im(ostr.str(),
                                     COLLADASW::URI(COLLADABU::Utils::EMPTY_STRING, matid));

      // create <bind_vertex_input> for each uv map
      Mesh *me = (Mesh *)ob->data;

      int num_layers = CustomData_number_of_layers(&me->ldata, CD_MLOOPUV);

      int map_index = 0;
      int active_uv_index = CustomData_get_active_layer_index(&me->ldata, CD_MLOOPUV);
      for (int b = 0; b < num_layers; b++) {
        if (!active_uv_only || b == active_uv_index) {
          char *name = bc_CustomData_get_layer_name(&me->ldata, CD_MLOOPUV, b);
          im.push_back(COLLADASW::BindVertexInput(name, "TEXCOORD", map_index++));
        }
      }

      iml.push_back(im);
    }
  }
}
