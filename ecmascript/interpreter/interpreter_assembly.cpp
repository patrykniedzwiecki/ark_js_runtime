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

#include "ecmascript/class_linker/program_object-inl.h"
#include "ecmascript/ecma_string.h"
#include "ecmascript/ecma_vm.h"
#include "ecmascript/global_env.h"
#include "ecmascript/ic/ic_runtime_stub-inl.h"
#include "ecmascript/interpreter/fast_runtime_stub-inl.h"
#include "ecmascript/interpreter/frame_handler.h"
#include "ecmascript/interpreter/interpreter_assembly.h"
#include "ecmascript/interpreter/slow_runtime_stub.h"
#include "ecmascript/js_generator_object.h"
#include "ecmascript/js_tagged_value.h"
#include "ecmascript/literal_data_extractor.h"
#include "ecmascript/mem/concurrent_marker.h"
#include "ecmascript/runtime_call_id.h"
#include "ecmascript/template_string.h"
#include "ecmascript/vmstat/runtime_stat.h"
#include "include/runtime_notification.h"
#include "libpandafile/code_data_accessor.h"
#include "libpandafile/file.h"
#include "libpandafile/method_data_accessor.h"

namespace panda::ecmascript {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvoid-ptr-dereference"
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_INST() LOG(DEBUG, INTERPRETER) << ": "

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ADVANCE_PC(offset) \
    pc += (offset);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-macro-usage)

#define GOTO_NEXT()  // NOLINT(clang-diagnostic-gnu-label-as-value, cppcoreguidelines-macro-usage)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DISPATCH_OFFSET(offset)                                                                               \
    do {                                                                                                      \
        ADVANCE_PC(offset)                                                                                    \
        SAVE_PC();                                                                                            \
        SAVE_ACC();                                                                                           \
        InterpretedFrame *frame = GET_FRAME(sp);                                                              \
        auto currentMethod = ECMAObject::Cast(frame->function.GetTaggedObject())->GetCallTarget();            \
        currentMethod->SetHotnessCounter(static_cast<uint32_t>(hotnessCounter));                              \
        return;                                                                                               \
    } while (false)

#define DISPATCH(format)  DISPATCH_OFFSET(BytecodeInstruction::Size(format))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define GET_FRAME(CurrentSp) \
    (reinterpret_cast<InterpretedFrame *>(CurrentSp) - 1)  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SAVE_PC() (GET_FRAME(sp)->pc = pc)  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SAVE_ACC() (GET_FRAME(sp)->acc = acc)  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define RESTORE_ACC() (acc = GET_FRAME(sp)->acc)  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define INTERPRETER_GOTO_EXCEPTION_HANDLER()                                                                        \
    do {                                                                                                            \
        SAVE_PC();                                                                                                  \
        return asmDispatchTable[EcmaOpcode::LAST_OPCODE](                                                           \
            thread, pc, sp, constpool, profileTypeInfo, acc, hotnessCounter);                                       \
    } while (false)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define INTERPRETER_RETURN_IF_ABRUPT(result)      \
    do {                                          \
        if (result.IsException()) {               \
            INTERPRETER_GOTO_EXCEPTION_HANDLER(); \
        }                                         \
    } while (false)

#if ECMASCRIPT_ENABLE_IC
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define UPDATE_HOTNESS_COUNTER(offset)                            \
    do {                                                          \
        hotnessCounter += offset;                                 \
        if (UNLIKELY(hotnessCounter <= 0)) {                      \
            profileTypeInfo = UpdateHotnessCounter(thread, sp);   \
            hotnessCounter = std::numeric_limits<int32_t>::max(); \
        }                                                         \
    } while (false)
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define UPDATE_HOTNESS_COUNTER(offset) static_cast<void>(0)
#endif

#define READ_INST_OP() READ_INST_8(0)               // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_4_0() (READ_INST_8(1) & 0xf)      // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_4_1() (READ_INST_8(1) >> 4 & 0xf) // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_4_2() (READ_INST_8(2) & 0xf)      // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_4_3() (READ_INST_8(2) >> 4 & 0xf) // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_0() READ_INST_8(1)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_1() READ_INST_8(2)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_2() READ_INST_8(3)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_3() READ_INST_8(4)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_4() READ_INST_8(5)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_5() READ_INST_8(6)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_6() READ_INST_8(7)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_7() READ_INST_8(8)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8_8() READ_INST_8(9)              // NOLINT(hicpp-signed-bitwise, cppcoreguidelines-macro-usage)
#define READ_INST_8(offset) (*(pc + offset))
#define MOVE_AND_READ_INST_8(currentInst, offset) \
    currentInst <<= 8;                            \
    currentInst += READ_INST_8(offset);           \

#define READ_INST_16_0() READ_INST_16(2)
#define READ_INST_16_1() READ_INST_16(3)
#define READ_INST_16_2() READ_INST_16(4)
#define READ_INST_16_3() READ_INST_16(5)
#define READ_INST_16_5() READ_INST_16(7)
#define READ_INST_16(offset)                          \
    ({                                                \
        uint16_t currentInst = READ_INST_8(offset);   \
        MOVE_AND_READ_INST_8(currentInst, offset - 1) \
    })

#define READ_INST_32_0() READ_INST_32(4)
#define READ_INST_32_1() READ_INST_32(5)
#define READ_INST_32_2() READ_INST_32(6)
#define READ_INST_32(offset)                          \
    ({                                                \
        uint32_t currentInst = READ_INST_8(offset);   \
        MOVE_AND_READ_INST_8(currentInst, offset - 1) \
        MOVE_AND_READ_INST_8(currentInst, offset - 2) \
        MOVE_AND_READ_INST_8(currentInst, offset - 3) \
    })

#define READ_INST_64_0()                       \
    ({                                         \
        uint64_t currentInst = READ_INST_8(8); \
        MOVE_AND_READ_INST_8(currentInst, 7)   \
        MOVE_AND_READ_INST_8(currentInst, 6)   \
        MOVE_AND_READ_INST_8(currentInst, 5)   \
        MOVE_AND_READ_INST_8(currentInst, 4)   \
        MOVE_AND_READ_INST_8(currentInst, 3)   \
        MOVE_AND_READ_INST_8(currentInst, 2)   \
        MOVE_AND_READ_INST_8(currentInst, 1)   \
    })

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define GET_VREG(idx) (sp[idx])  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define GET_VREG_VALUE(idx) (JSTaggedValue(sp[idx]))  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define SET_VREG(idx, val) (sp[idx] = (val));  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
#define GET_ACC() (acc)                        // NOLINT(cppcoreguidelines-macro-usage)
#define SET_ACC(val) (acc = val);              // NOLINT(cppcoreguidelines-macro-usage)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define HANDLERCALL()                                                                                                \
    do {                                                                                                             \
        JSTaggedValue func = GET_VREG_VALUE(funcReg);                                                                \
        if (!func.IsCallable()) {                                                                                    \
            {                                                                                                        \
                [[maybe_unused]] EcmaHandleScope handleScope(thread);                                                \
                EcmaVM *ecmaVm = thread->GetEcmaVM();                                                                \
                 ObjectFactory *factory = ecmaVm->GetFactory();                                                      \
                JSHandle<JSObject> error = factory->GetJSError(ErrorType::TYPE_ERROR, "is not callable");            \
                thread->SetException(error.GetTaggedValue());                                                        \
            }                                                                                                        \
            INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                                    \
        }                                                                                                            \
        ECMAObject *thisFunc = ECMAObject::Cast(func.GetTaggedObject());                                             \
        JSMethod *methodToCall = thisFunc->GetCallTarget();                                                          \
        if (methodToCall->IsNative()) {                                                                              \
            ASSERT(methodToCall->GetNumVregs() == 0);                                                                \
            size_t frameSize = FRAME_STATE_SIZE + actualNumArgs + NUM_MANDATORY_JSFUNC_ARGS;                         \
            JSTaggedType *newSp = sp - frameSize;                                                                    \
            if (thread->DoStackOverflowCheck(newSp) || thread->HasPendingException()) {                              \
                INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                                \
            }                                                                                                        \
            EcmaRuntimeCallInfo ecmaRuntimeCallInfo(thread, actualNumArgs + NUM_MANDATORY_JSFUNC_ARGS,               \
                                                    reinterpret_cast<JSTaggedValue *>(newSp));                       \
            uint32_t startIdx = 0;                                                                                   \
            newSp[startIdx++] = static_cast<JSTaggedType>(ToUintPtr(thisFunc));                                      \
            newSp[startIdx++] = JSTaggedValue::VALUE_UNDEFINED;                                                      \
                                                                                                                     \
            if (callRange) {                                                                                         \
                size_t copyArgs = actualNumArgs + NUM_MANDATORY_JSFUNC_ARGS - 2;                                     \
                if (!callThis) {                                                                                     \
                    newSp[startIdx++] = JSTaggedValue::VALUE_UNDEFINED;                                              \
                    copyArgs--;                                                                                      \
                }                                                                                                    \
                for (size_t i = 1; i <= copyArgs; i++) {                                                             \
                    JSTaggedValue arg = GET_VREG_VALUE(funcReg + i);                                                 \
                    newSp[startIdx++] = arg.GetRawData();                                                            \
                }                                                                                                    \
            } else {                                                                                                 \
                newSp[startIdx] = JSTaggedValue::VALUE_UNDEFINED;                                                    \
                switch (actualNumArgs) {                                                                             \
                    case ActualNumArgsOfCall::CALLARGS3: {                                                           \
                        uint32_t reg = READ_INST_8_4();                                                              \
                        JSTaggedValue arg = GET_VREG_VALUE(reg);                                                     \
                        newSp[ActualNumArgsOfCall::CALLARGS3 + NUM_MANDATORY_JSFUNC_ARGS - 1] = arg.GetRawData();    \
                        [[fallthrough]];                                                                             \
                    }                                                                                                \
                    case ActualNumArgsOfCall::CALLARGS2: {                                                           \
                        uint32_t reg = READ_INST_8_3();                                                              \
                        JSTaggedValue arg = GET_VREG_VALUE(reg);                                                     \
                        newSp[ActualNumArgsOfCall::CALLARGS2 + NUM_MANDATORY_JSFUNC_ARGS - 1] = arg.GetRawData();    \
                        [[fallthrough]];                                                                             \
                    }                                                                                                \
                    case ActualNumArgsOfCall::CALLARG1: {                                                            \
                        uint32_t reg = READ_INST_8_2();                                                              \
                        JSTaggedValue arg = GET_VREG_VALUE(reg);                                                     \
                        newSp[ActualNumArgsOfCall::CALLARG1 + NUM_MANDATORY_JSFUNC_ARGS - 1] = arg.GetRawData();     \
                        break;                                                                                       \
                    }                                                                                                \
                    case ActualNumArgsOfCall::CALLARG0: {                                                            \
                        break;                                                                                       \
                    }                                                                                                \
                    default:                                                                                         \
                        UNREACHABLE();                                                                               \
                }                                                                                                    \
            }                                                                                                        \
                                                                                                                     \
            InterpretedFrame *state = GET_FRAME(newSp);                                                              \
            state->base.prev = sp;                                                                                   \
            state->base.frameType = static_cast<uintptr_t>(FrameType::INTERPRETER_FRAME);                            \
            state->pc = nullptr;                                                                                     \
            state->sp = newSp;                                                                                       \
            state->function = func;                                                                                  \
            thread->SetCurrentSPFrame(newSp);                                                                        \
            LOG(DEBUG, INTERPRETER) << "Entry: Runtime Call.";                                                       \
            JSTaggedValue retValue = reinterpret_cast<EcmaEntrypoint>(                                               \
                const_cast<void *>(methodToCall->GetNativePointer()))(&ecmaRuntimeCallInfo);                         \
            if (UNLIKELY(thread->HasPendingException())) {                                                           \
                INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                                \
            }                                                                                                        \
            LOG(DEBUG, INTERPRETER) << "Exit: Runtime Call.";                                                        \
            thread->SetCurrentSPFrame(sp);                                                                           \
            SET_ACC(retValue);                                                                                       \
            size_t jumpSize = GetJumpSizeAfterCall(pc);                                                              \
            DISPATCH_OFFSET(jumpSize);                                                                               \
        } else {                                                                                                     \
            if (JSFunction::Cast(thisFunc)->IsClassConstructor()) {                                                  \
                {                                                                                                    \
                    [[maybe_unused]] EcmaHandleScope handleScope(thread);                                            \
                    EcmaVM *ecmaVm = thread->GetEcmaVM();                                                            \
                    ObjectFactory *factory = ecmaVm->GetFactory();                                                   \
                    JSHandle<JSObject> error =                                                                       \
                        factory->GetJSError(ErrorType::TYPE_ERROR, "class constructor cannot called without 'new'"); \
                    thread->SetException(error.GetTaggedValue());                                                    \
                }                                                                                                    \
                INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                                \
            }                                                                                                        \
            SAVE_PC();                                                                                               \
            JSTaggedType *newSp = nullptr;                                                                           \
            uint32_t callType = methodToCall->GetCallType();                                                         \
            if (callType == NORMAL_CALL_TYPE) {                                                                      \
                uint32_t numVregs = methodToCall->GetNumVregs();                                                     \
                uint32_t numDeclaredArgs = methodToCall->GetNumArgs();                                               \
                uint32_t copyArgs = std::min(numDeclaredArgs, actualNumArgs);                                        \
                size_t frameSize = FRAME_STATE_SIZE + numVregs + numDeclaredArgs;                                    \
                newSp = sp - frameSize;                                                                              \
                if (thread->DoStackOverflowCheck(newSp) || thread->HasPendingException()) {                          \
                    INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                            \
                }                                                                                                    \
                                                                                                                     \
                uint32_t startIdx = numVregs;                                                                        \
                if (callRange) {                                                                                     \
                    for (size_t i = 1; i <= copyArgs; i++) {                                                         \
                        newSp[startIdx++] = sp[funcReg + i + callThis];                                              \
                    }                                                                                                \
                } else {                                                                                             \
                    switch (copyArgs) {                                                                              \
                        case ActualNumArgsOfCall::CALLARGS3: {                                                       \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARGS3 - 1] = sp[READ_INST_8_4()];              \
                            [[fallthrough]];                                                                         \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARGS2: {                                                       \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARGS2 - 1] = sp[READ_INST_8_3()];              \
                            [[fallthrough]];                                                                         \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARG1: {                                                        \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARG1 - 1] = sp[READ_INST_8_2()];               \
                            break;                                                                                   \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARG0: {                                                        \
                            break;                                                                                   \
                        }                                                                                            \
                        default:                                                                                     \
                            UNREACHABLE();                                                                           \
                    }                                                                                                \
                }                                                                                                    \
                InterpreterFrameCopyArgs(newSp, numVregs, copyArgs, numDeclaredArgs, false);                         \
            } else {                                                                                                 \
                bool haveThis = callType & HAVE_THIS_BIT;                                                            \
                bool haveNewTarget = callType & HAVE_NEWTARGET_BIT;                                                  \
                bool haveExtra = callType & HAVE_EXTRA_BIT;                                                          \
                bool haveFunc = callType & HAVE_FUNC_BIT;                                                            \
                uint32_t numVregs = methodToCall->GetNumVregs();                                                     \
                uint32_t numDeclaredArgs = methodToCall->GetNumArgs();                                               \
                uint32_t copyArgs = haveFunc + haveNewTarget + haveThis;                                             \
                size_t frameSize = FRAME_STATE_SIZE + numVregs;                                                      \
                if (haveExtra) {                                                                                     \
                    copyArgs += actualNumArgs;                                                                       \
                    frameSize += std::max(numDeclaredArgs, copyArgs) + 1;                                            \
                } else {                                                                                             \
                    actualNumArgs = std::min(numDeclaredArgs - copyArgs, actualNumArgs);                             \
                    copyArgs += actualNumArgs;                                                                       \
                    frameSize += numDeclaredArgs;                                                                    \
                }                                                                                                    \
                newSp = sp - frameSize;                                                                              \
                if (thread->DoStackOverflowCheck(newSp) || thread->HasPendingException()) {                          \
                    INTERPRETER_GOTO_EXCEPTION_HANDLER();                                                            \
                }                                                                                                    \
                                                                                                                     \
                uint32_t startIdx = numVregs;                                                                        \
                if (haveFunc) {                                                                                      \
                    newSp[startIdx++] = static_cast<JSTaggedType>(ToUintPtr(thisFunc));                              \
                }                                                                                                    \
                if (haveNewTarget) {                                                                                 \
                    newSp[startIdx++] = JSTaggedValue::VALUE_UNDEFINED;                                              \
                }                                                                                                    \
                if (callRange) {                                                                                     \
                    if (haveThis) {                                                                                  \
                        newSp[startIdx++] = callThis ? sp[funcReg + callThis] : JSTaggedValue::VALUE_UNDEFINED;      \
                    }                                                                                                \
                    for (size_t i = 1; i <= actualNumArgs; i++) {                                                    \
                        newSp[startIdx++] = sp[funcReg + i + callThis];                                              \
                    }                                                                                                \
                } else {                                                                                             \
                    if (haveThis) {                                                                                  \
                        newSp[startIdx++] = JSTaggedValue::VALUE_UNDEFINED;                                          \
                    }                                                                                                \
                    switch (actualNumArgs) {                                                                         \
                        case ActualNumArgsOfCall::CALLARGS3: {                                                       \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARGS3 - 1] = sp[READ_INST_8_4()];              \
                            [[fallthrough]];                                                                         \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARGS2: {                                                       \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARGS2 - 1] = sp[READ_INST_8_3()];              \
                            [[fallthrough]];                                                                         \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARG1: {                                                        \
                            newSp[startIdx + ActualNumArgsOfCall::CALLARG1 - 1] = sp[READ_INST_8_2()];               \
                            break;                                                                                   \
                        }                                                                                            \
                        case ActualNumArgsOfCall::CALLARG0: {                                                        \
                            break;                                                                                   \
                        }                                                                                            \
                        default:                                                                                     \
                            UNREACHABLE();                                                                           \
                    }                                                                                                \
                }                                                                                                    \
                InterpreterFrameCopyArgs(newSp, numVregs, copyArgs, numDeclaredArgs, haveExtra);                     \
            }                                                                                                        \
                                                                                                                     \
            InterpretedFrame *state = GET_FRAME(newSp);                                                              \
            state->base.prev = sp;                                                                                   \
            state->base.frameType = static_cast<uintptr_t>(FrameType::INTERPRETER_FRAME);                            \
            state->pc = pc = JSMethod::Cast(methodToCall)->GetBytecodeArray();                                       \
            state->sp = sp = newSp;                                                                                  \
            state->function = func;                                                                                  \
            state->acc = JSTaggedValue::Hole();                                                                      \
            state->constpool = JSFunction::Cast(thisFunc)->GetConstantPool();                                        \
            constpool = state->constpool;                                                                            \
            state->profileTypeInfo = JSFunction::Cast(thisFunc)->GetProfileTypeInfo();                               \
            profileTypeInfo = state->profileTypeInfo;                                                                \
            JSTaggedValue env = JSFunction::Cast(thisFunc)->GetLexicalEnv();                                         \
            state->env = env;                                                                                        \
            hotnessCounter = static_cast<int32_t>(methodToCall->GetHotnessCounter());                                \
                                                                                                                     \
            thread->SetCurrentSPFrame(newSp);                                                                        \
            LOG(DEBUG, INTERPRETER) << "Entry: Runtime Call " << std::hex << reinterpret_cast<uintptr_t>(sp) << " "  \
                                    << std::hex << reinterpret_cast<uintptr_t>(pc);                                  \
            DISPATCH_OFFSET(0);                                                                                      \
        }                                                                                                            \
    } while (false)

