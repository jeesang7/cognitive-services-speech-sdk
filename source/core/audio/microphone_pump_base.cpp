//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"

#include "create_object_helpers.h"
#include "service_helpers.h"
#include "microphone_pump_base.h"
#include "speechapi_cxx_enums.h"
#include "property_id_2_name_map.h"


namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {

using namespace std;

CSpxMicrophonePumpBase::CSpxMicrophonePumpBase():
    m_state {State::NoInput},
    m_format { WAVE_FORMAT_PCM, CHANNELS, SAMPLES_PER_SECOND, AVG_BYTES_PER_SECOND, BLOCK_ALIGN, BITS_PER_SAMPLE, 0 }
{    
}

void CSpxMicrophonePumpBase::Init()
{
    auto sys_audio_format = SetOptionsBeforeCreateAudioHandle();

    m_audioHandle = audio_create_with_parameters(sys_audio_format);

    SPX_IFTRUE_THROW_HR(m_audioHandle == nullptr, SPXERR_MIC_NOT_AVAILABLE);

    auto result = audio_setcallbacks(m_audioHandle,
        NULL, NULL,
        &CSpxMicrophonePumpBase::OnInputStateChange, (void*)this,
        &CSpxMicrophonePumpBase::OnInputWrite, (void*)this,
        NULL, NULL);
    SPX_IFTRUE_THROW_HR(result != AUDIO_RESULT_OK, SPXERR_MIC_ERROR);

    SetOptionsAfterCreateAudioHandle();
}

void CSpxMicrophonePumpBase::Term()
{
    audio_destroy(m_audioHandle);
}

AUDIO_WAVEFORMAT CSpxMicrophonePumpBase::SetOptionsBeforeCreateAudioHandle()
{
    return { m_format.wFormatTag, m_format.nChannels, m_format.nSamplesPerSec, m_format.nAvgBytesPerSec, m_format.nBlockAlign, m_format.wBitsPerSample };
}

void CSpxMicrophonePumpBase::SetOptionsAfterCreateAudioHandle()
{    
}

uint16_t CSpxMicrophonePumpBase::GetFormat(SPXWAVEFORMATEX* format, uint16_t size)
{
    auto totalSize = uint16_t(sizeof(SPXWAVEFORMATEX) + m_format.cbSize);
    if (format != nullptr)
    {
        memcpy(format, &m_format, min(totalSize, size));
    }
    return totalSize;
}

void CSpxMicrophonePumpBase::StartPump(SinkType processor)
{
    SPX_DBG_TRACE_SCOPE("MicrophonePumpBase::StartPump() ...", "MicrophonePumpBase::StartPump ... Done!");

    {
        unique_lock<mutex> lock(m_mutex);

        SPX_IFTRUE_THROW_HR(processor == nullptr, SPXERR_INVALID_ARG);
        SPX_IFTRUE_THROW_HR(m_audioHandle == nullptr, SPXERR_INVALID_ARG);
        SPX_IFTRUE_THROW_HR(m_state == State::Processing, SPXERR_AUDIO_IS_PUMPING);

        m_sink = std::move(processor);
    }

    SPX_DBG_TRACE_VERBOSE("%s starting audio input", __FUNCTION__);
    auto result = audio_input_start(m_audioHandle);
    SPX_IFTRUE_THROW_HR(result != AUDIO_RESULT_OK, SPXERR_MIC_ERROR);
    SPX_DBG_TRACE_VERBOSE("%s audio input started!", __FUNCTION__);

    unique_lock<mutex> lock(m_mutex);
    // wait for audio capture thread finishing getAudioReady.
    bool pred = m_cv.wait_for(lock, std::chrono::milliseconds(m_waitMsStartPumpRequestTimeout), [&state = m_state] { return (state != State::NoInput && state != State::Idle ); });
    SPX_IFTRUE_THROW_HR(pred == false, SPXERR_TIMEOUT);
}

void CSpxMicrophonePumpBase::StopPump()
{
    ReleaseSink resetSinkWhenExit(m_sink);

    SPX_DBG_TRACE_SCOPE("MicrophonePumpBase::StopPump ...", "MicrophonePumpBase::StopPump ... Done");

    SPX_IFTRUE_THROW_HR(m_audioHandle == nullptr, SPXERR_INVALID_ARG);
    SPX_IFTRUE_THROW_HR(m_sink == nullptr, SPXERR_INVALID_ARG);

    {
        unique_lock<mutex> lock(m_mutex);
        if (m_state == State::NoInput || m_state == State::Idle)
        {
            SPX_DBG_TRACE_VERBOSE("%s when we're already in State::Idle or State::NoInput state", __FUNCTION__);
            return;
        }
    }

    auto result = audio_input_stop(m_audioHandle);
    SPX_IFTRUE_THROW_HR(result != AUDIO_RESULT_OK, SPXERR_MIC_ERROR);

    // wait for the audio capture thread finishing setFormat(null)
    {
        unique_lock<mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(m_waitMsStopPumpRequestTimeout), [&state = m_state] { return state != State::Processing; });
    }

    // not release the sink may result in assert in m_resetRecoAdapter == nullptr in ~CSpxAudioStreamSession
}

