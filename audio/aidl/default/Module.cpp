/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <set>

#define LOG_TAG "AHAL_Module"
#include <android-base/logging.h>
#include <android/binder_ibinder_platform.h>

#include <Utils.h>
#include <aidl/android/media/audio/common/AudioInputFlags.h>
#include <aidl/android/media/audio/common/AudioOutputFlags.h>

#include "core-impl/Bluetooth.h"
#include "core-impl/Module.h"
#include "core-impl/ModuleUsb.h"
#include "core-impl/SoundDose.h"
#include "core-impl/StreamStub.h"
#include "core-impl/StreamUsb.h"
#include "core-impl/Telephony.h"
#include "core-impl/utils.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::hardware::audio::core::sounddose::ISoundDose;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioDevice;
using aidl::android::media::audio::common::AudioFormatDescription;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioInputFlags;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioMMapPolicy;
using aidl::android::media::audio::common::AudioMMapPolicyInfo;
using aidl::android::media::audio::common::AudioMMapPolicyType;
using aidl::android::media::audio::common::AudioMode;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioOutputFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioProfile;
using aidl::android::media::audio::common::Boolean;
using aidl::android::media::audio::common::Int;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::media::audio::common::PcmType;
using android::hardware::audio::common::getFrameSizeInBytes;
using android::hardware::audio::common::isBitPositionFlagSet;
using android::hardware::audio::common::isValidAudioMode;

