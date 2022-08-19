# SPDX-License-Identifier: GPL-2.0-or-later

#  Filename : polygonalize.py
#  Author   : Stephane Grabli
#  Date     : 04/08/2005
#  Purpose  : Make the strokes more "polygonal"

from freestyle.chainingiterators import ChainSilhouetteIterator
from freestyle.predicates import (
    NotUP1D,
    QuantitativeInvisibilityUP1D,
    TrueUP1D,
)
from freestyle.shaders import (
    ConstantColorShader,
    ConstantThicknessShader,
    PolygonalizationShader,
    SamplingShader,
)
from freestyle.types import Operators


Operators.select(QuantitativeInvisibilityUP1D(0))
Operators.bidirectional_chain(ChainSilhouetteIterator(), NotUP1D(QuantitativeInvisibilityUP1D(0)))
shaders_list = [
    SamplingShader(2.0),
    ConstantThicknessShader(3),
    ConstantColorShader(0.0, 0.0, 0.0),
    PolygonalizationShader(8),
]
Operators.create(TrueUP1D(), shaders_list)