// NOLINTNEXTLINE(readability-function-size)
void InterpreterAssembly::RunInternal(JSThread *thread, ConstantPool *constpool, const uint8_t *pc,
                                           JSTaggedType *sp)
{
    JSTaggedValue acc = JSTaggedValue::Hole();
    InterpretedFrame *state = GET_FRAME(sp);
    auto method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    auto hotnessCounter = static_cast<int32_t>(method->GetHotnessCounter());
    auto profileTypeInfo = JSFunction::Cast(state->function.GetTaggedObject())->GetProfileTypeInfo();

    auto stubAddr = thread->GetFastStubEntry(FAST_STUB_ID(AsmInterpreterEntry));
    AsmDispatchEntryPoint asmEntry = reinterpret_cast<AsmDispatchEntryPoint>(stubAddr);
    asmEntry(thread->GetGlueAddr(), pc, sp, JSTaggedValue(constpool), profileTypeInfo, acc, hotnessCounter);
}

void InterpreterAssembly::HandleMovV4V4(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vdst = READ_INST_4_0();
    uint16_t vsrc = READ_INST_4_1();
    LOG_INST() << "mov v" << vdst << ", v" << vsrc;
    uint64_t value = GET_VREG(vsrc);
    SET_VREG(vdst, value)
    DISPATCH(BytecodeInstruction::Format::V4_V4);
}

void InterpreterAssembly::HandleMovDynV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vdst = READ_INST_8_0();
    uint16_t vsrc = READ_INST_8_1();
    LOG_INST() << "mov.dyn v" << vdst << ", v" << vsrc;
    uint64_t value = GET_VREG(vsrc);
    SET_VREG(vdst, value)
    DISPATCH(BytecodeInstruction::Format::V8_V8);
}

void InterpreterAssembly::HandleMovDynV16V16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vdst = READ_INST_16_0();
    uint16_t vsrc = READ_INST_16_2();
    LOG_INST() << "mov.dyn v" << vdst << ", v" << vsrc;
    uint64_t value = GET_VREG(vsrc);
    SET_VREG(vdst, value)
    DISPATCH(BytecodeInstruction::Format::V16_V16);
}

void InterpreterAssembly::HandleLdaStrId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_0();
    LOG_INST() << "lda.str " << std::hex << stringId;
    SET_ACC(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId));
    DISPATCH(BytecodeInstruction::Format::ID32);
}

void InterpreterAssembly::HandleJmpImm8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int8_t offset = READ_INST_8_0();
    UPDATE_HOTNESS_COUNTER(offset);
    LOG_INST() << "jmp " << std::hex << static_cast<int32_t>(offset);
    DISPATCH_OFFSET(offset);
}

void InterpreterAssembly::HandleJmpImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int16_t offset = READ_INST_16_0();
    UPDATE_HOTNESS_COUNTER(offset);
    LOG_INST() << "jmp " << std::hex << static_cast<int32_t>(offset);
    DISPATCH_OFFSET(offset);
}

void InterpreterAssembly::HandleJmpImm32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int32_t offset = READ_INST_32_0();
    UPDATE_HOTNESS_COUNTER(offset);
    LOG_INST() << "jmp " << std::hex << offset;
    DISPATCH_OFFSET(offset);
}

void InterpreterAssembly::HandleJeqzImm8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int8_t offset = READ_INST_8_0();
    LOG_INST() << "jeqz ->\t"
                << "cond jmpz " << std::hex << static_cast<int32_t>(offset);
    if (GET_ACC() == JSTaggedValue::False() || (GET_ACC().IsInt() && GET_ACC().GetInt() == 0) ||
        (GET_ACC().IsDouble() && GET_ACC().GetDouble() == 0)) {
        UPDATE_HOTNESS_COUNTER(offset);
        DISPATCH_OFFSET(offset);
    } else {
        DISPATCH(BytecodeInstruction::Format::PREF_NONE);
    }
}

void InterpreterAssembly::HandleJeqzImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int16_t offset = READ_INST_16_0();
    LOG_INST() << "jeqz ->\t"
                << "cond jmpz " << std::hex << static_cast<int32_t>(offset);
    if (GET_ACC() == JSTaggedValue::False() || (GET_ACC().IsInt() && GET_ACC().GetInt() == 0) ||
        (GET_ACC().IsDouble() && GET_ACC().GetDouble() == 0)) {
        UPDATE_HOTNESS_COUNTER(offset);
        DISPATCH_OFFSET(offset);
    } else {
        DISPATCH(BytecodeInstruction::Format::IMM16);
    }
}

void InterpreterAssembly::HandleJnezImm8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int8_t offset = READ_INST_8_0();
    LOG_INST() << "jnez ->\t"
                << "cond jmpz " << std::hex << static_cast<int32_t>(offset);
    if (GET_ACC() == JSTaggedValue::True() || (GET_ACC().IsInt() && GET_ACC().GetInt() != 0) ||
        (GET_ACC().IsDouble() && GET_ACC().GetDouble() != 0)) {
        UPDATE_HOTNESS_COUNTER(offset);
        DISPATCH_OFFSET(offset);
    } else {
        DISPATCH(BytecodeInstruction::Format::PREF_NONE);
    }
}

void InterpreterAssembly::HandleJnezImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int16_t offset = READ_INST_16_0();
    LOG_INST() << "jnez ->\t"
                << "cond jmpz " << std::hex << static_cast<int32_t>(offset);
    if (GET_ACC() == JSTaggedValue::True() || (GET_ACC().IsInt() && GET_ACC().GetInt() != 0) ||
        (GET_ACC().IsDouble() && GET_ACC().GetDouble() != 0)) {
        UPDATE_HOTNESS_COUNTER(offset);
        DISPATCH_OFFSET(offset);
    } else {
        DISPATCH(BytecodeInstruction::Format::IMM16);
    }
}

void InterpreterAssembly::HandleLdaDynV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vsrc = READ_INST_8_0();
    LOG_INST() << "lda.dyn v" << vsrc;
    uint64_t value = GET_VREG(vsrc);
    SET_ACC(JSTaggedValue(value))
    DISPATCH(BytecodeInstruction::Format::V8);
}

void InterpreterAssembly::HandleStaDynV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vdst = READ_INST_8_0();
    LOG_INST() << "sta.dyn v" << vdst;
    SET_VREG(vdst, GET_ACC().GetRawData())
    DISPATCH(BytecodeInstruction::Format::V8);
}

void InterpreterAssembly::HandleLdaiDynImm32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    int32_t imm = READ_INST_32_0();
    LOG_INST() << "ldai.dyn " << std::hex << imm;
    SET_ACC(JSTaggedValue(imm))
    DISPATCH(BytecodeInstruction::Format::IMM32);
}

void InterpreterAssembly::HandleFldaiDynImm64(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    auto imm = bit_cast<double>(READ_INST_64_0());
    LOG_INST() << "fldai.dyn " << imm;
    SET_ACC(JSTaggedValue(imm))
    DISPATCH(BytecodeInstruction::Format::IMM64);
}

