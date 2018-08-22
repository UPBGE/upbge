
/** \file RAS_Shader.h
 *  \ingroup bgerast
 */

#ifndef __RAS_SHADER_H__
#define __RAS_SHADER_H__

#include "mathfu.h"

#include <string>
#include <vector>
#include <unordered_map>

#define SORT_UNIFORMS 1

class RAS_Rasterizer;
struct GPUShader;

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
		int m_loc; // Uniform location
		unsigned int m_count; // Number of items
		void *m_data; // Memory allocated for variable
		bool m_dirty; // Caching variable
		int m_type; // Enum UniformTypes
		const int m_dataLen; // Length of our data
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
		void SetData(int location, int type, unsigned int count, bool transpose = false);
		int GetLocation();
		void *GetData();
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
			m_type(0),
			m_loc(0),
			m_flag(0)
		{
		}

		int m_type;
		int m_loc;
		unsigned int m_flag;
	};

	enum ProgramType {
		VERTEX_PROGRAM = 0,
		FRAGMENT_PROGRAM,
		GEOMETRY_PROGRAM,
		MAX_PROGRAM
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
		CONSTANT_TIMER,
		EYE
	};

protected:
	struct UniformInfo
	{
		/// Hashed uniform name.
		size_t nameHash;
		/// Uniform location.
		int location;

		/** Construct uniform info
		 * \param name The uniform name.
		 * \param shader The shader used to retrieve uniform location.
		 */
		UniformInfo(const std::string& name, GPUShader *shader);

		inline bool operator< (const UniformInfo& other) const
		{
			return (nameHash < other.nameHash);
		}
	};

	// Uniform information sorted by hashed name value.
	std::vector<UniformInfo> m_uniformInfos;

	typedef std::vector<RAS_Uniform *> RAS_UniformVec;
	typedef std::vector<RAS_DefUniform *> RAS_UniformVecDef;

	GPUShader *m_shader;
	bool m_use;
	std::string m_progs[MAX_PROGRAM];
	bool m_error;
	bool m_dirty;

	// Stored uniform variables
	RAS_UniformVec m_uniforms;
	RAS_UniformVecDef m_preDef;

	/** Parse shader program to prevent redundant macro directives.
	 * \param type The program type to parse.
	 * \return The parsed program.
	 */
	std::string GetParsedProgram(ProgramType type) const;

	// Compiles and links the shader
	virtual bool LinkProgram();
	void ValidateProgram();
	void ExtractUniformInfos();

	// search by location
	RAS_Uniform *FindUniform(const int location);

	// clears uniform data
	void ClearUniforms();

public:
	RAS_Shader();
	virtual ~RAS_Shader();


	bool GetError() const;
	bool Ok() const;
	GPUShader *GetGPUShader();

	unsigned int GetProg();

	void BindProg();
	void UnbindProg();

	void SetEnabled(bool enabled);
	bool GetEnabled() const;

	// Apply methods : sets colected uniforms
	void ApplyShader();
	void UnloadShader();
	void DeleteShader();

	// Update predefined uniforms each render call
	void Update(RAS_Rasterizer *rasty, const mt::mat4 &model);

	void SetSampler(int loc, int unit);

	void SetUniformfv(int location, int type, float *param, int size, unsigned int count, bool transpose = false);
	void SetUniformiv(int location, int type, int *param, int size, unsigned int count, bool transpose = false);
	int GetAttribLocation(const std::string& name);
	void BindAttribute(const std::string& attr, int loc);

	/** Return uniform location in the shader.
	 * \param name The uniform name.
	 * \param debug Print message for unfound coresponding uniform name.
	 */
	int GetUniformLocation(const std::string& name, bool debug=true);

	void SetUniform(int uniform, const mt::vec2 &vec);
	void SetUniform(int uniform, const mt::vec3 &vec);
	void SetUniform(int uniform, const mt::vec4 &vec);
	void SetUniform(int uniform, const mt::mat4 &vec, bool transpose = false);
	void SetUniform(int uniform, const mt::mat3 &vec, bool transpose = false);
	void SetUniform(int uniform, const float &val);
	void SetUniform(int uniform, const float *val, int len);
	void SetUniform(int uniform, const int *val, int len);
	void SetUniform(int uniform, const unsigned int &val);
	void SetUniform(int uniform, const int val);
};

#endif /* __RAS_SHADER_H__ */
