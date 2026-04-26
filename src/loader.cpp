//#define WINVER 0x0502
//#define _WIN32_WINNT 0x0502
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <string>


static void PrintMessage(const std::wstring& message)
{
  MessageBox(NULL, message.c_str(), NULL, MB_OK);
}

static bool FileExists(const std::wstring& inFileAddress)
{
  return GetFileAttributesW(inFileAddress.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool InjectDLL(const std::wstring& dllPath, HANDLE processHandle)
{
  bool result = false;

  if (FileExists(dllPath) == false )
  {
    PrintMessage(L"'" + dllPath + L"' could not be found");
    return false;
  }

  auto dllAddressSize = (dllPath.size() + 1) * sizeof(wchar_t);

  auto allocatedMemoryAddress = VirtualAllocEx(processHandle, NULL, dllAddressSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

  if (allocatedMemoryAddress == NULL)
  {
    PrintMessage(L"Failed to allocate memory in the remote process");
    return false;
  }

  if (WriteProcessMemory(processHandle, allocatedMemoryAddress, dllPath.c_str(), dllAddressSize, NULL) != TRUE)
  {
    PrintMessage(L"Failed to write to the allocated memory in the remote process");
    return false;
  }

  HANDLE threadHandle = CreateRemoteThread(
    processHandle, NULL, 0, (LPTHREAD_START_ROUTINE)LoadLibraryW, allocatedMemoryAddress, 0, NULL);

  if (threadHandle == NULL)
  {
    PrintMessage(L"Failed to create a remote thread in the target process");
    return false;
  }

  WaitForSingleObject(threadHandle, INFINITE);

  VirtualFreeEx(processHandle, allocatedMemoryAddress, 0, MEM_RELEASE);

  CloseHandle(threadHandle);

  return true;
}


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
  std::wstring exePath = L"BF1942.exe";
  std::wstring dllPath = L"bf42++.dll";

  SetEnvironmentVariable(L"BF42PLUSPLUS_INJECTED", L"");

  STARTUPINFO si = {0};
  si.cb = sizeof(STARTUPINFO);

  PROCESS_INFORMATION pi {};

  std::wstring commandLine = exePath + L" " + lpCmdLine;

  DWORD processId = CreateProcess(
    NULL, &commandLine.front(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);

  if (processId != 0 && InjectDLL(dllPath.c_str(), pi.hProcess))
    ResumeThread(pi.hThread);
  else
    PrintMessage(L"Failed to inject '" + dllPath + L"' into '" + exePath + L"'");

  return 0;
}
