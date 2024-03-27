/* SPDX-FileCopyrightText: 2004-2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_defs.h"
#include "DNA_listBase.h"

struct BodySpring;

/** #PartDeflect.forcefield: Effector Fields types. */
typedef enum ePFieldType {
  /** (this is used for general effector weight). */
  PFIELD_NULL = 0,
  /** Force away/towards a point depending on force strength. */
  PFIELD_FORCE = 1,
  /** Force around the effector normal. */
  PFIELD_VORTEX = 2,
  /** Force from the cross product of effector normal and point velocity. */
  PFIELD_MAGNET = 3,
  /** Force away and towards a point depending which side of the effector normal the point is. */
  PFIELD_WIND = 4,
  /** Force along curve for dynamics, a shaping curve for hair paths. */
  PFIELD_GUIDE = 5,
  /** Force based on texture values calculated at point coordinates. */
  PFIELD_TEXTURE = 6,
  /** Force of a harmonic (damped) oscillator. */
  PFIELD_HARMONIC = 7,
  /** Force away/towards a point depending on point charge. */
  PFIELD_CHARGE = 8,
  /** Force due to a Lennard-Jones potential. */
  PFIELD_LENNARDJ = 9,
  /** Defines predator / goal for boids. */
  PFIELD_BOID = 10,
  /** Force defined by BLI_noise_generic_turbulence. */
  PFIELD_TURBULENCE = 11,
  /** Linear & quadratic drag. */
  PFIELD_DRAG = 12,
  /** Force based on fluid simulation velocities. */
  PFIELD_FLUIDFLOW = 13,

  /* Keep last. */
  NUM_PFIELD_TYPES,
} ePFieldType;

typedef struct PartDeflect {
  /** General settings flag. */
  int flag;
  /** Deflection flag - does mesh deflect particles. */
  short deflect;
  /** Force field type, do the vertices attract / repel particles? */
  short forcefield;
  /** Fall-off type. */
  short falloff;
  /** Point, plane or surface. */
  short shape;
  /** Texture effector. */
  short tex_mode;
  /** For curve guide. */
  short kink, kink_axis;
  short zdir;

  /* Main effector values */
  /** The strength of the force (+ or - ). */
  float f_strength;
  /** Damping ratio of the harmonic effector. */
  float f_damp;
  /**
   * How much force is converted into "air flow", i.e.
   * force used as the velocity of surrounding medium. */
  float f_flow;
  /** How much force is reduced when acting parallel to a surface, e.g. cloth. */
  float f_wind_factor;

  char _pad0[4];

  /** Noise size for noise effector, restlength for harmonic effector. */
  float f_size;

  /* fall-off */
  /** The power law - real gravitation is 2 (square). */
  float f_power;
  /** If indicated, use this maximum. */
  float maxdist;
  /** If indicated, use this minimum. */
  float mindist;
  /** Radial fall-off power. */
  float f_power_r;
  /** Radial versions of above. */
  float maxrad;
  float minrad;

  /* particle collisions */
  /** Damping factor for particle deflection. */
  float pdef_damp;
  /** Random element of damping for deflection. */
  float pdef_rdamp;
  /** Chance of particle passing through mesh. */
  float pdef_perm;
  /** Friction factor for particle deflection. */
  float pdef_frict;
  /** Random element of friction for deflection. */
  float pdef_rfrict;
  /** Surface particle stickiness. */
  float pdef_stickness;

  /** Used for forces. */
  float absorption;

  /* softbody collisions */
  /** Damping factor for softbody deflection. */
  float pdef_sbdamp;
  /** Inner face thickness for softbody deflection. */
  float pdef_sbift;
  /** Outer face thickness for softbody deflection. */
  float pdef_sboft;

  /* guide curve, same as for particle child effects */
  float clump_fac, clump_pow;
  float kink_freq, kink_shape, kink_amp, free_end;

  /* texture effector */
  /** Used for calculating partial derivatives. */
  float tex_nabla;
  /** Texture of the texture effector. */
  struct Tex *tex;

  /* effector noise */
  /** Noise of force. */
  float f_noise;
  /** Noise random seed. */
  int seed;

  /* Display Size */
  /** Runtime only : start of the curve or draw scale. */
  float drawvec1[4];
  /** Runtime only : end of the curve. */
  float drawvec2[4];
  /** Runtime only. */
  float drawvec_falloff_min[3];
  char _pad1[4];
  /** Runtime only. */
  float drawvec_falloff_max[3];
  char _pad2[4];

  /** Force source object. */
  struct Object *f_source;

  /** Friction of cloth collisions. */
  float pdef_cfrict;
  char _pad[4];
} PartDeflect;