void InterpreterAssembly::HandleCallArg0DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t funcReg = READ_INST_8_1();
    uint32_t actualNumArgs = ActualNumArgsOfCall::CALLARG0;

    LOG_INST() << "callarg0.dyn "
               << "v" << funcReg;
    bool callRange = false;
    bool callThis = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto, hicpp-avoid-goto)
    HANDLERCALL();
}

void InterpreterAssembly::HandleCallArg1DynPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t funcReg = READ_INST_8_1();
    uint32_t actualNumArgs = ActualNumArgsOfCall::CALLARG1;

    LOG_INST() << "callarg1.dyn "
               << "v" << funcReg << ", v" << READ_INST_8_2();
    bool callRange = false;
    bool callThis = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto, hicpp-avoid-goto)
    HANDLERCALL();
}

void InterpreterAssembly::HandleCallArgs2DynPrefV8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t funcReg = READ_INST_8_1();
    uint32_t actualNumArgs = ActualNumArgsOfCall::CALLARGS2;

    LOG_INST() << "callargs2.dyn "
               << "v" << funcReg << ", v" << READ_INST_8_2() << ", v" << READ_INST_8_3();
    bool callRange = false;
    bool callThis = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto, hicpp-avoid-goto)
    HANDLERCALL();
}

void InterpreterAssembly::HandleCallArgs3DynPrefV8V8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t funcReg = READ_INST_8_1();
    uint32_t actualNumArgs = ActualNumArgsOfCall::CALLARGS3;

    LOG_INST() << "callargs3.dyn "
               << "v" << funcReg << ", v" << READ_INST_8_2() << ", v" << READ_INST_8_3()
               << ", v" << READ_INST_8_4();
    bool callRange = false;
    bool callThis = false;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto, hicpp-avoid-goto)
    HANDLERCALL();
}

void InterpreterAssembly::HandleCallIThisRangeDynPrefImm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t actualNumArgs = READ_INST_16_1() - 1;  // 1: exclude this
    uint32_t funcReg = READ_INST_8_3();

    LOG_INST() << "calli.dyn.this.range " << actualNumArgs << ", v" << funcReg;
    bool callRange = true;
    bool callThis = true;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-goto, hicpp-avoid-goto)
    HANDLERCALL();
}

void InterpreterAssembly::HandleCallSpreadDynPrefV8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    uint16_t v2 = READ_INST_8_3();
    LOG_INST() << "intrinsics::callspreaddyn"
               << " v" << v0 << " v" << v1 << " v" << v2;
    JSTaggedValue func = GET_VREG_VALUE(v0);
    JSTaggedValue obj = GET_VREG_VALUE(v1);
    JSTaggedValue array = GET_VREG_VALUE(v2);

    JSTaggedValue res = SlowRuntimeStub::CallSpreadDyn(thread, func, obj, array);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);

    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8_V8);
}

void InterpreterAssembly::HandleCallIRangeDynPrefImm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t actualNumArgs = READ_INST_16_1();
    uint32_t funcReg = READ_INST_8_3();
    bool callRange = true;
    bool callThis = false;
    LOG_INST() << "calli.rangedyn " << actualNumArgs << ", v" << funcReg;
    HANDLERCALL();
}

void InterpreterAssembly::HandleReturnDyn(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "returnla ";
    InterpretedFrame *state = GET_FRAME(sp);
    LOG(DEBUG, INTERPRETER) << "Exit: Runtime Call " << std::hex << reinterpret_cast<uintptr_t>(state->sp) << " "
                            << std::hex << reinterpret_cast<uintptr_t>(state->pc);
    JSMethod *method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    [[maybe_unused]] auto fistPC = method->GetInstructions();
    UPDATE_HOTNESS_COUNTER(-(pc - fistPC));
    method->SetHotnessCounter(static_cast<uint32_t>(hotnessCounter));
    sp = state->base.prev;
    ASSERT(sp != nullptr);
    InterpretedFrame *prevState = GET_FRAME(sp);
    pc = prevState->pc;

    // break frame
    if (pc == nullptr) {
        thread->SetCurrentSPFrame(sp);
        state->acc = acc;
        return;
    }
    thread->SetCurrentSPFrame(sp);

    constpool = prevState->constpool;
    profileTypeInfo = prevState->profileTypeInfo;
    method = ECMAObject::Cast(prevState->function.GetTaggedObject())->GetCallTarget();
    hotnessCounter = static_cast<int32_t>(method->GetHotnessCounter());

    size_t jumpSize = GetJumpSizeAfterCall(pc);
    DISPATCH_OFFSET(jumpSize);
}

void InterpreterAssembly::HandleReturnUndefinedPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "return.undefined";
    InterpretedFrame *state = GET_FRAME(sp);
    LOG(DEBUG, INTERPRETER) << "Exit: Runtime Call " << std::hex << reinterpret_cast<uintptr_t>(sp) << " "
                            << std::hex << reinterpret_cast<uintptr_t>(state->pc);
    JSMethod *method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    [[maybe_unused]] auto fistPC = method->GetInstructions();
    UPDATE_HOTNESS_COUNTER(-(pc - fistPC));
    method->SetHotnessCounter(static_cast<uint32_t>(hotnessCounter));
    sp = state->base.prev;
    ASSERT(sp != nullptr);
    InterpretedFrame *prevState = GET_FRAME(sp);
    pc = prevState->pc;

    // break frame
    if (pc == nullptr) {
        thread->SetCurrentSPFrame(sp);
        state->acc = JSTaggedValue::Undefined();
        return;
    }
    thread->SetCurrentSPFrame(sp);

    constpool = prevState->constpool;
    profileTypeInfo = prevState->profileTypeInfo;
    method = ECMAObject::Cast(prevState->function.GetTaggedObject())->GetCallTarget();
    hotnessCounter = static_cast<int32_t>(method->GetHotnessCounter());

    acc = JSTaggedValue::Undefined();
    size_t jumpSize = GetJumpSizeAfterCall(pc);
    DISPATCH_OFFSET(jumpSize);
}

