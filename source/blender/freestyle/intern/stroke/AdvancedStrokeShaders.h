/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup freestyle
 * \brief Fredo's stroke shaders
 */

#include "BasicStrokeShaders.h"

namespace Freestyle {

/** [ Thickness Shader ].
 *  Assigns thicknesses to the stroke vertices so that the stroke looks like made with a
 * calligraphic tool. i.e. The stroke will be the thickest in a main direction, the thinnest in the
 * direction perpendicular to this one, and an interpolation in between.
 */
class CalligraphicShader : public StrokeShader {
 public:
  /** Builds the shader.
   *  \param iMinThickness:
   *    The minimum thickness in the direction perpendicular to the main direction.
   *  \param iMaxThickness:
   *    The maximum thickness in the main direction.
   *  \param iOrientation:
   *    The 2D vector giving the main direction.
   *  \param clamp:
   *    Tells ???
   */
  CalligraphicShader(real iMinThickness,
                     real iMaxThickness,
                     const Vec2f &iOrientation,
                     bool clamp);

  /** Destructor. */
  virtual ~CalligraphicShader()
  {
  }

  /** The shading method */
  virtual int shade(Stroke &ioStroke) const;

 protected:
  real _maxThickness;
  real _minThickness;
  Vec2f _orientation;
  bool _clamp;
};

/** [ Geometry Shader ].
 *  Spatial Noise stroke shader.
 *  Moves the vertices to make the stroke more noisy.
 * \see \htmlonly <a href=noise/noise.html>noise/noise.html</a> \endhtmlonly
 */
class SpatialNoiseShader : public StrokeShader {
 public:
  /** Builds the shader.
   *  \param iAmount:
   *    The amplitude of the noise.
   *  \param ixScale:
   *    The noise frequency
   *  \param nbOctave:
   *    The number of octaves
   *  \param smooth:
   *    If you want the noise to be smooth
   *  \param pureRandom:
   *    If you don't want any coherence
   */
  SpatialNoiseShader(float iAmount, float ixScale, int nbOctave, bool smooth, bool pureRandom);

  /** Destructor. */
  virtual ~SpatialNoiseShader()
  {
  }

  /** The shading method. */
  virtual int shade(Stroke &ioStroke) const;

 protected:
  float _amount;
  float _xScale;
  int _nbOctave;
  bool _smooth;
  bool _pureRandom;
};

/** [ Geometry Shader ].
 *  Smooths the stroke.
 *  (Moves the vertices to make the stroke smoother).
 *  Uses curvature flow to converge towards a curve of constant curvature. The diffusion method we
 * use is anisotropic to prevent the diffusion across corners. \see \htmlonly <a
 * href=/smoothing/smoothing.html>smoothing/smoothing.html</a> \endhtmlonly
 */
class SmoothingShader : public StrokeShader {
 public:
  /** Builds the shader.
   *  \param iNbIteration:
   *    The number of iterations. (400)
   *  \param iFactorPoint:
   *    0
   *  \param ifactorCurvature:
   *    0
   *  \param iFactorCurvatureDifference:
   *    0.2
   *  \param iAnisoPoint:
   *    0
   *  \param iAnisoNormal:
   *    0
   *  \param iAnisoCurvature:
   *    0
   *  \param icarricatureFactor:
   *    1
   */
  SmoothingShader(int iNbIteration,
                  real iFactorPoint,
                  real ifactorCurvature,
                  real iFactorCurvatureDifference,
                  real iAnisoPoint,
                  real iAnisoNormal,
                  real iAnisoCurvature,
                  real icarricatureFactor);

  /** Destructor. */
  virtual ~SmoothingShader()
  {
  }

  /** The shading method. */
  virtual int shade(Stroke &ioStroke) const;

 protected:
  int _nbIterations;
  real _factorPoint;
  real _factorCurvature;
  real _factorCurvatureDifference;
  real _anisoPoint;
  real _anisoNormal;
  real _anisoCurvature;
  real _carricatureFactor;
};

class Smoother {
 public:
  Smoother(Stroke &ioStroke);

  virtual ~Smoother();

  void smooth(int nbIterations,
              real iFactorPoint,
              real ifactorCurvature,
              real iFactorCurvatureDifference,
              real iAnisoPoint,
              real iAnisoNormal,
              real iAnisoCurvature,
              real icarricatureFactor);

  void computeCurvature();

 protected:
  real _factorPoint;
  real _factorCurvature;
  real _factorCurvatureDifference;
  real _anisoPoint;
  real _anisoNormal;
  real _anisoCurvature;
  real _carricatureFactor;

  void iteration();
  void copyVertices();

  Stroke *_stroke;
  int _nbVertices;
  Vec2r *_vertex;
  Vec2r *_normal;
  real *_curvature;
  bool *_isFixedVertex;

  bool _isClosedCurve;
  bool _safeTest;
};

class Omitter : public Smoother {
 public:
  Omitter(Stroke &ioStroke);

  virtual ~Omitter()
  {
  }

  void omit(real sizeWindow, real thrVari, real thrFlat, real lFlat);

 protected:
  real *_u;

  real _sizeWindow;
  real _thresholdVariation;
  real _thresholdFlat;
  real _lengthFlat;
};

/** Omission shader */
class OmissionShader : public StrokeShader {
 public:
  OmissionShader(real sizeWindow, real thrVari, real thrFlat, real lFlat);
  virtual ~OmissionShader()
  {
  }

  virtual int shade(Stroke &ioStroke) const;

 protected:
  real _sizeWindow;
  real _thresholdVariation;
  real _thresholdFlat;
  real _lengthFlat;
};

} /* namespace Freestyle */