typedef struct EffectorWeights {
  /** Only use effectors from this group of objects. */
  struct Collection *group;

  /** Effector type specific weights. */
  float weight[14];
  float global_gravity;
  short flag;
  char _pad[2];
} EffectorWeights;

/** #EffectorWeights::flag */
enum {
  EFF_WEIGHT_DO_HAIR = 1,
};

typedef struct SBVertex {
  float vec[4];
} SBVertex;

/* Container for data that is shared among evaluated copies.
 *
 * This is placed in a separate struct so that values can be changed
 * without having to update all evaluated copies. */
typedef struct SoftBody_Shared {
  struct PointCache *pointcache;
  struct ListBase ptcaches;
} SoftBody_Shared;

typedef struct BulletSoftBody {
  int flag;       /* various boolean options */
  float linStiff; /* linear stiffness 0..1 */
  float angStiff; /* angular stiffness 0..1 */
  float volume;   /* volume preservation 0..1 */

  int viterations; /* Velocities solver iterations */
  int piterations; /* Positions solver iterations */
  int diterations; /* Drift solver iterations */
  int citerations; /* Cluster solver iterations */

  float kSRHR_CL;    /* Soft vs rigid hardness [0,1] (cluster only) */
  float kSKHR_CL;    /* Soft vs kinetic hardness [0,1] (cluster only) */
  float kSSHR_CL;    /* Soft vs soft hardness [0,1] (cluster only) */
  float kSR_SPLT_CL; /* Soft vs rigid impulse split [0,1] (cluster only) */

  float kSK_SPLT_CL; /* Soft vs rigid impulse split [0,1] (cluster only) */
  float kSS_SPLT_CL; /* Soft vs rigid impulse split [0,1] (cluster only) */
  float kVCF;        /* Velocities correction factor (Baumgarte) */
  float kDP;         /* Damping coefficient [0,1] */

  float kDG; /* Drag coefficient [0,+inf] */
  float kLF; /* Lift coefficient [0,+inf] */
  float kPR; /* Pressure coefficient [-inf,+inf] */
  float kVC; /* Volume conversation coefficient [0,+inf] */

  float kDF;  /* Dynamic friction coefficient [0,1] */
  float kMT;  /* Pose matching coefficient [0,1] */
  float kCHR; /* Rigid contacts hardness [0,1] */
  float kKHR; /* Kinetic contacts hardness [0,1] */

  float kSHR;         /* Soft contacts hardness [0,1] */
  float kAHR;         /* Anchors hardness [0,1] */
  int collisionflags; /* Vertex/Face or Signed Distance Field(SDF) or Clusters, Soft versus Soft or
                         Rigid */
  int numclusteriterations; /* number of iterations to refine collision clusters*/
  int bending_dist;         /* Bending constraint distance */
  float welding;            /* welding limit to remove duplicate/nearby vertices, 0.0..0.01 */
  float margin;             /* margin specific to softbody */
  int _pad;
} BulletSoftBody;

/* BulletSoftBody.flag */
#define OB_BSB_SHAPE_MATCHING 2
// #define OB_BSB_UNUSED 4
#define OB_BSB_BENDING_CONSTRAINTS 8
#define OB_BSB_AERO_VPOINT 16 /* aero model, Vertex normals are oriented toward velocity*/
// #define OB_BSB_AERO_VTWOSIDE 32 /* aero model, Vertex normals are flipped to match velocity */

