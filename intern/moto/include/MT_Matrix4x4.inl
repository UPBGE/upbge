#include "MT_Optimize.h"

GEN_INLINE void MT_Matrix4x4::invert()  {
	//
	// Inversion by Cramer's rule.  Code taken from an Intel publication
	//
	MT_Scalar tmp[12]; // temp array for pairs
	MT_Scalar src[16]; // array of transpose source matrix
	MT_Scalar det; // determinant
	// transpose matrix
	for (unsigned int i = 0; i < 4; i++) {
		src[i + 0] = m_el[i][0];
		src[i + 4] = m_el[i][1];
		src[i + 8] = m_el[i][2];
		src[i + 12] = m_el[i][3];
	}
	// calculate pairs for first 8 elements (cofactors)
	tmp[0] = src[10] * src[15];
	tmp[1] = src[11] * src[14];
	tmp[2] = src[9] * src[15];
	tmp[3] = src[11] * src[13];
	tmp[4] = src[9] * src[14];
	tmp[5] = src[10] * src[13];
	tmp[6] = src[8] * src[15];
	tmp[7] = src[11] * src[12];
	tmp[8] = src[8] * src[14];
	tmp[9] = src[10] * src[12];
	tmp[10] = src[8] * src[13];
	tmp[11] = src[9] * src[12];
	// calculate first 8 elements (cofactors)
	m_el[0][0] = tmp[0] * src[5] + tmp[3] * src[6] + tmp[4] * src[7];
	m_el[0][0] -= tmp[1] * src[5] + tmp[2] * src[6] + tmp[5] * src[7];
	m_el[0][1] = tmp[1] * src[4] + tmp[6] * src[6] + tmp[9] * src[7];
	m_el[0][1] -= tmp[0] * src[4] + tmp[7] * src[6] + tmp[8] * src[7];
	m_el[0][2] = tmp[2] * src[4] + tmp[7] * src[5] + tmp[10] * src[7];
	m_el[0][2] -= tmp[3] * src[4] + tmp[6] * src[5] + tmp[11] * src[7];
	m_el[0][3] = tmp[5] * src[4] + tmp[8] * src[5] + tmp[11] * src[6];
	m_el[0][3] -= tmp[4] * src[4] + tmp[9] * src[5] + tmp[10] * src[6];
	m_el[1][0] = tmp[1] * src[1] + tmp[2] * src[2] + tmp[5] * src[3];
	m_el[1][0] -= tmp[0] * src[1] + tmp[3] * src[2] + tmp[4] * src[3];
	m_el[1][1] = tmp[0] * src[0] + tmp[7] * src[2] + tmp[8] * src[3];
	m_el[1][1] -= tmp[1] * src[0] + tmp[6] * src[2] + tmp[9] * src[3];
	m_el[1][2] = tmp[3] * src[0] + tmp[6] * src[1] + tmp[11] * src[3];
	m_el[1][2] -= tmp[2] * src[0] + tmp[7] * src[1] + tmp[10] * src[3];
	m_el[1][3] = tmp[4] * src[0] + tmp[9] * src[1] + tmp[10] * src[2];
	m_el[1][3] -= tmp[5] * src[0] + tmp[8] * src[1] + tmp[11] * src[2];
	// calculate pairs for second 8 elements (cofactors)
	tmp[0] = src[2] * src[7];
	tmp[1] = src[3] * src[6];
	tmp[2] = src[1] * src[7];
	tmp[3] = src[3] * src[5];
	tmp[4] = src[1] * src[6];
	tmp[5] = src[2] * src[5];

	tmp[6] = src[0] * src[7];
	tmp[7] = src[3] * src[4];
	tmp[8] = src[0] * src[6];
	tmp[9] = src[2] * src[4];
	tmp[10] = src[0] * src[5];
	tmp[11] = src[1] * src[4];
	// calculate second 8 elements (cofactors)
	m_el[2][0] = tmp[0] * src[13] + tmp[3] * src[14] + tmp[4] * src[15];
	m_el[2][0] -= tmp[1] * src[13] + tmp[2] * src[14] + tmp[5] * src[15];
	m_el[2][1] = tmp[1] * src[12] + tmp[6] * src[14] + tmp[9] * src[15];
	m_el[2][1] -= tmp[0] * src[12] + tmp[7] * src[14] + tmp[8] * src[15];
	m_el[2][2] = tmp[2] * src[12] + tmp[7] * src[13] + tmp[10] * src[15];
	m_el[2][2] -= tmp[3] * src[12] + tmp[6] * src[13] + tmp[11] * src[15];
	m_el[2][3] = tmp[5] * src[12] + tmp[8] * src[13] + tmp[11] * src[14];
	m_el[2][3] -= tmp[4] * src[12] + tmp[9] * src[13] + tmp[10] * src[14];
	m_el[3][0] = tmp[2] * src[10] + tmp[5] * src[11] + tmp[1] * src[9];
	m_el[3][0] -= tmp[4] * src[11] + tmp[0] * src[9] + tmp[3] * src[10];
	m_el[3][1] = tmp[8] * src[11] + tmp[0] * src[8] + tmp[7] * src[10];
	m_el[3][1] -= tmp[6] * src[10] + tmp[9] * src[11] + tmp[1] * src[8];
	m_el[3][2] = tmp[6] * src[9] + tmp[11] * src[11] + tmp[3] * src[8];
	m_el[3][2] -= tmp[10] * src[11] + tmp[2] * src[8] + tmp[7] * src[9];
	m_el[3][3] = tmp[10] * src[10] + tmp[4] * src[8] + tmp[9] * src[9];
	m_el[3][3] -= tmp[8] * src[9] + tmp[11] * src[10] + tmp[5] * src[8];
	// calculate determinant
	det = 1.0f / (src[0] * m_el[0][0] + src[1] * m_el[0][1] + src[2] * m_el[0][2] + src[3] * m_el[0][3]);

	for (unsigned int i = 0; i < 4; i++) {
		for (unsigned int j = 0; j < 4; j++) {
			m_el[i][j] = m_el[i][j] * det;
		}
	}
}

