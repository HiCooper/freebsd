//===- X86LegalizerInfo.cpp --------------------------------------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the targeting of the Machinelegalizer class for X86.
/// \todo This should be generated by TableGen.
//===----------------------------------------------------------------------===//

#include "X86LegalizerInfo.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"

using namespace llvm;
using namespace TargetOpcode;
using namespace LegalizeActions;

/// FIXME: The following static functions are SizeChangeStrategy functions
/// that are meant to temporarily mimic the behaviour of the old legalization
/// based on doubling/halving non-legal types as closely as possible. This is
/// not entirly possible as only legalizing the types that are exactly a power
/// of 2 times the size of the legal types would require specifying all those
/// sizes explicitly.
/// In practice, not specifying those isn't a problem, and the below functions
/// should disappear quickly as we add support for legalizing non-power-of-2
/// sized types further.
static void
addAndInterleaveWithUnsupported(LegalizerInfo::SizeAndActionsVec &result,
                                const LegalizerInfo::SizeAndActionsVec &v) {
  for (unsigned i = 0; i < v.size(); ++i) {
    result.push_back(v[i]);
    if (i + 1 < v[i].first && i + 1 < v.size() &&
        v[i + 1].first != v[i].first + 1)
      result.push_back({v[i].first + 1, Unsupported});
  }
}

static LegalizerInfo::SizeAndActionsVec
widen_1(const LegalizerInfo::SizeAndActionsVec &v) {
  assert(v.size() >= 1);
  assert(v[0].first > 1);
  LegalizerInfo::SizeAndActionsVec result = {{1, WidenScalar},
                                             {2, Unsupported}};
  addAndInterleaveWithUnsupported(result, v);
  auto Largest = result.back().first;
  result.push_back({Largest + 1, Unsupported});
  return result;
}

X86LegalizerInfo::X86LegalizerInfo(const X86Subtarget &STI,
                                   const X86TargetMachine &TM)
    : Subtarget(STI), TM(TM) {

  setLegalizerInfo32bit();
  setLegalizerInfo64bit();
  setLegalizerInfoSSE1();
  setLegalizerInfoSSE2();
  setLegalizerInfoSSE41();
  setLegalizerInfoAVX();
  setLegalizerInfoAVX2();
  setLegalizerInfoAVX512();
  setLegalizerInfoAVX512DQ();
  setLegalizerInfoAVX512BW();

  setLegalizeScalarToDifferentSizeStrategy(G_PHI, 0, widen_1);
  for (unsigned BinOp : {G_SUB, G_MUL, G_AND, G_OR, G_XOR})
    setLegalizeScalarToDifferentSizeStrategy(BinOp, 0, widen_1);
  for (unsigned MemOp : {G_LOAD, G_STORE})
    setLegalizeScalarToDifferentSizeStrategy(MemOp, 0,
       narrowToSmallerAndWidenToSmallest);
  setLegalizeScalarToDifferentSizeStrategy(
      G_GEP, 1, widenToLargerTypesUnsupportedOtherwise);
  setLegalizeScalarToDifferentSizeStrategy(
      G_CONSTANT, 0, widenToLargerTypesAndNarrowToLargest);

  computeTables();
  verify(*STI.getInstrInfo());
}