void InterpreterAssembly::HandleLdNanPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldnan";
    SET_ACC(JSTaggedValue(base::NAN_VALUE));
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdInfinityPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldinfinity";
    SET_ACC(JSTaggedValue(base::POSITIVE_INFINITY));
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdGlobalThisPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldglobalthis";
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    JSTaggedValue globalObj = globalEnv->GetGlobalObject();
    SET_ACC(globalObj)
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdUndefinedPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldundefined";
    SET_ACC(JSTaggedValue::Undefined())
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdNullPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldnull";
    SET_ACC(JSTaggedValue::Null())
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdSymbolPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldsymbol";
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    SET_ACC(globalEnv->GetSymbolFunction().GetTaggedValue());
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdGlobalPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldglobal";
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    JSTaggedValue globalObj = globalEnv->GetGlobalObject();
    SET_ACC(globalObj)
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdTruePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldtrue";
    SET_ACC(JSTaggedValue::True())
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdFalsePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldfalse";
    SET_ACC(JSTaggedValue::False())
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleLdLexEnvDynPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldlexenvDyn ";
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue currentLexenv = state->env;
    SET_ACC(currentLexenv);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleGetUnmappedArgsPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::getunmappedargs";

    uint32_t startIdx = 0;
    uint32_t actualNumArgs = GetNumArgs(sp, 0, startIdx);

    JSTaggedValue res = SlowRuntimeStub::GetUnmapedArgs(thread, sp, actualNumArgs, startIdx);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleAsyncFunctionEnterPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::asyncfunctionenter";
    JSTaggedValue res = SlowRuntimeStub::AsyncFunctionEnter(thread);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleToNumberPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::tonumber"
                << " v" << v0;
    JSTaggedValue value = GET_VREG_VALUE(v0);
    if (value.IsNumber()) {
        // fast path
        SET_ACC(value);
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::ToNumber(thread, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleNegDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::negdyn"
                << " v" << v0;
    JSTaggedValue value = GET_VREG_VALUE(v0);
    // fast path
    if (value.IsInt()) {
        if (value.GetInt() == 0) {
            SET_ACC(JSTaggedValue(-0.0));
        } else {
            SET_ACC(JSTaggedValue(-value.GetInt()));
        }
    } else if (value.IsDouble()) {
        SET_ACC(JSTaggedValue(-value.GetDouble()));
    } else {  // slow path
        JSTaggedValue res = SlowRuntimeStub::NegDyn(thread, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleNotDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::notdyn"
                << " v" << v0;
    JSTaggedValue value = GET_VREG_VALUE(v0);
    int32_t number;
    // number, fast path
    if (value.IsInt()) {
        number = static_cast<int32_t>(value.GetInt());
        SET_ACC(JSTaggedValue(~number));  // NOLINT(hicpp-signed-bitwise)
    } else if (value.IsDouble()) {
        number = base::NumberHelper::DoubleToInt(value.GetDouble(), base::INT32_BITS);
        SET_ACC(JSTaggedValue(~number));  // NOLINT(hicpp-signed-bitwise)
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::NotDyn(thread, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleIncDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::incdyn"
                << " v" << v0;

    JSTaggedValue value = GET_VREG_VALUE(v0);
    // number fast path
    if (value.IsInt()) {
        int32_t a0 = value.GetInt();
        if (UNLIKELY(a0 == INT32_MAX)) {
            auto ret = static_cast<double>(a0) + 1.0;
            SET_ACC(JSTaggedValue(ret))
        } else {
            SET_ACC(JSTaggedValue(a0 + 1))
        }
    } else if (value.IsDouble()) {
        SET_ACC(JSTaggedValue(value.GetDouble() + 1.0))
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::IncDyn(thread, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleDecDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::decdyn"
                << " v" << v0;

    JSTaggedValue value = GET_VREG_VALUE(v0);
    // number, fast path
    if (value.IsInt()) {
        int32_t a0 = value.GetInt();
        if (UNLIKELY(a0 == INT32_MIN)) {
            auto ret = static_cast<double>(a0) - 1.0;
            SET_ACC(JSTaggedValue(ret))
        } else {
            SET_ACC(JSTaggedValue(a0 - 1))
        }
    } else if (value.IsDouble()) {
        SET_ACC(JSTaggedValue(value.GetDouble() - 1.0))
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::DecDyn(thread, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleThrowDynPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::throwdyn";
    SlowRuntimeStub::ThrowDyn(thread, GET_ACC());
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleTypeOfDynPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::typeofdyn";
    JSTaggedValue res = FastRuntimeStub::FastTypeOf(thread, GET_ACC());
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleGetPropIteratorPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::getpropiterator";
    JSTaggedValue res = SlowRuntimeStub::GetPropIterator(thread, GET_ACC());
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleResumeGeneratorPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::resumegenerator";
    uint16_t vs = READ_INST_8_1();
    JSGeneratorObject *obj = JSGeneratorObject::Cast(GET_VREG_VALUE(vs).GetTaggedObject());
    SET_ACC(obj->GetResumeResult());
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleGetResumeModePrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::getresumemode";
    uint16_t vs = READ_INST_8_1();
    JSGeneratorObject *obj = JSGeneratorObject::Cast(GET_VREG_VALUE(vs).GetTaggedObject());
    SET_ACC(obj->GetResumeMode());
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleGetIteratorPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::getiterator";
    JSTaggedValue obj = GET_ACC();

    // fast path: Generator obj is already store in acc
    if (!obj.IsGeneratorObject()) {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::GetIterator(thread, obj);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleThrowConstAssignmentPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "throwconstassignment"
                << " v" << v0;
    SlowRuntimeStub::ThrowConstAssignment(thread, GET_VREG_VALUE(v0));
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleThrowThrowNotExistsPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "throwthrownotexists";

    SlowRuntimeStub::ThrowThrowNotExists(thread);
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleThrowPatternNonCoerciblePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "throwpatternnoncoercible";

    SlowRuntimeStub::ThrowPatternNonCoercible(thread);
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleThrowIfNotObjectPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "throwifnotobject";
    uint16_t v0 = READ_INST_8_1();

    JSTaggedValue value = GET_VREG_VALUE(v0);
    // fast path
    if (value.IsECMAObject()) {
        DISPATCH(BytecodeInstruction::Format::PREF_V8);
    }

    // slow path
    SlowRuntimeStub::ThrowIfNotObject(thread);
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleIternextPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::iternext"
               << " v" << v0;
    JSTaggedValue iter = GET_VREG_VALUE(v0);
    JSTaggedValue res = SlowRuntimeStub::IterNext(thread, iter);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleCloseIteratorPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::closeiterator"
               << " v" << v0;
    JSTaggedValue iter = GET_VREG_VALUE(v0);
    JSTaggedValue res = SlowRuntimeStub::CloseIterator(thread, iter);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleAdd2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::add2dyn"
               << " v" << v0;
    int32_t a0;
    int32_t a1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // number, fast path
    if (left.IsInt() && right.IsInt()) {
        a0 = left.GetInt();
        a1 = right.GetInt();
        if ((a0 > 0 && a1 > INT32_MAX - a0) || (a0 < 0 && a1 < INT32_MIN - a0)) {
            auto ret = static_cast<double>(a0) + static_cast<double>(a1);
            SET_ACC(JSTaggedValue(ret))
        } else {
            SET_ACC(JSTaggedValue(a0 + a1))
        }
    } else if (left.IsNumber() && right.IsNumber()) {
        double a0Double = left.IsInt() ? left.GetInt() : left.GetDouble();
        double a1Double = right.IsInt() ? right.GetInt() : right.GetDouble();
        double ret = a0Double + a1Double;
        SET_ACC(JSTaggedValue(ret))
    } else {
        // one or both are not number, slow path
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSTaggedValue res = SlowRuntimeStub::Add2Dyn(thread, ecmaVm, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleSub2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::sub2dyn"
               << " v" << v0;
    int32_t a0;
    int32_t a1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    if (left.IsInt() && right.IsInt()) {
        a0 = left.GetInt();
        a1 = -right.GetInt();
        if ((a0 > 0 && a1 > INT32_MAX - a0) || (a0 < 0 && a1 < INT32_MIN - a0)) {
            auto ret = static_cast<double>(a0) + static_cast<double>(a1);
            SET_ACC(JSTaggedValue(ret))
        } else {
            SET_ACC(JSTaggedValue(a0 + a1))
        }
    } else if (left.IsNumber() && right.IsNumber()) {
        double a0Double = left.IsInt() ? left.GetInt() : left.GetDouble();
        double a1Double = right.IsInt() ? right.GetInt() : right.GetDouble();
        double ret = a0Double - a1Double;
        SET_ACC(JSTaggedValue(ret))
    } else {
        // one or both are not number, slow path
        JSTaggedValue res = SlowRuntimeStub::Sub2Dyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}
void InterpreterAssembly::HandleMul2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::mul2dyn"
                << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = acc;
    JSTaggedValue value = FastRuntimeStub::FastMul(left, right);
    if (!value.IsHole()) {
        SET_ACC(value);
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::Mul2Dyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleDiv2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::div2dyn"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = acc;
    // fast path
    JSTaggedValue res = FastRuntimeStub::FastDiv(left, right);
    if (!res.IsHole()) {
        SET_ACC(res);
    } else {
        // slow path
        JSTaggedValue slowRes = SlowRuntimeStub::Div2Dyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(slowRes);
        SET_ACC(slowRes);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleMod2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vs = READ_INST_8_1();
    LOG_INST() << "intrinsics::mod2dyn"
                << " v" << vs;
    JSTaggedValue left = GET_VREG_VALUE(vs);
    JSTaggedValue right = GET_ACC();

    JSTaggedValue res = FastRuntimeStub::FastMod(left, right);
    if (!res.IsHole()) {
        SET_ACC(res);
    } else {
        // slow path
        JSTaggedValue slowRes = SlowRuntimeStub::Mod2Dyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(slowRes);
        SET_ACC(slowRes);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::eqdyn"
                << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = acc;
    JSTaggedValue res = FastRuntimeStub::FastEqual(left, right);
    if (!res.IsHole()) {
        SET_ACC(res);
    } else {
        // slow path
        res = SlowRuntimeStub::EqDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }

    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleNotEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::noteqdyn"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = acc;

    JSTaggedValue res = FastRuntimeStub::FastEqual(left, right);
    if (!res.IsHole()) {
        res = res.IsTrue() ? JSTaggedValue::False() : JSTaggedValue::True();
        SET_ACC(res);
    } else {
        // slow path
        res = SlowRuntimeStub::NotEqDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleLessDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::lessdyn"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    if (left.IsNumber() && right.IsNumber()) {
        // fast path
        double valueA = left.IsInt() ? static_cast<double>(left.GetInt()) : left.GetDouble();
        double valueB = right.IsInt() ? static_cast<double>(right.GetInt()) : right.GetDouble();
        bool ret = JSTaggedValue::StrictNumberCompare(valueA, valueB) == ComparisonResult::LESS;
        SET_ACC(ret ? JSTaggedValue::True() : JSTaggedValue::False())
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::LessDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleLessEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vs = READ_INST_8_1();
    LOG_INST() << "intrinsics::lesseqdyn "
               << " v" << vs;
    JSTaggedValue left = GET_VREG_VALUE(vs);
    JSTaggedValue right = GET_ACC();
    if (left.IsNumber() && right.IsNumber()) {
        // fast path
        double valueA = left.IsInt() ? static_cast<double>(left.GetInt()) : left.GetDouble();
        double valueB = right.IsInt() ? static_cast<double>(right.GetInt()) : right.GetDouble();
        bool ret = JSTaggedValue::StrictNumberCompare(valueA, valueB) <= ComparisonResult::EQUAL;
        SET_ACC(ret ? JSTaggedValue::True() : JSTaggedValue::False())
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::LessEqDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleGreaterDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::greaterdyn"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = acc;
    if (left.IsNumber() && right.IsNumber()) {
        // fast path
        double valueA = left.IsInt() ? static_cast<double>(left.GetInt()) : left.GetDouble();
        double valueB = right.IsInt() ? static_cast<double>(right.GetInt()) : right.GetDouble();
        bool ret = JSTaggedValue::StrictNumberCompare(valueA, valueB) == ComparisonResult::GREAT;
        SET_ACC(ret ? JSTaggedValue::True() : JSTaggedValue::False())
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::GreaterDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleGreaterEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t vs = READ_INST_8_1();
    LOG_INST() << "intrinsics::greateqdyn "
               << " v" << vs;
    JSTaggedValue left = GET_VREG_VALUE(vs);
    JSTaggedValue right = GET_ACC();
    if (left.IsNumber() && right.IsNumber()) {
        // fast path
        double valueA = left.IsInt() ? static_cast<double>(left.GetInt()) : left.GetDouble();
        double valueB = right.IsInt() ? static_cast<double>(right.GetInt()) : right.GetDouble();
        ComparisonResult comparison = JSTaggedValue::StrictNumberCompare(valueA, valueB);
        bool ret = (comparison == ComparisonResult::GREAT) || (comparison == ComparisonResult::EQUAL);
        SET_ACC(ret ? JSTaggedValue::True() : JSTaggedValue::False())
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::GreaterEqDyn(thread, left, right);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleShl2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::shl2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // both number, fast path
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithUint32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }

    uint32_t shift =
        static_cast<uint32_t>(opNumber1) & 0x1f;  // NOLINT(hicpp-signed-bitwise, readability-magic-numbers)
    using unsigned_type = std::make_unsigned_t<int32_t>;
    auto ret =
        static_cast<int32_t>(static_cast<unsigned_type>(opNumber0) << shift);  // NOLINT(hicpp-signed-bitwise)
    SET_ACC(JSTaggedValue(ret))
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleShr2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::shr2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // both number, fast path
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithUint32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }

    uint32_t shift =
        static_cast<uint32_t>(opNumber1) & 0x1f;          // NOLINT(hicpp-signed-bitwise, readability-magic-numbers)
    auto ret = static_cast<int32_t>(opNumber0 >> shift);  // NOLINT(hicpp-signed-bitwise)
    SET_ACC(JSTaggedValue(ret))
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleAshr2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::ashr2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithUint32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithUint32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }

    uint32_t shift =
        static_cast<uint32_t>(opNumber1) & 0x1f;  // NOLINT(hicpp-signed-bitwise, readability-magic-numbers)
    using unsigned_type = std::make_unsigned_t<uint32_t>;
    auto ret =
        static_cast<uint32_t>(static_cast<unsigned_type>(opNumber0) >> shift);  // NOLINT(hicpp-signed-bitwise)
    SET_ACC(JSTaggedValue(ret))

    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleAnd2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::and2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // both number, fast path
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }
    // NOLINT(hicpp-signed-bitwise)
    auto ret = static_cast<uint32_t>(opNumber0) & static_cast<uint32_t>(opNumber1);
    SET_ACC(JSTaggedValue(static_cast<uint32_t>(ret)))
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleOr2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::or2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // both number, fast path
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }
    // NOLINT(hicpp-signed-bitwise)
    auto ret = static_cast<uint32_t>(opNumber0) | static_cast<uint32_t>(opNumber1);
    SET_ACC(JSTaggedValue(static_cast<uint32_t>(ret)))
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleXOr2DynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();

    LOG_INST() << "intrinsics::xor2dyn"
               << " v" << v0;
    int32_t opNumber0;
    int32_t opNumber1;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    // both number, fast path
    if (left.IsInt() && right.IsInt()) {
        opNumber0 = left.GetInt();
        opNumber1 = right.GetInt();
    } else if (left.IsNumber() && right.IsNumber()) {
        opNumber0 =
            left.IsInt() ? left.GetInt() : base::NumberHelper::DoubleToInt(left.GetDouble(), base::INT32_BITS);
        opNumber1 =
            right.IsInt() ? right.GetInt() : base::NumberHelper::DoubleToInt(right.GetDouble(), base::INT32_BITS);
    } else {
        // slow path
        SAVE_ACC();
        JSTaggedValue taggedNumber0 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, left);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber0);
        RESTORE_ACC();
        right = GET_ACC();  // Maybe moved by GC
        JSTaggedValue taggedNumber1 = SlowRuntimeStub::ToJSTaggedValueWithInt32(thread, right);
        INTERPRETER_RETURN_IF_ABRUPT(taggedNumber1);
        opNumber0 = taggedNumber0.GetInt();
        opNumber1 = taggedNumber1.GetInt();
    }
    // NOLINT(hicpp-signed-bitwise)
    auto ret = static_cast<uint32_t>(opNumber0) ^ static_cast<uint32_t>(opNumber1);
    SET_ACC(JSTaggedValue(static_cast<uint32_t>(ret)))
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleDelObjPropPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::delobjprop"
               << " v0" << v0 << " v1" << v1;

    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue prop = GET_VREG_VALUE(v1);
    JSTaggedValue res = SlowRuntimeStub::DelObjProp(thread, obj, prop);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);

    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleDefineFuncDynPrefId16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t length = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    LOG_INST() << "intrinsics::definefuncDyn length: " << length
               << " v" << v0;
    JSFunction *result = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(result != nullptr);
    if (result->IsResolved()) {
        auto res = SlowRuntimeStub::DefinefuncDyn(thread, result);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        result = JSFunction::Cast(res.GetTaggedObject());
        result->SetConstantPool(thread, JSTaggedValue(constpool));
    } else {
        result->SetResolved(thread);
    }

    result->SetPropertyInlinedProps(thread, JSFunction::LENGTH_INLINE_PROPERTY_INDEX, JSTaggedValue(length));
    JSTaggedValue envHandle = GET_VREG_VALUE(v0);
    result->SetLexicalEnv(thread, envHandle);
    SET_ACC(JSTaggedValue(result))

    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_V8);
}

void InterpreterAssembly::HandleDefineNCFuncDynPrefId16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t length = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    JSTaggedValue homeObject = GET_ACC();
    LOG_INST() << "intrinsics::definencfuncDyn length: " << length
               << " v" << v0;
    JSFunction *result = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(result != nullptr);
    if (result->IsResolved()) {
        SAVE_ACC();
        auto res = SlowRuntimeStub::DefineNCFuncDyn(thread, result);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        result = JSFunction::Cast(res.GetTaggedObject());
        result->SetConstantPool(thread, JSTaggedValue(constpool));
        RESTORE_ACC();
        homeObject = GET_ACC();
    } else {
        result->SetResolved(thread);
    }

    result->SetPropertyInlinedProps(thread, JSFunction::LENGTH_INLINE_PROPERTY_INDEX, JSTaggedValue(length));
    JSTaggedValue env = GET_VREG_VALUE(v0);
    result->SetLexicalEnv(thread, env);
    result->SetHomeObject(thread, homeObject);
    SET_ACC(JSTaggedValue(result));

    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_V8);
}

void InterpreterAssembly::HandleDefineMethodPrefId16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t length = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    JSTaggedValue homeObject = GET_ACC();
    LOG_INST() << "intrinsics::definemethod length: " << length
               << " v" << v0;
    JSFunction *result = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(result != nullptr);
    if (result->IsResolved()) {
        auto res = SlowRuntimeStub::DefineMethod(thread, result, homeObject);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        result = JSFunction::Cast(res.GetTaggedObject());
        result->SetConstantPool(thread, JSTaggedValue(constpool));
    } else {
        result->SetHomeObject(thread, homeObject);
        result->SetResolved(thread);
    }

    result->SetPropertyInlinedProps(thread, JSFunction::LENGTH_INLINE_PROPERTY_INDEX, JSTaggedValue(length));
    JSTaggedValue taggedCurEnv = GET_VREG_VALUE(v0);
    result->SetLexicalEnv(thread, taggedCurEnv);
    SET_ACC(JSTaggedValue(result));

    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_V8);
}

void InterpreterAssembly::HandleNewObjDynRangePrefImm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t numArgs = READ_INST_16_1();
    uint16_t firstArgRegIdx = READ_INST_8_3();
    LOG_INST() << "intrinsics::newobjDynrange " << numArgs << " v" << firstArgRegIdx;

    constexpr uint16_t firstArgOffset = 2;
    JSTaggedValue func = GET_VREG_VALUE(firstArgRegIdx);
    JSTaggedValue newTarget = GET_VREG_VALUE(firstArgRegIdx + 1);
    // Exclude func and newTarget
    uint16_t firstArgIdx = firstArgRegIdx + firstArgOffset;
    uint16_t length = numArgs - firstArgOffset;

    JSTaggedValue res = SlowRuntimeStub::NewObjDynRange(thread, func, newTarget, firstArgIdx, length);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16_V8);
}

void InterpreterAssembly::HandleExpDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::expdyn"
               << " v" << v0;
    JSTaggedValue base = GET_VREG_VALUE(v0);
    JSTaggedValue exponent = GET_ACC();
    if (base.IsNumber() && exponent.IsNumber()) {
        // fast path
        double doubleBase = base.IsInt() ? base.GetInt() : base.GetDouble();
        double doubleExponent = exponent.IsInt() ? exponent.GetInt() : exponent.GetDouble();
        if (std::abs(doubleBase) == 1 && std::isinf(doubleExponent)) {
            SET_ACC(JSTaggedValue(base::NAN_VALUE));
        }
        if ((doubleBase == 0 &&
            ((bit_cast<uint64_t>(doubleBase)) & base::DOUBLE_SIGN_MASK) == base::DOUBLE_SIGN_MASK) &&
            std::isfinite(doubleExponent) && base::NumberHelper::TruncateDouble(doubleExponent) == doubleExponent &&
            base::NumberHelper::TruncateDouble(doubleExponent / 2) + base::HALF ==  // 2 : half
            (doubleExponent / 2)) {  // 2 : half
            if (doubleExponent > 0) {
                SET_ACC(JSTaggedValue(-0.0));
            }
            if (doubleExponent < 0) {
                SET_ACC(JSTaggedValue(-base::POSITIVE_INFINITY));
            }
        }
        SET_ACC(JSTaggedValue(std::pow(doubleBase, doubleExponent)));
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::ExpDyn(thread, base, exponent);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleIsInDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::isindyn"
               << " v" << v0;
    JSTaggedValue prop = GET_VREG_VALUE(v0);
    JSTaggedValue obj = GET_ACC();
    JSTaggedValue res = SlowRuntimeStub::IsInDyn(thread, prop, obj);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleInstanceOfDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::instanceofdyn"
               << " v" << v0;
    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue target = GET_ACC();
    JSTaggedValue res = SlowRuntimeStub::InstanceofDyn(thread, obj, target);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleStrictNotEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::strictnoteq"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    bool res = FastRuntimeStub::FastStrictEqual(left, right);
    SET_ACC(JSTaggedValue(!res));
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleStrictEqDynPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::stricteq"
               << " v" << v0;
    JSTaggedValue left = GET_VREG_VALUE(v0);
    JSTaggedValue right = GET_ACC();
    bool res = FastRuntimeStub::FastStrictEqual(left, right);
    SET_ACC(JSTaggedValue(res));
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleLdLexVarDynPrefImm16Imm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_16_1();
    uint16_t slot = READ_INST_16_3();

    LOG_INST() << "intrinsics::ldlexvardyn"
               << " level:" << level << " slot:" << slot;
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue currentLexenv = state->env;
    JSTaggedValue env(currentLexenv);
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    SET_ACC(LexicalEnv::Cast(env.GetTaggedObject())->GetProperties(slot));
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16_IMM16);
}

void InterpreterAssembly::HandleLdLexVarDynPrefImm8Imm8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_8_1();
    uint16_t slot = READ_INST_8_2();

    LOG_INST() << "intrinsics::ldlexvardyn"
               << " level:" << level << " slot:" << slot;
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue currentLexenv = state->env;
    JSTaggedValue env(currentLexenv);
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    SET_ACC(LexicalEnv::Cast(env.GetTaggedObject())->GetProperties(slot));
    DISPATCH(BytecodeInstruction::Format::PREF_IMM8_IMM8);
}

void InterpreterAssembly::HandleLdLexVarDynPrefImm4Imm4(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_4_2();
    uint16_t slot = READ_INST_4_3();

    LOG_INST() << "intrinsics::ldlexvardyn"
               << " level:" << level << " slot:" << slot;
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue currentLexenv = state->env;
    JSTaggedValue env(currentLexenv);
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    SET_ACC(LexicalEnv::Cast(env.GetTaggedObject())->GetProperties(slot));
    DISPATCH(BytecodeInstruction::Format::PREF_IMM4_IMM4);
}

void InterpreterAssembly::HandleStLexVarDynPrefImm16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_16_1();
    uint16_t slot = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    LOG_INST() << "intrinsics::stlexvardyn"
               << " level:" << level << " slot:" << slot << " v" << v0;

    JSTaggedValue value = GET_VREG_VALUE(v0);
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue env = state->env;
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    LexicalEnv::Cast(env.GetTaggedObject())->SetProperties(thread, slot, value);

    DISPATCH(BytecodeInstruction::Format::PREF_IMM16_IMM16_V8);
}

void InterpreterAssembly::HandleStLexVarDynPrefImm8Imm8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_8_1();
    uint16_t slot = READ_INST_8_2();
    uint16_t v0 = READ_INST_8_3();
    LOG_INST() << "intrinsics::stlexvardyn"
               << " level:" << level << " slot:" << slot << " v" << v0;

    JSTaggedValue value = GET_VREG_VALUE(v0);
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue env = state->env;
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    LexicalEnv::Cast(env.GetTaggedObject())->SetProperties(thread, slot, value);

    DISPATCH(BytecodeInstruction::Format::PREF_IMM8_IMM8_V8);
}

