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
#include "service.hpp"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <utils/Log.h>

#include "thread_chip.hpp"

namespace aidl {
namespace android {
namespace hardware {
namespace threadnetwork {

Service::Service(char* urls[], int numUrls) : mBinderFd(-1) {
    int fd;

    CHECK_NE(urls, nullptr);
    CHECK_GT(numUrls, 0);

    for (int i = 0; i < numUrls; i++) {
        auto threadChip = ndk::SharedRefBase::make<ThreadChip>(i, urls[i]);
        CHECK_NE(threadChip, nullptr);
        mThreadChips.push_back(std::move(threadChip));
    }

    binder_status_t status = ABinderProcess_setupPolling(&fd);
    CHECK_EQ(status, ::STATUS_OK);
    CHECK_GE(fd, 0);
    mBinderFd.reset(fd);
}

void Service::Update(otSysMainloopContext& context) {
    FD_SET(mBinderFd.get(), &context.mReadFdSet);
    context.mMaxFd = std::max(context.mMaxFd, mBinderFd.get());
}

void Service::Process(const otSysMainloopContext& context) {
    if (FD_ISSET(mBinderFd.get(), &context.mReadFdSet)) {
        ABinderProcess_handlePolledCommands();
    }
}

void Service::startLoop(void) {
    const struct timeval kPollTimeout = {1, 0};
    otSysMainloopContext context;
    int rval;

    ot::Posix::Mainloop::Manager::Get().Add(*this);

    while (true) {
        context.mMaxFd = -1;
        context.mTimeout = kPollTimeout;

        FD_ZERO(&context.mReadFdSet);
        FD_ZERO(&context.mWriteFdSet);
        FD_ZERO(&context.mErrorFdSet);

        ot::Posix::Mainloop::Manager::Get().Update(context);

        rval = select(context.mMaxFd + 1, &context.mReadFdSet, &context.mWriteFdSet,
                      &context.mErrorFdSet, &context.mTimeout);

        if (rval >= 0) {
            ot::Posix::Mainloop::Manager::Get().Process(context);
        } else if (errno != EINTR) {
            ALOGE("select() failed: %s", strerror(errno));
            break;
        }
    }
}
}  // namespace threadnetwork
}  // namespace hardware
}  // namespace android
}  // namespace aidl
