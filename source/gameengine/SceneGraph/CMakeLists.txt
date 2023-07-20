# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2006 Blender Foundation. All rights reserved.

set(INC
  .
  ../Common
)

set(INC_SYS
  ../../../intern/moto/include
)

set(SRC
  SG_BBox.cpp
  SG_Controller.cpp
  SG_CullingNode.cpp
  SG_Familly.cpp
  SG_Frustum.cpp
  SG_Node.cpp

  SG_BBox.h
  SG_Controller.h
  SG_CullingNode.h
  SG_DList.h
  SG_Familly.h
  SG_Frustum.h
  SG_Node.h
  SG_ParentRelation.h
  SG_QList.h
)

set(LIB
  PRIVATE bf::blenlib
  ge_common
)

blender_add_lib(ge_scenegraph "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