void X86LegalizerInfo::setLegalizerInfo32bit() {

  const LLT p0 = LLT::pointer(0, TM.getPointerSizeInBits(0));
  const LLT s1 = LLT::scalar(1);
  const LLT s8 = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT s128 = LLT::scalar(128);

  for (auto Ty : {p0, s1, s8, s16, s32})
    setAction({G_IMPLICIT_DEF, Ty}, Legal);

  for (auto Ty : {s8, s16, s32, p0})
    setAction({G_PHI, Ty}, Legal);

  for (unsigned BinOp : {G_ADD, G_SUB, G_MUL, G_AND, G_OR, G_XOR})
    for (auto Ty : {s8, s16, s32})
      setAction({BinOp, Ty}, Legal);

  for (unsigned Op : {G_UADDE}) {
    setAction({Op, s32}, Legal);
    setAction({Op, 1, s1}, Legal);
  }

  for (unsigned MemOp : {G_LOAD, G_STORE}) {
    for (auto Ty : {s8, s16, s32, p0})
      setAction({MemOp, Ty}, Legal);

    // And everything's fine in addrspace 0.
    setAction({MemOp, 1, p0}, Legal);
  }

  // Pointer-handling
  setAction({G_FRAME_INDEX, p0}, Legal);
  setAction({G_GLOBAL_VALUE, p0}, Legal);

  setAction({G_GEP, p0}, Legal);
  setAction({G_GEP, 1, s32}, Legal);

  if (!Subtarget.is64Bit()) {
    getActionDefinitionsBuilder(G_PTRTOINT)
        .legalForCartesianProduct({s1, s8, s16, s32}, {p0})
        .maxScalar(0, s32)
        .widenScalarToNextPow2(0, /*Min*/ 8);
    getActionDefinitionsBuilder(G_INTTOPTR).legalFor({{p0, s32}});

    // Shifts and SDIV
    getActionDefinitionsBuilder(
        {G_SHL, G_LSHR, G_ASHR, G_SDIV, G_SREM, G_UDIV, G_UREM})
        .legalFor({s8, s16, s32})
        .clampScalar(0, s8, s32);
  }

  // Control-flow
  setAction({G_BRCOND, s1}, Legal);

  // Constants
  for (auto Ty : {s8, s16, s32, p0})
    setAction({TargetOpcode::G_CONSTANT, Ty}, Legal);

  // Extensions
  for (auto Ty : {s8, s16, s32}) {
    setAction({G_ZEXT, Ty}, Legal);
    setAction({G_SEXT, Ty}, Legal);
    setAction({G_ANYEXT, Ty}, Legal);
  }
  setAction({G_ANYEXT, s128}, Legal);

  // Comparison
  setAction({G_ICMP, s1}, Legal);

  for (auto Ty : {s8, s16, s32, p0})
    setAction({G_ICMP, 1, Ty}, Legal);

  // Merge/Unmerge
  for (const auto &Ty : {s16, s32, s64}) {
    setAction({G_MERGE_VALUES, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, 1, Ty}, Legal);
  }
  for (const auto &Ty : {s8, s16, s32}) {
    setAction({G_MERGE_VALUES, 1, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, Ty}, Legal);
  }
}

