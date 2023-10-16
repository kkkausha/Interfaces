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

#include "FakeFingerprintEngine.h"
#include <regex>
#include "Fingerprint.h"

#include <android-base/logging.h>
#include <android-base/parseint.h>

#include <fingerprint.sysprop.h>

#include "util/CancellationSignal.h"
#include "util/Util.h"

using namespace ::android::fingerprint::virt;
using ::android::base::ParseInt;

namespace aidl::android::hardware::biometrics::fingerprint {

void FakeFingerprintEngine::generateChallengeImpl(ISessionCallback* cb) {
    BEGIN_OP(0);
    std::uniform_int_distribution<int64_t> dist;
    auto challenge = dist(mRandom);
    FingerprintHalProperties::challenge(challenge);
    cb->onChallengeGenerated(challenge);
}

void FakeFingerprintEngine::revokeChallengeImpl(ISessionCallback* cb, int64_t challenge) {
    BEGIN_OP(0);
    FingerprintHalProperties::challenge({});
    cb->onChallengeRevoked(challenge);
}

void FakeFingerprintEngine::enrollImpl(ISessionCallback* cb,
                                       const keymaster::HardwareAuthToken& hat,
                                       const std::future<void>& cancel) {
    BEGIN_OP(getLatency(FingerprintHalProperties::operation_enroll_latency()));

    // Do proper HAT verification in the real implementation.
    if (hat.mac.empty()) {
        LOG(ERROR) << "Fail: hat";
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }

    // Force error-out
    auto err = FingerprintHalProperties::operation_enroll_error().value_or(0);
    if (err != 0) {
        LOG(ERROR) << "Fail: operation_enroll_error";
        auto ec = convertError(err);
        cb->onError(ec.first, ec.second);
        return;
    }

    // Format is "<id>:<progress_ms-[acquiredInfo..]>,...:<result>
    auto nextEnroll = FingerprintHalProperties::next_enrollment().value_or("");
    auto parts = Util::split(nextEnroll, ":");
    if (parts.size() != 3) {
        LOG(ERROR) << "Fail: invalid next_enrollment:" << nextEnroll;
        cb->onError(Error::VENDOR, 0 /* vendorError */);
        return;
    }
    auto enrollmentId = std::stoi(parts[0]);
    auto progress = parseEnrollmentCapture(parts[1]);
    for (size_t i = 0; i < progress.size(); i += 2) {
        auto left = (progress.size() - i) / 2 - 1;
        auto duration = progress[i][0];
        auto acquired = progress[i + 1];
        auto N = acquired.size();

        for (int j = 0; j < N; j++) {
            SLEEP_MS(duration / N);

            if (shouldCancel(cancel)) {
                LOG(ERROR) << "Fail: cancel";
                cb->onError(Error::CANCELED, 0 /* vendorCode */);
                return;
            }
            auto ac = convertAcquiredInfo(acquired[j]);
            cb->onAcquired(ac.first, ac.second);
        }

        if (left == 0 && !IS_TRUE(parts[2])) {  // end and failed
            LOG(ERROR) << "Fail: requested by caller: " << nextEnroll;
            FingerprintHalProperties::next_enrollment({});
            cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorCode */);
        } else {  // progress and update props if last time
            LOG(INFO) << "onEnroll: " << enrollmentId << " left: " << left;
            if (left == 0) {
                auto enrollments = FingerprintHalProperties::enrollments();
                enrollments.emplace_back(enrollmentId);
                FingerprintHalProperties::enrollments(enrollments);
                FingerprintHalProperties::next_enrollment({});
                // change authenticatorId after new enrollment
                auto id = FingerprintHalProperties::authenticator_id().value_or(0);
                auto newId = id + 1;
                FingerprintHalProperties::authenticator_id(newId);
                LOG(INFO) << "Enrolled: " << enrollmentId;
            }
            cb->onEnrollmentProgress(enrollmentId, left);
        }
    }
}

