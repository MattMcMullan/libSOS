/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2011 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreD3D11HLSLProgram.h"
#include "OgreException.h"
#include "OgreRenderSystem.h"
#include "OgreD3D11Device.h"
#include "OgreRoot.h"
#include "OgreD3D11Mappings.h"
#include "OgreGpuProgramManager.h"

namespace Ogre {
	//-----------------------------------------------------------------------
	D3D11HLSLProgram::CmdEntryPoint D3D11HLSLProgram::msCmdEntryPoint;
	D3D11HLSLProgram::CmdTarget D3D11HLSLProgram::msCmdTarget;
	D3D11HLSLProgram::CmdPreprocessorDefines D3D11HLSLProgram::msCmdPreprocessorDefines;
	D3D11HLSLProgram::CmdColumnMajorMatrices D3D11HLSLProgram::msCmdColumnMajorMatrices;
	D3D11HLSLProgram::CmdEnableBackwardsCompatibility D3D11HLSLProgram::msCmdEnableBackwardsCompatibility;
	//-----------------------------------------------------------------------
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::createConstantBuffer(const UINT ByteWidth)
	{

		// Create a constant buffer
		D3D11_BUFFER_DESC cbDesc;
		cbDesc.ByteWidth = ByteWidth;
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		cbDesc.MiscFlags = 0;
		HRESULT hr = mDevice->CreateBuffer( &cbDesc, NULL, &mConstantBuffer );
		if (FAILED(hr) || mDevice.isError())
		{
			String errorDescription = mDevice.getErrorDescription(hr);
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"D3D11 device Cannot create constant buffer.\nError Description:" + errorDescription,
				"D3D11HLSLProgram::createConstantBuffer");  
		}
	}

    class HLSLIncludeHandler : public ID3D10Include
	{
	public:
		HLSLIncludeHandler(Resource* sourceProgram) 
			: mProgram(sourceProgram) {}
		~HLSLIncludeHandler() {}

		STDMETHOD(Open)(D3D10_INCLUDE_TYPE IncludeType,
			LPCSTR pFileName,
			LPCVOID pParentData,
			LPCVOID *ppData,
			UINT *pByteLen
			)
		{
			// find & load source code
			DataStreamPtr stream = 
				ResourceGroupManager::getSingleton().openResource(
				String(pFileName), mProgram->getGroup(), true, mProgram);

			String source = stream->getAsString();
			// copy into separate c-string
			// Note - must NOT copy the null terminator, otherwise this will terminate
			// the entire program string!
			*pByteLen = static_cast<UINT>(source.length());
			char* pChar = new char[*pByteLen];
			memcpy(pChar, source.c_str(), *pByteLen);
			*ppData = pChar;

			return S_OK;
		}

		STDMETHOD(Close)(LPCVOID pData)
		{
			char* pChar = (char*)pData;
			delete [] pChar;
			return S_OK;
		}
	protected:
		Resource* mProgram;
	};

	static void getDefines(vector<D3D10_SHADER_MACRO>::type& defines, const String& definesString)
	{
		// Populate preprocessor defines
		String stringBuffer = definesString;

		defines.clear();

		if (stringBuffer.empty())
			return;

		// Split preprocessor defines and build up macro array
		D3D10_SHADER_MACRO macro;
		String::size_type pos = 0;
		while (pos != String::npos)
		{
			macro.Name = &stringBuffer[pos];
			macro.Definition = 0;

			String::size_type start_pos=pos;

			// Find delims
			pos = stringBuffer.find_first_of(";,=", pos);

			if(start_pos==pos)
			{
				if(pos==stringBuffer.length())
				{
					break;
				}
				pos++;
				continue;
			}

			if (pos != String::npos)
			{
				// Check definition part
				if (stringBuffer[pos] == '=')
				{
					// Setup null character for macro name
					stringBuffer[pos++] = '\0';
					macro.Definition = &stringBuffer[pos];
					pos = stringBuffer.find_first_of(";,", pos);
				}
				else
				{
					// No definition part, define as "1"
					macro.Definition = "1";
				}

				if (pos != String::npos)
				{
					// Setup null character for macro name or definition
					stringBuffer[pos++] = '\0';
				}
			}
			else
			{
				macro.Definition = "1";
			}
			if(strlen(macro.Name)>0)
			{
				defines.push_back(macro);
			}
			else
			{
				break;
			}
		}

		// Add NULL terminator
		macro.Name = 0;
		macro.Definition = 0;
			defines.push_back(macro);
    }		
    //-----------------------------------------------------------------------
    void D3D11HLSLProgram::loadFromSource(void)
    {
		if ( GpuProgramManager::getSingleton().isMicrocodeAvailableInCache(mName) )
		{
			getMicrocodeFromCache();
		}
		else
		{
			compileMicrocode();
		}
	}
    //-----------------------------------------------------------------------
    void D3D11HLSLProgram::getMicrocodeFromCache(void)
    {
		GpuProgramManager::Microcode cacheMicrocode = 
			GpuProgramManager::getSingleton().getMicrocodeFromCache(mName);
		
		mMicroCode.resize(cacheMicrocode->size());
		cacheMicrocode->read(&mMicroCode[0], cacheMicrocode->size());
		
		analizeMicrocode();
	}
    //-----------------------------------------------------------------------
    void D3D11HLSLProgram::compileMicrocode(void)
	{
		// include handler
		HLSLIncludeHandler includeHandler(this);

		ID3D10Blob * errors = 0;

        const D3D10_SHADER_MACRO* pDefines = 0;
        vector<D3D10_SHADER_MACRO>::type defines;
		getDefines(defines, mPreprocessorDefines);
        if (!defines.empty())
        { 
			pDefines = &defines[0];
        }

		UINT compileFlags=0;
		#ifdef OGRE_DEBUG_MODE
			compileFlags|=D3D10_SHADER_DEBUG;
			compileFlags|=D3D10_SHADER_SKIP_OPTIMIZATION;
		#endif
		
		if (mColumnMajorMatrices)
        {
            compileFlags |= D3D10_SHADER_PACK_MATRIX_COLUMN_MAJOR;
        }
        else
        {
            compileFlags |= D3D10_SHADER_PACK_MATRIX_ROW_MAJOR;
        }

		if (mEnableBackwardsCompatibility)
        {
			compileFlags |= D3D10_SHADER_ENABLE_BACKWARDS_COMPATIBILITY;
        }

		ID3D10Blob * pMicroCode;

		HRESULT hr = D3DX11CompileFromMemory(
			mSource.c_str(),	// [in] Pointer to the shader in memory. 
			mSource.size(),		// [in] Size of the shader in memory.  
			NULL,				// [in] The name of the file that contains the shader code. 
			pDefines,			// [in] Optional. Pointer to a NULL-terminated array of macro definitions. See D3D10_SHADER_MACRO. If not used, set this to NULL. 
			&includeHandler,	// [in] Optional. Pointer to an ID3D11Include Interface interface for handling include files. Setting this to NULL will cause a compile error if a shader contains a #include. 
			mEntryPoint.c_str(), // [in] Name of the shader-entrypoint function where shader execution begins. 
			mTarget.c_str(),			// [in] A string that specifies the shader model; can be any profile in shader model 2, shader model 3, or shader model 4. 
			compileFlags,				// [in] Effect compile flags - no D3D11_SHADER_ENABLE_BACKWARDS_COMPATIBILITY at the first try...
			NULL,				// [in] Effect compile flags
			NULL,				// [in] A pointer to a thread pump interface (see ID3DX11ThreadPump Interface). Use NULL to specify that this function should not return until it is completed. 
			&pMicroCode,		// [out] A pointer to an ID3D10Blob Interface which contains the compiled shader, as well as any embedded debug and symbol-table information. 
			&errors,			// [out] A pointer to an ID3D10Blob Interface which contains a listing of errors and warnings that occurred during compilation. These errors and warnings are identical to the the debug output from a debugger.
			NULL				// [out] A pointer to the return value. May be NULL. If pPump is not NULL, then pHResult must be a valid memory location until the asynchronous execution completes. 
			);

#if 0 // this is how you disassemble
		LPCSTR commentString = NULL;
		ID3D10Blob* pIDisassembly = NULL;
		char* pDisassembly = NULL;
		if( pMicroCode )
		{
			D3D11DisassembleShader( (UINT*) &mMicroCode[0], 
				pMicroCode->GetBufferSize(), TRUE, commentString, &pIDisassembly );
		}

		const char* assemblyCode =  static_cast<const char*>(pIDisassembly->GetBufferPointer());
#endif
		if (FAILED(hr))
		{
			mErrorsInCompile = true;
			String message = "Cannot assemble D3D11 high-level shader " + mName + " Errors:\n" +
				static_cast<const char*>(errors->GetBufferPointer());
			errors->Release();
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, message,
				"D3D11HLSLProgram::loadFromSource");
		}
		else
		{
			mMicroCode.resize(pMicroCode->GetBufferSize());
			memcpy(&mMicroCode[0], pMicroCode->GetBufferPointer(), pMicroCode->GetBufferSize());
			SAFE_RELEASE(pMicroCode);

			if ( GpuProgramManager::getSingleton().getSaveMicrocodesToCache() )
			{
		        // create microcode
		        GpuProgramManager::Microcode newMicrocode = 
                    GpuProgramManager::getSingleton().createMicrocode(mMicroCode.size());

        		// save microcode
				newMicrocode->write(&mMicroCode[0], mMicroCode.size());

        		// add to the microcode to the cache
				GpuProgramManager::getSingleton().addMicrocodeToCache(mName, newMicrocode);
			}
		}

		analizeMicrocode();
	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::analizeMicrocode()
	{
		SIZE_T BytecodeLength = mMicroCode.size();

		HRESULT hr = D3DReflect( (void*) &mMicroCode[0], BytecodeLength,
			IID_ID3D11ShaderReflection, // can't do __uuidof(ID3D11ShaderReflection) here...
			(void**) &mIShaderReflection );


		if (!FAILED(hr))
		{
			hr = mIShaderReflection->GetDesc( &mShaderDesc );

			if (!FAILED(hr))
			{
				// enum parameters
				{
					mInputVertexDeclaration.removeAllElements();

					D3D11_SIGNATURE_PARAMETER_DESC	paramDesc;

					for (UINT k=0; k<mShaderDesc.InputParameters; k++)
					{
						mIShaderReflection->GetInputParameterDesc( k, &paramDesc);
						mInputVertexDeclaration.addElement(
							paramDesc.Register, 
							-1, // we don't need the offset
							VET_FLOAT1, // doesn't matter
							D3D11Mappings::get(paramDesc.SemanticName),
							paramDesc.SemanticIndex);
					}

				}

				if (mShaderDesc.ConstantBuffers > 1)
				{
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
						"Multi constant buffers are not supported for now.",
						"D3D11HLSLProgram::loadFromSource");
				}

				if (mShaderDesc.ConstantBuffers == 1)
				{
					mShaderReflectionConstantBuffer = mIShaderReflection->GetConstantBufferByIndex(0);


					hr = mShaderReflectionConstantBuffer->GetDesc(&mConstantBufferDesc);

					createConstantBuffer(mConstantBufferDesc.Size);

					for(unsigned int i = 0; i < mConstantBufferDesc.Variables ; i++)
					{

						ID3D11ShaderReflectionVariable* varRef;
						varRef = mShaderReflectionConstantBuffer->GetVariableByIndex(i);

						D3D11_SHADER_VARIABLE_DESC shaderVerDesc;
						HRESULT hr = varRef->GetDesc(&shaderVerDesc);

						ShaderVarWithPosInBuf newVar;
						newVar.var = shaderVerDesc;
						newVar.wasInit = false;
						newVar.name = shaderVerDesc.Name;

						// A hack for cg to get the "original name" of the var in the "auto comments"
						// that cg adds to the hlsl 4 output. This is to solve the issue that
						// in some cases cg changes the name of the var to a new name.
						{
							String varForSearch = String(" :  : ") + newVar.name;
							size_t startPosOfVarOrgNameInSource = 0;
							size_t endPosOfVarOrgNameInSource = mSource.find(varForSearch + " ");
							if(endPosOfVarOrgNameInSource == -1)
							{
								endPosOfVarOrgNameInSource = mSource.find(varForSearch + "[");
							}
							if(endPosOfVarOrgNameInSource != -1)
							{
								// find space before var;
								for (size_t i = endPosOfVarOrgNameInSource - 1 ; i > 0 ; i-- )
								{
									if (mSource[i] == ' ')
									{
										startPosOfVarOrgNameInSource = i + 1;
										break;
									}
								}
								if (startPosOfVarOrgNameInSource > 0)
								{
									newVar.name = mSource.substr(startPosOfVarOrgNameInSource, endPosOfVarOrgNameInSource - startPosOfVarOrgNameInSource);
								}
							}
						}

						mShaderVars.push_back(newVar);
					}
				}
			}
		}

		switch(mType)
		{
		case GPT_VERTEX_PROGRAM:
			CreateVertexShader();
			break;
		case GPT_FRAGMENT_PROGRAM:
			CreatePixelShader();
			break;
		case GPT_GEOMETRY_PROGRAM:
			CreateGeometryShader();
			break;

		}
	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::createLowLevelImpl(void)
	{
		// Create a low-level program, give it the same name as us
		mAssemblerProgram =GpuProgramPtr(dynamic_cast<GpuProgram*>(this));
	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::unloadHighLevelImpl(void)
	{
        SAFE_RELEASE(mVertexShader);
        SAFE_RELEASE(mPixelShader);
        SAFE_RELEASE(mGeometryShader);
        SAFE_RELEASE(mIShaderReflection);
        SAFE_RELEASE(mConstantBuffer);
	}

	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::buildConstantDefinitions() const
	{
		createParameterMappingStructures(true);

		if (mShaderReflectionConstantBuffer)
		{
			//			if (!FAILED(hr))
			{
				for(unsigned int i = 0; i < mConstantBufferDesc.Variables ; i++)
				{

					ID3D11ShaderReflectionVariable* varRef;
					varRef = mShaderReflectionConstantBuffer->GetVariableByIndex(i);

					D3D11_SHADER_VARIABLE_DESC shaderVerDesc;
					unsigned int numParams = 1;
					HRESULT hr = varRef->GetDesc(&shaderVerDesc);

					ID3D11ShaderReflectionType* varRefType;
					varRefType = varRef->GetType();

					// Recursively descend through the structure levels
					processParamElement( "", shaderVerDesc.Name, i, varRefType);

				}
			}
		}
	}
	//-----------------------------------------------------------------------

	/*    void D3D11HLSLProgram::populateParameterNames(GpuProgramParametersSharedPtr params)
	{

	buildConstantDefinitions();
	}*/
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::processParamElement(String prefix, LPCSTR pName, 
		size_t paramIndex, ID3D11ShaderReflectionType* varRefType) const
	{
		D3D11_SHADER_TYPE_DESC varRefTypeDesc;
		HRESULT hr = varRefType->GetDesc(&varRefTypeDesc);


		// Since D3D HLSL doesn't deal with naming of array and struct parameters
		// automatically, we have to do it by hand


		if (FAILED(hr))
		{
			OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, 
				"Cannot retrieve constant description from HLSL program.", 
				"D3D11HLSLProgram::processParamElement");
		}

		String paramName = pName;
		// trim the odd '$' which appears at the start of the names in HLSL
		if (paramName.at(0) == '$')
			paramName.erase(paramName.begin());

		// Also trim the '[0]' suffix if it exists, we will add our own indexing later
		if (StringUtil::endsWith(paramName, "[0]", false))
		{
			paramName.erase(paramName.size() - 3);
		}




		if (varRefTypeDesc.Class == D3D10_SVC_STRUCT)
		{
			// work out a new prefix for nested members, if it's an array, we need an index
			prefix = prefix + paramName + ".";
			// Cascade into struct
			for (unsigned int i = 0; i < varRefTypeDesc.Members; ++i)
			{
				processParamElement(prefix, varRefType->GetMemberTypeName(i), i,  varRefType->GetMemberTypeByIndex(i));
			}
		}
		else
		{
			// Process params
			if (varRefTypeDesc.Type == D3D10_SVT_FLOAT || varRefTypeDesc.Type == D3D10_SVT_INT || varRefTypeDesc.Type == D3D10_SVT_BOOL)
			{
				String name = prefix + paramName;

				GpuConstantDefinition def;
				def.logicalIndex = paramIndex;
				// populate type, array size & element size
				populateDef(varRefTypeDesc, def);
				if (def.isFloat())
				{
					def.physicalIndex = mFloatLogicalToPhysical->bufferSize;
					OGRE_LOCK_MUTEX(mFloatLogicalToPhysical->mutex)
						mFloatLogicalToPhysical->map.insert(
						GpuLogicalIndexUseMap::value_type(paramIndex, 
						GpuLogicalIndexUse(def.physicalIndex, def.arraySize * def.elementSize, GPV_GLOBAL)));
					mFloatLogicalToPhysical->bufferSize += def.arraySize * def.elementSize;
					mConstantDefs->floatBufferSize = mFloatLogicalToPhysical->bufferSize;
				}
				else
				{
					def.physicalIndex = mIntLogicalToPhysical->bufferSize;
					OGRE_LOCK_MUTEX(mIntLogicalToPhysical->mutex)
						mIntLogicalToPhysical->map.insert(
						GpuLogicalIndexUseMap::value_type(paramIndex, 
						GpuLogicalIndexUse(def.physicalIndex, def.arraySize * def.elementSize, GPV_GLOBAL)));
					mIntLogicalToPhysical->bufferSize += def.arraySize * def.elementSize;
					mConstantDefs->intBufferSize = mIntLogicalToPhysical->bufferSize;
				}

				mConstantDefs->map.insert(GpuConstantDefinitionMap::value_type(name, def));

				// Now deal with arrays
				mConstantDefs->generateConstantDefinitionArrayEntries(name, def);
			}
		}

	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::populateDef(D3D11_SHADER_TYPE_DESC& d3dDesc, GpuConstantDefinition& def) const
	{
		def.arraySize = d3dDesc.Elements + 1;
		switch(d3dDesc.Type)
		{
		case D3D10_SVT_INT:
			switch(d3dDesc.Columns)
			{
			case 1:
				def.constType = GCT_INT1;
				break;
			case 2:
				def.constType = GCT_INT2;
				break;
			case 3:
				def.constType = GCT_INT3;
				break;
			case 4:
				def.constType = GCT_INT4;
				break;
			} // columns
			break;
		case D3D10_SVT_FLOAT:
			switch(d3dDesc.Rows)
			{
			case 1:
				switch(d3dDesc.Columns)
				{
				case 1:
					def.constType = GCT_FLOAT1;
					break;
				case 2:
					def.constType = GCT_FLOAT2;
					break;
				case 3:
					def.constType = GCT_FLOAT3;
					break;
				case 4:
					def.constType = GCT_FLOAT4;
					break;
				} // columns
				break;
			case 2:
				switch(d3dDesc.Columns)
				{
				case 2:
					def.constType = GCT_MATRIX_2X2;
					break;
				case 3:
					def.constType = GCT_MATRIX_2X3;
					break;
				case 4:
					def.constType = GCT_MATRIX_2X4;
					break;
				} // columns
				break;
			case 3:
				switch(d3dDesc.Columns)
				{
				case 2:
					def.constType = GCT_MATRIX_3X2;
					break;
				case 3:
					def.constType = GCT_MATRIX_3X3;
					break;
				case 4:
					def.constType = GCT_MATRIX_3X4;
					break;
				} // columns
				break;
			case 4:
				switch(d3dDesc.Columns)
				{
				case 2:
					def.constType = GCT_MATRIX_4X2;
					break;
				case 3:
					def.constType = GCT_MATRIX_4X3;
					break;
				case 4:
					def.constType = GCT_MATRIX_4X4;
					break;
				} // columns
				break;

			} // rows
			break;

		default:
			// not mapping samplers, don't need to take the space 
			break;
		};

		// HLSL pads to 4 elements
		def.elementSize = GpuConstantDefinition::getElementSize(def.constType, true);


	}
	//-----------------------------------------------------------------------
	D3D11HLSLProgram::D3D11HLSLProgram(ResourceManager* creator, const String& name, 
		ResourceHandle handle, const String& group, bool isManual, 
		ManualResourceLoader* loader, D3D11Device & device)
		: HighLevelGpuProgram(creator, name, handle, group, isManual, loader)
		, mErrorsInCompile(false), mConstantBuffer(NULL), mDevice(device), 
		mIShaderReflection(NULL), mShaderReflectionConstantBuffer(NULL), mVertexShader(NULL)//, mConstTable(NULL)
		,mPixelShader(NULL),mGeometryShader(NULL),mColumnMajorMatrices(true), mEnableBackwardsCompatibility(false), mInputVertexDeclaration(device)
	{
		ZeroMemory(&mConstantBufferDesc, sizeof(mConstantBufferDesc)) ;
		ZeroMemory(&mShaderDesc, sizeof(mShaderDesc)) ;

		if (createParamDictionary("D3D11HLSLProgram"))
		{
			setupBaseParamDictionary();
			ParamDictionary* dict = getParamDictionary();

			dict->addParameter(ParameterDef("entry_point", 
				"The entry point for the HLSL program.",
				PT_STRING),&msCmdEntryPoint);
			dict->addParameter(ParameterDef("target", 
				"Name of the assembler target to compile down to.",
				PT_STRING),&msCmdTarget);
			dict->addParameter(ParameterDef("preprocessor_defines", 
				"Preprocessor defines use to compile the program.",
				PT_STRING),&msCmdPreprocessorDefines);
			dict->addParameter(ParameterDef("column_major_matrices", 
				"Whether matrix packing in column-major order.",
				PT_BOOL),&msCmdColumnMajorMatrices);
			dict->addParameter(ParameterDef("enable_backwards_compatibility", 
				"enable backwards compatibility.",
				PT_BOOL),&msCmdEnableBackwardsCompatibility);
		}

	}
	//-----------------------------------------------------------------------
	D3D11HLSLProgram::~D3D11HLSLProgram()
	{
		SAFE_RELEASE(mConstantBuffer);

		// this is a hack - to solve that problem that we are the mAssemblerProgram of ourselves
		if ( !mAssemblerProgram.isNull() )
		{
			*( mAssemblerProgram.useCountPointer() ) = 0;
			mAssemblerProgram.setNull();
		}

		// have to call this here reather than in Resource destructor
		// since calling virtual methods in base destructors causes crash
		if ( isLoaded() )
		{
			unload();
		}
		else
		{
			unloadHighLevel();
		}
	}
	//-----------------------------------------------------------------------
	bool D3D11HLSLProgram::isSupported(void) const
	{
		// Use the current render system
		RenderSystem* rs = Root::getSingleton().getRenderSystem();

		// Get the supported syntaxed from RenderSystemCapabilities 
		return rs->getCapabilities()->isShaderProfileSupported(mTarget) && GpuProgram::isSupported();

	}
	//-----------------------------------------------------------------------
	GpuProgramParametersSharedPtr D3D11HLSLProgram::createParameters(void)
	{
		// Call superclass
		GpuProgramParametersSharedPtr params = HighLevelGpuProgram::createParameters();

		// D3D HLSL uses column-major matrices
		params->setTransposeMatrices(mColumnMajorMatrices);

		return params;
	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::setTarget(const String& target)
	{
		mTarget = target;
	}

	//-----------------------------------------------------------------------
	const String& D3D11HLSLProgram::getLanguage(void) const
	{
		static const String language = "hlsl";

		return language;
	}

	//-----------------------------------------------------------------------
	//-----------------------------------------------------------------------
	String D3D11HLSLProgram::CmdEntryPoint::doGet(const void *target) const
	{
		return static_cast<const D3D11HLSLProgram*>(target)->getEntryPoint();
	}
	void D3D11HLSLProgram::CmdEntryPoint::doSet(void *target, const String& val)
	{
		static_cast<D3D11HLSLProgram*>(target)->setEntryPoint(val);
	}
	//-----------------------------------------------------------------------
	String D3D11HLSLProgram::CmdTarget::doGet(const void *target) const
	{
		return static_cast<const D3D11HLSLProgram*>(target)->getTarget();
	}
	void D3D11HLSLProgram::CmdTarget::doSet(void *target, const String& val)
	{
		static_cast<D3D11HLSLProgram*>(target)->setTarget(val);
	}
	//-----------------------------------------------------------------------
	String D3D11HLSLProgram::CmdPreprocessorDefines::doGet(const void *target) const
	{
		return static_cast<const D3D11HLSLProgram*>(target)->getPreprocessorDefines();
	}
	void D3D11HLSLProgram::CmdPreprocessorDefines::doSet(void *target, const String& val)
	{
		static_cast<D3D11HLSLProgram*>(target)->setPreprocessorDefines(val);
	}
	//-----------------------------------------------------------------------
	String D3D11HLSLProgram::CmdColumnMajorMatrices::doGet(const void *target) const
	{
		return StringConverter::toString(static_cast<const D3D11HLSLProgram*>(target)->getColumnMajorMatrices());
	}
	void D3D11HLSLProgram::CmdColumnMajorMatrices::doSet(void *target, const String& val)
	{
		static_cast<D3D11HLSLProgram*>(target)->setColumnMajorMatrices(StringConverter::parseBool(val));
	}
	//-----------------------------------------------------------------------
	String D3D11HLSLProgram::CmdEnableBackwardsCompatibility::doGet(const void *target) const
	{
		return StringConverter::toString(static_cast<const D3D11HLSLProgram*>(target)->getEnableBackwardsCompatibility());
	}
	void D3D11HLSLProgram::CmdEnableBackwardsCompatibility::doSet(void *target, const String& val)
	{
		static_cast<D3D11HLSLProgram*>(target)->setEnableBackwardsCompatibility(StringConverter::parseBool(val));
	}
	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::CreateVertexShader()
	{
		if (isSupported())
		{
			// Create the shader
			HRESULT hr = mDevice->CreateVertexShader( 
				&mMicroCode[0],
				mMicroCode.size(),
				NULL,
				&mVertexShader);

			assert(mVertexShader);

			if (FAILED(hr) || mDevice.isError())
			{
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Cannot create D3D11 vertex shader " + mName + " from microcode.\nError Description:" + errorDescription,
					"D3D11GpuVertexProgram::loadFromMicrocode");

			}
		}
		else
		{
			assert(false);
			LogManager::getSingleton().logMessage(
				"Unsupported D3D11 vertex shader '" + mName + "' was not loaded.");
		}
	}

	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::CreatePixelShader()
	{
		if (isSupported())
		{
			// Create the shader
			HRESULT hr = mDevice->CreatePixelShader( 
				&mMicroCode[0], 
				mMicroCode.size(),
				NULL,
				&mPixelShader);

			if (FAILED(hr) || mDevice.isError())
			{
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Cannot create D3D11 Pixel shader " + mName + " from microcode.\nError Description:" + errorDescription,
					"D3D11GpuPixelProgram::loadFromMicrocode");

			}
		}
		else
		{
			LogManager::getSingleton().logMessage(
				"Unsupported D3D11 Pixel shader '" + mName + "' was not loaded.");
		}
	}

    void D3D11HLSLProgram::reinterpretGSForStreamOut(void)
	{
		assert(mGeometryShader);
		unloadHighLevel();
		mReinterpretingGS = true;
		loadHighLevel();
		mReinterpretingGS = false;
	}

	D3D11_SIGNATURE_PARAMETER_DESC D3D11HLSLProgram::getInputParamDesc(unsigned int index) const
	{
		assert(index<mShaderDesc.InputParameters);
		D3D11_SIGNATURE_PARAMETER_DESC desc;
		mIShaderReflection->GetInputParameterDesc(index, &desc);
		return desc;
	}
	D3D11_SIGNATURE_PARAMETER_DESC D3D11HLSLProgram::getOutputParamDesc(unsigned int index) const
	{
		assert(index<mShaderDesc.OutputParameters);
		D3D11_SIGNATURE_PARAMETER_DESC desc;
		mIShaderReflection->GetOutputParameterDesc(index, &desc);
		return desc;
	}

	static unsigned int getComponentCount(BYTE mask)
	{
		unsigned int compCount = 0;
		if (mask&1)
			++compCount;
		if (mask&2)
			++compCount;
		if (mask&4)
			++compCount;
		if (mask&8)
			++compCount;
		return compCount;
	}

	//-----------------------------------------------------------------------
	void D3D11HLSLProgram::CreateGeometryShader()
	{
		if (isSupported())
		{
			HRESULT hr;
			if (mReinterpretingGS)
			{
				D3D11_SO_DECLARATION_ENTRY* soDeclarations = new D3D11_SO_DECLARATION_ENTRY[mShaderDesc.OutputParameters];
				int totalComp = 0;
				for(unsigned int i = 0; i < getNumOutputs(); ++i)
				{
					D3D11_SIGNATURE_PARAMETER_DESC pDesc = getOutputParamDesc(i);

					soDeclarations[i].Stream = 0;
					soDeclarations[i].SemanticName = pDesc.SemanticName;
					soDeclarations[i].SemanticIndex= pDesc.SemanticIndex;

					int compCount = getComponentCount(pDesc.Mask);
					soDeclarations[i].StartComponent = 0;
					soDeclarations[i].ComponentCount = compCount;
					soDeclarations[i].OutputSlot = 0;

					totalComp += compCount;
				}

				// Create the shader
				UINT bufferStrides[1];
				bufferStrides[0] = totalComp*sizeof(float);
				hr = mDevice->CreateGeometryShaderWithStreamOutput( 
					&mMicroCode[0], 
					mMicroCode.size(),
					soDeclarations,
					mShaderDesc.OutputParameters,

					bufferStrides,
					1,

					0,
					NULL,
					&mGeometryShader);

			//	delete [] soDeclarations;

			}
			else
			{
				// Create the shader
				hr = mDevice->CreateGeometryShader( 
                    &mMicroCode[0], 
                    mMicroCode.size(),
					NULL,
					&mGeometryShader);
			}


			assert(mGeometryShader);

			if (FAILED(hr) || mDevice.isError())
			{
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, "Cannot create D3D11 Pixel shader " + mName + " from microcode.\nError Description:" + errorDescription,
					"D3D11GpuPixelProgram::loadFromMicrocode");

			}
		}
		else
		{
			LogManager::getSingleton().logMessage(
				"Unsupported D3D11 Pixel shader '" + mName + "' was not loaded.");
		}
	}

	//-----------------------------------------------------------------------------
	ID3D11Buffer* D3D11HLSLProgram::getConstantBuffer(GpuProgramParametersSharedPtr params, uint16 variabilityMask)
	{
		// Update the Constant Buffer

		D3D11_MAPPED_SUBRESOURCE pConstData;
		if(mConstantBuffer)
		{
			HRESULT hr = mDevice.GetImmediateContext()->Map(mConstantBuffer,0, D3D11_MAP_WRITE_DISCARD, NULL,  &pConstData );
			if (FAILED(hr))
			{
				String errorDescription = mDevice.getErrorDescription(hr);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
					"D3D11 device cannot map constant buffer\nError Description:" + errorDescription,
					"D3D11HLSLProgram::getConstantBuffer");
			}

			ShaderVarWithPosInBuf * iter = &mShaderVars[0];
			for (size_t i = 0 ; i < mConstantBufferDesc.Variables ; i++, iter++)
			{
				String  varName = iter->name;

				// hack for cg parameter with strange prefix
				if (varName.size() > 0 && varName[0] == '_')
				{
					varName.erase(0,1);
				}

				const GpuConstantDefinition& def = params->getConstantDefinition(varName);
				// Since we are mapping with write discard, contents of the buffer are undefined.
				// We must set every variable, even if it has not changed.
				//if (def.variability & variabilityMask)
				{

					iter->isFloat = def.isFloat();
					iter->physicalIndex = def.physicalIndex;
					iter->wasInit = true;
					
					if(iter->isFloat)
					{
						iter->src = (void *)&(*(params->getFloatConstantList().begin()+iter->physicalIndex));
					}
					else
					{
						iter->src = (void *)&(*(params->getIntConstantList().begin()+iter->physicalIndex));
					}

				

					memcpy( &(((char *)(pConstData.pData))[iter->var.StartOffset]), iter->src , iter->var.Size);
				}
			}

			mDevice.GetImmediateContext()->Unmap(mConstantBuffer, 0);
		}

		return mConstantBuffer;
	}
	//-----------------------------------------------------------------------------
	ID3D11VertexShader* D3D11HLSLProgram::getVertexShader(void) const 
	{ 
		assert(mType == GPT_VERTEX_PROGRAM);
		assert(mVertexShader);
		return mVertexShader; 
	}
	//-----------------------------------------------------------------------------
	ID3D11PixelShader* D3D11HLSLProgram::getPixelShader(void) const 
	{ 
		assert(mType == GPT_FRAGMENT_PROGRAM);
		assert(mPixelShader);
		return mPixelShader; 
	}
	//-----------------------------------------------------------------------------
	ID3D11GeometryShader* D3D11HLSLProgram::getGeometryShader(void) const 
	{ 
		assert(mType == GPT_GEOMETRY_PROGRAM);
		assert(mGeometryShader);
		return mGeometryShader; 
	}

	//-----------------------------------------------------------------------------
	const MicroCode & D3D11HLSLProgram::getMicroCode(void) const 
	{ 
		assert(mMicroCode.size() > 0);
		return mMicroCode; 
	}

}