void X86LegalizerInfo::setLegalizerInfo64bit() {

  if (!Subtarget.is64Bit())
    return;

  const LLT p0 = LLT::pointer(0, TM.getPointerSizeInBits(0));
  const LLT s1 = LLT::scalar(1);
  const LLT s8 = LLT::scalar(8);
  const LLT s16 = LLT::scalar(16);
  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT s128 = LLT::scalar(128);

  setAction({G_IMPLICIT_DEF, s64}, Legal);
  // Need to have that, as tryFoldImplicitDef will create this pattern:
  // s128 = EXTEND (G_IMPLICIT_DEF s32/s64) -> s128 = G_IMPLICIT_DEF
  setAction({G_IMPLICIT_DEF, s128}, Legal);

  setAction({G_PHI, s64}, Legal);

  for (unsigned BinOp : {G_ADD, G_SUB, G_MUL, G_AND, G_OR, G_XOR})
    setAction({BinOp, s64}, Legal);

  for (unsigned MemOp : {G_LOAD, G_STORE})
    setAction({MemOp, s64}, Legal);

  // Pointer-handling
  setAction({G_GEP, 1, s64}, Legal);
  getActionDefinitionsBuilder(G_PTRTOINT)
      .legalForCartesianProduct({s1, s8, s16, s32, s64}, {p0})
      .maxScalar(0, s64)
      .widenScalarToNextPow2(0, /*Min*/ 8);
  getActionDefinitionsBuilder(G_INTTOPTR).legalFor({{p0, s64}});

  // Constants
  setAction({TargetOpcode::G_CONSTANT, s64}, Legal);

  // Extensions
  for (unsigned extOp : {G_ZEXT, G_SEXT, G_ANYEXT}) {
    setAction({extOp, s64}, Legal);
  }

  getActionDefinitionsBuilder(G_SITOFP)
    .legalForCartesianProduct({s32, s64})
      .clampScalar(1, s32, s64)
      .widenScalarToNextPow2(1)
      .clampScalar(0, s32, s64)
      .widenScalarToNextPow2(0);

  getActionDefinitionsBuilder(G_FPTOSI)
      .legalForCartesianProduct({s32, s64})
      .clampScalar(1, s32, s64)
      .widenScalarToNextPow2(0)
      .clampScalar(0, s32, s64)
      .widenScalarToNextPow2(1);

  // Comparison
  setAction({G_ICMP, 1, s64}, Legal);

  getActionDefinitionsBuilder(G_FCMP)
      .legalForCartesianProduct({s8}, {s32, s64})
      .clampScalar(0, s8, s8)
      .clampScalar(1, s32, s64)
      .widenScalarToNextPow2(1);

  // Shifts and SDIV
  getActionDefinitionsBuilder(
      {G_SHL, G_LSHR, G_ASHR, G_SDIV, G_SREM, G_UDIV, G_UREM})
      .legalFor({s8, s16, s32, s64})
      .clampScalar(0, s8, s64);

  // Merge/Unmerge
  setAction({G_MERGE_VALUES, s128}, Legal);
  setAction({G_UNMERGE_VALUES, 1, s128}, Legal);
  setAction({G_MERGE_VALUES, 1, s128}, Legal);
  setAction({G_UNMERGE_VALUES, s128}, Legal);
}

void X86LegalizerInfo::setLegalizerInfoSSE1() {
  if (!Subtarget.hasSSE1())
    return;

  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT v4s32 = LLT::vector(4, 32);
  const LLT v2s64 = LLT::vector(2, 64);

  for (unsigned BinOp : {G_FADD, G_FSUB, G_FMUL, G_FDIV})
    for (auto Ty : {s32, v4s32})
      setAction({BinOp, Ty}, Legal);

  for (unsigned MemOp : {G_LOAD, G_STORE})
    for (auto Ty : {v4s32, v2s64})
      setAction({MemOp, Ty}, Legal);

  // Constants
  setAction({TargetOpcode::G_FCONSTANT, s32}, Legal);

  // Merge/Unmerge
  for (const auto &Ty : {v4s32, v2s64}) {
    setAction({G_CONCAT_VECTORS, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, 1, Ty}, Legal);
  }
  setAction({G_MERGE_VALUES, 1, s64}, Legal);
  setAction({G_UNMERGE_VALUES, s64}, Legal);
}

void X86LegalizerInfo::setLegalizerInfoSSE2() {
  if (!Subtarget.hasSSE2())
    return;

  const LLT s32 = LLT::scalar(32);
  const LLT s64 = LLT::scalar(64);
  const LLT v16s8 = LLT::vector(16, 8);
  const LLT v8s16 = LLT::vector(8, 16);
  const LLT v4s32 = LLT::vector(4, 32);
  const LLT v2s64 = LLT::vector(2, 64);

  const LLT v32s8 = LLT::vector(32, 8);
  const LLT v16s16 = LLT::vector(16, 16);
  const LLT v8s32 = LLT::vector(8, 32);
  const LLT v4s64 = LLT::vector(4, 64);

  for (unsigned BinOp : {G_FADD, G_FSUB, G_FMUL, G_FDIV})
    for (auto Ty : {s64, v2s64})
      setAction({BinOp, Ty}, Legal);

  for (unsigned BinOp : {G_ADD, G_SUB})
    for (auto Ty : {v16s8, v8s16, v4s32, v2s64})
      setAction({BinOp, Ty}, Legal);

  setAction({G_MUL, v8s16}, Legal);

  setAction({G_FPEXT, s64}, Legal);
  setAction({G_FPEXT, 1, s32}, Legal);

  setAction({G_FPTRUNC, s32}, Legal);
  setAction({G_FPTRUNC, 1, s64}, Legal);

  // Constants
  setAction({TargetOpcode::G_FCONSTANT, s64}, Legal);

  // Merge/Unmerge
  for (const auto &Ty :
       {v16s8, v32s8, v8s16, v16s16, v4s32, v8s32, v2s64, v4s64}) {
    setAction({G_CONCAT_VECTORS, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, 1, Ty}, Legal);
  }
  for (const auto &Ty : {v16s8, v8s16, v4s32, v2s64}) {
    setAction({G_CONCAT_VECTORS, 1, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, Ty}, Legal);
  }
}