void InterpreterAssembly::HandleStLexVarDynPrefImm4Imm4V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t level = READ_INST_4_2();
    uint16_t slot = READ_INST_4_3();
    uint16_t v0 = READ_INST_8_2();
    LOG_INST() << "intrinsics::stlexvardyn"
               << " level:" << level << " slot:" << slot << " v" << v0;

    JSTaggedValue value = GET_VREG_VALUE(v0);
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue env = state->env;
    for (int i = 0; i < level; i++) {
        JSTaggedValue taggedParentEnv = LexicalEnv::Cast(env.GetTaggedObject())->GetParentEnv();
        ASSERT(!taggedParentEnv.IsUndefined());
        env = taggedParentEnv;
    }
    LexicalEnv::Cast(env.GetTaggedObject())->SetProperties(thread, slot, value);

    DISPATCH(BytecodeInstruction::Format::PREF_IMM4_IMM4_V8);
}

void InterpreterAssembly::HandleNewLexEnvDynPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t numVars = READ_INST_16_1();
    LOG_INST() << "intrinsics::newlexenvdyn"
               << " imm " << numVars;
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = FastRuntimeStub::NewLexicalEnvDyn(thread, factory, numVars);
    if (res.IsHole()) {
        res = SlowRuntimeStub::NewLexicalEnvDyn(thread, numVars);
        INTERPRETER_RETURN_IF_ABRUPT(res);
    }
    SET_ACC(res);
    GET_FRAME(sp)->env = res;
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandlePopLexEnvDynPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    InterpretedFrame *state = GET_FRAME(sp);
    JSTaggedValue currentLexenv = state->env;
    JSTaggedValue parentLexenv = LexicalEnv::Cast(currentLexenv.GetTaggedObject())->GetParentEnv();
    GET_FRAME(sp)->env = parentLexenv;
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleCreateIterResultObjPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::createiterresultobj"
               << " v" << v0 << " v" << v1;
    JSTaggedValue value = GET_VREG_VALUE(v0);
    JSTaggedValue flag = GET_VREG_VALUE(v1);
    JSTaggedValue res = SlowRuntimeStub::CreateIterResultObj(thread, value, flag);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleSuspendGeneratorPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::suspendgenerator"
                << " v" << v0 << " v" << v1;
    JSTaggedValue genObj = GET_VREG_VALUE(v0);
    JSTaggedValue value = GET_VREG_VALUE(v1);
    // suspend will record bytecode offset
    SAVE_PC();
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::SuspendGenerator(thread, genObj, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);

    InterpretedFrame *state = GET_FRAME(sp);
    JSMethod *method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    [[maybe_unused]] auto fistPC = method->GetInstructions();
    UPDATE_HOTNESS_COUNTER(-(pc - fistPC));
    LOG(DEBUG, INTERPRETER) << "Exit: SuspendGenerator " << std::hex << reinterpret_cast<uintptr_t>(sp) << " "
                            << std::hex << reinterpret_cast<uintptr_t>(state->pc);
    sp = state->base.prev;
    ASSERT(sp != nullptr);
    InterpretedFrame *prevState = GET_FRAME(sp);
    pc = prevState->pc;

    // break frame
    if (pc == nullptr) {
        thread->SetCurrentSPFrame(sp);
        state->acc = acc;
        return;
    }
    thread->SetCurrentSPFrame(sp);
    constpool = prevState->constpool;

    size_t jumpSize = GetJumpSizeAfterCall(pc);
    DISPATCH_OFFSET(jumpSize);
}

void InterpreterAssembly::HandleAsyncFunctionAwaitUncaughtPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::asyncfunctionawaituncaught"
               << " v" << v0 << " v" << v1;
    JSTaggedValue asyncFuncObj = GET_VREG_VALUE(v0);
    JSTaggedValue value = GET_VREG_VALUE(v1);
    JSTaggedValue res = SlowRuntimeStub::AsyncFunctionAwaitUncaught(thread, asyncFuncObj, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleAsyncFunctionResolvePrefV8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    [[maybe_unused]] uint16_t v1 = READ_INST_8_2();
    uint16_t v2 = READ_INST_8_3();
    LOG_INST() << "intrinsics::asyncfunctionresolve"
               << " v" << v0 << " v" << v1 << " v" << v2;

    JSTaggedValue asyncFuncObj = GET_VREG_VALUE(v0);
    JSTaggedValue value = GET_VREG_VALUE(v2);
    JSTaggedValue res = SlowRuntimeStub::AsyncFunctionResolveOrReject(thread, asyncFuncObj, value, true);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8_V8);
}

void InterpreterAssembly::HandleAsyncFunctionRejectPrefV8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    [[maybe_unused]] uint16_t v1 = READ_INST_8_2();
    uint16_t v2 = READ_INST_8_3();
    LOG_INST() << "intrinsics::asyncfunctionreject"
               << " v" << v0 << " v" << v1 << " v" << v2;

    JSTaggedValue asyncFuncObj = GET_VREG_VALUE(v0);
    JSTaggedValue value = GET_VREG_VALUE(v2);
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::AsyncFunctionResolveOrReject(thread, asyncFuncObj, value, false);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8_V8);
}

void InterpreterAssembly::HandleNewObjSpreadDynPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsic::newobjspearddyn"
               << " v" << v0 << " v" << v1;
    JSTaggedValue func = GET_VREG_VALUE(v0);
    JSTaggedValue newTarget = GET_VREG_VALUE(v1);
    JSTaggedValue array = GET_ACC();
    JSTaggedValue res = SlowRuntimeStub::NewObjSpreadDyn(thread, func, newTarget, array);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleThrowUndefinedIfHolePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsic::throwundefinedifhole"
               << " v" << v0 << " v" << v1;
    JSTaggedValue hole = GET_VREG_VALUE(v0);
    if (!hole.IsHole()) {
        DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
    }
    JSTaggedValue obj = GET_VREG_VALUE(v1);
    ASSERT(obj.IsString());
    SlowRuntimeStub::ThrowUndefinedIfHole(thread, obj);
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleStOwnByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    uint32_t v0 = READ_INST_8_5();
    LOG_INST() << "intrinsics::stownbyname "
               << "v" << v0 << " stringId:" << stringId;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    if (receiver.IsJSObject() && !receiver.IsClassConstructor() && !receiver.IsClassPrototype()) {
        JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
        JSTaggedValue value = GET_ACC();
        // fast path
        SAVE_ACC();
        JSTaggedValue res = FastRuntimeStub::SetPropertyByName<true>(thread, receiver, propKey, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
        RESTORE_ACC();
    }
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);                           // Maybe moved by GC
    auto propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);  // Maybe moved by GC
    auto value = GET_ACC();                                  // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StOwnByName(thread, receiver, propKey, value);
    RESTORE_ACC();
    INTERPRETER_RETURN_IF_ABRUPT(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleCreateEmptyArrayPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::createemptyarray";
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = SlowRuntimeStub::CreateEmptyArray(thread, factory, globalEnv);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleCreateEmptyObjectPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::createemptyobject";
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = SlowRuntimeStub::CreateEmptyObject(thread, factory, globalEnv);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleCreateObjectWithBufferPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t imm = READ_INST_16_1();
    LOG_INST() << "intrinsics::createobjectwithbuffer"
               << " imm:" << imm;
    JSObject *result = JSObject::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(imm).GetTaggedObject());
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = SlowRuntimeStub::CreateObjectWithBuffer(thread, factory, result);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandleSetObjectWithProtoPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::setobjectwithproto"
               << " v" << v0 << " v" << v1;
    JSTaggedValue proto = GET_VREG_VALUE(v0);
    JSTaggedValue obj = GET_VREG_VALUE(v1);
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::SetObjectWithProto(thread, proto, obj);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleCreateArrayWithBufferPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t imm = READ_INST_16_1();
    LOG_INST() << "intrinsics::createarraywithbuffer"
               << " imm:" << imm;
    JSArray *result = JSArray::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(imm).GetTaggedObject());
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = SlowRuntimeStub::CreateArrayWithBuffer(thread, factory, result);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandleImportModulePrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    auto prop = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);

    LOG_INST() << "intrinsics::importmodule "
               << "stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(prop.GetTaggedObject()));

    JSTaggedValue moduleRef = SlowRuntimeStub::ImportModule(thread, prop);
    SET_ACC(moduleRef);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleStModuleVarPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    auto prop = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);

    LOG_INST() << "intrinsics::stmodulevar "
                << "stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(prop.GetTaggedObject()));
    JSTaggedValue value = GET_ACC();

    SAVE_ACC();
    SlowRuntimeStub::StModuleVar(thread, prop, value);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleCopyModulePrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    JSTaggedValue srcModule = GET_VREG_VALUE(v0);

    LOG_INST() << "intrinsics::copymodule ";

    SAVE_ACC();
    SlowRuntimeStub::CopyModule(thread, srcModule);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleLdModVarByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    uint32_t v0 = READ_INST_8_5();
    JSTaggedValue itemName = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    JSTaggedValue moduleObj = GET_VREG_VALUE(v0);
    LOG_INST() << "intrinsics::ldmodvarbyname "
               << "string_id:" << stringId << ", "
               << "itemName: " << ConvertToString(EcmaString::Cast(itemName.GetTaggedObject()));

    JSTaggedValue moduleVar = SlowRuntimeStub::LdModvarByName(thread, moduleObj, itemName);
    SET_ACC(moduleVar);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleCreateRegExpWithLiteralPrefId32Imm8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue pattern = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    uint8_t flags = READ_INST_8_5();
    LOG_INST() << "intrinsics::createregexpwithliteral "
               << "stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(pattern.GetTaggedObject()))
               << ", flags:" << flags;
    JSTaggedValue res = SlowRuntimeStub::CreateRegExpWithLiteral(thread, pattern, flags);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_IMM8);
}

