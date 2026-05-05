#include "stl.h"

// disable warnings about unreferenced parameters, uninitialized object variables, __asm blocks, ...
#pragma warning(push)
#pragma warning(disable: 26495 4100 4410 4409 4740)

namespace bfs {
    __declspec(naked) void* operator_new(size_t)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x0045BAF0
        #else
            mov eax, 0x00494E6C
        #endif
            jmp eax
        }
    }

    __declspec(naked) void operator_delete(void*)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x0045BB60
        #else
            mov eax, 0x00494F3A
        #endif
            jmp eax
        }
    }

    __declspec(naked) string::string(char const* s)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3134
        #else
            mov eax, 0x00D3EED4
        #endif
            jmp [eax]
        }
    }
    __declspec(naked) string::string(char const* s, size_t count)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3220
        #else
            mov eax, 0x00D3EFD8
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) string::string(string const&)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3114
        #else
            mov eax, 0x00D3EEB4
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) string::string(string const&, size_t pos, size_t count)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3048
        #else
            mov eax, 0x00D3EDCC
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) string::string(size_t count, char c)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30BC
        #else
            mov eax, 0x00D3EE44
        #endif
            jmp[eax]
        }
    }

    __declspec(naked) string::string()
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3244
        #else
            mov eax, 0x00D3EEC4
        #endif
            jmp[eax]
        }
    }

    __declspec(naked) string::~string()
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C311C
        #else
            mov eax, 0x00D3EEBC
        #endif
            jmp[eax]
        }
    }

    __declspec(naked) const char* string::c_str() const
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30DC
        #else
            mov eax, 0x00D3EE08
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) string& string::replace(size_t pos, size_t len, string const& str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3160
        #else
            mov eax, 0x00D3EF08
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) string& string::append(const char* str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30C8
        #else
            mov eax, 0x00D3EDF4
        #endif
            jmp[eax]
        }
    }

    __declspec(naked) int string::compare(const char* str) const
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30F0
        #else
            mov eax, 0x00D3EF44
        #endif
            jmp[eax]
        }
    }

    __declspec(naked) wstring::wstring(wstring const&)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3170
        #else
            mov eax, 0x00D3EF24
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring::wstring(wchar_t const*)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30D8
        #else
            mov eax, 0x00D3EE04
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring::wstring()
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3184
        #else
            mov eax, 0x00D3EF38
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring::~wstring()
    {
        __asm {
            // make sure the destructor is not called on dumb objects
            mov eax,[ecx]
            cmp eax,0xff748751 // DUMB_OBJECT
            jnz dtor
            ret

            dtor:
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3178
        #else
            mov eax, 0x00D3EF2C
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) const wchar_t* wstring::c_str() const
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C3028
        #else
            mov eax, 0x00D3EE14
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring& wstring::replace(size_t pos, size_t length, wstring const& str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C329C
        #else
            mov eax, 0x00D3F050
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring& wstring::replace(size_t pos, size_t length, wchar_t const* str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C323C
        #else
            mov eax, 0x00D3F038
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring& wstring::append(wchar_t const* str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30C4
        #else
            mov eax, 0x00D3EDF0
        #endif
            jmp [eax]
        }
    }

    __declspec(naked) wstring& wstring::operator=(wstring const& str)
    {
        __asm {
        #ifndef TARGET_BF1942_R
            mov eax, 0x008C30CC
        #else
            mov eax, 0x00D3EDF8
        #endif
            jmp [eax]
        }
    }

}

#pragma warning(pop)