void X86LegalizerInfo::setLegalizerInfoSSE41() {
  if (!Subtarget.hasSSE41())
    return;

  const LLT v4s32 = LLT::vector(4, 32);

  setAction({G_MUL, v4s32}, Legal);
}

void X86LegalizerInfo::setLegalizerInfoAVX() {
  if (!Subtarget.hasAVX())
    return;

  const LLT v16s8 = LLT::vector(16, 8);
  const LLT v8s16 = LLT::vector(8, 16);
  const LLT v4s32 = LLT::vector(4, 32);
  const LLT v2s64 = LLT::vector(2, 64);

  const LLT v32s8 = LLT::vector(32, 8);
  const LLT v64s8 = LLT::vector(64, 8);
  const LLT v16s16 = LLT::vector(16, 16);
  const LLT v32s16 = LLT::vector(32, 16);
  const LLT v8s32 = LLT::vector(8, 32);
  const LLT v16s32 = LLT::vector(16, 32);
  const LLT v4s64 = LLT::vector(4, 64);
  const LLT v8s64 = LLT::vector(8, 64);

  for (unsigned MemOp : {G_LOAD, G_STORE})
    for (auto Ty : {v8s32, v4s64})
      setAction({MemOp, Ty}, Legal);

  for (auto Ty : {v32s8, v16s16, v8s32, v4s64}) {
    setAction({G_INSERT, Ty}, Legal);
    setAction({G_EXTRACT, 1, Ty}, Legal);
  }
  for (auto Ty : {v16s8, v8s16, v4s32, v2s64}) {
    setAction({G_INSERT, 1, Ty}, Legal);
    setAction({G_EXTRACT, Ty}, Legal);
  }
  // Merge/Unmerge
  for (const auto &Ty :
       {v32s8, v64s8, v16s16, v32s16, v8s32, v16s32, v4s64, v8s64}) {
    setAction({G_CONCAT_VECTORS, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, 1, Ty}, Legal);
  }
  for (const auto &Ty :
       {v16s8, v32s8, v8s16, v16s16, v4s32, v8s32, v2s64, v4s64}) {
    setAction({G_CONCAT_VECTORS, 1, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, Ty}, Legal);
  }
}

void X86LegalizerInfo::setLegalizerInfoAVX2() {
  if (!Subtarget.hasAVX2())
    return;

  const LLT v32s8 = LLT::vector(32, 8);
  const LLT v16s16 = LLT::vector(16, 16);
  const LLT v8s32 = LLT::vector(8, 32);
  const LLT v4s64 = LLT::vector(4, 64);

  const LLT v64s8 = LLT::vector(64, 8);
  const LLT v32s16 = LLT::vector(32, 16);
  const LLT v16s32 = LLT::vector(16, 32);
  const LLT v8s64 = LLT::vector(8, 64);

  for (unsigned BinOp : {G_ADD, G_SUB})
    for (auto Ty : {v32s8, v16s16, v8s32, v4s64})
      setAction({BinOp, Ty}, Legal);

  for (auto Ty : {v16s16, v8s32})
    setAction({G_MUL, Ty}, Legal);

  // Merge/Unmerge
  for (const auto &Ty : {v64s8, v32s16, v16s32, v8s64}) {
    setAction({G_CONCAT_VECTORS, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, 1, Ty}, Legal);
  }
  for (const auto &Ty : {v32s8, v16s16, v8s32, v4s64}) {
    setAction({G_CONCAT_VECTORS, 1, Ty}, Legal);
    setAction({G_UNMERGE_VALUES, Ty}, Legal);
  }
}