/* BulletSoftBody.collisionflags */
#define OB_BSB_COL_SDF_RS 2 /* SDF based rigid vs soft */
#define OB_BSB_COL_CL_RS 4  /* Cluster based rigid vs soft */
#define OB_BSB_COL_CL_SS 8  /* Cluster based soft vs soft */
#define OB_BSB_COL_VF_SS 16 /* Vertex/Face based soft vs soft */

typedef struct SoftBody {
  /* dynamic data */
  int totpoint, totspring;
  /** Not saved in file. */
  struct BodyPoint *bpoint;
  /** Not saved in file. */
  struct BodySpring *bspring;
  char _pad;
  char msg_lock;
  short msg_value;

  /* part of UI: */

  /* general options */
  /** Softbody mass of *vertex*. */
  float nodemass;
  /**
   * Along with it introduce mass painting starting to fix old bug .. nastiness that VG are indexes
   * rather find them by name tag to find it -> jow20090613. #MAX_VGROUP_NAME size.
   */
  char namedVG_Mass[64];
  /** Softbody amount of gravitation to apply. */
  float grav;
  /** Friction to env. */
  float mediafrict;
  /** Error limit for ODE solver. */
  float rklimit;
  /** User control over simulation speed. */
  float physics_speed;

  /* goal */
  /** Softbody goal springs. */
  float goalspring;
  /** Softbody goal springs friction. */
  float goalfrict;
  /** Quick limits for goal. */
  float mingoal;
  float maxgoal;
  /** Default goal for vertices without vgroup. */
  float defgoal;
  /** Index starting at 1. */
  short vertgroup;
  /**
   * Starting to fix old bug .. nastiness that VG are indexes
   * rather find them by name tag to find it -> jow20090613. #MAX_VGROUP_NAME size.
   */
  char namedVG_Softgoal[64];

  short fuzzyness;

  /* springs */
  /** Softbody inner springs. */
  float inspring;
  /** Softbody inner springs friction. */
  float infrict;
  /**
   * Along with it introduce Spring_K painting
   * starting to fix old bug .. nastiness that VG are indexes
   * rather find them by name tag to find it -> jow20090613.
   * MAX_VGROUP_NAME
   */
  char namedVG_Spring_K[64];

  /* baking */
  char _pad1[6];
  /** Local==1: use local coords for baking. */
  char local, solverflags;

  /* -- these must be kept for backwards compatibility -- */
  /** Array of size totpointkey. */
  SBVertex **keys;
  /** If totpointkey != totpoint or totkey!- (efra-sfra)/interval -> free keys. */
  int totpointkey, totkey;
  /* ---------------------------------------------------- */
  float secondspring;

  /* Self collision. */
  /** Fixed collision ball size if > 0. */
  float colball;
  /** Cooling down collision response. */
  float balldamp;
  /** Pressure the ball is loaded with. */
  float ballstiff;
  short sbc_mode;
  short aeroedge;
  short minloops;
  short maxloops;
  short choke;
  short solver_ID;
  short plastic;
  short springpreload;

  /** Scratchpad/cache on live time not saved in file. */
  struct SBScratch *scratch;
  float shearstiff;
  float inpush;

  struct SoftBody_Shared *shared;
  /** Moved to SoftBody_Shared. */
  struct PointCache *pointcache DNA_DEPRECATED;
  /** Moved to SoftBody_Shared. */
  struct ListBase ptcaches DNA_DEPRECATED;

  struct Collection *collision_group;

  struct EffectorWeights *effector_weights;
  /* Reverse estimated object-matrix (run-time data, no need to store in the file). */
  float lcom[3];
  float lrot[3][3];
  float lscale[3][3];

  int last_frame;
} SoftBody;

