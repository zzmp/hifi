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

using Mutex = std::mutex;
using Lock = std::unique_lock<Mutex>;
extern Mutex deviceMutex; // defined in AudioClient.cpp

void inputDeleter(QAudioInput* input) {
    input->stop();
    input->deleteLater();
}

void deviceDeleter(QIODevice* device) { /* nop */ }

AudioInputs::AudioInputs(const QAudioFormat& format) : _format(format) {
    // initialize wasapi for QAudio::AudioInput
    checkDevices();

    // set up regular checks for device changes
    auto checkDevicesTimer = new QTimer(this);
    connect(checkDevicesTimer, &QTimer::timeout, [this] {
        QtConcurrent::run(QThreadPool::globalInstance(), [this] { checkDevices(); });
    });
    const unsigned long DEVICE_CHECK_INTERVAL_MSECS = 2 * 1000;
    checkDevicesTimer->start(DEVICE_CHECK_INTERVAL_MSECS);

    // set up regular updates to device loudness
    auto loudnessTimer = new QTimer(this);
    connect(loudnessTimer, &QTimer::timeout, [this] {
        QtConcurrent::run(QThreadPool::globalInstance(), [this] { updateLoudness(); });
    });
    const unsigned long LOUDNESS_INTERVAL_MSECS = 1000 / 20;
    loudnessTimer->start(LOUDNESS_INTERVAL_MSECS);
}

void AudioInputs::checkDevices() {
    auto devices = AudioClient::getAvailableDevices(QAudio::AudioInput);
    if (devices != _deviceInfoList) {
        QMetaObject::invokeMethod(this, "onDeviceListChanged", Q_ARG(QList<QAudioDeviceInfo>, devices));
    }
}

void AudioInputs::onDeviceListChanged(QList<QAudioDeviceInfo> devices) {
    Lock lock(deviceMutex);

    QList<QAudioFormat> formatList;
    QList<float> loudnessList;
    QList<std::shared_ptr<QAudioInput>> inputList;
    QList<std::shared_ptr<QIODevice>> deviceList;
    int selection = -1;
    bool shouldResetReadyRead = true;

    for (int i = 0; i < devices.size(); ++i) {
        formatList.push_back(QAudioFormat());
        loudnessList.push_back(0.0f);
        inputList.push_back(nullptr);
        deviceList.push_back(nullptr);

        // check for existing device
        for (int j = 0; j < _deviceInfoList.size(); ++j) {
            if (devices[i] == _deviceInfoList[j]) {
                if (_selected == j) {
                    selection = i;
                    shouldResetReadyRead = !_formatChanged;
                }

                // if the format changed, open a new device
                if (_formatChanged) {
                    _inputList[j].reset();
                    _deviceList[j].reset();
                    break;
                }

                // reuse the existing open device
                formatList[i] = _formatList[j];
                inputList[i] = _inputList[j];
                deviceList[i] = _deviceList[j];

                ++i;
                break;
            }
        }

        // open a new device
        QAudioDeviceInfo& deviceInfo = devices[i];

        // check compatibility
        QAudioFormat format;
        bool isCompatible = AudioClient::getAdjustedFormat(deviceInfo, _format, format);
        if (!isCompatible) {
            qCDebug(audioclient) << "AudioInputs - device incompatible:" << deviceInfo.deviceName() << _format;
            qCDebug(audioclient) << "AudioInputs - closest compatible format:" << deviceInfo.nearestFormat(_format);
            continue;
        }

        // check channel count
        if (format.channelCount() != _format.channelCount()) {
            qCDebug(audioclient) << "AudioInputs - channel count unavailable:" << deviceInfo.deviceName() << _format;
            continue;
        }

        // instantiate the device
        auto input = inputList[i] =
            std::shared_ptr<QAudioInput>(new QAudioInput(deviceInfo, format, this), &inputDeleter);
        int bufferSize = AudioClient::calculateBufferSize(format);
        input->setBufferSize(bufferSize);
        auto device = deviceList[i] = std::shared_ptr<QIODevice>(input->start(), &deviceDeleter);

        // check for success
        if (!device) {
            qCDebug(audioclient) << "AudioInputs - error starting:" << input->error();
            continue;
        }

        formatList[i] = format;
        qCDebug(audioclient) << "AudioInputs - set:" << deviceInfo.deviceName() << format;
    }

    _selected = -1;
    _formatChanged = false;
    _deviceInfoList.swap(devices);
    _formatList.swap(formatList);
    _loudnessList.swap(loudnessList);
    _inputList.swap(inputList);
    _deviceList.swap(deviceList);
    _selected = selection;

    if (shouldResetReadyRead) {
        if (_selected != -1) {
            resetReadyRead(_deviceList[_selected].get());
        }
    }

    emit deviceListChanged(devices);
}

