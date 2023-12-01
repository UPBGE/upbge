/**
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#pragma once

#include "DNA_armature_types.h"
#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_curve_types.h"
#include "DNA_key_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_particle_types.h"
#include "DNA_sound_types.h"
#include "DNA_speaker_types.h"
#include "DNA_text_types.h"
#include "DNA_texture_types.h"
#include "DNA_vfont_types.h"
#include "DNA_volume_types.h"
#include "DNA_world_types.h"

#include "DNA_listBase.h"

typedef struct PythonProxyProperty {
  struct PythonProxyProperty *next, *prev;
  char name[128]; /* 128 = MAX_PROPSTRING */
  short type;
  short boolval;
  int intval;
  float floatval;
  char strval[128]; /* 128 = MAX_PROPSTRING */
  int itemval;
  float vec[4];
  ListBase enumval;
  struct bAction *action;
  struct bArmature *armature;
  struct Camera *camera;
  struct Collection *collection;
  struct Curve *curve;
  struct Image *image;
  struct Key *key;
  struct Library *library;
  struct Light *light;
  struct Material *material;
  struct Mesh *mesh;
  struct MovieClip *movie_clip;
  struct bNodeTree *node_tree;
  struct Object *object;
  struct ParticleSettings *particle_settings;
  struct bSound *sound;
  struct Speaker *speaker;
  struct Text *text;
  struct Tex *texture;
  struct VFont *vector_font;
  struct Volume *volume;
  struct World *world;
} PythonProxyProperty;

typedef struct PythonProxy {
  struct PythonProxy *next, *prev;
  ListBase properties;
  char name[1024];   /* 1024 = FILE_MAX */
  char module[1024]; /* 1024 = FILE_MAX */
  int flag;
  int _pad;
} PythonProxy;

/* PythonProxyProperty.type */
#define PPROP_TYPE_INT 0
#define PPROP_TYPE_FLOAT 1
#define PPROP_TYPE_STRING 2
#define PPROP_TYPE_BOOLEAN 3
#define PPROP_TYPE_SET 4
#define PPROP_TYPE_VEC2 5
#define PPROP_TYPE_VEC3 6
#define PPROP_TYPE_VEC4 7
#define PPROP_TYPE_COL3 8
#define PPROP_TYPE_COL4 9
#define PPROP_TYPE_ACTION 10
#define PPROP_TYPE_ARMATURE 11
#define PPROP_TYPE_CAMERA 12
#define PPROP_TYPE_COLLECTION 13
#define PPROP_TYPE_CURVE 14
#define PPROP_TYPE_IMAGE 15
#define PPROP_TYPE_KEY 16
#define PPROP_TYPE_LIBRARY 17
#define PPROP_TYPE_LIGHT 18
#define PPROP_TYPE_MATERIAL 19
#define PPROP_TYPE_MESH 20
#define PPROP_TYPE_MOVIE_CLIP 21
#define PPROP_TYPE_NODE_TREE 22
#define PPROP_TYPE_OBJECT 23
#define PPROP_TYPE_PARTICLE_SETTINGS 24
#define PPROP_TYPE_SOUND 25
#define PPROP_TYPE_SPEAKER 26
#define PPROP_TYPE_TEXT 27
#define PPROP_TYPE_TEXTURE 28
#define PPROP_TYPE_VECTOR_FONT 29
#define PPROP_TYPE_VOLUME 30
#define PPROP_TYPE_WORLD 31

#define POINTER_TYPES \
  PT_DEF(Action, action, ACTION) \
  PT_DEF(Armature, armature, ARMATURE) \
  PT_DEF(Collection, collection, COLLECTION) \
  PT_DEF(Camera, camera, CAMERA) \
  PT_DEF(Curve, curve, CURVE) \
  PT_DEF(Image, image, IMAGE) \
  PT_DEF(Key, key, KEY) \
  PT_DEF(Library, library, LIBRARY) \
  PT_DEF(Light, light, LIGHT) \
  PT_DEF(Material, material, MATERIAL) \
  PT_DEF(Mesh, mesh, MESH) \
  PT_DEF(MovieClip, movie_clip, MOVIE_CLIP) \
  PT_DEF(NodeTree, node_tree, NODE_TREE) \
  PT_DEF(Object, object, OBJECT) \
  PT_DEF(ParticleSettings, particle_settings, PARTICLE_SETTINGS) \
  PT_DEF(Sound, sound, SOUND) \
  PT_DEF(Speaker, speaker, SPEAKER) \
  PT_DEF(Text, text, TEXT) \
  PT_DEF(Texture, texture, TEXTURE) \
  PT_DEF(VectorFont, vector_font, VECTOR_FONT) \
  PT_DEF(Volume, volume, VOLUME) \
  PT_DEF(World, world, WORLD)

enum { COMPONENT_SHOW = (1 << 0) };
