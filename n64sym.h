/*

n64sym

Symbol identifier tool for N64 games

shygoo 2017
License: MIT

*/

#include <elfio/elfio.hpp>
#include <stdarg.h>
#include <dirent.h>

#include "arutil.h"
#include "elfutil.h"

using namespace ELFIO;

typedef struct
{
	uint32_t link_address; // from jump target
	uint32_t file_address; // from data match
	uint32_t size; // data match size
	char name[64];
} SEARCH_RESULT;

typedef std::vector<SEARCH_RESULT> SEARCH_RESULTS;
typedef std::vector<const char*> STR_VECTOR;

class CN64Sym
{
	uint8_t* m_Data;
	uint32_t m_DataSize;
	SEARCH_RESULTS* m_Results;
	STR_VECTOR* m_LibPaths;
	bool m_bVerbose;
	bool m_bRomMode;

public:
	static CN64Sym* Create(const char* binPath)
	{
		FILE* pfile = fopen(binPath, "rb");

		if(pfile == NULL)
		{
			return NULL;
		}

		return new CN64Sym(pfile);
	}

private:
	CN64Sym(FILE* pfile) :
		m_Data(NULL),
		m_DataSize(0),
		m_Results(NULL),
		m_LibPaths(NULL),
		m_bVerbose(false),
		m_bRomMode(false)
	{
		fseek(pfile, 0, SEEK_END);
		
		m_DataSize = ftell(pfile);
		m_Data = (uint8_t*) malloc(m_DataSize);

		rewind(pfile);
		fread(m_Data, 1, m_DataSize, pfile);
		
		m_Results = new SEARCH_RESULTS;
		m_LibPaths = new STR_VECTOR;
	}

public:
	~CN64Sym()
	{
		m_Results->clear();
		delete m_Results;

		m_LibPaths->clear();
		delete m_LibPaths;

		free(m_Data);
	}

	void Log(const char* format, ...)
	{
		if(!m_bVerbose	)
		{
			return;
		}
	
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}

	void AddLibPath(const char* libPath)
	{
		m_LibPaths->push_back(libPath);
	}

	bool AddResult(SEARCH_RESULT result)
	{
		// have link address from jump target
		if(result.link_address != 0)
		{
			for(int i = 0; i < m_Results->size(); i++)
			{
				SEARCH_RESULT test_result = m_Results->at(i);

				if(test_result.link_address == result.link_address)
				{
					return false; // already have
				}
			}

			m_Results->push_back(result);

			Log("    %08X %s\n", result.link_address, result.name);

			return true;
		}
		// have file address and size from data match
		//for(int i = 0; i < results->size(); i++)
		//{
		//	search_result test_result = results->at(i);
		//	if(test_result.file_address == result.file_address)
		//	{
		//		return; // already have
		//	}
		//
		//	//search_result test_result = (*results)[i];
		//	//
		//	//// find existing symbol by name
		//	//if(strcmp(result.name, test_result.name) == 0)
		//	//{
		//	//	if(test_result.file_address == 0 && result.file_address != 0)
		//	//	{
		//	//		test_result.size = result.size;
		//	//		test_result.file_address = result.file_address;
		//	//		(*results)[i] = test_result;
		//	//		return;
		//	//	}
		//	//}
		//}
		m_Results->push_back(result);
	}

	static bool ResultCmp(SEARCH_RESULT a, SEARCH_RESULT b)
	{
		return (a.link_address < b.link_address);
	}

	static bool ResultCmp2(SEARCH_RESULT a, SEARCH_RESULT b)
	{
		return (a.file_address < b.file_address);
	}

	void SortResults()
	{
		std::sort(m_Results->begin(), m_Results->end(), ResultCmp);
		std::sort(m_Results->begin(), m_Results->end(), ResultCmp2);
	}

	void DumpResults()
	{
		for(int i = 0; i < m_Results->size(); i++)
		{
			SEARCH_RESULT result = m_Results->at(i);
			printf("%08X,code,%s\n", result.link_address, result.name);
		}
	}

	void SetVerbose(bool bVerbose)
	{
		m_bVerbose = bVerbose;
	}

	void SetRomMode(bool bRomMode)
	{
		m_bRomMode = bRomMode;
	}

	int DataSize()
	{
		return m_DataSize;
	}

	// For code match
	void AddSymbolResults(CElfObject* elf, uint32_t baseAddress)
	{
		int textOffset = 0;

		int nSymbols = elf->GetNumSymbols();

		for(int i = nSymbols - 1; i >= 0; i--)
		{
			ELF_SYMBOL symbol;
			elf->GetSymbol(i, symbol);

			if(symbol.size > 0 && symbol.bind == 1 && symbol.type != 0)
			{
				SEARCH_RESULT result;
				result.file_address = 0;
				result.link_address = 0x80000000 | (baseAddress + textOffset);
				result.size = symbol.size;
				strcpy(result.name, symbol.name.c_str());

				AddResult(result);

				textOffset += symbol.size;
			}
		}
	}