void InterpreterAssembly::HandleGetTemplateObjectPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsic::gettemplateobject"
               << " v" << v0;

    JSTaggedValue literal = GET_VREG_VALUE(v0);
    JSTaggedValue res = SlowRuntimeStub::GetTemplateObject(thread, literal);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleGetNextPropNamePrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsic::getnextpropname"
                << " v" << v0;
    JSTaggedValue iter = GET_VREG_VALUE(v0);
    JSTaggedValue res = SlowRuntimeStub::GetNextPropName(thread, iter);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleCopyDataPropertiesPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsic::copydataproperties"
               << " v" << v0 << " v" << v1;
    JSTaggedValue dst = GET_VREG_VALUE(v0);
    JSTaggedValue src = GET_VREG_VALUE(v1);
    JSTaggedValue res = SlowRuntimeStub::CopyDataProperties(thread, dst, src);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleStOwnByIndexPrefV8Imm32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t index = READ_INST_32_2();
    LOG_INST() << "intrinsics::stownbyindex"
               << " v" << v0 << " imm" << index;
    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    // fast path
    if (receiver.IsHeapObject() && !receiver.IsClassConstructor() && !receiver.IsClassPrototype()) {
        SAVE_ACC();
        JSTaggedValue value = GET_ACC();
        // fast path
        JSTaggedValue res =
            FastRuntimeStub::SetPropertyByIndex<true>(thread, receiver, index, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
        }
        RESTORE_ACC();
    }
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);  // Maybe moved by GC
    auto value = GET_ACC();         // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StOwnByIndex(thread, receiver, index, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
}

void InterpreterAssembly::HandleStOwnByValuePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::stownbyvalue"
               << " v" << v0 << " v" << v1;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    if (receiver.IsHeapObject() && !receiver.IsClassConstructor() && !receiver.IsClassPrototype()) {
        SAVE_ACC();
        JSTaggedValue propKey = GET_VREG_VALUE(v1);
        JSTaggedValue value = GET_ACC();
        // fast path
        JSTaggedValue res = FastRuntimeStub::SetPropertyByValue<true>(thread, receiver, propKey, value);

        // SetPropertyByValue maybe gc need update the value
        RESTORE_ACC();
        propKey = GET_VREG_VALUE(v1);
        value = GET_ACC();
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
    }

    // slow path
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);      // Maybe moved by GC
    auto propKey = GET_VREG_VALUE(v1);  // Maybe moved by GC
    auto value = GET_ACC();             // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StOwnByValue(thread, receiver, propKey, value);
    RESTORE_ACC();
    INTERPRETER_RETURN_IF_ABRUPT(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleCreateObjectWithExcludedKeysPrefImm16V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t numKeys = READ_INST_16_1();
    uint16_t v0 = READ_INST_8_3();
    uint16_t firstArgRegIdx = READ_INST_8_4();
    LOG_INST() << "intrinsics::createobjectwithexcludedkeys " << numKeys << " v" << firstArgRegIdx;

    JSTaggedValue obj = GET_VREG_VALUE(v0);

    JSTaggedValue res = SlowRuntimeStub::CreateObjectWithExcludedKeys(thread, numKeys, obj, firstArgRegIdx);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16_V8_V8);
}

void InterpreterAssembly::HandleDefineGeneratorFuncPrefId16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t length = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    LOG_INST() << "define gengerator function length: " << length
               << " v" << v0;
    JSFunction *result = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(result != nullptr);
    if (result->IsResolved()) {
        auto res = SlowRuntimeStub::DefineGeneratorFunc(thread, result);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        result = JSFunction::Cast(res.GetTaggedObject());
        result->SetConstantPool(thread, JSTaggedValue(constpool));
    } else {
        result->SetResolved(thread);
    }

    result->SetPropertyInlinedProps(thread, JSFunction::LENGTH_INLINE_PROPERTY_INDEX, JSTaggedValue(length));
    JSTaggedValue env = GET_VREG_VALUE(v0);
    result->SetLexicalEnv(thread, env);
    SET_ACC(JSTaggedValue(result))
    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_V8);
}

void InterpreterAssembly::HandleDefineAsyncFuncPrefId16Imm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t length = READ_INST_16_3();
    uint16_t v0 = READ_INST_8_5();
    LOG_INST() << "define async function length: " << length
               << " v" << v0;
    JSFunction *result = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(result != nullptr);
    if (result->IsResolved()) {
        auto res = SlowRuntimeStub::DefineAsyncFunc(thread, result);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        result = JSFunction::Cast(res.GetTaggedObject());
        result->SetConstantPool(thread, JSTaggedValue(constpool));
    } else {
        result->SetResolved(thread);
    }

    result->SetPropertyInlinedProps(thread, JSFunction::LENGTH_INLINE_PROPERTY_INDEX, JSTaggedValue(length));
    JSTaggedValue env = GET_VREG_VALUE(v0);
    result->SetLexicalEnv(thread, env);
    SET_ACC(JSTaggedValue(result))
    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_V8);
}

void InterpreterAssembly::HandleLdHolePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsic::ldhole";
    SET_ACC(JSTaggedValue::Hole());
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleCopyRestArgsPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t restIdx = READ_INST_16_1();
    LOG_INST() << "intrinsics::copyrestargs"
               << " index: " << restIdx;

    uint32_t startIdx = 0;
    uint32_t restNumArgs = GetNumArgs(sp, restIdx, startIdx);

    JSTaggedValue res = SlowRuntimeStub::CopyRestArgs(thread, sp, restNumArgs, startIdx);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandleDefineGetterSetterByValuePrefV8V8V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    uint16_t v2 = READ_INST_8_3();
    uint16_t v3 = READ_INST_8_4();
    LOG_INST() << "intrinsics::definegettersetterbyvalue"
               << " v" << v0 << " v" << v1 << " v" << v2 << " v" << v3;

    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue prop = GET_VREG_VALUE(v1);
    JSTaggedValue getter = GET_VREG_VALUE(v2);
    JSTaggedValue setter = GET_VREG_VALUE(v3);
    JSTaggedValue flag = GET_ACC();
    JSTaggedValue res =
        SlowRuntimeStub::DefineGetterSetterByValue(thread, obj, prop, getter, setter, flag.ToBoolean());
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8_V8_V8);
}

void InterpreterAssembly::HandleLdObjByIndexPrefV8Imm32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint32_t idx = READ_INST_32_2();
    LOG_INST() << "intrinsics::ldobjbyindex"
                << " v" << v0 << " imm" << idx;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    // fast path
    if (LIKELY(receiver.IsHeapObject())) {
        JSTaggedValue res = FastRuntimeStub::GetPropertyByIndex(thread, receiver, idx);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
            DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
        }
    }
    // not meet fast condition or fast path return hole, walk slow path
    // slow stub not need receiver
    JSTaggedValue res = SlowRuntimeStub::LdObjByIndex(thread, receiver, idx, false, JSTaggedValue::Undefined());
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
}

void InterpreterAssembly::HandleStObjByIndexPrefV8Imm32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint32_t index = READ_INST_32_2();
    LOG_INST() << "intrinsics::stobjbyindex"
                << " v" << v0 << " imm" << index;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    if (receiver.IsHeapObject()) {
        SAVE_ACC();
        JSTaggedValue value = GET_ACC();
        // fast path
        JSTaggedValue res = FastRuntimeStub::SetPropertyByIndex(thread, receiver, index, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
        }
        RESTORE_ACC();
    }
    // slow path
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);    // Maybe moved by GC
    JSTaggedValue value = GET_ACC();  // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StObjByIndex(thread, receiver, index, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8_IMM32);
}

void InterpreterAssembly::HandleLdObjByValuePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::Ldobjbyvalue"
                << " v" << v0 << " v" << v1;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    JSTaggedValue propKey = GET_VREG_VALUE(v1);

#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        auto profileTypeArray = ProfileTypeInfo::Cast(profileTypeInfo.GetTaggedObject());
        JSTaggedValue firstValue = profileTypeArray->Get(slotId);
        JSTaggedValue res = JSTaggedValue::Hole();

        if (LIKELY(firstValue.IsHeapObject())) {
            JSTaggedValue secondValue = profileTypeArray->Get(slotId + 1);
            res = ICRuntimeStub::TryLoadICByValue(thread, receiver, propKey, firstValue, secondValue);
        }
        // IC miss and not enter the megamorphic state, store as polymorphic
        if (res.IsHole() && !firstValue.IsHole()) {
            res = ICRuntimeStub::LoadICByValue(thread,
                                                profileTypeArray,
                                                receiver, propKey, slotId);
        }

        if (LIKELY(!res.IsHole())) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
    }
#endif
    // fast path
    if (LIKELY(receiver.IsHeapObject())) {
        JSTaggedValue res = FastRuntimeStub::GetPropertyByValue(thread, receiver, propKey);
        if (!res.IsHole()) {
            ASSERT(!res.IsAccessor());
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
    }
    // slow path
    JSTaggedValue res = SlowRuntimeStub::LdObjByValue(thread, receiver, propKey, false, JSTaggedValue::Undefined());
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleStObjByValuePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();

    LOG_INST() << "intrinsics::stobjbyvalue"
               << " v" << v0 << " v" << v1;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        auto profileTypeArray = ProfileTypeInfo::Cast(profileTypeInfo.GetTaggedObject());
        JSTaggedValue firstValue = profileTypeArray->Get(slotId);
        JSTaggedValue propKey = GET_VREG_VALUE(v1);
        JSTaggedValue value = GET_ACC();
        JSTaggedValue res = JSTaggedValue::Hole();
        SAVE_ACC();

        if (LIKELY(firstValue.IsHeapObject())) {
            JSTaggedValue secondValue = profileTypeArray->Get(slotId + 1);
            res = ICRuntimeStub::TryStoreICByValue(thread, receiver, propKey, firstValue, secondValue, value);
        }
        // IC miss and not enter the megamorphic state, store as polymorphic
        if (res.IsHole() && !firstValue.IsHole()) {
            res = ICRuntimeStub::StoreICByValue(thread,
                                                profileTypeArray,
                                                receiver, propKey, value, slotId);
        }

        if (LIKELY(!res.IsHole())) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
    }
#endif
    if (receiver.IsHeapObject()) {
        SAVE_ACC();
        JSTaggedValue propKey = GET_VREG_VALUE(v1);
        JSTaggedValue value = GET_ACC();
        // fast path
        JSTaggedValue res = FastRuntimeStub::SetPropertyByValue(thread, receiver, propKey, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
        RESTORE_ACC();
    }
    {
        // slow path
        SAVE_ACC();
        receiver = GET_VREG_VALUE(v0);  // Maybe moved by GC
        JSTaggedValue propKey = GET_VREG_VALUE(v1);   // Maybe moved by GC
        JSTaggedValue value = GET_ACC();              // Maybe moved by GC
        JSTaggedValue res = SlowRuntimeStub::StObjByValue(thread, receiver, propKey, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        RESTORE_ACC();
    }
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleLdSuperByValuePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::Ldsuperbyvalue"
               << " v" << v0 << " v" << v1;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    JSTaggedValue propKey = GET_VREG_VALUE(v1);

    // slow path
    JSTaggedValue thisFunc = GetThisFunction(sp);
    JSTaggedValue res = SlowRuntimeStub::LdSuperByValue(thread, receiver, propKey, thisFunc);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleStSuperByValuePrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();

    LOG_INST() << "intrinsics::stsuperbyvalue"
               << " v" << v0 << " v" << v1;
    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    JSTaggedValue propKey = GET_VREG_VALUE(v1);
    JSTaggedValue value = GET_ACC();

    // slow path
    SAVE_ACC();
    JSTaggedValue thisFunc = GetThisFunction(sp);
    JSTaggedValue res = SlowRuntimeStub::StSuperByValue(thread, receiver, propKey, value, thisFunc);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleTryLdGlobalByNamePrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    auto prop = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);

    LOG_INST() << "intrinsics::tryldglobalbyname "
                << "stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(prop.GetTaggedObject()));

#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
        JSTaggedValue globalObj = globalEnv->GetGlobalObject();
        JSTaggedValue res = ICRuntimeStub::LoadGlobalICByName(thread,
                                                              ProfileTypeInfo::Cast(
                                                                  profileTypeInfo.GetTaggedObject()),
                                                              globalObj, prop, slotId);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
        DISPATCH(BytecodeInstruction::Format::PREF_ID32);
    }
