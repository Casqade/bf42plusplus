#include "pch.h"
#include <shellapi.h>
#include <Shlwapi.h>

#include "gitversion.h"

const wchar_t* get_update_release_channel()
{
#ifdef _DEBUG
    return L"debug";
#else
    return L"release";
#endif
}

const wchar_t* get_build_release_channel()
{
#ifdef _DEBUG
    return L"debug";
#else
    return L"release";
#endif
}


#define M_TO_STRING_(s)	#s
#define M_TO_STRING(s)	M_TO_STRING_(s)

const wchar_t* get_build_version()
{
    return L"" M_TO_STRING(GIT_VERSION);
}

extern const char _build_version_dummy[] = "_build_version_=" M_TO_STRING(GIT_VERSION);

static const wchar_t* get_user_agent()
{
    return L"BF42Plus/" M_TO_STRING(GIT_VERSION);
}

void get_build_version_components(uint8_t& major, uint8_t& minor, uint8_t& patch, uint8_t& build)
{
    uint16_t tmp;
    major = minor = patch = build = 0;
    auto ver = std::stringstream(M_TO_STRING(GIT_VERSION));
    ver >> tmp; major = (uint8_t)tmp;
    ver.get(); // skip delimiter
    ver >> tmp; minor = (uint8_t)tmp;
    ver.get(); // skip delimiter
    ver >> tmp; patch = (uint8_t)tmp;
    ver.get(); // skip delimiter
    ver >> tmp; build = (uint8_t)tmp;
}

int compare_version(std::wstring older, std::wstring newer)
{
    auto sOld = std::wistringstream(older), sNew = std::wistringstream(newer);
    for (int i = 0; i < 4; i++) {
        int oldN = 0, newN = 0;
        sOld >> oldN; sNew >> newN; // read a number from each version, default to 0
        if (oldN < newN) return 1; // newer is newer than older
        else if (oldN > newN) return -1; // newer is older
        sOld.get(); sNew.get(); // skip a delimiter character
    }
    return 0; // versions are equal
}
