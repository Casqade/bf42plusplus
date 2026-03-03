#pragma once

// A version number used for versioning customizations in the game protocol.
// Increment this on breaking network protocol change
const uint8_t PLUS_PROTOCOL_VERSION = 2;

/// <summary>
/// Get the release channel the updater updates to
/// </summary>
/// <returns></returns>
const wchar_t* get_update_release_channel();

/// <summary>
/// Get the release channel this binary is built for
/// Is this function needed?
/// </summary>
/// <returns></returns>
const wchar_t* get_build_release_channel();

/// <summary>
/// Get the binary's version
/// </summary>
/// <returns></returns>
const wchar_t* get_build_version();

/// <summary>
/// Get the binary's version number's components
/// </summary>
void get_build_version_components(uint8_t& major, uint8_t& minor, uint8_t& patch, uint8_t& build);
