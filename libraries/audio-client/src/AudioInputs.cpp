//
//  AudioInputs.cpp
//  libraries/audio-client/src
//
//  Created by Zach Pomerantz on 6/20/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AudioInputs.h"

#include <QtConcurrent/QtConcurrent>

#include "AudioClient.h"
#include "AudioClientLogging.h"
#include "AudioConstants.h"

#ifdef WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#endif

using Mutex = std::mutex;
using Lock = std::unique_lock<Mutex>;
extern Mutex deviceMutex; // defined in AudioClient.cpp

void inputDeleter(QAudioInput* input) {
    input->stop();
    input->deleteLater();
}

void deviceDeleter(QIODevice* device) { /* nop */ }

AudioInputs::AudioInputs(const QAudioFormat& format) :
    _format(format),
    _input(nullptr, &inputDeleter),
    _device(nullptr, &deviceDeleter)
{
    // initialize wasapi for QAudio::AudioInput
    checkDevices();

    // set up regular checks for device changes
    auto checkDevicesTimer = new QTimer(this);
    connect(checkDevicesTimer, &QTimer::timeout, [this] {
        QtConcurrent::run(QThreadPool::globalInstance(), [this] { checkDevices(); });
    });
    const unsigned long DEVICE_CHECK_INTERVAL_MSECS = 2 * 1000;
    checkDevicesTimer->start(DEVICE_CHECK_INTERVAL_MSECS);

#ifdef WIN32
    auto loudnessTimer = new QTimer(this);
    connect(loudnessTimer, &QTimer::timeout, [this] {
        QtConcurrent::run(QThreadPool::globalInstance(), [this] { updateLoudness(); });
    });
    const unsigned long LOUDNESS_INTERVAL_MSECS = 1000 / 30; // QML refresh rate
    loudnessTimer->start(LOUDNESS_INTERVAL_MSECS);
#endif
}

void AudioInputs::checkDevices() {
    auto devices = AudioClient::getAvailableDevices(QAudio::AudioInput);
    if (devices != _deviceList) {
        QMetaObject::invokeMethod(this, "onDeviceListChanged", Q_ARG(QList<QAudioDeviceInfo>, devices));
    }
}

void AudioInputs::onDeviceListChanged(QList<QAudioDeviceInfo> devices) {
    _deviceList.swap(devices);
    emit deviceListChanged(devices);
}

QAudioFormat AudioInputs::setAudioDevice(const QAudioDeviceInfo& deviceInfo) {
    Lock lock(deviceMutex);
    _deviceInfo = deviceInfo;

    // reset the current audio device
    if (_input) {
        _input.reset();
        _device.reset();
    }

    if (deviceInfo.isNull()) {
        return QAudioFormat();
    }

    // emit the change so long as it is non-null, regardless of success
    emit deviceChanged(deviceInfo);

    // check compatibility
    QAudioFormat format;
    bool isCompatible = AudioClient::getAdjustedFormat(deviceInfo, _format, format);
    if (!isCompatible) {
        qCDebug(audioclient) << "AudioInputs - device incompatible:" << deviceInfo.deviceName() << _format;
        qCDebug(audioclient) << "AudioInputs - closest compatible format:" << deviceInfo.nearestFormat(_format);
        return QAudioFormat();
    }

    // check channel count
    if (format.channelCount() != _format.channelCount()) {
        qCDebug(audioclient) << "AudioInputs - channel count unavailable:" << deviceInfo.deviceName() << _format;
        return QAudioFormat();
    }

    // instantiate the device
    _input = Pointer<QAudioInput>(new QAudioInput(deviceInfo, format, this), &inputDeleter);
    int bufferSize = AudioClient::calculateBufferSize(format);
    _input->setBufferSize(bufferSize);
    _device = Pointer<QIODevice>(_input->start(), &deviceDeleter);

    // check for success
    if (!_device) {
        qCDebug(audioclient) << "AudioInputs - error starting:" << _input->error();
        return QAudioFormat();
    }

    qCDebug(audioclient) << "AudioInputs - set:" << deviceInfo.deviceName() << format;

    // connect directly to avoid audio lag
    connect(_device.get(), &QIODevice::readyRead, this, &AudioInputs::readyRead, Qt::DirectConnection);

    return format;
}

#ifdef WIN32
std::vector<IAudioClient*> activeClients;
#endif

void AudioInputs::updateLoudness() {
#ifdef WIN32
    // initialize the payload
    QList<float> loudness;
    for (int i = 0; i < _deviceList.size(); ++i) {
        loudness.push_back(0.0f);
    }

    CoInitialize(NULL);

    if (!_enableLoudness) {
        for (auto audioClient : activeClients) {
            audioClient->Stop();
            audioClient->Release();
        }
        activeClients.clear();
    }

    HRESULT result;
    IMMDeviceEnumerator* enumerator;
    result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(result)) {
        return;
    }

    IMMDeviceCollection* endpoints;
    result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &endpoints);
    if (FAILED(result)) {
        enumerator->Release();
        return;
    }

    UINT count;
    result = endpoints->GetCount(&count);
    if (FAILED(result)) {
        endpoints->Release();
        enumerator->Release();
        return;
    }

    IMMDevice* device;
    IAudioMeterInformation* meterInfo;
    float peakValue;
    for (UINT i = 0; i < count; ++i) {
        result = endpoints->Item(i, &device);
        if (FAILED(result)) {
            continue;
        }

        result = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, NULL, (void**)&meterInfo);
        if (FAILED(result)) {
            device->Release();
            continue;
        }

        if (_enableLoudness) {
            DWORD hardwareSupport;
            result = meterInfo->QueryHardwareSupport(&hardwareSupport);
            if (FAILED(result)) {
                device->Release();
                continue;
            }

            IAudioClient *audioClient = NULL;
            if (!(hardwareSupport & ENDPOINT_HARDWARE_SUPPORT_METER)) {
                result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&audioClient);
                LPWAVEFORMATEX format = NULL;
                audioClient->GetMixFormat(&format);
                audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, format, NULL);
                audioClient->Start();
                activeClients.push_back(audioClient);
            }
        }

        meterInfo->GetPeakValue(&peakValue);
        QString deviceName = AudioClient::getWinDeviceName(device);
        for (int j = 0; j < loudness.size(); ++j) {
            if (deviceName == _deviceList[j].deviceName()) {
                loudness[j] = peakValue;
                break;
            }
        }

        meterInfo->Release();
    }

    endpoints->Release();
    enumerator->Release();

    emit deviceListLoudnessChanged(loudness);
#endif
}

void AudioInputs::setIsStereo(bool stereo) {
    if (isStereo() != stereo) {
        _isStereo = stereo;
        _format.setChannelCount(stereo ? AudioConstants::STEREO : AudioConstants::MONO);
        setAudioDevice(_deviceInfo);
    }
}