/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */


#ifndef CTSAUDIO_TASKINPUT_H
#define CTSAUDIO_TASKINPUT_H

#include <utils/StrongPointer.h>
#include "TaskAsync.h"
#include "audio/AudioHardware.h"
#include "audio/Buffer.h"

class TaskInput: public TaskAsync {
public:
    TaskInput();
    virtual ~TaskInput();
    virtual bool parseAttribute(const android::String8& name, const android::String8& value);
    virtual TaskGeneric::ExecutionResult start();
    virtual TaskGeneric::ExecutionResult complete();
private:
    int mRecordingTimeInMs;
    android::sp<AudioHardware> mHw;
    android::sp<Buffer> mBuffer;
};


#endif // CTSAUDIO_TASKINPUT_H