	void AddRelocationResults(CElfObject* elf, const char* block, const char* altNamePrefix)
	{
		int nRelocations = elf->GetNumRelTextEntries();

		for(int i = 0; i < nRelocations; i++)
		{
			ELF_RELOCATION relocation;
			elf->GetRelTextEntry(i, relocation);

			int textOffset = relocation.offset;

			if(relocation.type == R_MIPS_26 && block[textOffset] == 0x0C)
			{
				ELF_SYMBOL symbol;
				elf->GetSymbol(relocation.symbol_index, symbol);

				uint32_t jalOpcode = *(uint32_t*)&block[textOffset];
				jalOpcode = __builtin_bswap32(jalOpcode);

				uint32_t jalTarget = 0x80000000 | ((jalOpcode & 0x00FFFFFF) * 4);
				
				SEARCH_RESULT result;
				result.link_address = jalTarget;
				result.file_address = 0;
				result.size = 0;
				strcpy(result.name, symbol.name.c_str());

				if(relocation.symbol_index == 1)
				{
					// Static function, compiler tossed out the symbol
					// Use object file name and text offset as a replacement
					int len = sprintf(result.name, "%s_%04X", altNamePrefix, textOffset);
					for(int i = 0; i < len; i++)
					{
						if(result.name[i] == '.')
						{
							result.name[i] = '_';
						}
					}
				}

				AddResult(result);
			}
			else if(relocation.type == R_MIPS_LO16 && i > 0)
			{
				ELF_RELOCATION prevRelocation;
				elf->GetRelTextEntry(i - 1, prevRelocation);

				if(prevRelocation.type == R_MIPS_HI16)
				{
					// TODO: Implement
				}
			}
		}
	}

	void ProcessElf(std::istream& stream, std::streampos pos, const char* altNamePrefix)
	{
		Log("  %s\n", altNamePrefix);

		CElfObject* elf = CElfObject::Create(stream, pos);

		if(!elf->HasTextSection())
		{
			delete elf;
			return;
		}

		int endAddress = DataSize() - elf->TextSize();

		for(int baseAddress = 0; baseAddress < endAddress; baseAddress += 4)
		{
			const char* block = (const char*)&m_Data[baseAddress];

			if(elf->TextMatchesData(block))
			{
				AddSymbolResults(elf, baseAddress);
				AddRelocationResults(elf, block, altNamePrefix);
				break;
			}
		}

		delete elf;
	}

	void ProcessElf(const char* path)
	{
		std::ifstream stream;

		stream.open(path, std::ios::in | std::ios::binary);
		
		ProcessElf(stream, 0, path);
	}

	static void CbProcessElf(const ar_block_info* blockInfo, void* p)
	{
		CN64Sym* _this = (CN64Sym*) p;
		
		std::istream& stream = *blockInfo->lpStream;

		_this->ProcessElf(stream, stream.tellg(), blockInfo->sz_identifier);
	}

	// returns true if 'str' endis with 'suffix'
	static bool EndsWith(const char *str, const char *suffix)
	{
		if (!str || !suffix)
		{
			return false;
		}
		size_t len_str = strlen(str);
		size_t len_suffix = strlen(suffix);
		if (len_suffix > len_str)
		{
			return false;
		}
		return (0 == strncmp(str + len_str - len_suffix, suffix, len_suffix));
	}

	static bool PathIsStaticLibrary(const char *path)
	{
		if (strlen(path) < 3)
		{
			return false;
		}
		return EndsWith(path, ".a") || EndsWith(path, ".A");
	}

	static bool PathIsObjectFile(const char *path)
	{
		if (strlen(path) < 3)
		{
			return false;
		}
		return EndsWith(path, ".o") || EndsWith(path, ".O");
	}

	static bool IsFileWithSymbols(const char *path)
	{
		return PathIsStaticLibrary(path) || PathIsObjectFile(path);
	}

	void ProcessFile(const char* filePath)
	{
		Log("%s\n", filePath);
		if (PathIsStaticLibrary(filePath))
		{
			ar_process_blocks(filePath, CbProcessElf, this);
		}
		else if (PathIsObjectFile(filePath))
		{
			ProcessElf(filePath);
		}
	}

	void ScanRecursive(const char* path)
	{
		if (IsFileWithSymbols(path))
		{
			ProcessFile(path);
			return;
		}

		DIR *dir;
		dir = opendir(path);
		if (dir == NULL)
		{
			printf("%s is neither a directory or file with symbols.\n", path);
			return;
		}

		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL)
		{
			char next_path[PATH_MAX];
			if (!entry->d_name) continue;
			snprintf(next_path, sizeof(next_path), "%s/%s", path, entry->d_name);
			switch (entry->d_type) {
				case DT_DIR:
					// skip "." dirs
					if (entry->d_name[0] == '.')
					{
						continue;
					}
					// scan subdirectory
					ScanRecursive(next_path);
					break;
				case DT_REG:
				{
					if (IsFileWithSymbols(next_path))
					{
						ProcessFile(next_path);
					}
					break;
				}
				default:
					break;
			}
		}
		closedir(dir);

	}

	bool Run()
	{
		for(int i = 0; i < m_LibPaths->size(); i++)
		{
			ScanRecursive(m_LibPaths->at(i));
		}
		Log("\n");
	}
};
