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

#include <atomic>
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

    QAudioDeviceInfo getAudioDevice() const;
    QList<QAudioDeviceInfo> getAudioDeviceList() const;

    bool isStereo() const;
    void setIsStereo(bool stereo);

    float getVolume() const; //{ return _input ? (float)_input->volume() : 0.0f; }
    void setVolume(float volume); //{ if (_input) { _input->setVolume(volume); } }

    QByteArray readAll();

signals:
    void deviceChanged(const QAudioDeviceInfo& device);
    void deviceListChanged(const QList<QAudioDeviceInfo>& devices);
    void deviceListLoudnessChanged(const QList<float>& loudness);

    void readyRead();

private slots:
    void onDeviceListChanged(QList<QAudioDeviceInfo> devices);

private:
    QAudioFormat setAudioDevice(int selection);
    void resetReadyRead(QIODevice* device);
    void checkDevices();
    void updateLoudness();

    QAudioFormat _format;
    std::atomic<bool> _formatChanged { false };
    bool _isStereo;

    QMetaObject::Connection readyReadConnection;

    QList<QAudioDeviceInfo> _deviceInfoList;
    QList<QAudioFormat> _formatList;
    QList<float> _loudnessList;
    QList<std::shared_ptr<QAudioInput>> _inputList;
    QList<std::shared_ptr<QIODevice>> _deviceList;
    std::atomic<int> _selected { -1 };
};

#endif // hifi_AudioInputs_h
