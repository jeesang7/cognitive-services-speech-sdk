//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// pull_audio_input_stream.cpp: Implementation definitions for CSpxPullAudioInputStream C++ class
//

#include "stdafx.h"
#include "pull_audio_input_stream.h"
#include "ispxinterfaces.h"
#include <chrono>
#include <cstring>


namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {


CSpxPullAudioInputStream::CSpxPullAudioInputStream()
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
}

CSpxPullAudioInputStream::~CSpxPullAudioInputStream()
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
}

void CSpxPullAudioInputStream::SetFormat(SPXWAVEFORMATEX* format)
{
    SPX_IFTRUE_THROW_HR(m_format != nullptr, SPXERR_ALREADY_INITIALIZED);

    // Allocate the buffer for the format
    auto formatSize = sizeof(SPXWAVEFORMATEX) + format->cbSize;
    m_format = SpxAllocWAVEFORMATEX(formatSize);

    // Copy the format
    memcpy(m_format.get(), format, formatSize);
}

void CSpxPullAudioInputStream::SetCallbacks(ReadCallbackFunction_Type readCallback, CloseCallbackFunction_Type closeCallback)
{
    m_readCallback = readCallback;
    m_closeCallback = closeCallback;
}

void CSpxPullAudioInputStream::SetPropertyCallback(GetPropertyCallbackFunction_Type getPropertyCallBack)
{
    m_getPropertyCallback = getPropertyCallBack;
}

uint16_t CSpxPullAudioInputStream::GetFormat(SPXWAVEFORMATEX* formatBuffer, uint16_t formatSize)
{
    uint16_t formatSizeRequired = sizeof(SPXWAVEFORMATEX) + m_format->cbSize;

    if (formatBuffer != nullptr)
    {
        size_t size = std::min(formatSize, formatSizeRequired);
        std::memcpy(formatBuffer, m_format.get(), size);
    }

    return formatSizeRequired;
}

uint32_t CSpxPullAudioInputStream::Read(uint8_t* buffer, uint32_t bytesToRead)
{
    auto bytesActuallyRead = m_readCallback(buffer, bytesToRead);
    return bytesActuallyRead;
}

void CSpxPullAudioInputStream::Close()
{
    SPX_DBG_TRACE_SCOPE(__FUNCTION__, __FUNCTION__);
    if (m_closeCallback != nullptr)
    {
        m_closeCallback();
    }
}

// Note: GetProperty should be called after Read the data buffer.
SPXSTRING CSpxPullAudioInputStream::GetProperty(PropertyId propertyId)
{
    if (m_getPropertyCallback != nullptr)
    {
        uint8_t result[m_maxPropertyLenInBytes];
        memset(result, 0, m_maxPropertyLenInBytes);

        m_getPropertyCallback(propertyId, result, m_maxPropertyLenInBytes);
        return reinterpret_cast<char*>(result);
    }
    return "";
}

} } } } // Microsoft::CognitiveServices::Speech::Impl