GEN_INLINE MT_Matrix4x4 MT_Matrix4x4::inverse() const
{
	MT_Matrix4x4 invmat = *this;

	invmat.invert();

	return invmat;
}

GEN_INLINE MT_Matrix4x4& MT_Matrix4x4::operator*=(const MT_Matrix4x4& m)
{
	setValue(m.tdot(0, m_el[0]), m.tdot(1, m_el[0]), m.tdot(2, m_el[0]), m.tdot(3, m_el[0]),
             m.tdot(0, m_el[1]), m.tdot(1, m_el[1]), m.tdot(2, m_el[1]), m.tdot(3, m_el[1]),
             m.tdot(0, m_el[2]), m.tdot(1, m_el[2]), m.tdot(2, m_el[2]), m.tdot(3, m_el[2]),
             m.tdot(0, m_el[3]), m.tdot(1, m_el[3]), m.tdot(2, m_el[3]), m.tdot(3, m_el[3]));
    return *this;

}

GEN_INLINE MT_Vector4 operator*(const MT_Matrix4x4& m, const MT_Vector4& v) {
    return MT_Vector4(MT_dot(m[0], v), MT_dot(m[1], v), MT_dot(m[2], v), MT_dot(m[3], v));
}

GEN_INLINE MT_Vector4 operator*(const MT_Vector4& v, const MT_Matrix4x4& m) {
    return MT_Vector4(m.tdot(0, v), m.tdot(1, v), m.tdot(2, v), m.tdot(3, v));
}

GEN_INLINE MT_Matrix4x4 operator*(const MT_Matrix4x4& m1, const MT_Matrix4x4& m2) {
	return 
		MT_Matrix4x4(m2.tdot(0, m1[0]), m2.tdot(1, m1[0]), m2.tdot(2, m1[0]), m2.tdot(3, m1[0]),
                     m2.tdot(0, m1[1]), m2.tdot(1, m1[1]), m2.tdot(2, m1[1]), m2.tdot(3, m1[1]),
                     m2.tdot(0, m1[2]), m2.tdot(1, m1[2]), m2.tdot(2, m1[2]), m2.tdot(3, m1[2]),
                     m2.tdot(0, m1[3]), m2.tdot(1, m1[3]), m2.tdot(2, m1[3]), m2.tdot(3, m1[3]));
}


GEN_INLINE MT_Matrix4x4 MT_Matrix4x4::transposed() const {
    return MT_Matrix4x4(m_el[0][0], m_el[1][0], m_el[2][0], m_el[3][0],
                        m_el[0][1], m_el[1][1], m_el[2][1], m_el[3][1],
                        m_el[0][2], m_el[1][2], m_el[2][2], m_el[3][2],
                        m_el[0][3], m_el[1][3], m_el[2][3], m_el[3][3]);
}

GEN_INLINE void MT_Matrix4x4::transpose() {
	*this = transposed();
}

GEN_INLINE MT_Matrix4x4 MT_Matrix4x4::absolute() const {
    return 
        MT_Matrix4x4(MT_abs(m_el[0][0]), MT_abs(m_el[0][1]), MT_abs(m_el[0][2]), MT_abs(m_el[0][3]),
                     MT_abs(m_el[1][0]), MT_abs(m_el[1][1]), MT_abs(m_el[1][2]), MT_abs(m_el[1][3]),
                     MT_abs(m_el[2][0]), MT_abs(m_el[2][1]), MT_abs(m_el[2][2]), MT_abs(m_el[2][3]),
                     MT_abs(m_el[3][0]), MT_abs(m_el[3][1]), MT_abs(m_el[3][2]), MT_abs(m_el[3][3]));
}
