/*
  Copyright (C) 2005 Kimmo Pekkola, 2009 Greg Schoppe

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Sddl.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <process.h>
#include <vector>
#include "../../Library/Export.h"	// Rainmeter's exported functions
#include "../../Library/DisableThreadLibraryCalls.h"	// contains DllMain entry point

struct MeasureData
{
	bool count;

	MeasureData() : count(false) {}
};

struct BinData
{
	union
	{
		ULONGLONG lastWrite;
		UINT lastCount;
	};

	HANDLE directory;
	WCHAR drive;
	bool isFAT;
};

unsigned int __stdcall QueryRecycleBinThreadProc(void* pParam);
HRESULT GetFolderCLSID(LPCWSTR pszPath, CLSID* pathCLSID);
LPWSTR GetCurrentUserSid();
HANDLE GetRecycleBinHandle(WCHAR drive, bool& isFAT);

std::vector<BinData> g_BinData;
double g_BinCount = 0;
double g_BinSize = 0;

bool g_IsXP = false;

int g_UpdateCount = 0;
int g_InstanceCount = 0;
HANDLE g_Thread = NULL;

PLUGIN_EXPORT void Initialize(void** data, void* rm)
{
	MeasureData* measure = new MeasureData;
	*data = measure;

	if (g_InstanceCount == 0)
	{
		OSVERSIONINFOEX osvi = {sizeof(OSVERSIONINFOEX)};
		if (GetVersionEx((OSVERSIONINFO*)&osvi))
		{
			if (osvi.dwMajorVersion == 5)
			{
				// Not checking for osvi.dwMinorVersion >= 1 because we won't run on pre-XP
				g_IsXP = true;
			}
		}
	}

	++g_InstanceCount;
}

PLUGIN_EXPORT void Reload(void* data, void* rm, double* maxValue)
{
	MeasureData* measure = (MeasureData*)data;

	LPCWSTR value = RmReadString(rm, L"RecycleType", L"COUNT");
	if (_wcsicmp(L"COUNT", value) == 0)
	{
		measure->count = true;
	}
	else if (_wcsicmp(L"SIZE", value) == 0)
	{
		measure->count = false;
	}
	else
	{
		WCHAR buffer[256];
		_snwprintf_s(buffer, _TRUNCATE, L"RecycleManager.dll: RecycleType=%s is not valid in [%s]", value, RmGetMeasureName(rm));
		RmLog(LOG_ERROR, buffer);
	}
}

PLUGIN_EXPORT double Update(void* data)
{
	MeasureData* measure = (MeasureData*)data;

	++g_UpdateCount;
	if (g_UpdateCount > g_InstanceCount && !g_Thread)
	{
		g_UpdateCount = 0;

		WCHAR buffer[128];
		DWORD len = GetLogicalDriveStrings(128, buffer);

		// Convert "A:\\\0B:\\\0" -> "AB\0"
		int index = 0;
		for (int i = 0; i < len; i += 4)
		{
			buffer[index] = buffer[i];
			++index;
		}
		buffer[index] = L'\0';

		const WCHAR DRIVE_HANDLED = 1;
		bool changed = false;
		auto iter = g_BinData.begin();
		while (iter != g_BinData.end())
		{
			BinData& data = (*iter);

			WCHAR* pos = wcschr(buffer, data.drive);
			if (pos != NULL)
			{
				if (data.isFAT)
				{
					// FAT/FAT32 doesn't update directory last write time.
					// Use directory content count instead.
					WCHAR filter[] = L"\0:\\$RECYCLE.BIN\\*";
					WCHAR filterXP[] = L"\0:\\RECYCLED\\*";
					filter[0] = *pos;
					filterXP[0] = *pos;

					WIN32_FIND_DATA fd;
					HANDLE hSearch = FindFirstFile(g_IsXP ? filterXP : filter, &fd);
					if (hSearch != INVALID_HANDLE_VALUE)
					{
						UINT count = 0;
						do
						{
							++count;
						}
						while (FindNextFile(hSearch, &fd));

						FindClose(hSearch);

						if (count != data.lastCount)
						{
							data.lastCount = count;
							changed = true;
						}
					}
				}
				else if (data.directory)
				{
					ULONGLONG lastWrite;
					GetFileTime(data.directory, NULL, NULL, (FILETIME*)&lastWrite);
					if (data.lastWrite != lastWrite)
					{
						data.lastWrite = lastWrite;
						changed = true;
					}
				}

				*pos = DRIVE_HANDLED;
				++iter;
			}
			else
			{
				// Drive removed
				changed = true;

				if (data.directory)
				{
					CloseHandle(data.directory);
				}

				iter = g_BinData.erase(iter);
			}
		}

		for (int i = 0; i < index; ++i)
		{
			if (buffer[i] != DRIVE_HANDLED)
			{
				// New drive
				BinData data = {0};
				data.drive = buffer[i];

				WCHAR drive[] = L"\0:\\";
				drive[0] = buffer[i];
				if (GetDriveType(drive) == DRIVE_FIXED)
				{
					data.directory = GetRecycleBinHandle(buffer[i], data.isFAT);
				}

				g_BinData.push_back(data);
			}
		}

		if (changed)
		{
			g_UpdateCount = -8;
			g_Thread = (HANDLE)_beginthreadex(NULL, 0, QueryRecycleBinThreadProc, NULL, 0, NULL);
		}
	}

	return measure->count ? g_BinCount : g_BinSize;
}

PLUGIN_EXPORT void Finalize(void* data)
{
	MeasureData* measure = (MeasureData*)data;
	delete measure;

	--g_InstanceCount;
	if (g_InstanceCount == 0)
	{
		for (auto iter = g_BinData.cbegin(); iter != g_BinData.cend(); ++iter)
		{
			if ((*iter).directory)
			{
				CloseHandle((*iter).directory);
			}
		}

		WaitForSingleObject(g_Thread, INFINITE);
	}
}

PLUGIN_EXPORT void ExecuteBang(void* data, LPCWSTR args)
{
	MeasureData* measure = (MeasureData*)data;

	if (_wcsicmp(args, L"EmptyBin") == 0)
	{
		SHEmptyRecycleBin(NULL, NULL, 0);
	}
	else if (_wcsicmp(args, L"EmptyBinSilent") == 0)
	{
		SHEmptyRecycleBin(NULL, NULL, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
	}
	else if (_wcsicmp(args, L"OpenBin") == 0)
	{
		ShellExecute(NULL, L"open", L"explorer.exe", L"/N,::{645FF040-5081-101B-9F08-00AA002F954E}", NULL, SW_SHOW);
	}
}

unsigned int __stdcall QueryRecycleBinThreadProc(void* pParam)
{
	SHQUERYRBINFO rbi = {0};
	rbi.cbSize = sizeof(SHQUERYRBINFO);
	SHQueryRecycleBin(NULL, &rbi);
	g_BinCount = (double)rbi.i64NumItems;
	g_BinSize = (double)rbi.i64Size;

	CloseHandle(g_Thread);
	g_Thread = NULL;

	return 0;
}

HRESULT GetFolderCLSID(LPCWSTR path, CLSID* clsid)
{
	LPITEMIDLIST pidl;
	HRESULT hr = SHParseDisplayName(path, NULL, &pidl, 0, NULL);
	if (SUCCEEDED(hr))
	{
		IShellFolder* sf;
		LPCITEMIDLIST pidlLast;
		hr = SHBindToParent(pidl, IID_IShellFolder, (void**)&sf, &pidlLast);
		if (SUCCEEDED(hr))
		{
			SHDESCRIPTIONID did;
			hr = SHGetDataFromIDList(sf, pidlLast, SHGDFIL_DESCRIPTIONID, &did, sizeof(did));
			*clsid = did.clsid;

			sf->Release();
		}

		CoTaskMemFree(pidl);
	}
	return hr;
}

// Return value must be freed with LocalFree
LPWSTR GetCurrentUserSid()
{
	LPWSTR sidStr = NULL;
	HANDLE hToken;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
	{
		DWORD dwBufSize = 0;
		GetTokenInformation(hToken, TokenUser, NULL, 0, &dwBufSize);
		BYTE* buf = new BYTE[dwBufSize];

		if (GetTokenInformation(hToken, TokenUser, buf, dwBufSize, &dwBufSize))
		{
			TOKEN_USER* tu = (TOKEN_USER*)buf;
			if (ConvertSidToStringSid(tu->User.Sid, &sidStr))
			{
			}
		}

		delete [] buf;

		CloseHandle(hToken);
	}

	return sidStr;
}

HANDLE GetRecycleBinHandle(WCHAR drive, bool& isFAT)
{
	WCHAR search[] = L"\0:\\";
	search[0] = drive;

	WCHAR filesystem[16];
	if (!GetVolumeInformation(search, NULL, 0, NULL, NULL, NULL, filesystem, _countof(filesystem)))
	{
		return NULL;
	}

	if (wcscmp(filesystem, L"NTFS") == 0)
	{
		isFAT = false;
	}
	else if (wcscmp(filesystem, L"FAT") == 0 || wcscmp(filesystem, L"FAT32") == 0)
	{
		isFAT = true;
	}
	else
	{
		RmLog(LOG_ERROR, L"RecycleManager.dll: Unsupported filesystem");
		return NULL;
	}

	const WCHAR* binFolder;
	if (g_IsXP)
	{
		binFolder = isFAT ? L"RECYCLED" : L"RECYCLER";
	}
	else
	{
		binFolder = L"$RECYCLE.BIN";
	}

	HANDLE hDir = NULL;
	WCHAR binPath[MAX_PATH];
	_snwprintf_s(binPath, _TRUNCATE, L"%s%s\\", search, binFolder);

	DWORD binAttributes = GetFileAttributes(binPath);
	if (binAttributes != INVALID_FILE_ATTRIBUTES &&
		binAttributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY))
	{
		if (isFAT)
		{
			if (_waccess(binPath, 0) != -1)
			{
				isFAT = true;
				return hDir;
			}
		}
		else
		{
			// Get the correct, SID-specific bin for NTFS
			LPWSTR currentSid = GetCurrentUserSid();
			if (currentSid)
			{
				wcscat(binPath, currentSid);
				binAttributes = GetFileAttributes(binPath);

				if (binAttributes != INVALID_FILE_ATTRIBUTES &&
					binAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					CLSID id;
					HRESULT hr = GetFolderCLSID(binPath, &id);
					if (SUCCEEDED(hr) && IsEqualGUID(CLSID_RecycleBin, id))
					{
						hDir = CreateFile(
							binPath,
							GENERIC_READ,
							FILE_SHARE_READ | FILE_SHARE_WRITE,
							NULL,
							OPEN_EXISTING,
							FILE_FLAG_BACKUP_SEMANTICS,
							NULL);
					}
				}

				LocalFree(currentSid);
			}
		}
	}

	if (!hDir) RmLog(LOG_ERROR, L"RecycleManager.dll: Unable to find bin");
	return hDir;
}
