/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#ifndef OS_ANDROID_HWC_HWCSERVICEHELPER_H_
#define OS_ANDROID_HWC_HWCSERVICEHELPER_H_

// Inline c++ helpers to augment HwcServiceApi.h

#include <HwcServiceApi.h>

#if __ANDROID__
#include <utils/RefBase.h>

class HwcServiceConnection : public android::RefBase {
 public:
  HwcServiceConnection() {
    mHwcs = HwcService_Connect();
  }
  ~HwcServiceConnection() {
    HwcService_Disconnect(mHwcs);
  }
  operator HWCSHANDLE() {
    return mHwcs;
  }
  HWCSHANDLE handle() {
    return mHwcs;
  }

 private:
  // Non-copyable.
  HwcServiceConnection(HwcServiceConnection const &);
  void operator=(HwcServiceConnection const &);

  HWCSHANDLE mHwcs;
};

#endif  // __ANDROID__

#endif  // OS_ANDROID_HWC_HWCSERVICEHELPER_H_