ISpxAudioPump::State CSpxMicrophonePumpBase::GetState()
{
    SPX_DBG_TRACE_SCOPE("MicrophonePumpBase::GetState() ...", "MicrophonePumpBase::GetState ... Done");
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_state;
}

// this is called by audioCaptureThread, state value only changes here.
void CSpxMicrophonePumpBase::UpdateState(AUDIO_STATE state)
{
    SPX_DBG_TRACE_SCOPE("MicrophonePumpBase::UpdateState() ...", "MicrophonePumpBase::UpdateState ... Done!");
    unique_lock<mutex> lock(m_mutex);
    SPX_IFTRUE_THROW_HR(m_sink == nullptr, SPXERR_INVALID_ARG);

    SPX_DBG_TRACE_VERBOSE("%s: UpdateState with state as %d.", __FUNCTION__, int(state));
    switch (state)
    {
    case AUDIO_STATE_STARTING:
        m_sink->SetFormat(const_cast<SPXWAVEFORMATEX*>(&m_format));
        m_state = State::Processing;
        m_cv.notify_one();
        break;

    case AUDIO_STATE_STOPPED:
        // Let the sink know we're done for now...
        m_sink->SetFormat(nullptr);
        m_state = State::Idle;
        m_cv.notify_one();
        break;

    case AUDIO_STATE_RUNNING:
        break;

    default:
        SPX_DBG_TRACE_VERBOSE("%s: unexpected audio state: %d.", __FUNCTION__, int(state));
        SPX_THROW_ON_FAIL(SPXERR_INVALID_STATE);
    }

}

int CSpxMicrophonePumpBase::Process(const uint8_t* pBuffer, uint32_t size)
{
    int result = 0;
    SPX_IFTRUE_THROW_HR(m_sink == nullptr, SPXERR_INVALID_ARG);

    if (pBuffer != nullptr)
    {
        auto sharedBuffer = SpxAllocSharedAudioBuffer(size);
        memcpy(sharedBuffer.get(), pBuffer, size);
        m_sink->ProcessAudio(sharedBuffer, size);
    }

    return result;
}

uint16_t CSpxMicrophonePumpBase::GetChannelsFromConfig()
{
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    auto channels = properties->GetStringValue(GetPropertyName(PropertyId::AudioConfig_NumberOfChannelsForCapture));
    SPX_TRACE_INFO("The number of channels as a property is '%s' in CSpxMicrophonePump", channels.c_str());
    return channels.empty() ? 0 : static_cast<uint16_t>(stoi(channels));
}

std::string CSpxMicrophonePumpBase::GetDeviceNameFromConfig()
{
    auto properties = SpxQueryService<ISpxNamedProperties>(GetSite());
    SPX_IFTRUE_THROW_HR(properties == nullptr, SPXERR_INVALID_ARG);

    auto deviceName = properties->GetStringValue(GetPropertyName(PropertyId::AudioConfig_DeviceNameForCapture));
    SPX_TRACE_INFO("The device name of microphone as a property is '%s'", deviceName.c_str());

    return deviceName;
}

} } } } // Microsoft::CognitiveServices::Speech::Impl