#endif

    bool found = false;
    // order: 1. global record 2. global object
    JSTaggedValue result = SlowRuntimeStub::LdGlobalRecord(thread, prop, &found);
    if (found) {
        SET_ACC(PropertyBox::Cast(result.GetTaggedObject())->GetValue());
    } else {
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
        JSTaggedValue globalObj = globalEnv->GetGlobalObject();
        JSTaggedValue globalResult = FastRuntimeStub::GetGlobalOwnProperty(globalObj, prop, &found);
        if (found) {
            SET_ACC(globalResult);
        } else {
            // slow path
            JSTaggedValue res = SlowRuntimeStub::TryLdGlobalByName(thread, globalObj, prop);
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
        }
    }

    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleTryStGlobalByNamePrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    LOG_INST() << "intrinsics::trystglobalbyname"
               << " stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject()));

#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        JSTaggedValue value = GET_ACC();
        SAVE_ACC();
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
        JSTaggedValue globalObj = globalEnv->GetGlobalObject();
        JSTaggedValue res = ICRuntimeStub::StoreGlobalICByName(thread,
                                                               ProfileTypeInfo::Cast(
                                                                   profileTypeInfo.GetTaggedObject()),
                                                               globalObj, propKey, value, slotId);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        RESTORE_ACC();
        DISPATCH(BytecodeInstruction::Format::PREF_ID32);
    }
#endif

    bool found = false;
    SlowRuntimeStub::LdGlobalRecord(thread, propKey, &found);
    // 1. find from global record
    if (found) {
        JSTaggedValue value = GET_ACC();
        SAVE_ACC();
        JSTaggedValue res = SlowRuntimeStub::TryUpdateGlobalRecord(thread, propKey, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        RESTORE_ACC();
    } else {
        // 2. find from global object
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
        JSTaggedValue globalObj = globalEnv->GetGlobalObject();
        FastRuntimeStub::GetGlobalOwnProperty(globalObj, propKey, &found);
        if (!found) {
            auto result = SlowRuntimeStub::ThrowReferenceError(thread, propKey, " is not defined");
            INTERPRETER_RETURN_IF_ABRUPT(result);
        }
        JSTaggedValue value = GET_ACC();
        SAVE_ACC();
        JSTaggedValue res = SlowRuntimeStub::StGlobalVar(thread, propKey, value);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        RESTORE_ACC();
    }
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleStConstToGlobalRecordPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    LOG_INST() << "intrinsics::stconsttoglobalrecord"
               << " stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject()));

    JSTaggedValue value = GET_ACC();
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::StGlobalRecord(thread, propKey, value, true);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleStLetToGlobalRecordPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    LOG_INST() << "intrinsics::stlettoglobalrecord"
               << " stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject()));

    JSTaggedValue value = GET_ACC();
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::StGlobalRecord(thread, propKey, value, false);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleStClassToGlobalRecordPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    LOG_INST() << "intrinsics::stclasstoglobalrecord"
               << " stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject()));

    JSTaggedValue value = GET_ACC();
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::StGlobalRecord(thread, propKey, value, false);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleStOwnByValueWithNameSetPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_1();
    uint32_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsics::stownbyvaluewithnameset"
               << " v" << v0 << " v" << v1;
    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    if (receiver.IsHeapObject() && !receiver.IsClassConstructor() && !receiver.IsClassPrototype()) {
        SAVE_ACC();
        JSTaggedValue propKey = GET_VREG_VALUE(v1);
        JSTaggedValue value = GET_ACC();
        // fast path
        JSTaggedValue res = FastRuntimeStub::SetPropertyByValue<true>(thread, receiver, propKey, value);

        // SetPropertyByValue maybe gc need update the value
        RESTORE_ACC();
        propKey = GET_VREG_VALUE(v1);
        value = GET_ACC();
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            JSFunction::SetFunctionNameNoPrefix(thread, JSFunction::Cast(value.GetTaggedObject()), propKey);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
        }
    }

    // slow path
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);      // Maybe moved by GC
    auto propKey = GET_VREG_VALUE(v1);  // Maybe moved by GC
    auto value = GET_ACC();             // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StOwnByValueWithNameSet(thread, receiver, propKey, value);
    RESTORE_ACC();
    INTERPRETER_RETURN_IF_ABRUPT(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleStOwnByNameWithNameSetPrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    uint32_t v0 = READ_INST_8_5();
    LOG_INST() << "intrinsics::stownbynamewithnameset "
                << "v" << v0 << " stringId:" << stringId;

    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    if (receiver.IsJSObject() && !receiver.IsClassConstructor() && !receiver.IsClassPrototype()) {
        JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
        JSTaggedValue value = GET_ACC();
        // fast path
        SAVE_ACC();
        JSTaggedValue res = FastRuntimeStub::SetPropertyByName<true>(thread, receiver, propKey, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            JSFunction::SetFunctionNameNoPrefix(thread, JSFunction::Cast(value.GetTaggedObject()), propKey);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
        RESTORE_ACC();
    }

    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);                           // Maybe moved by GC
    auto propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);  // Maybe moved by GC
    auto value = GET_ACC();                                  // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StOwnByNameWithNameSet(thread, receiver, propKey, value);
    RESTORE_ACC();
    INTERPRETER_RETURN_IF_ABRUPT(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleLdGlobalVarPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
    JSTaggedValue globalObj = globalEnv->GetGlobalObject();
#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        JSTaggedValue res = ICRuntimeStub::LoadGlobalICByName(thread,
                                                              ProfileTypeInfo::Cast(
                                                                  profileTypeInfo.GetTaggedObject()),
                                                              globalObj, propKey, slotId);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
        DISPATCH(BytecodeInstruction::Format::PREF_ID32);
    }
#endif
    bool found = false;
    JSTaggedValue result = FastRuntimeStub::GetGlobalOwnProperty(globalObj, propKey, &found);
    if (found) {
        SET_ACC(result);
    } else {
        // slow path
        JSTaggedValue res = SlowRuntimeStub::LdGlobalVar(thread, globalObj, propKey);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        SET_ACC(res);
    }
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleLdObjByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_5();
    JSTaggedValue receiver = GET_VREG_VALUE(v0);

#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        auto profileTypeArray = ProfileTypeInfo::Cast(profileTypeInfo.GetTaggedObject());
        JSTaggedValue firstValue = profileTypeArray->Get(slotId);
        JSTaggedValue res = JSTaggedValue::Hole();

        if (LIKELY(firstValue.IsHeapObject())) {
            JSTaggedValue secondValue = profileTypeArray->Get(slotId + 1);
            res = ICRuntimeStub::TryLoadICByName(thread, receiver, firstValue, secondValue);
        }
        // IC miss and not enter the megamorphic state, store as polymorphic
        if (res.IsHole() && !firstValue.IsHole()) {
            uint32_t stringId = READ_INST_32_1();
            JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
            res = ICRuntimeStub::LoadICByName(thread,
                                                profileTypeArray,
                                                receiver, propKey, slotId);
        }

        if (LIKELY(!res.IsHole())) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
    }
#endif
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    LOG_INST() << "intrinsics::ldobjbyname "
                << "v" << v0 << " stringId:" << stringId << ", "
                << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject())) << ", obj:" << receiver.GetRawData();

    if (LIKELY(receiver.IsHeapObject())) {
        // fast path
        JSTaggedValue res = FastRuntimeStub::GetPropertyByName(thread, receiver, propKey);
        if (!res.IsHole()) {
            ASSERT(!res.IsAccessor());
            INTERPRETER_RETURN_IF_ABRUPT(res);
            SET_ACC(res);
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
    }
    // not meet fast condition or fast path return hole, walk slow path
    // slow stub not need receiver
    JSTaggedValue res = SlowRuntimeStub::LdObjByName(thread, receiver, propKey, false, JSTaggedValue::Undefined());
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleStObjByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t v0 = READ_INST_8_5();
    JSTaggedValue receiver = GET_VREG_VALUE(v0);
    JSTaggedValue value = GET_ACC();
#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        auto profileTypeArray = ProfileTypeInfo::Cast(profileTypeInfo.GetTaggedObject());
        JSTaggedValue firstValue = profileTypeArray->Get(slotId);
        JSTaggedValue res = JSTaggedValue::Hole();
        SAVE_ACC();

        if (LIKELY(firstValue.IsHeapObject())) {
            JSTaggedValue secondValue = profileTypeArray->Get(slotId + 1);
            res = ICRuntimeStub::TryStoreICByName(thread, receiver, firstValue, secondValue, value);
        }
        // IC miss and not enter the megamorphic state, store as polymorphic
        if (res.IsHole() && !firstValue.IsHole()) {
            uint32_t stringId = READ_INST_32_1();
            JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
            res = ICRuntimeStub::StoreICByName(thread,
                                                profileTypeArray,
                                                receiver, propKey, value, slotId);
        }

        if (LIKELY(!res.IsHole())) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
    }
#endif
    uint32_t stringId = READ_INST_32_1();
    LOG_INST() << "intrinsics::stobjbyname "
                << "v" << v0 << " stringId:" << stringId;
    if (receiver.IsHeapObject()) {
        JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
        value = GET_ACC();
        // fast path
        SAVE_ACC();
        JSTaggedValue res = FastRuntimeStub::SetPropertyByName(thread, receiver, propKey, value);
        if (!res.IsHole()) {
            INTERPRETER_RETURN_IF_ABRUPT(res);
            RESTORE_ACC();
            DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
        }
        RESTORE_ACC();
    }
    // slow path
    SAVE_ACC();
    receiver = GET_VREG_VALUE(v0);                           // Maybe moved by GC
    auto propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);  // Maybe moved by GC
    value = GET_ACC();                                  // Maybe moved by GC
    JSTaggedValue res = SlowRuntimeStub::StObjByName(thread, receiver, propKey, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleLdSuperByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    uint32_t v0 = READ_INST_8_5();
    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);

    LOG_INST() << "intrinsics::ldsuperbyname"
               << "v" << v0 << " stringId:" << stringId << ", "
               << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject())) << ", obj:" << obj.GetRawData();

    JSTaggedValue thisFunc = GetThisFunction(sp);
    JSTaggedValue res = SlowRuntimeStub::LdSuperByValue(thread, obj, propKey, thisFunc);

    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleStSuperByNamePrefId32V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    uint32_t v0 = READ_INST_8_5();

    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue propKey = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    JSTaggedValue value = GET_ACC();

    LOG_INST() << "intrinsics::stsuperbyname"
               << "v" << v0 << " stringId:" << stringId << ", "
               << ConvertToString(EcmaString::Cast(propKey.GetTaggedObject())) << ", obj:" << obj.GetRawData()
               << ", value:" << value.GetRawData();

    // slow path
    SAVE_ACC();
    JSTaggedValue thisFunc = GetThisFunction(sp);
    JSTaggedValue res = SlowRuntimeStub::StSuperByValue(thread, obj, propKey, value, thisFunc);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32_V8);
}

void InterpreterAssembly::HandleStGlobalVarPrefId32(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint32_t stringId = READ_INST_32_1();
    JSTaggedValue prop = ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(stringId);
    JSTaggedValue value = GET_ACC();

    LOG_INST() << "intrinsics::stglobalvar "
               << "stringId:" << stringId << ", " << ConvertToString(EcmaString::Cast(prop.GetTaggedObject()))
               << ", value:" << value.GetRawData();
#if ECMASCRIPT_ENABLE_IC
    if (!profileTypeInfo.IsUndefined()) {
        uint16_t slotId = READ_INST_8_0();
        EcmaVM *ecmaVm = thread->GetEcmaVM();
        JSHandle<GlobalEnv> globalEnv = ecmaVm->GetGlobalEnv();
        JSTaggedValue globalObj = globalEnv->GetGlobalObject();
        SAVE_ACC();
        JSTaggedValue res = ICRuntimeStub::StoreGlobalICByName(thread,
                                                               ProfileTypeInfo::Cast(
                                                                   profileTypeInfo.GetTaggedObject()),
                                                               globalObj, prop, value, slotId);
        INTERPRETER_RETURN_IF_ABRUPT(res);
        RESTORE_ACC();
        DISPATCH(BytecodeInstruction::Format::PREF_ID32);
    }
#endif
    SAVE_ACC();
    JSTaggedValue res = SlowRuntimeStub::StGlobalVar(thread, prop, value);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    RESTORE_ACC();
    DISPATCH(BytecodeInstruction::Format::PREF_ID32);
}