void FakeFingerprintEngine::authenticateImpl(ISessionCallback* cb, int64_t /* operationId */,
                                             const std::future<void>& cancel) {
    BEGIN_OP(getLatency(FingerprintHalProperties::operation_authenticate_latency()));

    int64_t now = Util::getSystemNanoTime();
    int64_t duration = FingerprintHalProperties::operation_authenticate_duration().value_or(10);
    auto acquired = FingerprintHalProperties::operation_authenticate_acquired().value_or("1");
    auto acquiredInfos = parseIntSequence(acquired);
    int N = acquiredInfos.size();

    if (N == 0) {
        LOG(ERROR) << "Fail to parse authentiate acquired info: " + acquired;
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }

    // got lockout?
    FakeLockoutTracker::LockoutMode lockoutMode = mLockoutTracker.getMode();
    if (lockoutMode == FakeLockoutTracker::LockoutMode::kPermanent) {
        LOG(ERROR) << "Fail: lockout permanent";
        cb->onLockoutPermanent();
        return;
    } else if (lockoutMode == FakeLockoutTracker::LockoutMode::kTimed) {
        int64_t timeLeft = mLockoutTracker.getLockoutTimeLeft();
        LOG(ERROR) << "Fail: lockout timed " << timeLeft;
        cb->onLockoutTimed(timeLeft);
    }

    int i = 0;
    do {
        if (FingerprintHalProperties::operation_authenticate_fails().value_or(false)) {
            LOG(ERROR) << "Fail: operation_authenticate_fails";
            mLockoutTracker.addFailedAttempt();
            cb->onAuthenticationFailed();
            return;
        }

        auto err = FingerprintHalProperties::operation_authenticate_error().value_or(0);
        if (err != 0) {
            LOG(ERROR) << "Fail: operation_authenticate_error";
            auto ec = convertError(err);
            cb->onError(ec.first, ec.second);
            return;
        }

        if (FingerprintHalProperties::lockout().value_or(false)) {
            LOG(ERROR) << "Fail: lockout";
            cb->onLockoutPermanent();
            cb->onError(Error::HW_UNAVAILABLE, 0 /* vendorError */);
            return;
        }

        if (shouldCancel(cancel)) {
            LOG(ERROR) << "Fail: cancel";
            cb->onError(Error::CANCELED, 0 /* vendorCode */);
            return;
        }

        if (i < N) {
            auto ac = convertAcquiredInfo(acquiredInfos[i]);
            cb->onAcquired(ac.first, ac.second);
            i++;
        }

        SLEEP_MS(duration / N);
    } while (!Util::hasElapsed(now, duration));

    auto id = FingerprintHalProperties::enrollment_hit().value_or(0);
    auto enrolls = FingerprintHalProperties::enrollments();
    auto isEnrolled = std::find(enrolls.begin(), enrolls.end(), id) != enrolls.end();
    if (id > 0 && isEnrolled) {
        cb->onAuthenticationSucceeded(id, {} /* hat */);
        mLockoutTracker.reset();
        return;
    } else {
        LOG(ERROR) << "Fail: fingerprint not enrolled";
        cb->onAuthenticationFailed();
        mLockoutTracker.addFailedAttempt();
    }
}

void FakeFingerprintEngine::detectInteractionImpl(ISessionCallback* cb,
                                                  const std::future<void>& cancel) {
    BEGIN_OP(getLatency(FingerprintHalProperties::operation_detect_interaction_latency()));

    int64_t duration =
            FingerprintHalProperties::operation_detect_interaction_duration().value_or(10);

    auto detectInteractionSupported =
            FingerprintHalProperties::detect_interaction().value_or(false);
    if (!detectInteractionSupported) {
        LOG(ERROR) << "Detect interaction is not supported";
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }

    auto acquired = FingerprintHalProperties::operation_detect_interaction_acquired().value_or("1");
    auto acquiredInfos = parseIntSequence(acquired);
    int N = acquiredInfos.size();
    int64_t now = Util::getSystemNanoTime();

    if (N == 0) {
        LOG(ERROR) << "Fail to parse detect interaction acquired info: " + acquired;
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }

    int i = 0;
    do {
        auto err = FingerprintHalProperties::operation_detect_interaction_error().value_or(0);
        if (err != 0) {
            LOG(ERROR) << "Fail: operation_detect_interaction_error";
            auto ec = convertError(err);
            cb->onError(ec.first, ec.second);
            return;
        }

        if (shouldCancel(cancel)) {
            LOG(ERROR) << "Fail: cancel";
            cb->onError(Error::CANCELED, 0 /* vendorCode */);
            return;
        }

        if (i < N) {
            auto ac = convertAcquiredInfo(acquiredInfos[i]);
            cb->onAcquired(ac.first, ac.second);
            i++;
        }
        SLEEP_MS(duration / N);
    } while (!Util::hasElapsed(now, duration));

    auto id = FingerprintHalProperties::enrollment_hit().value_or(0);
    auto enrolls = FingerprintHalProperties::enrollments();
    auto isEnrolled = std::find(enrolls.begin(), enrolls.end(), id) != enrolls.end();
    if (id <= 0 || !isEnrolled) {
        LOG(ERROR) << "Fail: not enrolled";
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }

    cb->onInteractionDetected();
}