void X86LegalizerInfo::setLegalizerInfoAVX512() {
  if (!Subtarget.hasAVX512())
    return;

  const LLT v16s8 = LLT::vector(16, 8);
  const LLT v8s16 = LLT::vector(8, 16);
  const LLT v4s32 = LLT::vector(4, 32);
  const LLT v2s64 = LLT::vector(2, 64);

  const LLT v32s8 = LLT::vector(32, 8);
  const LLT v16s16 = LLT::vector(16, 16);
  const LLT v8s32 = LLT::vector(8, 32);
  const LLT v4s64 = LLT::vector(4, 64);

  const LLT v64s8 = LLT::vector(64, 8);
  const LLT v32s16 = LLT::vector(32, 16);
  const LLT v16s32 = LLT::vector(16, 32);
  const LLT v8s64 = LLT::vector(8, 64);

  for (unsigned BinOp : {G_ADD, G_SUB})
    for (auto Ty : {v16s32, v8s64})
      setAction({BinOp, Ty}, Legal);

  setAction({G_MUL, v16s32}, Legal);

  for (unsigned MemOp : {G_LOAD, G_STORE})
    for (auto Ty : {v16s32, v8s64})
      setAction({MemOp, Ty}, Legal);

  for (auto Ty : {v64s8, v32s16, v16s32, v8s64}) {
    setAction({G_INSERT, Ty}, Legal);
    setAction({G_EXTRACT, 1, Ty}, Legal);
  }
  for (auto Ty : {v32s8, v16s16, v8s32, v4s64, v16s8, v8s16, v4s32, v2s64}) {
    setAction({G_INSERT, 1, Ty}, Legal);
    setAction({G_EXTRACT, Ty}, Legal);
  }

  /************ VLX *******************/
  if (!Subtarget.hasVLX())
    return;

  for (auto Ty : {v4s32, v8s32})
    setAction({G_MUL, Ty}, Legal);
}

void X86LegalizerInfo::setLegalizerInfoAVX512DQ() {
  if (!(Subtarget.hasAVX512() && Subtarget.hasDQI()))
    return;

  const LLT v8s64 = LLT::vector(8, 64);

  setAction({G_MUL, v8s64}, Legal);

  /************ VLX *******************/
  if (!Subtarget.hasVLX())
    return;

  const LLT v2s64 = LLT::vector(2, 64);
  const LLT v4s64 = LLT::vector(4, 64);

  for (auto Ty : {v2s64, v4s64})
    setAction({G_MUL, Ty}, Legal);
}

void X86LegalizerInfo::setLegalizerInfoAVX512BW() {
  if (!(Subtarget.hasAVX512() && Subtarget.hasBWI()))
    return;

  const LLT v64s8 = LLT::vector(64, 8);
  const LLT v32s16 = LLT::vector(32, 16);

  for (unsigned BinOp : {G_ADD, G_SUB})
    for (auto Ty : {v64s8, v32s16})
      setAction({BinOp, Ty}, Legal);

  setAction({G_MUL, v32s16}, Legal);

  /************ VLX *******************/
  if (!Subtarget.hasVLX())
    return;

  const LLT v8s16 = LLT::vector(8, 16);
  const LLT v16s16 = LLT::vector(16, 16);

  for (auto Ty : {v8s16, v16s16})
    setAction({G_MUL, Ty}, Legal);
}
