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

static LPCWSTR FormatSystemMessage(DWORD errorCode)
{
  static WCHAR messageBuf[256] {};

  auto result = FormatMessage(
    FORMAT_MESSAGE_FROM_SYSTEM |
    FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL, errorCode, 0,
    messageBuf, sizeof(messageBuf),
    NULL );

  if (result != 0)
    return messageBuf;

  return nullptr;
}

static void PrintLastErrorMessage(const std::wstring& message)
{
  DWORD errorCode = GetLastError();
  PrintMessage(message);
  PrintMessage(FormatSystemMessage(errorCode));
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
    PrintLastErrorMessage(L"Failed to allocate memory in the remote process");
    return false;
  }

  if (WriteProcessMemory(processHandle, allocatedMemoryAddress, dllPath.c_str(), dllAddressSize, NULL) != TRUE)
  {
    PrintLastErrorMessage(L"Failed to write to the allocated memory in the remote process");
    return false;
  }

  HANDLE threadHandle = CreateRemoteThread(
    processHandle, NULL, 0, (LPTHREAD_START_ROUTINE)LoadLibraryW, allocatedMemoryAddress, 0, NULL);

  if (threadHandle == NULL)
  {
    PrintLastErrorMessage(L"Failed to create a remote thread in the target process");
    return false;
  }

  WaitForSingleObject(threadHandle, INFINITE);

  VirtualFreeEx(processHandle, allocatedMemoryAddress, 0, MEM_RELEASE);

  CloseHandle(threadHandle);

  return true;
}


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
#ifndef TARGET_BF1942_R
  std::wstring exePath = L"BF1942.exe";
  std::wstring dllPath = L"bf42++.dll";
#else
  std::wstring exePath = L"BF1942_r.exe";
  std::wstring dllPath = L"bf42_r++.dll";
#endif

  SetEnvironmentVariable(L"BF42PLUSPLUS_INJECTED", L"");

  STARTUPINFO si = {0};
  si.cb = sizeof(STARTUPINFO);

  PROCESS_INFORMATION pi {};

  std::wstring commandLine = exePath + L" " + lpCmdLine;

  BOOL success = CreateProcess(
    exePath.c_str(), &commandLine.front(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);

  if (success == FALSE || InjectDLL(dllPath.c_str(), pi.hProcess) == false)
  {
    std::wstring message = L"Failed to inject '" + dllPath + L"' into '" + exePath + L"'";

    if (success == FALSE)
    {
      PrintLastErrorMessage(message);
      return 0;
    }
    else
    {
      PrintMessage(message);
      TerminateProcess(pi.hProcess, 0);
    }
  }
  else
    ResumeThread(pi.hThread);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return 0;
}
