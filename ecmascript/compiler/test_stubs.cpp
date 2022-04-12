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

#include "test_stubs.h"

#include "ecmascript/compiler/llvm_ir_builder.h"
#include "ecmascript/compiler/stub-inl.h"
#include "ecmascript/compiler/variable_type.h"
#include "ecmascript/message_string.h"

namespace panda::ecmascript::kungfu {
using namespace panda::ecmascript;
void FooAOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    (void)calltarget;
    GateRef barIndex = IntBuildTaggedWithNoGC(Int32(CommonStubCSigns::BarAOT));
    GateRef numArgs = IntBuildTaggedWithNoGC(Int32(2));
    GateRef barfunc = CallRuntime(glue, RTSTUB_ID(DefineAotFunc), {barIndex, numArgs});
    GateRef result = CallNGCRuntime(glue, RTSTUB_ID(JSCall), {glue, argc, barfunc, newtarget, thisObj, a, b});
    Return(result);
}

void BarAOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    (void)argc;
    (void)calltarget;
    (void)newtarget;
    (void)thisObj;
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    GateRef result = CallRuntime(glue, RTSTUB_ID(Add2Dyn), {a, b});
    Return(result);
}

void Foo1AOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    (void)calltarget;
    GateRef barIndex = IntBuildTaggedWithNoGC(Int32(CommonStubCSigns::Bar1AOT));
    GateRef numArgs = IntBuildTaggedWithNoGC(Int32(3));
    GateRef barfunc = CallRuntime(glue, RTSTUB_ID(DefineAotFunc), {barIndex, numArgs});
    GateRef result = CallNGCRuntime(glue, RTSTUB_ID(JSCall), {glue, argc, barfunc, newtarget, thisObj, a, b});
    Return(result);
}

void Bar1AOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    (void)argc;
    (void)calltarget;
    (void)newtarget;
    (void)thisObj;
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    GateRef c = TaggedArgument(7);
    GateRef result = CallRuntime(glue, RTSTUB_ID(Add2Dyn), {a, b});
    GateRef result2 = CallRuntime(glue, RTSTUB_ID(Add2Dyn), {result, c});
    Return(result2);
}

void Foo2AOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    (void)calltarget;
    GateRef actualArgC = Int32Add(argc, Int32(1));
    GateRef barIndex = IntBuildTaggedWithNoGC(Int32(CommonStubCSigns::BarAOT));
    GateRef numArgs = IntBuildTaggedWithNoGC(Int32(2));
    GateRef barfunc = CallRuntime(glue, RTSTUB_ID(DefineAotFunc), {barIndex, numArgs});
    GateRef result = CallNGCRuntime(glue, RTSTUB_ID(JSCall), {glue, actualArgC, barfunc, newtarget, thisObj,
                                    a, b, Undefined()});
    Return(result);
}

void FooNativeAOTStub::GenerateCircuit(const CompilationConfig *cfg)
{
    Stub::GenerateCircuit(cfg);
    GateRef glue = PtrArgument(0);
    GateRef argc = Int32Argument(1);
    GateRef calltarget = TaggedArgument(2);
    GateRef newtarget = TaggedArgument(3);
    GateRef thisObj = TaggedArgument(4);
    GateRef a = TaggedArgument(5);
    GateRef b = TaggedArgument(6);
    (void)calltarget;
    GateRef actualArgC = Int32Add(argc, Int32(1));
    GateRef printfunc = CallRuntime(glue, RTSTUB_ID(GetPrintFunc), {});
    GateRef result = CallNGCRuntime(glue, RTSTUB_ID(JSCall), {glue, actualArgC, printfunc, newtarget, thisObj,
                                    a, b, Undefined()});
    Return(result);
}
}   // namespace panda::ecmascript::kungfu