QAudioFormat AudioInputs::setAudioDevice(const QAudioDeviceInfo& deviceInfo) {
    Lock lock(deviceMutex);

    for (int i = 0; i < _deviceInfoList.size(); ++i) {
        if (deviceInfo == _deviceInfoList[i]) {
            return setAudioDevice(i);
        }
    }

    resetReadyRead(nullptr);
    return QAudioFormat();
}

QAudioFormat AudioInputs::setAudioDevice(int selection) {
    _selected = selection;

    // emit the change so long as it is non-null, regardless of success
    emit deviceChanged(_deviceInfoList[_selected]);
    resetReadyRead(_deviceList[_selected].get());

    if (_deviceList[_selected]) {
        qCDebug(audioclient) << "AudioInputs - device switched:" << _deviceInfoList[_selected].deviceName();
        return _formatList[_selected];
    } else {
        qCDebug(audioclient) << "AudioInputs - device unavailable:" << _deviceInfoList[_selected].deviceName();
        qCDebug(audioclient) << "AudioInputs - see device initialization for details";
        return QAudioFormat();
    }
}

QAudioDeviceInfo AudioInputs::getAudioDevice() const {
    Lock lock(deviceMutex);
    return (_selected != -1) ? _deviceInfoList[_selected] : QAudioDeviceInfo();
}

QList<QAudioDeviceInfo> AudioInputs::getAudioDeviceList() const {
    Lock lock(deviceMutex);
    return _deviceInfoList;
}

inline float getLoudness(const QByteArray& buffer) {
    const int16_t* samples = reinterpret_cast<const int16_t*>(buffer.data());
    int numSamples = buffer.size() / AudioConstants::SAMPLE_SIZE;
    assert(numSamples < 65536); // int32_t loudness cannot overflow

    int32_t loudness = 0;
    for (int i = 0; i < numSamples; ++i) {
        int32_t sample = std::abs((int32_t)samples[i]);
        loudness += sample;
    }

    return (float)loudness / numSamples;
}

void AudioInputs::updateLoudness() {
    Lock lock(deviceMutex);

    for (int i = 0; i < _deviceList.size(); ++i) {
        if (_selected != i && _deviceList[i]) {
            QByteArray buffer = _deviceList[i]->readAll();
            _loudnessList[i] = getLoudness(buffer);
        }
    }

    emit deviceListLoudnessChanged(_loudnessList);
}

void AudioInputs::resetReadyRead(QIODevice* device) {
    disconnect(readyReadConnection);
    if (device) {
        readyReadConnection = connect(device, &QIODevice::readyRead, this, &AudioInputs::readyRead,
            // connect directly to avoid audio lag
            Qt::DirectConnection);
    }
}

QByteArray AudioInputs::readAll() {
    if (_selected == -1 || !_deviceList[_selected]) {
        return QByteArray();
    }

    QByteArray buffer = _deviceList[_selected]->readAll();
    _loudnessList[_selected] = getLoudness(buffer);
    return buffer;
}

bool AudioInputs::isStereo() const {
    // this is called before every call to readAll, so it is cached
    return _isStereo;
}

void AudioInputs::setIsStereo(bool stereo) {
    if (isStereo() != stereo) {
        _format.setChannelCount(stereo ? AudioConstants::STEREO : AudioConstants::MONO);
        _isStereo = stereo;

        // reopen devices with new stereo setting
        _formatChanged = true;
        QMetaObject::invokeMethod(this, "onDeviceListChanged", Q_ARG(QList<QAudioDeviceInfo>, _deviceInfoList));
    }
}

float AudioInputs::getVolume() const {
    Lock(deviceMutex);
    if (_selected != -1 && _inputList[_selected]) {
        return (float)_inputList[_selected]->volume();
    } else {
        return 0.0f;
    }
}

void AudioInputs::setVolume(float volume) {
    Lock(deviceMutex);
    if (_selected != -1 && _inputList[_selected]) {
        _inputList[_selected]->setVolume(volume);
    }
}