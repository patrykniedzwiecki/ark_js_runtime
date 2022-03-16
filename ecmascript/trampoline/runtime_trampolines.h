/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ECMASCRIPT_RUNTIME_TRAMPOLINES_NEW_H
#define ECMASCRIPT_RUNTIME_TRAMPOLINES_NEW_H
#include "ecmascript/ecma_macros.h"
#include "ecmascript/interpreter/frame_handler.h"
#include "ecmascript/js_thread.h"
#include "ecmascript/trampoline/runtime_define.h"

namespace panda::ecmascript {
extern "C" JSTaggedType RuntimeCallTrampolineAot(uintptr_t glue, uint64_t runtime_id,
                                                 uint64_t argc, ...);
extern "C" JSTaggedType RuntimeCallTrampolineInterpreterAsm(uintptr_t glue, uint64_t runtime_id,
                                                            uint64_t argc, ...);
class RuntimeTrampolines {
public:
    static void InitializeRuntimeTrampolines(JSThread *thread)
    {
    #define DEF_RUNTIME_STUB(name, counter) RuntimeTrampolineId::RUNTIME_ID_##name
    #define INITIAL_RUNTIME_FUNCTIONS(name, count) \
        thread->SetRuntimeFunction(DEF_RUNTIME_STUB(name, count), reinterpret_cast<uintptr_t>(name));
        ALL_RUNTIME_CALL_LIST(INITIAL_RUNTIME_FUNCTIONS)
    #undef INITIAL_RUNTIME_FUNCTIONS
    #undef DEF_RUNTIME_STUB
    }

#define DECLARE_RUNTIME_TRAMPOLINES(name, counter) \
    static JSTaggedType name(uintptr_t argGlue, uint32_t argc, uintptr_t argv);
    RUNTIME_CALL_LIST(DECLARE_RUNTIME_TRAMPOLINES)
#undef DECLARE_RUNTIME_TRAMPOLINES

    static void DebugPrint(int fmtMessageId, ...);
    static void MarkingBarrier([[maybe_unused]]uintptr_t argGlue, uintptr_t slotAddr,
        Region *objectRegion, TaggedObject *value, Region *valueRegion);
    static void InsertOldToNewRememberedSet([[maybe_unused]]uintptr_t argGlue, Region* region, uintptr_t addr);
    static int32_t DoubleToInt(double x);

private:
    static void PrintHeapReginInfo(uintptr_t argGlue);
};
}  // namespace panda::ecmascript
#endif