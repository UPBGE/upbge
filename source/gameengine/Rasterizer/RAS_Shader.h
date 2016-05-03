
/** \file RAS_Shader.h
 *  \ingroup ketsji
 */

#ifndef __RAS_SHADER_H__
#define __RAS_SHADER_H__

#include "MT_Matrix4x4.h"

#include <vector>

class RAS_IRasterizer;

/**
 * RAS_Shader
 * shader access
 */
class RAS_Shader
{
public:
	/**
	* RAS_Uniform
	* uniform storage
	*/
	class RAS_Uniform
	{
	private:
		int mLoc; // Uniform location
		void *mData; // Memory allocated for variable
		bool mDirty; // Caching variable
		int mType; // Enum UniformTypes
		bool mTranspose; // Transpose matrices
		const int mDataLen; // Length of our data
	public:
		RAS_Uniform(int data_size);
		~RAS_Uniform();

		enum UniformTypes {
			UNI_NONE = 0,
			UNI_INT,
			UNI_FLOAT,
			UNI_INT2,
			UNI_FLOAT2,
			UNI_INT3,
			UNI_FLOAT3,
			UNI_INT4,
			UNI_FLOAT4,
			UNI_MAT3,
			UNI_MAT4,
			UNI_MAX
		};

		void Apply(RAS_Shader *shader);
		void SetData(int location, int type, bool transpose = false);
		int GetLocation();
		void *getData();

	#ifdef WITH_CXX_GUARDEDALLOC
		MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_Uniform")
	#endif
	};

	/**
	* RAS_DefUniform
	* pre defined uniform storage
	*/
	class RAS_DefUniform
	{
	public:
		RAS_DefUniform()
			:
			mType(0),
			mLoc(0),
			mFlag(0)
		{
		}

		int mType;
		int mLoc;
		unsigned int mFlag;

	#ifdef WITH_CXX_GUARDEDALLOC
		MEM_CXX_CLASS_ALLOC_FUNCS("GE:RAS_DefUniform")
	#endif
	};

protected:
	typedef std::vector<RAS_Uniform *> RAS_UniformVec;
	typedef std::vector<RAS_DefUniform *> RAS_UniformVecDef;

	unsigned int mShader; // Shader object
	int mPass; // 1.. unused
	bool mOk; // Valid and ok
	bool mUse;
	int mAttr; // Tangent attribute
	const char *vertProg; // Vertex program string
	const char *fragProg; // Fragment program string
	bool mError;
	bool mDirty;

	// Stored uniform variables
	RAS_UniformVec mUniforms;
	RAS_UniformVecDef mPreDef;

	// Compiles and links the shader
	bool LinkProgram();

	// search by location
	RAS_Uniform *FindUniform(const int location);

	// clears uniform data
	void ClearUniforms();

public:
	RAS_Shader();
	virtual ~RAS_Shader();

	// Unused for now tangent is set as tex coords
	enum AttribTypes {
		SHD_TANGENT = 1
	};

	enum GenType {
		MODELVIEWMATRIX,
		MODELVIEWMATRIX_TRANSPOSE,
		MODELVIEWMATRIX_INVERSE,
		MODELVIEWMATRIX_INVERSETRANSPOSE,
		MODELMATRIX,
		MODELMATRIX_TRANSPOSE,
		MODELMATRIX_INVERSE,
		MODELMATRIX_INVERSETRANSPOSE,
		VIEWMATRIX,
		VIEWMATRIX_TRANSPOSE,
		VIEWMATRIX_INVERSE,
		VIEWMATRIX_INVERSETRANSPOSE,
		CAM_POS,
		CONSTANT_TIMER
	};

	const char *GetVertPtr();
	const char *GetFragPtr();
	void SetVertPtr(char *vert);
	void SetFragPtr(char *frag);
	int getNumPass();
	bool GetError();

	void SetSampler(int loc, int unit);
	bool Ok() const;
	unsigned int GetProg();
	void SetProg(bool enable);
	int GetAttribute();

	// Apply methods : sets colected uniforms
	void ApplyShader();
	void UnloadShader();

	// Update predefined uniforms each render call
	void Update(RAS_IRasterizer *rasty, MT_Matrix4x4 model);

	void SetUniformfv(int location, int type, float *param, int size, bool transpose = false);
	void SetUniformiv(int location, int type, int *param, int size, bool transpose = false);
	int GetAttribLocation(const char *name);
	void BindAttribute(const char *attr, int loc);

	/** Return uniform location in the shader.
	 * \param name The uniform name.
	 * \param debug Print message for unfound coresponding uniform name.
	 */
	int GetUniformLocation(const char *name, bool debug=true);

	void SetUniform(int uniform, const MT_Vector2 &vec);
	void SetUniform(int uniform, const MT_Vector3 &vec);
	void SetUniform(int uniform, const MT_Vector4 &vec);
	void SetUniform(int uniform, const MT_Matrix4x4 &vec, bool transpose = false);
	void SetUniform(int uniform, const MT_Matrix3x3 &vec, bool transpose = false);
	void SetUniform(int uniform, const float &val);
	void SetUniform(int uniform, const float *val, int len);
	void SetUniform(int uniform, const int *val, int len);
	void SetUniform(int uniform, const unsigned int &val);
	void SetUniform(int uniform, const int val);
};

#endif /* __RAS_SHADER_H__ */
