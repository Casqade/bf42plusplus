#include "pch.h"


std::wstring UTF8ToWideString(const char* s, size_t len)
{
    std::wstring result;
    if (len == (size_t)-1) len = strlen(s);

    auto newLength = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    if (newLength == 0) {
        return result;
    }
    result.resize(newLength);
    MultiByteToWideChar(CP_UTF8, 0, s, len, result.data(), newLength);

    return result;
}

std::string WideStringToISO88591(std::wstring s)
{
    std::string result;
    result.resize(s.size());
    size_t i = 0;
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        if (*c < 256 && *c >= 0) result[i++] = (char)*c;
        else result[i++] = '?';
    }
    return result;
}

std::wstring ISO88591ToWideString(const std::string& s)
{
    std::wstring result;
    result.resize(s.size());
    size_t i = 0;
    for (auto c = s.cbegin(); c != s.cend(); c++) {
        // make sure char is casted to unsigned before it is casted to wide char
        result[i++] = (wchar_t)(uint8_t)*c;
    }
    return result;
}

bool HexStringToData(const std::wstring& str, std::vector<unsigned char>& data)
{
    if ((str.length() % 2) != 0) {
        return false;
    }
    data.resize(str.length() / 2);
    char high = -1;
    size_t idx = 0;
    for (auto it = str.cbegin(); it != str.cend(); it++) {
        int value;
        if (*it >= '0' && *it <= '9') value = *it - '0';
        else if (*it >= 'A' && *it <= 'F') value = *it - 'A' + 10;
        else if (*it >= 'a' && *it <= 'f') value = *it - 'a' + 10;
        else return false;
        if (high != -1) {
            data[idx++] = (high << 4) | (char)value;
            high = -1;
        }
        else {
            high = (char)value;
        }
    }
    return true;
}

bool HexDataToData(const std::vector<char>& hex, size_t offset, size_t length , std::vector<unsigned char>& data)
{
    if ((length % 2) != 0 || offset+length > hex.size()) {
        return false;
    }
    data.resize(length / 2);
    char high = -1;
    size_t idx = 0;
    for (size_t i = offset; i < (offset + length); i++) {
        int value;
        char c = hex.at(i);
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else return false;
        if (high != -1) {
            data[idx++] = (high << 4) | (char)value;
            high = -1;
        }
        else {
            high = (char)value;
        }
    }
    return true;
}

bool CenterWindow(HWND window)
{
    RECT windowsize, desktopsize;
    GetWindowRect(GetDesktopWindow(), &desktopsize);
    GetWindowRect(window, &windowsize);
    OffsetRect(&desktopsize, -((windowsize.right - windowsize.left) / 2), -((windowsize.bottom - windowsize.top) / 2));
    return SetWindowPos(window, 0, desktopsize.left + (desktopsize.right - desktopsize.left) / 2, desktopsize.top + (desktopsize.bottom - desktopsize.top) / 2, 0, 0, SWP_NOSIZE);
}

typedef std::pair<const char*, uint32_t> WebcolorItem;
static constexpr auto webcolors = std::to_array<WebcolorItem>({
    {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e}, {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
    {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1}, {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000},
    {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4}, {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32}, {"linen", 0xfaf0e6}, {"magenta", 0xff00ff},
    {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970}, {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080}, {"oldlace", 0xfdf5e6},
    {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
    {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32},
});

uint32_t GetColorFromString(const std::string& name)
{
    if (name.empty()) return InvalidColor;
    if (name.at(0) == '#') {
        // parse "#rrggbb" into 0xRRGGBB
        // parse "#rgb" into 0xRRGGBB
        const size_t size = name.size();
        uint32_t color;
        if (size != 4 && size != 7) return InvalidColor;

        auto res = std::from_chars(&name[1], &name[size], color, 16);
        if (res.ptr != &name[size]) return InvalidColor;

        if (size == 4) { // "#rgb" -> "#rrggbb"
            color = (0xF00000 & (color << 12)) | (0x0F0000 & (color << 8)) | (0x00F000 & (color << 8)) | (0x000F00 & (color << 4)) | (0x0000F0 & (color << 4)) | (0x00000F & color);
        }
        return color;
    }
    auto it = std::lower_bound(webcolors.begin(), webcolors.end(), name, [](const WebcolorItem& a, const std::string& b) {
        return _stricmp(a.first, b.c_str()) < 0;
    });
    if (it != webcolors.end() && _stricmp(it->first, name.c_str()) == 0) return it->second;

    return InvalidColor;
}

std::string GetStringFromColor(uint32_t color)
{
    for (auto& it : webcolors) {
        if (it.second == color) {
            return it.first;
        }
    }
    return std::format("#{:06x}", color & 0xFFFFFF);
}

std::string GetStringFromColors(std::vector<uint32_t>& colors)
{
    auto s = std::stringstream();
    bool first = true;
    for (auto& color : colors) {
        if (first) {
            first = false;
        }
        else {
            s << ' ';
        }
        s << GetStringFromColor(color);
    }
    return s.str();
}

uint8_t crc8(uint8_t* data, size_t length)
{
    uint8_t crc = 0xff;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (size_t n = 0; n < 8; n++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x97) : (crc << 1);
        }
    }
    return crc;
}