void FakeFingerprintEngine::enumerateEnrollmentsImpl(ISessionCallback* cb) {
    BEGIN_OP(0);

    std::vector<int32_t> ids;
    // There are some enrollment sync issue with framework, which results in
    //  a single template removal during the very firt sync command after reboot.
    //  This is a workaround for now. TODO(b/243129174)
    ids.push_back(-1);

    for (auto& enrollment : FingerprintHalProperties::enrollments()) {
        auto id = enrollment.value_or(0);
        if (id > 0) {
            ids.push_back(id);
        }
    }

    cb->onEnrollmentsEnumerated(ids);
}

void FakeFingerprintEngine::removeEnrollmentsImpl(ISessionCallback* cb,
                                                  const std::vector<int32_t>& enrollmentIds) {
    BEGIN_OP(0);

    std::vector<std::optional<int32_t>> newEnrollments;
    std::vector<int32_t> removed;
    for (auto& enrollment : FingerprintHalProperties::enrollments()) {
        auto id = enrollment.value_or(0);
        if (std::find(enrollmentIds.begin(), enrollmentIds.end(), id) != enrollmentIds.end()) {
            removed.push_back(id);
        } else if (id > 0) {
            newEnrollments.emplace_back(id);
        }
    }
    FingerprintHalProperties::enrollments(newEnrollments);

    cb->onEnrollmentsRemoved(enrollmentIds);
}

void FakeFingerprintEngine::getAuthenticatorIdImpl(ISessionCallback* cb) {
    BEGIN_OP(0);
    int64_t authenticatorId;
    if (FingerprintHalProperties::enrollments().size() == 0) {
        authenticatorId = 0;
    } else {
        authenticatorId = FingerprintHalProperties::authenticator_id().value_or(0);
        if (authenticatorId == 0) authenticatorId = 1;
    }
    cb->onAuthenticatorIdRetrieved(authenticatorId);
}

void FakeFingerprintEngine::invalidateAuthenticatorIdImpl(ISessionCallback* cb) {
    BEGIN_OP(0);
    int64_t newId;
    if (FingerprintHalProperties::enrollments().size() == 0) {
        newId = 0;
    } else {
        auto id = FingerprintHalProperties::authenticator_id().value_or(0);
        newId = id + 1;
    }
    FingerprintHalProperties::authenticator_id(newId);
    cb->onAuthenticatorIdInvalidated(newId);
}

void FakeFingerprintEngine::resetLockoutImpl(ISessionCallback* cb,
                                             const keymaster::HardwareAuthToken& hat) {
    BEGIN_OP(0);
    if (hat.mac.empty()) {
        LOG(ERROR) << "Fail: hat in resetLockout()";
        cb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorError */);
        return;
    }
    FingerprintHalProperties::lockout(false);
    cb->onLockoutCleared();
    mLockoutTracker.reset();
}

