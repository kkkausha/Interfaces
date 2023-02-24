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

#pragma once

#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <fmq/AidlMessageQueue.h>
#include <cstdlib>
#include <memory>

#include "effect-impl/EffectImpl.h"
#include "effect-impl/EffectUUID.h"

namespace aidl::android::hardware::audio::effect {

class AutomaticGainControlV2SwContext final : public EffectContext {
  public:
    AutomaticGainControlV2SwContext(int statusDepth, const Parameter::Common& common)
        : EffectContext(statusDepth, common) {
        LOG(DEBUG) << __func__;
    }

    RetCode setDigitalGain(int gain);
    int getDigitalGain();
    RetCode setLevelEstimator(AutomaticGainControlV2::LevelEstimator levelEstimator);
    AutomaticGainControlV2::LevelEstimator getLevelEstimator();
    RetCode setSaturationMargin(int margin);
    int getSaturationMargin();

  private:
    int mDigitalGain = 0;
    AutomaticGainControlV2::LevelEstimator mLevelEstimator =
            AutomaticGainControlV2::LevelEstimator::RMS;
    int mSaturationMargin = 0;
};

class AutomaticGainControlV2Sw final : public EffectImpl {
  public:
    static const std::string kEffectName;
    static const bool kStrengthSupported;
    static const Capability kCapability;
    static const Descriptor kDescriptor;
    AutomaticGainControlV2Sw() { LOG(DEBUG) << __func__; }
    ~AutomaticGainControlV2Sw() {
        cleanUp();
        LOG(DEBUG) << __func__;
    }

    ndk::ScopedAStatus getDescriptor(Descriptor* _aidl_return) override;
    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id,
                                            Parameter::Specific* specific) override;

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common) override;
    std::shared_ptr<EffectContext> getContext() override;
    RetCode releaseContext() override;

    std::string getEffectName() override { return kEffectName; };
    IEffect::Status effectProcessImpl(float* in, float* out, int samples) override;

  private:
    static const std::vector<Range::AutomaticGainControlV2Range> kRanges;
    std::shared_ptr<AutomaticGainControlV2SwContext> mContext;
    ndk::ScopedAStatus getParameterAutomaticGainControlV2(const AutomaticGainControlV2::Tag& tag,
                                                          Parameter::Specific* specific);
};
}  // namespace aidl::android::hardware::audio::effect