/** #PartDeflect::flag: various settings. */
enum {
  PFIELD_USEMAX = 1 << 0,
  // PDEFLE_DEFORM = 1 << 1, /* UNUSED */
  /** TODO: do_versions for below */
  PFIELD_GUIDE_PATH_ADD = 1 << 2,
  /** used for do_versions */
  PFIELD_PLANAR = 1 << 3,
  PDEFLE_KILL_PART = 1 << 4,
  /** used for do_versions */
  PFIELD_POSZ = 1 << 5,
  PFIELD_TEX_OBJECT = 1 << 6,
  /** used for turbulence */
  PFIELD_GLOBAL_CO = 1 << 6,
  PFIELD_TEX_2D = 1 << 7,
  /** used for harmonic force */
  PFIELD_MULTIPLE_SPRINGS = 1 << 7,
  PFIELD_USEMIN = 1 << 8,
  PFIELD_USEMAXR = 1 << 9,
  PFIELD_USEMINR = 1 << 10,
  PFIELD_TEX_ROOTCO = 1 << 11,
  /** used for do_versions */
  PFIELD_SURFACE = 1 << 12,
  PFIELD_VISIBILITY = 1 << 13,
  PFIELD_DO_LOCATION = 1 << 14,
  PFIELD_DO_ROTATION = 1 << 15,
  /** apply curve weights */
  PFIELD_GUIDE_PATH_WEIGHT = 1 << 16,
  /** multiply smoke force by density */
  PFIELD_SMOKE_DENSITY = 1 << 17,
  /** used for (simple) force */
  PFIELD_GRAVITATION = 1 << 18,
  /** Enable cloth collision side detection based on normal. */
  PFIELD_CLOTH_USE_CULLING = 1 << 19,
  /** Replace collision direction with collider normal. */
  PFIELD_CLOTH_USE_NORMAL = 1 << 20,
};

/** #PartDeflect::falloff */
enum {
  PFIELD_FALL_SPHERE = 0,
  PFIELD_FALL_TUBE = 1,
  PFIELD_FALL_CONE = 2,
};

/** #PartDeflect::shape */
enum {
  PFIELD_SHAPE_POINT = 0,
  PFIELD_SHAPE_PLANE = 1,
  PFIELD_SHAPE_SURFACE = 2,
  PFIELD_SHAPE_POINTS = 3,
  PFIELD_SHAPE_LINE = 4,
};

/** #PartDeflect::tex_mode */
enum {
  PFIELD_TEX_RGB = 0,
  PFIELD_TEX_GRAD = 1,
  PFIELD_TEX_CURL = 2,
};

/** #PartDeflect::zdir */
enum {
  PFIELD_Z_BOTH = 0,
  PFIELD_Z_POS = 1,
  PFIELD_Z_NEG = 2,
};

/** #Object::softflag */
enum {
  OB_SB_ENABLE = 1 << 0, /* Deprecated (use modifier). */
  OB_SB_GOAL = 1 << 1,
  OB_SB_EDGES = 1 << 2,
  OB_SB_QUADS = 1 << 3,
  OB_SB_POSTDEF = 1 << 4,
  // OB_SB_REDO = 1 << 5,
  // OB_SB_BAKESET = 1 << 6,
  // OB_SB_BAKEDO = 1 << 7,
  // OB_SB_RESET = 1 << 8,
  OB_SB_SELF = 1 << 9,
  OB_SB_FACECOLL = 1 << 10,
  OB_SB_EDGECOLL = 1 << 11,
  // OB_SB_COLLFINAL = 1 << 12,  /* Deprecated. */
  // OB_SB_BIG_UI = 1 << 13,     /* Deprecated. */
  OB_SB_AERO_ANGLE = 1 << 14,
};

/** #SoftBody::solverflags */
enum {
  SBSO_MONITOR = 1 << 0,
  SBSO_OLDERR = 1 << 1,
  SBSO_ESTIMATEIPO = 1 << 2,
};

/** #SoftBody::sbc_mode */
enum {
  SBC_MODE_MANUAL = 0,
  SBC_MODE_AVG = 1,
  SBC_MODE_MIN = 2,
  SBC_MODE_MAX = 3,
  SBC_MODE_AVGMINMAX = 4,
};