ndk::ScopedAStatus FakeFingerprintEngine::onPointerDownImpl(int32_t /*pointerId*/, int32_t /*x*/,
                                                            int32_t /*y*/, float /*minor*/,
                                                            float /*major*/) {
    BEGIN_OP(0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus FakeFingerprintEngine::onPointerUpImpl(int32_t /*pointerId*/) {
    BEGIN_OP(0);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus FakeFingerprintEngine::onUiReadyImpl() {
    BEGIN_OP(0);
    return ndk::ScopedAStatus::ok();
}

bool FakeFingerprintEngine::getSensorLocationConfig(SensorLocation& out) {
    auto loc = FingerprintHalProperties::sensor_location().value_or("");
    auto isValidStr = false;
    auto dim = Util::split(loc, ":");

    if (dim.size() < 3 or dim.size() > 4) {
        if (!loc.empty()) LOG(WARNING) << "Invalid sensor location input (x:y:radius):" + loc;
        return false;
    } else {
        int32_t x, y, r;
        std::string d = "";
        if (dim.size() >= 3) {
            isValidStr = ParseInt(dim[0], &x) && ParseInt(dim[1], &y) && ParseInt(dim[2], &r);
        }
        if (dim.size() >= 4) {
            d = dim[3];
        }
        if (isValidStr) out = {0, x, y, r, d};

        return isValidStr;
    }
}
SensorLocation FakeFingerprintEngine::getSensorLocation() {
    SensorLocation location;

    if (getSensorLocationConfig(location)) {
        return location;
    } else {
        return defaultSensorLocation();
    }
}

SensorLocation FakeFingerprintEngine::defaultSensorLocation() {
    return {0 /* displayId (not used) */, 0 /* sensorLocationX */, 0 /* sensorLocationY */,
            0 /* sensorRadius */, "" /* display */};
}

std::vector<int32_t> FakeFingerprintEngine::parseIntSequence(const std::string& str,
                                                             const std::string& sep) {
    std::vector<std::string> seqs = Util::split(str, sep);
    std::vector<int32_t> res;

    for (const auto& seq : seqs) {
        int32_t val;
        if (ParseInt(seq, &val)) {
            res.push_back(val);
        } else {
            LOG(WARNING) << "Invalid int sequence:" + str;
            res.clear();
            break;
        }
    }

    return res;
}

bool FakeFingerprintEngine::parseEnrollmentCaptureSingle(const std::string& str,
                                                         std::vector<std::vector<int32_t>>& res) {
    std::vector<int32_t> defaultAcquiredInfo = {(int32_t)AcquiredInfo::GOOD};
    bool aborted = true;

    do {
        std::smatch sms;
        // Parses strings like "1000-[5,1]" or "500"
        std::regex ex("((\\d+)(-\\[([\\d|,]+)\\])?)");
        if (!regex_match(str.cbegin(), str.cend(), sms, ex)) break;
        int32_t duration;
        if (!ParseInt(sms.str(2), &duration)) break;
        res.push_back({duration});
        if (!sms.str(4).empty()) {
            auto acqv = parseIntSequence(sms.str(4));
            if (acqv.empty()) break;
            res.push_back(acqv);
        } else
            res.push_back(defaultAcquiredInfo);
        aborted = false;
    } while (0);

    return !aborted;
}

std::vector<std::vector<int32_t>> FakeFingerprintEngine::parseEnrollmentCapture(
        const std::string& str) {
    std::vector<std::vector<int32_t>> res;

    std::string s(str);
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    bool aborted = false;
    std::smatch sms;
    // Parses strings like "1000-[5,1],500,800-[6,5,1]"
    //                      ---------- --- -----------
    //  into parts:             A       B       C
    while (regex_search(s, sms, std::regex("^(,)?(\\d+(-\\[[\\d|,]+\\])?)"))) {
        if (!parseEnrollmentCaptureSingle(sms.str(2), res)) {
            aborted = true;
            break;
        }
        s = sms.suffix();
    }
    if (aborted || s.length() != 0) {
        res.clear();
        LOG(ERROR) << "Failed to parse enrollment captures:" + str;
    }

    return res;
}

std::pair<AcquiredInfo, int32_t> FakeFingerprintEngine::convertAcquiredInfo(int32_t code) {
    std::pair<AcquiredInfo, int32_t> res;
    if (code > FINGERPRINT_ACQUIRED_VENDOR_BASE) {
        res.first = AcquiredInfo::VENDOR;
        res.second = code - FINGERPRINT_ACQUIRED_VENDOR_BASE;
    } else {
        res.first = (AcquiredInfo)code;
        res.second = 0;
    }
    return res;
}

std::pair<Error, int32_t> FakeFingerprintEngine::convertError(int32_t code) {
    std::pair<Error, int32_t> res;
    if (code > FINGERPRINT_ERROR_VENDOR_BASE) {
        res.first = Error::VENDOR;
        res.second = code - FINGERPRINT_ERROR_VENDOR_BASE;
    } else {
        res.first = (Error)code;
        res.second = 0;
    }
    return res;
}

int32_t FakeFingerprintEngine::getLatency(
        const std::vector<std::optional<std::int32_t>>& latencyIn) {
    int32_t res = DEFAULT_LATENCY;

    std::vector<int32_t> latency;
    for (auto x : latencyIn)
        if (x.has_value()) latency.push_back(*x);

    switch (latency.size()) {
        case 0:
            break;
        case 1:
            res = latency[0];
            break;
        case 2:
            res = getRandomInRange(latency[0], latency[1]);
            break;
        default:
            LOG(ERROR) << "ERROR: unexpected input of size " << latency.size();
            break;
    }

    return res;
}

int32_t FakeFingerprintEngine::getRandomInRange(int32_t bound1, int32_t bound2) {
    std::uniform_int_distribution<int32_t> dist(std::min(bound1, bound2), std::max(bound1, bound2));
    return dist(mRandom);
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
