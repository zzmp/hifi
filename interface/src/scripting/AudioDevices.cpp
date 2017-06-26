//
//  AudioDevices.cpp
//  interface/src/scripting
//
//  Created by Zach Pomerantz on 28/5/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <map>

#include "AudioDevices.h"

#include "Application.h"
#include "AudioClient.h"
#include "Audio.h"

#include "UserActivityLogger.h"

using namespace scripting;

static Setting::Handle<QString> desktopInputDeviceSetting { QStringList { Audio::AUDIO, Audio::DESKTOP, "INPUT" }};
static Setting::Handle<QString> desktopOutputDeviceSetting { QStringList { Audio::AUDIO, Audio::DESKTOP, "OUTPUT" }};
static Setting::Handle<QString> hmdInputDeviceSetting { QStringList { Audio::AUDIO, Audio::HMD, "INPUT" }};
static Setting::Handle<QString> hmdOutputDeviceSetting { QStringList { Audio::AUDIO, Audio::HMD, "OUTPUT" }};

Setting::Handle<QString>& getSetting(bool contextIsHMD, QAudio::Mode mode) {
    if (mode == QAudio::AudioInput) {
        return contextIsHMD ? hmdInputDeviceSetting : desktopInputDeviceSetting;
    } else { // if (mode == QAudio::AudioOutput)
        return contextIsHMD ? hmdOutputDeviceSetting : desktopOutputDeviceSetting;
    }
}

enum AudioDeviceRole {
    DisplayRole = Qt::DisplayRole,
    CheckStateRole = Qt::CheckStateRole,
    PeakRole = Qt::UserRole
};

QHash<int, QByteArray> AudioDeviceList::_roles {
    { DisplayRole, "display" },
    { CheckStateRole, "selected" },
    { PeakRole, "peak" }
};
Qt::ItemFlags AudioDeviceList::_flags { Qt::ItemIsSelectable | Qt::ItemIsEnabled };

QVariant AudioDeviceList::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) {
        return QVariant();
    }

    if (role == DisplayRole) {
        return _devices.at(index.row())->display;
    } else if (role == CheckStateRole) {
        return _devices.at(index.row())->selected;
    } else {
        return QVariant();
    }
}

QVariant AudioInputDeviceList::data(const QModelIndex& index, int role) const {
    if (index.isValid() && index.row() < rowCount() && role == PeakRole) {
        return std::static_pointer_cast<AudioInputDevice>(_devices.at(index.row()))->peak;
    } else {
        return AudioDeviceList::data(index, role);
    }
}

bool AudioDeviceList::setData(const QModelIndex& index, const QVariant& value, int role) {
	if (!index.isValid() || index.row() >= rowCount() || role != CheckStateRole) {
        return false;
    }

    // only allow switching to a new device, not deactivating an in-use device
    auto selected = value.toBool();
    if (!selected) {
        return false;
    }

    return setDevice(index.row(), true);
}

bool AudioDeviceList::setDevice(int row, bool fromUser) {
    bool success = false;
    auto& device = _devices[row];

    // skip if already selected
    if (!device->selected) {
        auto client = DependencyManager::get<AudioClient>();
        QMetaObject::invokeMethod(client.data(), "switchAudioDevice", Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, success),
            Q_ARG(QAudio::Mode, _mode),
            Q_ARG(const QAudioDeviceInfo&, device->info));

        if (success) {
            device->selected = true;
            if (fromUser) {
                emit deviceSelected(device->info, _selectedDevice);
            }
            emit deviceChanged(device->info);
        }
    }

    emit dataChanged(createIndex(0, 0), createIndex(rowCount() - 1, 0));
    return success;
}

void AudioDeviceList::resetDevice(bool contextIsHMD, const QString& device) {
    bool success { false };

    // try to set the last selected device
    if (!device.isNull()) {
        auto i = 0;
        for (; i < rowCount(); ++i) {
            if (device == _devices[i]->info.deviceName()) {
                break;
            }
        }
        if (i < rowCount()) {
            success = setDevice(i, false);
        }

        // the selection failed - reset it
        if (!success) {
            emit deviceSelected();
        }
    }

    // try to set to the default device for this mode
    if (!success) {
        auto client = DependencyManager::get<AudioClient>().data();
        if (contextIsHMD) {
            QString deviceName;
            if (_mode == QAudio::AudioInput) {
                deviceName = qApp->getActiveDisplayPlugin()->getPreferredAudioInDevice();
            } else { // if (_mode == QAudio::AudioOutput)
                deviceName = qApp->getActiveDisplayPlugin()->getPreferredAudioOutDevice();
            }
            if (!deviceName.isNull()) {
                QMetaObject::invokeMethod(client, "switchAudioDevice", Q_ARG(QAudio::Mode, _mode), Q_ARG(QString, deviceName));
            }
        } else {
            // use the system default
            QMetaObject::invokeMethod(client, "switchAudioDevice", Q_ARG(QAudio::Mode, _mode));
        }
    }
}

void AudioDeviceList::onDeviceChanged(const QAudioDeviceInfo& device) {
    _selectedDevice = device;
    QModelIndex index;

    for (auto i = 0; i < rowCount(); ++i) {
        AudioDevice& device = *_devices[i];

        if (device.selected && device.info != _selectedDevice) {
            device.selected = false;
        } else if (device.info == _selectedDevice) {
            device.selected = true;
            index = createIndex(i, 0);
        }
    }

    emit deviceChanged(_selectedDevice);
    emit dataChanged(createIndex(0, 0), createIndex(rowCount() - 1, 0));
}