namespace aidl::android::hardware::audio::core {

namespace {

bool generateDefaultPortConfig(const AudioPort& port, AudioPortConfig* config) {
    *config = {};
    config->portId = port.id;
    if (port.profiles.empty()) {
        LOG(ERROR) << __func__ << ": port " << port.id << " has no profiles";
        return false;
    }
    const auto& profile = port.profiles.begin();
    config->format = profile->format;
    if (profile->channelMasks.empty()) {
        LOG(ERROR) << __func__ << ": the first profile in port " << port.id
                   << " has no channel masks";
        return false;
    }
    config->channelMask = *profile->channelMasks.begin();
    if (profile->sampleRates.empty()) {
        LOG(ERROR) << __func__ << ": the first profile in port " << port.id
                   << " has no sample rates";
        return false;
    }
    Int sampleRate;
    sampleRate.value = *profile->sampleRates.begin();
    config->sampleRate = sampleRate;
    config->flags = port.flags;
    config->ext = port.ext;
    return true;
}

bool findAudioProfile(const AudioPort& port, const AudioFormatDescription& format,
                      AudioProfile* profile) {
    if (auto profilesIt =
                find_if(port.profiles.begin(), port.profiles.end(),
                        [&format](const auto& profile) { return profile.format == format; });
        profilesIt != port.profiles.end()) {
        *profile = *profilesIt;
        return true;
    }
    return false;
}

}  // namespace

// static
std::shared_ptr<Module> Module::createInstance(Type type) {
    switch (type) {
        case Module::Type::USB:
            return ndk::SharedRefBase::make<ModuleUsb>(type);
        case Type::DEFAULT:
        case Type::R_SUBMIX:
        default:
            return ndk::SharedRefBase::make<Module>(type);
    }
}

// static
StreamIn::CreateInstance Module::getStreamInCreator(Type type) {
    switch (type) {
        case Type::USB:
            return StreamInUsb::createInstance;
        case Type::DEFAULT:
        case Type::R_SUBMIX:
        default:
            return StreamInStub::createInstance;
    }
}

// static
StreamOut::CreateInstance Module::getStreamOutCreator(Type type) {
    switch (type) {
        case Type::USB:
            return StreamOutUsb::createInstance;
        case Type::DEFAULT:
        case Type::R_SUBMIX:
        default:
            return StreamOutStub::createInstance;
    }
}

void Module::cleanUpPatch(int32_t patchId) {
    erase_all_values(mPatches, std::set<int32_t>{patchId});
}

ndk::ScopedAStatus Module::createStreamContext(
        int32_t in_portConfigId, int64_t in_bufferSizeFrames,
        std::shared_ptr<IStreamCallback> asyncCallback,
        std::shared_ptr<IStreamOutEventCallback> outEventCallback, StreamContext* out_context) {
    if (in_bufferSizeFrames <= 0) {
        LOG(ERROR) << __func__ << ": non-positive buffer size " << in_bufferSizeFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_bufferSizeFrames < kMinimumStreamBufferSizeFrames) {
        LOG(ERROR) << __func__ << ": insufficient buffer size " << in_bufferSizeFrames
                   << ", must be at least " << kMinimumStreamBufferSizeFrames;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& configs = getConfig().portConfigs;
    auto portConfigIt = findById<AudioPortConfig>(configs, in_portConfigId);
    // Since this is a private method, it is assumed that
    // validity of the portConfigId has already been checked.
    const size_t frameSize =
            getFrameSizeInBytes(portConfigIt->format.value(), portConfigIt->channelMask.value());
    if (frameSize == 0) {
        LOG(ERROR) << __func__ << ": could not calculate frame size for port config "
                   << portConfigIt->toString();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    LOG(DEBUG) << __func__ << ": frame size " << frameSize << " bytes";
    if (frameSize > kMaximumStreamBufferSizeBytes / in_bufferSizeFrames) {
        LOG(ERROR) << __func__ << ": buffer size " << in_bufferSizeFrames
                   << " frames is too large, maximum size is "
                   << kMaximumStreamBufferSizeBytes / frameSize;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const auto& flags = portConfigIt->flags.value();
    if ((flags.getTag() == AudioIoFlags::Tag::input &&
         !isBitPositionFlagSet(flags.get<AudioIoFlags::Tag::input>(),
                               AudioInputFlags::MMAP_NOIRQ)) ||
        (flags.getTag() == AudioIoFlags::Tag::output &&
         !isBitPositionFlagSet(flags.get<AudioIoFlags::Tag::output>(),
                               AudioOutputFlags::MMAP_NOIRQ))) {
        StreamContext::DebugParameters params{mDebug.streamTransientStateDelayMs,
                                              mVendorDebug.forceTransientBurst,
                                              mVendorDebug.forceSynchronousDrain};
        StreamContext temp(
                std::make_unique<StreamContext::CommandMQ>(1, true /*configureEventFlagWord*/),
                std::make_unique<StreamContext::ReplyMQ>(1, true /*configureEventFlagWord*/),
                portConfigIt->format.value(), portConfigIt->channelMask.value(),
                portConfigIt->sampleRate.value().value,
                std::make_unique<StreamContext::DataMQ>(frameSize * in_bufferSizeFrames),
                asyncCallback, outEventCallback, params);
        if (temp.isValid()) {
            *out_context = std::move(temp);
        } else {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    } else {
        // TODO: Implement simulation of MMAP buffer allocation
    }
    return ndk::ScopedAStatus::ok();
}

std::vector<AudioDevice> Module::findConnectedDevices(int32_t portConfigId) {
    std::vector<AudioDevice> result;
    auto& ports = getConfig().ports;
    auto portIds = portIdsFromPortConfigIds(findConnectedPortConfigIds(portConfigId));
    for (auto it = portIds.begin(); it != portIds.end(); ++it) {
        auto portIt = findById<AudioPort>(ports, *it);
        if (portIt != ports.end() && portIt->ext.getTag() == AudioPortExt::Tag::device) {
            result.push_back(portIt->ext.template get<AudioPortExt::Tag::device>().device);
        }
    }
    return result;
}

std::set<int32_t> Module::findConnectedPortConfigIds(int32_t portConfigId) {
    std::set<int32_t> result;
    auto patchIdsRange = mPatches.equal_range(portConfigId);
    auto& patches = getConfig().patches;
    for (auto it = patchIdsRange.first; it != patchIdsRange.second; ++it) {
        auto patchIt = findById<AudioPatch>(patches, it->second);
        if (patchIt == patches.end()) {
            LOG(FATAL) << __func__ << ": patch with id " << it->second << " taken from mPatches "
                       << "not found in the configuration";
        }
        if (std::find(patchIt->sourcePortConfigIds.begin(), patchIt->sourcePortConfigIds.end(),
                      portConfigId) != patchIt->sourcePortConfigIds.end()) {
            result.insert(patchIt->sinkPortConfigIds.begin(), patchIt->sinkPortConfigIds.end());
        } else {
            result.insert(patchIt->sourcePortConfigIds.begin(), patchIt->sourcePortConfigIds.end());
        }
    }
    return result;
}

ndk::ScopedAStatus Module::findPortIdForNewStream(int32_t in_portConfigId, AudioPort** port) {
    auto& configs = getConfig().portConfigs;
    auto portConfigIt = findById<AudioPortConfig>(configs, in_portConfigId);
    if (portConfigIt == configs.end()) {
        LOG(ERROR) << __func__ << ": existing port config id " << in_portConfigId << " not found";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const int32_t portId = portConfigIt->portId;
    // In our implementation, configs of mix ports always have unique IDs.
    CHECK(portId != in_portConfigId);
    auto& ports = getConfig().ports;
    auto portIt = findById<AudioPort>(ports, portId);
    if (portIt == ports.end()) {
        LOG(ERROR) << __func__ << ": port id " << portId << " used by port config id "
                   << in_portConfigId << " not found";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (mStreams.count(in_portConfigId) != 0) {
        LOG(ERROR) << __func__ << ": port config id " << in_portConfigId
                   << " already has a stream opened on it";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (portIt->ext.getTag() != AudioPortExt::Tag::mix) {
        LOG(ERROR) << __func__ << ": port config id " << in_portConfigId
                   << " does not correspond to a mix port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const int32_t maxOpenStreamCount = portIt->ext.get<AudioPortExt::Tag::mix>().maxOpenStreamCount;
    if (maxOpenStreamCount != 0 && mStreams.count(portId) >= maxOpenStreamCount) {
        LOG(ERROR) << __func__ << ": port id " << portId
                   << " has already reached maximum allowed opened stream count: "
                   << maxOpenStreamCount;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    *port = &(*portIt);
    return ndk::ScopedAStatus::ok();
}

template <typename C>
std::set<int32_t> Module::portIdsFromPortConfigIds(C portConfigIds) {
    std::set<int32_t> result;
    auto& portConfigs = getConfig().portConfigs;
    for (auto it = portConfigIds.begin(); it != portConfigIds.end(); ++it) {
        auto portConfigIt = findById<AudioPortConfig>(portConfigs, *it);
        if (portConfigIt != portConfigs.end()) {
            result.insert(portConfigIt->portId);
        }
    }
    return result;
}

internal::Configuration& Module::getConfig() {
    if (!mConfig) {
        switch (mType) {
            case Type::DEFAULT:
                mConfig = std::move(internal::getPrimaryConfiguration());
                break;
            case Type::R_SUBMIX:
                mConfig = std::move(internal::getRSubmixConfiguration());
                break;
            case Type::USB:
                mConfig = std::move(internal::getUsbConfiguration());
                break;
        }
    }
    return *mConfig;
}

void Module::registerPatch(const AudioPatch& patch) {
    auto& configs = getConfig().portConfigs;
    auto do_insert = [&](const std::vector<int32_t>& portConfigIds) {
        for (auto portConfigId : portConfigIds) {
            auto configIt = findById<AudioPortConfig>(configs, portConfigId);
            if (configIt != configs.end()) {
                mPatches.insert(std::pair{portConfigId, patch.id});
                if (configIt->portId != portConfigId) {
                    mPatches.insert(std::pair{configIt->portId, patch.id});
                }
            }
        };
    };
    do_insert(patch.sourcePortConfigIds);
    do_insert(patch.sinkPortConfigIds);
}

void Module::updateStreamsConnectedState(const AudioPatch& oldPatch, const AudioPatch& newPatch) {
    // Streams from the old patch need to be disconnected, streams from the new
    // patch need to be connected. If the stream belongs to both patches, no need
    // to update it.
    std::set<int32_t> idsToDisconnect, idsToConnect;
    idsToDisconnect.insert(oldPatch.sourcePortConfigIds.begin(),
                           oldPatch.sourcePortConfigIds.end());
    idsToDisconnect.insert(oldPatch.sinkPortConfigIds.begin(), oldPatch.sinkPortConfigIds.end());
    idsToConnect.insert(newPatch.sourcePortConfigIds.begin(), newPatch.sourcePortConfigIds.end());
    idsToConnect.insert(newPatch.sinkPortConfigIds.begin(), newPatch.sinkPortConfigIds.end());
    std::for_each(idsToDisconnect.begin(), idsToDisconnect.end(), [&](const auto& portConfigId) {
        if (idsToConnect.count(portConfigId) == 0) {
            LOG(DEBUG) << "The stream on port config id " << portConfigId << " is not connected";
            mStreams.setStreamIsConnected(portConfigId, {});
        }
    });
    std::for_each(idsToConnect.begin(), idsToConnect.end(), [&](const auto& portConfigId) {
        if (idsToDisconnect.count(portConfigId) == 0) {
            const auto connectedDevices = findConnectedDevices(portConfigId);
            LOG(DEBUG) << "The stream on port config id " << portConfigId
                       << " is connected to: " << ::android::internal::ToString(connectedDevices);
            mStreams.setStreamIsConnected(portConfigId, connectedDevices);
        }
    });
}

ndk::ScopedAStatus Module::setModuleDebug(
        const ::aidl::android::hardware::audio::core::ModuleDebug& in_debug) {
    LOG(DEBUG) << __func__ << ": old flags:" << mDebug.toString()
               << ", new flags: " << in_debug.toString();
    if (mDebug.simulateDeviceConnections != in_debug.simulateDeviceConnections &&
        !mConnectedDevicePorts.empty()) {
        LOG(ERROR) << __func__ << ": attempting to change device connections simulation "
                   << "while having external devices connected";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (in_debug.streamTransientStateDelayMs < 0) {
        LOG(ERROR) << __func__ << ": streamTransientStateDelayMs is negative: "
                   << in_debug.streamTransientStateDelayMs;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    mDebug = in_debug;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getTelephony(std::shared_ptr<ITelephony>* _aidl_return) {
    if (!mTelephony) {
        mTelephony = ndk::SharedRefBase::make<Telephony>();
    }
    *_aidl_return = mTelephony.getPtr();
    LOG(DEBUG) << __func__ << ": returning instance of ITelephony: " << _aidl_return->get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getBluetooth(std::shared_ptr<IBluetooth>* _aidl_return) {
    if (!mBluetooth) {
        mBluetooth = ndk::SharedRefBase::make<Bluetooth>();
    }
    *_aidl_return = mBluetooth.getPtr();
    LOG(DEBUG) << __func__ << ": returning instance of IBluetooth: " << _aidl_return->get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* _aidl_return) {
    if (!mBluetoothA2dp) {
        mBluetoothA2dp = ndk::SharedRefBase::make<BluetoothA2dp>();
    }
    *_aidl_return = mBluetoothA2dp.getPtr();
    LOG(DEBUG) << __func__ << ": returning instance of IBluetoothA2dp: " << _aidl_return->get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getBluetoothLe(std::shared_ptr<IBluetoothLe>* _aidl_return) {
    if (!mBluetoothLe) {
        mBluetoothLe = ndk::SharedRefBase::make<BluetoothLe>();
    }
    *_aidl_return = mBluetoothLe.getPtr();
    LOG(DEBUG) << __func__ << ": returning instance of IBluetoothLe: " << _aidl_return->get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::connectExternalDevice(const AudioPort& in_templateIdAndAdditionalData,
                                                 AudioPort* _aidl_return) {
    const int32_t templateId = in_templateIdAndAdditionalData.id;
    auto& ports = getConfig().ports;
    AudioPort connectedPort;
    {  // Scope the template port so that we don't accidentally modify it.
        auto templateIt = findById<AudioPort>(ports, templateId);
        if (templateIt == ports.end()) {
            LOG(ERROR) << __func__ << ": port id " << templateId << " not found";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (templateIt->ext.getTag() != AudioPortExt::Tag::device) {
            LOG(ERROR) << __func__ << ": port id " << templateId << " is not a device port";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (!templateIt->profiles.empty()) {
            LOG(ERROR) << __func__ << ": port id " << templateId
                       << " does not have dynamic profiles";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        auto& templateDevicePort = templateIt->ext.get<AudioPortExt::Tag::device>();
        if (templateDevicePort.device.type.connection.empty()) {
            LOG(ERROR) << __func__ << ": port id " << templateId << " is permanently attached";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        // Postpone id allocation until we ensure that there are no client errors.
        connectedPort = *templateIt;
        connectedPort.extraAudioDescriptors = in_templateIdAndAdditionalData.extraAudioDescriptors;
        const auto& inputDevicePort =
                in_templateIdAndAdditionalData.ext.get<AudioPortExt::Tag::device>();
        auto& connectedDevicePort = connectedPort.ext.get<AudioPortExt::Tag::device>();
        connectedDevicePort.device.address = inputDevicePort.device.address;
        LOG(DEBUG) << __func__ << ": device port " << connectedPort.id << " device set to "
                   << connectedDevicePort.device.toString();
        // Check if there is already a connected port with for the same external device.
        for (auto connectedPortId : mConnectedDevicePorts) {
            auto connectedPortIt = findById<AudioPort>(ports, connectedPortId);
            if (connectedPortIt->ext.get<AudioPortExt::Tag::device>().device ==
                connectedDevicePort.device) {
                LOG(ERROR) << __func__ << ": device " << connectedDevicePort.device.toString()
                           << " is already connected at the device port id " << connectedPortId;
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
            }
        }
    }

    if (!mDebug.simulateDeviceConnections) {
        // In a real HAL here we would attempt querying the profiles from the device.
        LOG(ERROR) << __func__ << ": failed to query supported device profiles";
        // TODO: Check the return value when it is ready for actual devices.
        populateConnectedDevicePort(&connectedPort);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }

    connectedPort.id = ++getConfig().nextPortId;
    mConnectedDevicePorts.insert(connectedPort.id);
    LOG(DEBUG) << __func__ << ": template port " << templateId << " external device connected, "
               << "connected port ID " << connectedPort.id;
    auto& connectedProfiles = getConfig().connectedProfiles;
    if (auto connectedProfilesIt = connectedProfiles.find(templateId);
        connectedProfilesIt != connectedProfiles.end()) {
        connectedPort.profiles = connectedProfilesIt->second;
    }
    ports.push_back(connectedPort);
    onExternalDeviceConnectionChanged(connectedPort, true /*connected*/);
    *_aidl_return = std::move(connectedPort);

    std::vector<AudioRoute> newRoutes;
    auto& routes = getConfig().routes;
    for (auto& r : routes) {
        if (r.sinkPortId == templateId) {
            AudioRoute newRoute;
            newRoute.sourcePortIds = r.sourcePortIds;
            newRoute.sinkPortId = connectedPort.id;
            newRoute.isExclusive = r.isExclusive;
            newRoutes.push_back(std::move(newRoute));
        } else {
            auto& srcs = r.sourcePortIds;
            if (std::find(srcs.begin(), srcs.end(), templateId) != srcs.end()) {
                srcs.push_back(connectedPort.id);
            }
        }
    }
    routes.insert(routes.end(), newRoutes.begin(), newRoutes.end());

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::disconnectExternalDevice(int32_t in_portId) {
    auto& ports = getConfig().ports;
    auto portIt = findById<AudioPort>(ports, in_portId);
    if (portIt == ports.end()) {
        LOG(ERROR) << __func__ << ": port id " << in_portId << " not found";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (portIt->ext.getTag() != AudioPortExt::Tag::device) {
        LOG(ERROR) << __func__ << ": port id " << in_portId << " is not a device port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (mConnectedDevicePorts.count(in_portId) == 0) {
        LOG(ERROR) << __func__ << ": port id " << in_portId << " is not a connected device port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& configs = getConfig().portConfigs;
    auto& initials = getConfig().initialConfigs;
    auto configIt = std::find_if(configs.begin(), configs.end(), [&](const auto& config) {
        if (config.portId == in_portId) {
            // Check if the configuration was provided by the client.
            const auto& initialIt = findById<AudioPortConfig>(initials, config.id);
            return initialIt == initials.end() || config != *initialIt;
        }
        return false;
    });
    if (configIt != configs.end()) {
        LOG(ERROR) << __func__ << ": port id " << in_portId << " has a non-default config with id "
                   << configIt->id;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    onExternalDeviceConnectionChanged(*portIt, false /*connected*/);
    ports.erase(portIt);
    mConnectedDevicePorts.erase(in_portId);
    LOG(DEBUG) << __func__ << ": connected device port " << in_portId << " released";

    auto& routes = getConfig().routes;
    for (auto routesIt = routes.begin(); routesIt != routes.end();) {
        if (routesIt->sinkPortId == in_portId) {
            routesIt = routes.erase(routesIt);
        } else {
            // Note: the list of sourcePortIds can't become empty because there must
            // be the id of the template port in the route.
            erase_if(routesIt->sourcePortIds, [in_portId](auto src) { return src == in_portId; });
            ++routesIt;
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAudioPatches(std::vector<AudioPatch>* _aidl_return) {
    *_aidl_return = getConfig().patches;
    LOG(DEBUG) << __func__ << ": returning " << _aidl_return->size() << " patches";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAudioPort(int32_t in_portId, AudioPort* _aidl_return) {
    auto& ports = getConfig().ports;
    auto portIt = findById<AudioPort>(ports, in_portId);
    if (portIt != ports.end()) {
        *_aidl_return = *portIt;
        LOG(DEBUG) << __func__ << ": returning port by id " << in_portId;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": port id " << in_portId << " not found";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Module::getAudioPortConfigs(std::vector<AudioPortConfig>* _aidl_return) {
    *_aidl_return = getConfig().portConfigs;
    LOG(DEBUG) << __func__ << ": returning " << _aidl_return->size() << " port configs";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAudioPorts(std::vector<AudioPort>* _aidl_return) {
    *_aidl_return = getConfig().ports;
    LOG(DEBUG) << __func__ << ": returning " << _aidl_return->size() << " ports";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAudioRoutes(std::vector<AudioRoute>* _aidl_return) {
    *_aidl_return = getConfig().routes;
    LOG(DEBUG) << __func__ << ": returning " << _aidl_return->size() << " routes";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAudioRoutesForAudioPort(int32_t in_portId,
                                                      std::vector<AudioRoute>* _aidl_return) {
    auto& ports = getConfig().ports;
    if (auto portIt = findById<AudioPort>(ports, in_portId); portIt == ports.end()) {
        LOG(ERROR) << __func__ << ": port id " << in_portId << " not found";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& routes = getConfig().routes;
    std::copy_if(routes.begin(), routes.end(), std::back_inserter(*_aidl_return),
                 [&](const auto& r) {
                     const auto& srcs = r.sourcePortIds;
                     return r.sinkPortId == in_portId ||
                            std::find(srcs.begin(), srcs.end(), in_portId) != srcs.end();
                 });
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::openInputStream(const OpenInputStreamArguments& in_args,
                                           OpenInputStreamReturn* _aidl_return) {
    LOG(DEBUG) << __func__ << ": port config id " << in_args.portConfigId << ", buffer size "
               << in_args.bufferSizeFrames << " frames";
    AudioPort* port = nullptr;
    if (auto status = findPortIdForNewStream(in_args.portConfigId, &port); !status.isOk()) {
        return status;
    }
    if (port->flags.getTag() != AudioIoFlags::Tag::input) {
        LOG(ERROR) << __func__ << ": port config id " << in_args.portConfigId
                   << " does not correspond to an input mix port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    StreamContext context;
    if (auto status = createStreamContext(in_args.portConfigId, in_args.bufferSizeFrames, nullptr,
                                          nullptr, &context);
        !status.isOk()) {
        return status;
    }
    context.fillDescriptor(&_aidl_return->desc);
    std::shared_ptr<StreamIn> stream;
    ndk::ScopedAStatus status = getStreamInCreator(mType)(in_args.sinkMetadata, std::move(context),
                                                          mConfig->microphones, &stream);
    if (!status.isOk()) {
        return status;
    }
    StreamWrapper streamWrapper(stream);
    AIBinder_setMinSchedulerPolicy(streamWrapper.getBinder().get(), SCHED_NORMAL,
                                   ANDROID_PRIORITY_AUDIO);
    auto patchIt = mPatches.find(in_args.portConfigId);
    if (patchIt != mPatches.end()) {
        streamWrapper.setStreamIsConnected(findConnectedDevices(in_args.portConfigId));
    }
    mStreams.insert(port->id, in_args.portConfigId, std::move(streamWrapper));
    _aidl_return->stream = std::move(stream);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::openOutputStream(const OpenOutputStreamArguments& in_args,
                                            OpenOutputStreamReturn* _aidl_return) {
    LOG(DEBUG) << __func__ << ": port config id " << in_args.portConfigId << ", has offload info? "
               << (in_args.offloadInfo.has_value()) << ", buffer size " << in_args.bufferSizeFrames
               << " frames";
    AudioPort* port = nullptr;
    if (auto status = findPortIdForNewStream(in_args.portConfigId, &port); !status.isOk()) {
        return status;
    }
    if (port->flags.getTag() != AudioIoFlags::Tag::output) {
        LOG(ERROR) << __func__ << ": port config id " << in_args.portConfigId
                   << " does not correspond to an output mix port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const bool isOffload = isBitPositionFlagSet(port->flags.get<AudioIoFlags::Tag::output>(),
                                                AudioOutputFlags::COMPRESS_OFFLOAD);
    if (isOffload && !in_args.offloadInfo.has_value()) {
        LOG(ERROR) << __func__ << ": port id " << port->id
                   << " has COMPRESS_OFFLOAD flag set, requires offload info";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const bool isNonBlocking = isBitPositionFlagSet(port->flags.get<AudioIoFlags::Tag::output>(),
                                                    AudioOutputFlags::NON_BLOCKING);
    if (isNonBlocking && in_args.callback == nullptr) {
        LOG(ERROR) << __func__ << ": port id " << port->id
                   << " has NON_BLOCKING flag set, requires async callback";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    StreamContext context;
    if (auto status = createStreamContext(in_args.portConfigId, in_args.bufferSizeFrames,
                                          isNonBlocking ? in_args.callback : nullptr,
                                          in_args.eventCallback, &context);
        !status.isOk()) {
        return status;
    }
    context.fillDescriptor(&_aidl_return->desc);
    std::shared_ptr<StreamOut> stream;
    ndk::ScopedAStatus status = getStreamOutCreator(mType)(
            in_args.sourceMetadata, std::move(context), in_args.offloadInfo, &stream);
    if (!status.isOk()) {
        return status;
    }
    StreamWrapper streamWrapper(stream);
    AIBinder_setMinSchedulerPolicy(streamWrapper.getBinder().get(), SCHED_NORMAL,
                                   ANDROID_PRIORITY_AUDIO);
    auto patchIt = mPatches.find(in_args.portConfigId);
    if (patchIt != mPatches.end()) {
        streamWrapper.setStreamIsConnected(findConnectedDevices(in_args.portConfigId));
    }
    mStreams.insert(port->id, in_args.portConfigId, std::move(streamWrapper));
    _aidl_return->stream = std::move(stream);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getSupportedPlaybackRateFactors(
        SupportedPlaybackRateFactors* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Module::setAudioPatch(const AudioPatch& in_requested, AudioPatch* _aidl_return) {
    LOG(DEBUG) << __func__ << ": requested patch " << in_requested.toString();
    if (in_requested.sourcePortConfigIds.empty()) {
        LOG(ERROR) << __func__ << ": requested patch has empty sources list";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!all_unique<int32_t>(in_requested.sourcePortConfigIds)) {
        LOG(ERROR) << __func__ << ": requested patch has duplicate ids in the sources list";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (in_requested.sinkPortConfigIds.empty()) {
        LOG(ERROR) << __func__ << ": requested patch has empty sinks list";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!all_unique<int32_t>(in_requested.sinkPortConfigIds)) {
        LOG(ERROR) << __func__ << ": requested patch has duplicate ids in the sinks list";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    auto& configs = getConfig().portConfigs;
    std::vector<int32_t> missingIds;
    auto sources =
            selectByIds<AudioPortConfig>(configs, in_requested.sourcePortConfigIds, &missingIds);
    if (!missingIds.empty()) {
        LOG(ERROR) << __func__ << ": following source port config ids not found: "
                   << ::android::internal::ToString(missingIds);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto sinks = selectByIds<AudioPortConfig>(configs, in_requested.sinkPortConfigIds, &missingIds);
    if (!missingIds.empty()) {
        LOG(ERROR) << __func__ << ": following sink port config ids not found: "
                   << ::android::internal::ToString(missingIds);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    // bool indicates whether a non-exclusive route is available.
    // If only an exclusive route is available, that means the patch can not be
    // established if there is any other patch which currently uses the sink port.
    std::map<int32_t, bool> allowedSinkPorts;
    auto& routes = getConfig().routes;
    for (auto src : sources) {
        for (const auto& r : routes) {
            const auto& srcs = r.sourcePortIds;
            if (std::find(srcs.begin(), srcs.end(), src->portId) != srcs.end()) {
                if (!allowedSinkPorts[r.sinkPortId]) {  // prefer non-exclusive
                    allowedSinkPorts[r.sinkPortId] = !r.isExclusive;
                }
            }
        }
    }
    for (auto sink : sinks) {
        if (allowedSinkPorts.count(sink->portId) == 0) {
            LOG(ERROR) << __func__ << ": there is no route to the sink port id " << sink->portId;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }

    if (auto status = checkAudioPatchEndpointsMatch(sources, sinks); !status.isOk()) {
        return status;
    }

    auto& patches = getConfig().patches;
    auto existing = patches.end();
    std::optional<decltype(mPatches)> patchesBackup;
    if (in_requested.id != 0) {
        existing = findById<AudioPatch>(patches, in_requested.id);
        if (existing != patches.end()) {
            patchesBackup = mPatches;
            cleanUpPatch(existing->id);
        } else {
            LOG(ERROR) << __func__ << ": not found existing patch id " << in_requested.id;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
    // Validate the requested patch.
    for (const auto& [sinkPortId, nonExclusive] : allowedSinkPorts) {
        if (!nonExclusive && mPatches.count(sinkPortId) != 0) {
            LOG(ERROR) << __func__ << ": sink port id " << sinkPortId
                       << "is exclusive and is already used by some other patch";
            if (patchesBackup.has_value()) {
                mPatches = std::move(*patchesBackup);
            }
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
    }
    *_aidl_return = in_requested;
    _aidl_return->minimumStreamBufferSizeFrames = kMinimumStreamBufferSizeFrames;
    _aidl_return->latenciesMs.clear();
    _aidl_return->latenciesMs.insert(_aidl_return->latenciesMs.end(),
                                     _aidl_return->sinkPortConfigIds.size(), kLatencyMs);
    AudioPatch oldPatch{};
    if (existing == patches.end()) {
        _aidl_return->id = getConfig().nextPatchId++;
        patches.push_back(*_aidl_return);
        existing = patches.begin() + (patches.size() - 1);
    } else {
        oldPatch = *existing;
        *existing = *_aidl_return;
    }
    registerPatch(*existing);
    updateStreamsConnectedState(oldPatch, *_aidl_return);

    LOG(DEBUG) << __func__ << ": " << (oldPatch.id == 0 ? "created" : "updated") << " patch "
               << _aidl_return->toString();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::setAudioPortConfig(const AudioPortConfig& in_requested,
                                              AudioPortConfig* out_suggested, bool* _aidl_return) {
    LOG(DEBUG) << __func__ << ": requested " << in_requested.toString();
    auto& configs = getConfig().portConfigs;
    auto existing = configs.end();
    if (in_requested.id != 0) {
        if (existing = findById<AudioPortConfig>(configs, in_requested.id);
            existing == configs.end()) {
            LOG(ERROR) << __func__ << ": existing port config id " << in_requested.id
                       << " not found";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }

    const int portId = existing != configs.end() ? existing->portId : in_requested.portId;
    if (portId == 0) {
        LOG(ERROR) << __func__ << ": input port config does not specify portId";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& ports = getConfig().ports;
    auto portIt = findById<AudioPort>(ports, portId);
    if (portIt == ports.end()) {
        LOG(ERROR) << __func__ << ": input port config points to non-existent portId " << portId;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (existing != configs.end()) {
        *out_suggested = *existing;
    } else {
        AudioPortConfig newConfig;
        if (generateDefaultPortConfig(*portIt, &newConfig)) {
            *out_suggested = newConfig;
        } else {
            LOG(ERROR) << __func__ << ": unable generate a default config for port " << portId;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
    }
    // From this moment, 'out_suggested' is either an existing port config,
    // or a new generated config. Now attempt to update it according to the specified
    // fields of 'in_requested'.

    bool requestedIsValid = true, requestedIsFullySpecified = true;

    AudioIoFlags portFlags = portIt->flags;
    if (in_requested.flags.has_value()) {
        if (in_requested.flags.value() != portFlags) {
            LOG(WARNING) << __func__ << ": requested flags "
                         << in_requested.flags.value().toString() << " do not match port's "
                         << portId << " flags " << portFlags.toString();
            requestedIsValid = false;
        }
    } else {
        requestedIsFullySpecified = false;
    }

    AudioProfile portProfile;
    if (in_requested.format.has_value()) {
        const auto& format = in_requested.format.value();
        if (findAudioProfile(*portIt, format, &portProfile)) {
            out_suggested->format = format;
        } else {
            LOG(WARNING) << __func__ << ": requested format " << format.toString()
                         << " is not found in port's " << portId << " profiles";
            requestedIsValid = false;
        }
    } else {
        requestedIsFullySpecified = false;
    }
    if (!findAudioProfile(*portIt, out_suggested->format.value(), &portProfile)) {
        LOG(ERROR) << __func__ << ": port " << portId << " does not support format "
                   << out_suggested->format.value().toString() << " anymore";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    if (in_requested.channelMask.has_value()) {
        const auto& channelMask = in_requested.channelMask.value();
        if (find(portProfile.channelMasks.begin(), portProfile.channelMasks.end(), channelMask) !=
            portProfile.channelMasks.end()) {
            out_suggested->channelMask = channelMask;
        } else {
            LOG(WARNING) << __func__ << ": requested channel mask " << channelMask.toString()
                         << " is not supported for the format " << portProfile.format.toString()
                         << " by the port " << portId;
            requestedIsValid = false;
        }
    } else {
        requestedIsFullySpecified = false;
    }

    if (in_requested.sampleRate.has_value()) {
        const auto& sampleRate = in_requested.sampleRate.value();
        if (find(portProfile.sampleRates.begin(), portProfile.sampleRates.end(),
                 sampleRate.value) != portProfile.sampleRates.end()) {
            out_suggested->sampleRate = sampleRate;
        } else {
            LOG(WARNING) << __func__ << ": requested sample rate " << sampleRate.value
                         << " is not supported for the format " << portProfile.format.toString()
                         << " by the port " << portId;
            requestedIsValid = false;
        }
    } else {
        requestedIsFullySpecified = false;
    }

    if (in_requested.gain.has_value()) {
        // Let's pretend that gain can always be applied.
        out_suggested->gain = in_requested.gain.value();
    }

    if (in_requested.ext.getTag() != AudioPortExt::Tag::unspecified) {
        if (in_requested.ext.getTag() == out_suggested->ext.getTag()) {
            if (out_suggested->ext.getTag() == AudioPortExt::Tag::mix) {
                // 'AudioMixPortExt.handle' is set by the client, copy from in_requested
                out_suggested->ext.get<AudioPortExt::Tag::mix>().handle =
                        in_requested.ext.get<AudioPortExt::Tag::mix>().handle;
            }
        } else {
            LOG(WARNING) << __func__ << ": requested ext tag "
                         << toString(in_requested.ext.getTag()) << " do not match port's tag "
                         << toString(out_suggested->ext.getTag());
            requestedIsValid = false;
        }
    }

    if (existing == configs.end() && requestedIsValid && requestedIsFullySpecified) {
        out_suggested->id = getConfig().nextPortId++;
        configs.push_back(*out_suggested);
        *_aidl_return = true;
        LOG(DEBUG) << __func__ << ": created new port config " << out_suggested->toString();
    } else if (existing != configs.end() && requestedIsValid) {
        *existing = *out_suggested;
        *_aidl_return = true;
        LOG(DEBUG) << __func__ << ": updated port config " << out_suggested->toString();
    } else {
        LOG(DEBUG) << __func__ << ": not applied; existing config ? " << (existing != configs.end())
                   << "; requested is valid? " << requestedIsValid << ", fully specified? "
                   << requestedIsFullySpecified;
        *_aidl_return = false;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::resetAudioPatch(int32_t in_patchId) {
    auto& patches = getConfig().patches;
    auto patchIt = findById<AudioPatch>(patches, in_patchId);
    if (patchIt != patches.end()) {
        cleanUpPatch(patchIt->id);
        updateStreamsConnectedState(*patchIt, AudioPatch{});
        patches.erase(patchIt);
        LOG(DEBUG) << __func__ << ": erased patch " << in_patchId;
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": patch id " << in_patchId << " not found";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Module::resetAudioPortConfig(int32_t in_portConfigId) {
    auto& configs = getConfig().portConfigs;
    auto configIt = findById<AudioPortConfig>(configs, in_portConfigId);
    if (configIt != configs.end()) {
        if (mStreams.count(in_portConfigId) != 0) {
            LOG(ERROR) << __func__ << ": port config id " << in_portConfigId
                       << " has a stream opened on it";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        auto patchIt = mPatches.find(in_portConfigId);
        if (patchIt != mPatches.end()) {
            LOG(ERROR) << __func__ << ": port config id " << in_portConfigId
                       << " is used by the patch with id " << patchIt->second;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        auto& initials = getConfig().initialConfigs;
        auto initialIt = findById<AudioPortConfig>(initials, in_portConfigId);
        if (initialIt == initials.end()) {
            configs.erase(configIt);
            LOG(DEBUG) << __func__ << ": erased port config " << in_portConfigId;
        } else if (*configIt != *initialIt) {
            *configIt = *initialIt;
            LOG(DEBUG) << __func__ << ": reset port config " << in_portConfigId;
        }
        return ndk::ScopedAStatus::ok();
    }
    LOG(ERROR) << __func__ << ": port config id " << in_portConfigId << " not found";
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Module::getMasterMute(bool* _aidl_return) {
    *_aidl_return = mMasterMute;
    LOG(DEBUG) << __func__ << ": returning " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::setMasterMute(bool in_mute) {
    LOG(DEBUG) << __func__ << ": " << in_mute;
    auto result = mDebug.simulateDeviceConnections ? ndk::ScopedAStatus::ok()
                                                   : onMasterMuteChanged(in_mute);
    if (result.isOk()) {
        mMasterMute = in_mute;
    } else {
        LOG(ERROR) << __func__ << ": failed calling onMasterMuteChanged(" << in_mute
                   << "), error=" << result;
        // Reset master mute if it failed.
        onMasterMuteChanged(mMasterMute);
    }
    return std::move(result);
}

ndk::ScopedAStatus Module::getMasterVolume(float* _aidl_return) {
    *_aidl_return = mMasterVolume;
    LOG(DEBUG) << __func__ << ": returning " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::setMasterVolume(float in_volume) {
    LOG(DEBUG) << __func__ << ": " << in_volume;
    if (in_volume >= 0.0f && in_volume <= 1.0f) {
        auto result = mDebug.simulateDeviceConnections ? ndk::ScopedAStatus::ok()
                                                       : onMasterVolumeChanged(in_volume);
        if (result.isOk()) {
            mMasterVolume = in_volume;
        } else {
            // Reset master volume if it failed.
            LOG(ERROR) << __func__ << ": failed calling onMasterVolumeChanged(" << in_volume
                       << "), error=" << result;
            onMasterVolumeChanged(mMasterVolume);
        }
        return std::move(result);
    }
    LOG(ERROR) << __func__ << ": invalid master volume value: " << in_volume;
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Module::getMicMute(bool* _aidl_return) {
    *_aidl_return = mMicMute;
    LOG(DEBUG) << __func__ << ": returning " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::setMicMute(bool in_mute) {
    LOG(DEBUG) << __func__ << ": " << in_mute;
    mMicMute = in_mute;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getMicrophones(std::vector<MicrophoneInfo>* _aidl_return) {
    *_aidl_return = getConfig().microphones;
    LOG(DEBUG) << __func__ << ": returning " << ::android::internal::ToString(*_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::updateAudioMode(AudioMode in_mode) {
    if (!isValidAudioMode(in_mode)) {
        LOG(ERROR) << __func__ << ": invalid mode " << toString(in_mode);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    // No checks for supported audio modes here, it's an informative notification.
    LOG(DEBUG) << __func__ << ": " << toString(in_mode);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::updateScreenRotation(ScreenRotation in_rotation) {
    LOG(DEBUG) << __func__ << ": " << toString(in_rotation);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::updateScreenState(bool in_isTurnedOn) {
    LOG(DEBUG) << __func__ << ": " << in_isTurnedOn;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getSoundDose(std::shared_ptr<ISoundDose>* _aidl_return) {
    if (!mSoundDose) {
        mSoundDose = ndk::SharedRefBase::make<sounddose::SoundDose>();
    }
    *_aidl_return = mSoundDose.getPtr();
    LOG(DEBUG) << __func__ << ": returning instance of ISoundDose: " << _aidl_return->get();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::generateHwAvSyncId(int32_t* _aidl_return) {
    LOG(DEBUG) << __func__;
    (void)_aidl_return;
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

const std::string Module::VendorDebug::kForceTransientBurstName = "aosp.forceTransientBurst";
const std::string Module::VendorDebug::kForceSynchronousDrainName = "aosp.forceSynchronousDrain";

ndk::ScopedAStatus Module::getVendorParameters(const std::vector<std::string>& in_ids,
                                               std::vector<VendorParameter>* _aidl_return) {
    LOG(DEBUG) << __func__ << ": id count: " << in_ids.size();
    bool allParametersKnown = true;
    for (const auto& id : in_ids) {
        if (id == VendorDebug::kForceTransientBurstName) {
            VendorParameter forceTransientBurst{.id = id};
            forceTransientBurst.ext.setParcelable(Boolean{mVendorDebug.forceTransientBurst});
            _aidl_return->push_back(std::move(forceTransientBurst));
        } else if (id == VendorDebug::kForceSynchronousDrainName) {
            VendorParameter forceSynchronousDrain{.id = id};
            forceSynchronousDrain.ext.setParcelable(Boolean{mVendorDebug.forceSynchronousDrain});
            _aidl_return->push_back(std::move(forceSynchronousDrain));
        } else {
            allParametersKnown = false;
            LOG(ERROR) << __func__ << ": unrecognized parameter \"" << id << "\"";
        }
    }
    if (allParametersKnown) return ndk::ScopedAStatus::ok();
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

namespace {

template <typename W>
bool extractParameter(const VendorParameter& p, decltype(W::value)* v) {
    std::optional<W> value;
    binder_status_t result = p.ext.getParcelable(&value);
    if (result == STATUS_OK && value.has_value()) {
        *v = value.value().value;
        return true;
    }
    LOG(ERROR) << __func__ << ": failed to read the value of the parameter \"" << p.id
               << "\": " << result;
    return false;
}

}  // namespace

ndk::ScopedAStatus Module::setVendorParameters(const std::vector<VendorParameter>& in_parameters,
                                               bool in_async) {
    LOG(DEBUG) << __func__ << ": parameter count " << in_parameters.size()
               << ", async: " << in_async;
    bool allParametersKnown = true;
    for (const auto& p : in_parameters) {
        if (p.id == VendorDebug::kForceTransientBurstName) {
            if (!extractParameter<Boolean>(p, &mVendorDebug.forceTransientBurst)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
        } else if (p.id == VendorDebug::kForceSynchronousDrainName) {
            if (!extractParameter<Boolean>(p, &mVendorDebug.forceSynchronousDrain)) {
                return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
            }
        } else {
            allParametersKnown = false;
            LOG(ERROR) << __func__ << ": unrecognized parameter \"" << p.id << "\"";
        }
    }
    if (allParametersKnown) return ndk::ScopedAStatus::ok();
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Module::addDeviceEffect(
        int32_t in_portConfigId,
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(DEBUG) << __func__ << ": port id " << in_portConfigId << ", null effect";
    } else {
        LOG(DEBUG) << __func__ << ": port id " << in_portConfigId << ", effect Binder "
                   << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Module::removeDeviceEffect(
        int32_t in_portConfigId,
        const std::shared_ptr<::aidl::android::hardware::audio::effect::IEffect>& in_effect) {
    if (in_effect == nullptr) {
        LOG(DEBUG) << __func__ << ": port id " << in_portConfigId << ", null effect";
    } else {
        LOG(DEBUG) << __func__ << ": port id " << in_portConfigId << ", effect Binder "
                   << in_effect->asBinder().get();
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Module::getMmapPolicyInfos(AudioMMapPolicyType mmapPolicyType,
                                              std::vector<AudioMMapPolicyInfo>* _aidl_return) {
    LOG(DEBUG) << __func__ << ": mmap policy type " << toString(mmapPolicyType);
    std::set<int32_t> mmapSinks;
    std::set<int32_t> mmapSources;
    auto& ports = getConfig().ports;
    for (const auto& port : ports) {
        if (port.flags.getTag() == AudioIoFlags::Tag::input &&
            isBitPositionFlagSet(port.flags.get<AudioIoFlags::Tag::input>(),
                                 AudioInputFlags::MMAP_NOIRQ)) {
            mmapSinks.insert(port.id);
        } else if (port.flags.getTag() == AudioIoFlags::Tag::output &&
                   isBitPositionFlagSet(port.flags.get<AudioIoFlags::Tag::output>(),
                                        AudioOutputFlags::MMAP_NOIRQ)) {
            mmapSources.insert(port.id);
        }
    }
    for (const auto& route : getConfig().routes) {
        if (mmapSinks.count(route.sinkPortId) != 0) {
            // The sink is a mix port, add the sources if they are device ports.
            for (int sourcePortId : route.sourcePortIds) {
                auto sourcePortIt = findById<AudioPort>(ports, sourcePortId);
                if (sourcePortIt == ports.end()) {
                    // This must not happen
                    LOG(ERROR) << __func__ << ": port id " << sourcePortId << " cannot be found";
                    continue;
                }
                if (sourcePortIt->ext.getTag() != AudioPortExt::Tag::device) {
                    // The source is not a device port, skip
                    continue;
                }
                AudioMMapPolicyInfo policyInfo;
                policyInfo.device = sourcePortIt->ext.get<AudioPortExt::Tag::device>().device;
                // Always return AudioMMapPolicy.AUTO if the device supports mmap for
                // default implementation.
                policyInfo.mmapPolicy = AudioMMapPolicy::AUTO;
                _aidl_return->push_back(policyInfo);
            }
        } else {
            auto sinkPortIt = findById<AudioPort>(ports, route.sinkPortId);
            if (sinkPortIt == ports.end()) {
                // This must not happen
                LOG(ERROR) << __func__ << ": port id " << route.sinkPortId << " cannot be found";
                continue;
            }
            if (sinkPortIt->ext.getTag() != AudioPortExt::Tag::device) {
                // The sink is not a device port, skip
                continue;
            }
            if (count_any(mmapSources, route.sourcePortIds)) {
                AudioMMapPolicyInfo policyInfo;
                policyInfo.device = sinkPortIt->ext.get<AudioPortExt::Tag::device>().device;
                // Always return AudioMMapPolicy.AUTO if the device supports mmap for
                // default implementation.
                policyInfo.mmapPolicy = AudioMMapPolicy::AUTO;
                _aidl_return->push_back(policyInfo);
            }
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::supportsVariableLatency(bool* _aidl_return) {
    LOG(DEBUG) << __func__;
    *_aidl_return = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAAudioMixerBurstCount(int32_t* _aidl_return) {
    if (!isMmapSupported()) {
        LOG(DEBUG) << __func__ << ": mmap is not supported ";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *_aidl_return = DEFAULT_AAUDIO_MIXER_BURST_COUNT;
    LOG(DEBUG) << __func__ << ": returning " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::getAAudioHardwareBurstMinUsec(int32_t* _aidl_return) {
    if (!isMmapSupported()) {
        LOG(DEBUG) << __func__ << ": mmap is not supported ";
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *_aidl_return = DEFAULT_AAUDIO_HARDWARE_BURST_MIN_DURATION_US;
    LOG(DEBUG) << __func__ << ": returning " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

bool Module::isMmapSupported() {
    if (mIsMmapSupported.has_value()) {
        return mIsMmapSupported.value();
    }
    std::vector<AudioMMapPolicyInfo> mmapPolicyInfos;
    if (!getMmapPolicyInfos(AudioMMapPolicyType::DEFAULT, &mmapPolicyInfos).isOk()) {
        mIsMmapSupported = false;
    } else {
        mIsMmapSupported =
                std::find_if(mmapPolicyInfos.begin(), mmapPolicyInfos.end(), [](const auto& info) {
                    return info.mmapPolicy == AudioMMapPolicy::AUTO ||
                           info.mmapPolicy == AudioMMapPolicy::ALWAYS;
                }) != mmapPolicyInfos.end();
    }
    return mIsMmapSupported.value();
}

ndk::ScopedAStatus Module::populateConnectedDevicePort(AudioPort* audioPort __unused) {
    LOG(VERBOSE) << __func__ << ": do nothing and return ok";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::checkAudioPatchEndpointsMatch(
        const std::vector<AudioPortConfig*>& sources __unused,
        const std::vector<AudioPortConfig*>& sinks __unused) {
    LOG(VERBOSE) << __func__ << ": do nothing and return ok";
    return ndk::ScopedAStatus::ok();
}

void Module::onExternalDeviceConnectionChanged(
        const ::aidl::android::media::audio::common::AudioPort& audioPort __unused,
        bool connected __unused) {
    LOG(DEBUG) << __func__ << ": do nothing and return";
}

ndk::ScopedAStatus Module::onMasterMuteChanged(bool mute __unused) {
    LOG(VERBOSE) << __func__ << ": do nothing and return ok";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Module::onMasterVolumeChanged(float volume __unused) {
    LOG(VERBOSE) << __func__ << ": do nothing and return ok";
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