void InterpreterAssembly::HandleCreateGeneratorObjPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsics::creategeneratorobj"
               << " v" << v0;
    JSTaggedValue genFunc = GET_VREG_VALUE(v0);
    JSTaggedValue res = SlowRuntimeStub::CreateGeneratorObj(thread, genFunc);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleStArraySpreadPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "ecmascript::intrinsics::starrayspread"
               << " v" << v0 << " v" << v1 << "acc";
    JSTaggedValue dst = GET_VREG_VALUE(v0);
    JSTaggedValue index = GET_VREG_VALUE(v1);
    JSTaggedValue src = GET_ACC();
    JSTaggedValue res = SlowRuntimeStub::StArraySpread(thread, dst, index, src);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleGetIteratorNextPrefV8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    uint16_t v1 = READ_INST_8_2();
    LOG_INST() << "intrinsic::getiteratornext"
               << " v" << v0 << " v" << v1;
    JSTaggedValue obj = GET_VREG_VALUE(v0);
    JSTaggedValue method = GET_VREG_VALUE(v1);
    JSTaggedValue res = SlowRuntimeStub::GetIteratorNext(thread, obj, method);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8_V8);
}

void InterpreterAssembly::HandleDefineClassWithBufferPrefId16Imm16Imm16V8V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t methodId = READ_INST_16_1();
    uint16_t imm = READ_INST_16_3();
    uint16_t length = READ_INST_16_5();
    uint16_t v0 = READ_INST_8_7();
    uint16_t v1 = READ_INST_8_8();
    LOG_INST() << "intrinsics::defineclasswithbuffer"
                << " method id:" << methodId << " literal id:" << imm << " lexenv: v" << v0 << " parent: v" << v1;
    JSFunction *classTemplate = JSFunction::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(methodId).GetTaggedObject());
    ASSERT(classTemplate != nullptr);

    TaggedArray *literalBuffer = TaggedArray::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(imm).GetTaggedObject());
    JSTaggedValue lexenv = GET_VREG_VALUE(v0);
    JSTaggedValue proto = GET_VREG_VALUE(v1);

    JSTaggedValue res;
    if (LIKELY(!classTemplate->IsResolved())) {
        res = SlowRuntimeStub::ResolveClass(thread, JSTaggedValue(classTemplate), literalBuffer,
                                            proto, lexenv, ConstantPool::Cast(constpool.GetTaggedObject()));
    } else {
        res = SlowRuntimeStub::CloneClassFromTemplate(thread, JSTaggedValue(classTemplate),
                                                        proto, lexenv, ConstantPool::Cast(constpool.GetTaggedObject()));
    }

    INTERPRETER_RETURN_IF_ABRUPT(res);
    ASSERT(res.IsClassConstructor());
    JSFunction *cls = JSFunction::Cast(res.GetTaggedObject());

    lexenv = GET_VREG_VALUE(v0);  // slow runtime may gc
    cls->SetLexicalEnv(thread, lexenv);

    SlowRuntimeStub::SetClassConstructorLength(thread, res, JSTaggedValue(length));

    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_ID16_IMM16_IMM16_V8_V8);
}

void InterpreterAssembly::HandleLdFunctionPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsic::ldfunction";
    SET_ACC(GetThisFunction(sp));
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleSuperCallPrefImm16V8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t range = READ_INST_16_1();
    uint16_t v0 = READ_INST_8_3();
    LOG_INST() << "intrinsics::supercall"
                << " range: " << range << " v" << v0;

    JSTaggedValue thisFunc = GET_ACC();
    JSTaggedValue newTarget = GetNewTarget(sp);

    JSTaggedValue res = SlowRuntimeStub::SuperCall(thread, thisFunc, newTarget, v0, range);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16_V8);
}

void InterpreterAssembly::HandleSuperCallSpreadPrefV8(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t v0 = READ_INST_8_1();
    LOG_INST() << "intrinsic::supercallspread"
               << " array: v" << v0;

    JSTaggedValue thisFunc = GET_ACC();
    JSTaggedValue newTarget = GetNewTarget(sp);
    JSTaggedValue array = GET_VREG_VALUE(v0);

    JSTaggedValue res = SlowRuntimeStub::SuperCallSpread(thread, thisFunc, newTarget, array);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_V8);
}

void InterpreterAssembly::HandleCreateObjectHavingMethodPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t imm = READ_INST_16_1();
    LOG_INST() << "intrinsics::createobjecthavingmethod"
               << " imm:" << imm;
    JSObject *result = JSObject::Cast(ConstantPool::Cast(constpool.GetTaggedObject())->GetObjectFromCache(imm).GetTaggedObject());
    JSTaggedValue env = GET_ACC();
    EcmaVM *ecmaVm = thread->GetEcmaVM();
    ObjectFactory *factory = ecmaVm->GetFactory();
    JSTaggedValue res = SlowRuntimeStub::CreateObjectHavingMethod(thread, factory, result, env, ConstantPool::Cast(constpool.GetTaggedObject()));
    INTERPRETER_RETURN_IF_ABRUPT(res);
    SET_ACC(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandleThrowIfSuperNotCorrectCallPrefImm16(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    uint16_t imm = READ_INST_16_1();
    JSTaggedValue thisValue = GET_ACC();
    LOG_INST() << "intrinsic::throwifsupernotcorrectcall"
               << " imm:" << imm;
    JSTaggedValue res = SlowRuntimeStub::ThrowIfSuperNotCorrectCall(thread, imm, thisValue);
    INTERPRETER_RETURN_IF_ABRUPT(res);
    DISPATCH(BytecodeInstruction::Format::PREF_IMM16);
}

void InterpreterAssembly::HandleLdHomeObjectPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::ldhomeobject";

    JSTaggedValue thisFunc = GetThisFunction(sp);
    JSTaggedValue homeObject = JSFunction::Cast(thisFunc.GetTaggedObject())->GetHomeObject();

    SET_ACC(homeObject);
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleThrowDeleteSuperPropertyPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "throwdeletesuperproperty";

    SlowRuntimeStub::ThrowDeleteSuperProperty(thread);
    INTERPRETER_GOTO_EXCEPTION_HANDLER();
}

void InterpreterAssembly::HandleDebuggerPref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::debugger";
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleIsTruePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::istrue";
    if (GET_ACC().ToBoolean()) {
        SET_ACC(JSTaggedValue::True());
    } else {
        SET_ACC(JSTaggedValue::False());
    }
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::HandleIsFalsePref(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG_INST() << "intrinsics::isfalse";
    if (!GET_ACC().ToBoolean()) {
        SET_ACC(JSTaggedValue::True());
    } else {
        SET_ACC(JSTaggedValue::False());
    }
    DISPATCH(BytecodeInstruction::Format::PREF_NONE);
}

void InterpreterAssembly::ExceptionHandler(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    auto exception = thread->GetException();

    InterpretedFrameHandler frameHandler(sp);
    uint32_t pcOffset = panda_file::INVALID_OFFSET;
    for (; frameHandler.HasFrame(); frameHandler.PrevInterpretedFrame()) {
        if (frameHandler.IsBreakFrame()) {
            return;
        }
        auto method = frameHandler.GetMethod();
        pcOffset = FindCatchBlock(method, frameHandler.GetBytecodeOffset());
        if (pcOffset != panda_file::INVALID_OFFSET) {
            sp = frameHandler.GetSp();
            constpool = JSTaggedValue(frameHandler.GetConstpool());
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            pc = method->GetBytecodeArray() + pcOffset;
            break;
        }
    }
    if (pcOffset == panda_file::INVALID_OFFSET) {
        return;
    }

    SET_ACC(exception);
    thread->ClearException();
    thread->SetCurrentSPFrame(sp);
    DISPATCH_OFFSET(0);
}
void InterpreterAssembly::HandleOverflow(
    JSThread *thread, const uint8_t *pc, JSTaggedType *sp, JSTaggedValue constpool, JSTaggedValue profileTypeInfo,
    JSTaggedValue acc, int32_t hotnessCounter)
{
    LOG(FATAL, INTERPRETER) << "opcode overflow";
}

inline uint32_t InterpreterAssembly::FindCatchBlock(JSMethod *caller, uint32_t pc)
{
    auto *pandaFile = caller->GetPandaFile();
    panda_file::MethodDataAccessor mda(*pandaFile, caller->GetFileId());
    panda_file::CodeDataAccessor cda(*pandaFile, mda.GetCodeId().value());

    uint32_t pcOffset = panda_file::INVALID_OFFSET;
    cda.EnumerateTryBlocks([&pcOffset, pc](panda_file::CodeDataAccessor::TryBlock &try_block) {
        if ((try_block.GetStartPc() <= pc) && ((try_block.GetStartPc() + try_block.GetLength()) > pc)) {
            try_block.EnumerateCatchBlocks([&](panda_file::CodeDataAccessor::CatchBlock &catch_block) {
                pcOffset = catch_block.GetHandlerPc();
                return false;
            });
        }
        return pcOffset == panda_file::INVALID_OFFSET;
    });
    return pcOffset;
}

inline void InterpreterAssembly::InterpreterFrameCopyArgs(
    JSTaggedType *newSp, uint32_t numVregs, uint32_t numActualArgs, uint32_t numDeclaredArgs, bool haveExtraArgs)
{
    size_t i = 0;
    for (; i < numVregs; i++) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        newSp[i] = JSTaggedValue::VALUE_UNDEFINED;
    }
    for (i = numActualArgs; i < numDeclaredArgs; i++) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        newSp[numVregs + i] = JSTaggedValue::VALUE_UNDEFINED;
    }
    if (haveExtraArgs) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        newSp[numVregs + i] = JSTaggedValue(numActualArgs).GetRawData();  // numActualArgs is stored at the end
    }
}

inline JSTaggedValue InterpreterAssembly::GetThisFunction(JSTaggedType *sp)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    InterpretedFrame *state = reinterpret_cast<InterpretedFrame *>(sp) - 1;
    return JSTaggedValue(state->function);
}

inline JSTaggedValue InterpreterAssembly::GetNewTarget(JSTaggedType *sp)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    InterpretedFrame *state = reinterpret_cast<InterpretedFrame *>(sp) - 1;
    JSMethod *method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    uint32_t callType = method->GetCallType();
    ASSERT(callType & HAVE_NEWTARGET_BIT);
    uint32_t numVregs = method->GetNumVregs();
    bool haveFunc = callType & HAVE_FUNC_BIT;
    return JSTaggedValue(sp[numVregs + haveFunc]);
}

inline uint32_t InterpreterAssembly::GetNumArgs(JSTaggedType *sp, uint32_t restIdx, uint32_t &startIdx)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    InterpretedFrame *state = reinterpret_cast<InterpretedFrame *>(sp) - 1;
    JSMethod *method = ECMAObject::Cast(state->function.GetTaggedObject())->GetCallTarget();
    uint32_t callType = method->GetCallType();
    ASSERT(callType & HAVE_EXTRA_BIT);
    bool haveThis = callType & HAVE_THIS_BIT;
    bool haveNewTarget = callType & HAVE_NEWTARGET_BIT;
    bool haveFunc = callType & HAVE_FUNC_BIT;
    uint32_t numVregs = method->GetNumVregs();
    uint32_t copyArgs = haveFunc + haveNewTarget + haveThis;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    JSTaggedType *lastFrame = state->base.prev - FRAME_STATE_SIZE - 1;
    JSTaggedValue numArgs(*lastFrame);
    // actualNumArgs is stored at the end
    ASSERT(numArgs.IsInt());
    int32_t actualNumArgs = numArgs.GetInt() - copyArgs;
    int32_t tmp = actualNumArgs - restIdx;
    uint32_t restNumArgs = (tmp > 0) ? tmp : 0;
    startIdx = numVregs + copyArgs + restIdx;
    return restNumArgs;
}

inline size_t InterpreterAssembly::GetJumpSizeAfterCall(const uint8_t *prevPc)
{
    uint8_t op = *prevPc;
    size_t jumpSize;
    switch (op) {
        case (EcmaOpcode::CALLARG0DYN_PREF_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_V8);
            break;
        case (EcmaOpcode::CALLARG1DYN_PREF_V8_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_V8_V8);
            break;
        case (EcmaOpcode::CALLARGS2DYN_PREF_V8_V8_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_V8_V8_V8);
            break;
        case (EcmaOpcode::CALLARGS3DYN_PREF_V8_V8_V8_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_V8_V8_V8_V8);
            break;
        case (EcmaOpcode::CALLIRANGEDYN_PREF_IMM16_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_IMM16_V8);
            break;
        case (EcmaOpcode::CALLITHISRANGEDYN_PREF_IMM16_V8):
            jumpSize = BytecodeInstruction::Size(BytecodeInstruction::Format::PREF_IMM16_V8);
            break;
        default:
            UNREACHABLE();
    }

    return jumpSize;
}

inline JSTaggedValue InterpreterAssembly::UpdateHotnessCounter(JSThread* thread, TaggedType *sp)
{
    InterpretedFrame *state = GET_FRAME(sp);
    thread->CheckSafepoint();
    if (state->profileTypeInfo == JSTaggedValue::Undefined()) {
        auto thisFunc = state->function;
        auto method = ECMAObject::Cast(thisFunc.GetTaggedObject())->GetCallTarget();
        auto res = SlowRuntimeStub::NotifyInlineCache(
            thread, JSFunction::Cast(thisFunc.GetHeapObject()), method);
            state->profileTypeInfo = res;
        return res;
    }
    return state->profileTypeInfo;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}  // namespace panda::ecmascript