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

    // stop the current audio device
    if (_input) {
        _input->stop();
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

bool AudioInputs::isStereo() const {
    return _format.channelCount() == AudioConstants::STEREO;
}

void AudioInputs::setIsStereo(bool stereo) {
    if (isStereo() != stereo) {
        _format.setChannelCount(stereo ? AudioConstants::STEREO : AudioConstants::MONO);
        setAudioDevice(_deviceInfo);
    }
}