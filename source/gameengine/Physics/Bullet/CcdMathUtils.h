#pragma once

#include "LinearMath/btMatrix3x3.h"
#include "LinearMath/btQuaternion.h"
#include "LinearMath/btVector3.h"

#include "MT_Matrix3x3.h"
#include "MT_Quaternion.h"
#include "MT_Vector3.h"
#include "MT_Vector4.h"

inline MT_Vector3 ToMoto(const btVector3 &vec)
{
  return MT_Vector3(vec.x(), vec.y(), vec.z());
}

inline MT_Vector4 ToMoto(const btVector4 &vec)
{
  return MT_Vector4(vec.x(), vec.y(), vec.z(), vec.w());
}

inline MT_Matrix3x3 ToMoto(const btMatrix3x3 &mat)
{
  return MT_Matrix3x3(mat[0][0],
                      mat[0][1],
                      mat[0][2],
                      mat[1][0],
                      mat[1][1],
                      mat[1][2],
                      mat[2][0],
                      mat[2][1],
                      mat[2][2]);
}

inline MT_Quaternion ToMoto(const btQuaternion &quat)
{
  return MT_Quaternion(quat.x(), quat.y(), quat.z(), quat.w());
}

inline btVector3 ToBullet(const MT_Vector3 &vec)
{
  return btVector3(vec.x(), vec.y(), vec.z());
}

inline btVector4 ToBullet(const MT_Vector4 &vec)
{
  return btVector4(vec.x(), vec.y(), vec.z(), vec.w());
}

inline btMatrix3x3 ToBullet(const MT_Matrix3x3 &mat)
{
  return btMatrix3x3(mat[0][0],
                     mat[0][1],
                     mat[0][2],
                     mat[1][0],
                     mat[1][1],
                     mat[1][2],
                     mat[2][0],
                     mat[2][1],
                     mat[2][2]);
}

inline btQuaternion ToBullet(const MT_Quaternion &quat)
{
  return btQuaternion(quat.x(), quat.y(), quat.z(), quat.w());
}
