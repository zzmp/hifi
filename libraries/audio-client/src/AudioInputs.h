//
//  AudioInputs.h
//  libraries/audio-client/src
//
//  Created by Zach Pomerantz on 6/20/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_AudioInputs_h
#define hifi_AudioInputs_h

#include <memory>

#include <QObject>

#include <QAudioDeviceInfo>
#include <QAudioInput>

class AudioInputs : public QObject {
    Q_OBJECT

public:
    AudioInputs(const QAudioFormat& format);

    // returns the actual format on success, QAudioFormat() on failure
    QAudioFormat setAudioDevice(const QAudioDeviceInfo& deviceInfo = QAudioDeviceInfo());

    QAudioDeviceInfo getAudioDevice() const { return _deviceInfo; }
    QList<QAudioDeviceInfo> getAudioDeviceList() const { return _deviceList;  }

    bool isStereo() const;
    void setIsStereo(bool stereo);

    float getVolume() const { return _input ? (float)_input->volume() : 0.0f; }
    void setVolume(float volume) { if (_input) { _input->setVolume(volume); } }

    QByteArray readAll() { return _device ? _device->readAll() : QByteArray(); }

signals:
    void deviceChanged(const QAudioDeviceInfo& device);
    void deviceListChanged(const QList<QAudioDeviceInfo>& devices);

    void readyRead();

private slots:
    void onDeviceListChanged(QList<QAudioDeviceInfo> devices);

private:
    void checkDevices();

    QAudioFormat _format;

    QList<QAudioDeviceInfo> _deviceList;
    QAudioDeviceInfo _deviceInfo;

    // unique pointers with deleters
    template <class T> using Pointer = std::unique_ptr<T, void(*)(T*)>;
    Pointer<QAudioInput> _input;
    Pointer<QIODevice> _device;
};

#endif // hifi_AudioInputs_h
