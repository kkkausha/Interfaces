/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef android_hardware_vehicle_V2_0_VehicleHal_H_
#define android_hardware_vehicle_V2_0_VehicleHal_H_

#include <android/hardware/vehicle/2.0/IVehicle.h>
#include "vehicle_hal_manager/VehicleObjectPool.h"


namespace android {
namespace hardware {
namespace vehicle {
namespace V2_0 {

/**
 * This is a low-level vehicle hal interface that should be implemented by
 * Vendor.
 */
class VehicleHal {
public:
    using VehiclePropValuePtr = recyclable_ptr<VehiclePropValue>;

    using HalEventFunction = std::function<void(VehiclePropValuePtr)>;
    using HalErrorFunction = std::function<void(
            VehicleProperty property,
            status_t errorCode,
            VehiclePropertyOperation operation)>;

    virtual ~VehicleHal() {}

    virtual std::vector<VehiclePropConfig> listProperties() = 0;
    virtual VehiclePropValuePtr get(VehicleProperty property,
                                    int32_t areaId,
                                    status_t* outStatus) = 0;

    virtual status_t set(const VehiclePropValue& propValue) = 0;

    /**
     * Subscribe to HAL property events. This method might be called multiple
     * times for the same vehicle property to update subscribed areas or sample
     * rate.
     *
     * @param property to subscribe
     * @param areas a bitwise vehicle areas or 0 for all supported areas
     * @param sampleRate sample rate in Hz for properties that support sample
     *                   rate, e.g. for properties with
     *                   VehiclePropertyChangeMode::CONTINUOUS
     */
    virtual status_t subscribe(VehicleProperty property,
                               int32_t areas,
                               float sampleRate) = 0;

    /**
     * Unsubscribe from HAL events for given property
     *
     * @param property vehicle property to unsubscribe
     */
    virtual status_t unsubscribe(VehicleProperty property) = 0;

    /**
     * Override this method if you need to do one-time initialization.
     */
    virtual void onCreate() {}

    void init(
        VehiclePropValuePool* valueObjectPool,
        const HalEventFunction& onHalEvent,
        const HalErrorFunction& onHalError) {
        mValuePool = valueObjectPool;
        mOnHalEvent = onHalEvent;
        mOnHalError = onHalError;

        onCreate();
    }

    VehiclePropValuePool* getValuePool() {
        return mValuePool;
    }
protected:
    void doHalEvent(VehiclePropValuePtr v) {
        mOnHalEvent(std::move(v));
    }

    void doHalError(VehicleProperty property,
                    status_t errorCode,
                    VehiclePropertyOperation operation) {
        mOnHalError(property, errorCode, operation);
    }

private:
    HalEventFunction mOnHalEvent;
    HalErrorFunction mOnHalError;
    VehiclePropValuePool* mValuePool;
};

}  // namespace V2_0
}  // namespace vehicle
}  // namespace hardware
}  // namespace android

#endif //android_hardware_vehicle_V2_0_VehicleHal_H_