void AudioDeviceList::onDeviceListChanged(const QList<QAudioDeviceInfo>& devices) {
    beginResetModel();

    _devices.clear();

    foreach(const QAudioDeviceInfo& deviceInfo, devices) {
        AudioDevice device;
        device.info = deviceInfo;
        device.display = device.info.deviceName()
            .replace("High Definition", "HD")
            .remove("Device")
            .replace(" )", ")");
        device.selected = (device.info == _selectedDevice);
        _devices.push_back(newDevice(device));
    }

    endResetModel();
}

void AudioInputDeviceList::onInputListLoudnessChanged(const QList<float>& loudness) {
    assert(_mode == QAudio::AudioInput);

    // loudness is not available on all OSes: mark it available when received
    std::call_once(_peakFlag, [&] { _peakAvailable = true; });

    if (loudness.length() != rowCount()) {
        qWarning() << "AudioDeviceList" << __FUNCTION__ << "length mismatch";
    }

    for (auto i = 0; i < rowCount(); ++i) {
        std::static_pointer_cast<AudioInputDevice>(_devices[i])->peak = Audio::loudnessToLevel(loudness[i]);
    }

    emit dataChanged(createIndex(0, 0), createIndex(rowCount() - 1, 0), { PeakRole });
}

AudioDevices::AudioDevices(bool& contextIsHMD) : _contextIsHMD(contextIsHMD) {
    auto client = DependencyManager::get<AudioClient>();

    connect(client.data(), &AudioClient::deviceChanged, this, &AudioDevices::onDeviceChanged, Qt::QueuedConnection);
    connect(client.data(), &AudioClient::deviceListChanged, this, &AudioDevices::onDeviceListChanged, Qt::QueuedConnection);
    connect(client.data(), &AudioClient::inputListLoudnessChanged, &_inputs, &AudioInputDeviceList::onInputListLoudnessChanged, Qt::QueuedConnection);

    // connections are made after client is initialized, so we must also fetch the devices
    _inputs.onDeviceChanged(client->getActiveAudioDevice(QAudio::AudioInput));
    _outputs.onDeviceChanged(client->getActiveAudioDevice(QAudio::AudioOutput));
    _inputs.onDeviceListChanged(client->getAudioDevices(QAudio::AudioInput));
    _outputs.onDeviceListChanged(client->getAudioDevices(QAudio::AudioOutput));

    connect(&_inputs, &AudioDeviceList::deviceSelected, [&](const QAudioDeviceInfo& device, const QAudioDeviceInfo& previousDevice) {
        onDeviceSelected(QAudio::AudioInput, device, previousDevice);
    });
    connect(&_outputs, &AudioDeviceList::deviceSelected, [&](const QAudioDeviceInfo& device, const QAudioDeviceInfo& previousDevice) {
        onDeviceSelected(QAudio::AudioOutput, device, previousDevice);
    });
}

void AudioDevices::onContextChanged(const QString& context) {
    auto input = getSetting(_contextIsHMD, QAudio::AudioInput).get();
    auto output = getSetting(_contextIsHMD, QAudio::AudioOutput).get();

    _inputs.resetDevice(_contextIsHMD, input);
    _outputs.resetDevice(_contextIsHMD, output);
}

void AudioDevices::onDeviceSelected(QAudio::Mode mode, const QAudioDeviceInfo& device, const QAudioDeviceInfo& previousDevice) {
    QString deviceName = device.isNull() ? QString() : device.deviceName();

    auto& setting = getSetting(_contextIsHMD, mode);

    // check for a previous device
    auto wasDefault = setting.get().isNull();

    // store the selected device
    setting.set(deviceName);

    // log the selected device
    if (!device.isNull()) {
        QJsonObject data;

        const QString MODE = "audio_mode";
        const QString INPUT = "INPUT";
        const QString OUTPUT = "OUTPUT"; data[MODE] = mode == QAudio::AudioInput ? INPUT : OUTPUT;

        const QString CONTEXT = "display_mode";
        data[CONTEXT] = _contextIsHMD ? Audio::HMD : Audio::DESKTOP;

        const QString DISPLAY = "display_device";
        data[DISPLAY] = qApp->getActiveDisplayPlugin()->getName();

        const QString DEVICE = "device";
        const QString PREVIOUS_DEVICE = "previous_device";
        const QString WAS_DEFAULT = "was_default";
        data[DEVICE] = deviceName;
        data[PREVIOUS_DEVICE] = previousDevice.deviceName();
        data[WAS_DEFAULT] = wasDefault;

        UserActivityLogger::getInstance().logAction("selected_audio_device", data);
    }
}

void AudioDevices::onDeviceChanged(QAudio::Mode mode, const QAudioDeviceInfo& device) {
    if (mode == QAudio::AudioInput) {
        _inputs.onDeviceChanged(device);
    } else { // if (mode == QAudio::AudioOutput)
        _outputs.onDeviceChanged(device);
    }
}

void AudioDevices::onDeviceListChanged(QAudio::Mode mode, const QList<QAudioDeviceInfo>& devices) {
    static bool initialized { false };
    auto initialize = [&]{
        if (initialized) {
            onContextChanged(QString());
        } else {
            initialized = true;
        }
    };

    if (mode == QAudio::AudioInput) {
        _inputs.onDeviceListChanged(devices);
        static std::once_flag inputFlag;
        std::call_once(inputFlag, initialize);
    } else { // if (mode == QAudio::AudioOutput)
        _outputs.onDeviceListChanged(devices);
        static std::once_flag outputFlag;
        std::call_once(outputFlag, initialize);
    }
}
