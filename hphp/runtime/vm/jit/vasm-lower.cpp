/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/vasm-lower.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/containers.h"
#include "hphp/runtime/vm/jit/phys-reg.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-unit.h"
#include "hphp/runtime/vm/jit/vasm-util.h"
#include "hphp/runtime/vm/jit/vasm-visit.h"

#include <folly/ScopeGuard.h>

namespace HPHP { namespace jit {

///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////

template<typename Lower>
void lower_impl(Vunit& unit, Vlabel b, size_t i, Lower lower) {
  vmodify(unit, b, i, [&] (Vout& v) { lower(v); return 1; });
}

template<class Inst>
void lower_vcall(Vunit& unit, Inst& inst, Vlabel b, size_t i) {
  auto& blocks = unit.blocks;
  auto const& vinstr = blocks[b].code[i];

  auto const is_vcall = vinstr.op == Vinstr::vcall;
  auto const vcall = vinstr.vcall_;
  auto const vinvoke = vinstr.vinvoke_;

  // We lower vinvoke in two phases, and `inst' is overwritten after the first
  // phase.  We need to save any of its parameters that we care about in the
  // second phase ahead of time.
  auto const& vargs = unit.vcallArgs[inst.args];
  auto const dests = unit.tuples[inst.d];
  auto const destType = inst.destType;

  auto const scratch = unit.makeScratchBlock();
  SCOPE_EXIT { unit.freeScratchBlock(scratch); };
  Vout v(unit, scratch, vinstr.irctx());

  // Push stack arguments, in reverse order. Push in pairs without padding
  // except for the last argument (pushed first) which should be padded if
  // there are an odd number of arguments.
  auto numArgs = vargs.stkArgs.size();
  int32_t const adjust = (numArgs & 0x1) ? sizeof(uintptr_t) : 0;
  if (adjust) {
    // Using InvalidReg below fails SSA checks and simplify pass, so just
    // push the arg twice. It's on the same cacheline and will actually
    // perform faster than an explicit lea.
    v << pushp{vargs.stkArgs[numArgs - 1], vargs.stkArgs[numArgs - 1]};
    --numArgs;
  }
  for (auto i = numArgs; i >= 2; i -= 2) {
    v << pushp{vargs.stkArgs[i - 1], vargs.stkArgs[i - 2]};
  }

  // Get the arguments in the proper registers.
  RegSet argRegs;
  auto doArgs = [&] (const VregList& srcs, PhysReg (*r)(size_t)) {
    VregList argDests;
    for (size_t i = 0, n = srcs.size(); i < n; ++i) {
      auto const reg = r(i);
      argDests.push_back(reg);
      argRegs |= reg;
    }
    if (argDests.size()) {
      v << copyargs{v.makeTuple(srcs),
                    v.makeTuple(std::move(argDests))};
    }
  };
  doArgs(vargs.indRetArgs, rarg_ind_ret);
  doArgs(vargs.args, rarg);
  doArgs(vargs.simdArgs, rarg_simd);

  // Emit the appropriate call instruction sequence.
  emitCall(v, inst.call, argRegs);

  // Handle fixup and unwind information.
  if (inst.fixup.isValid()) {
    v << syncpoint{inst.fixup};
  }

  if (!is_vcall) {
    auto& targets = vinvoke.targets;
    v << unwind{{targets[0], targets[1]}};

    // Insert an lea fixup for any stack args at the beginning of the catch
    // block.
    if (auto rspOffset = ((vargs.stkArgs.size() + 1) & ~1) *
                         sizeof(uintptr_t)) {
      auto& taken = unit.blocks[targets[1]].code;
      assertx(taken.front().op == Vinstr::landingpad ||
              taken.front().op == Vinstr::jmp);

      Vinstr vi { lea{rsp()[rspOffset], rsp()}, taken.front().irctx() };

      if (taken.front().op == Vinstr::jmp) {
        taken.insert(taken.begin(), vi);
      } else {
        taken.insert(taken.begin() + 1, vi);
      }
    }

    // Write out the code so far to the end of b.  Remaining code will be
    // emitted to the next block.
    vector_splice(blocks[b].code, i, 1, blocks[scratch].code);
  } else if (vcall.nothrow) {
    v << nothrow{};
  }
  // For vinvoke, `inst' is no longer valid after this point.

  // Copy the call result to the destination register(s).
  switch (destType) {
    case DestType::TV:
      static_assert(offsetof(TypedValue, m_data) == 0, "");
      static_assert(offsetof(TypedValue, m_type) == 8, "");

      if (dests.size() == 2) {
        v << copy2{rret(0), rret(1), dests[0], dests[1]};
      } else {
        // We have cases where we statically know the type but need the value
        // from native call.  Even if the type does not really need a register
        // (e.g., InitNull), a Vreg is still allocated in assignRegs(), so the
        // following assertion holds.
        assertx(dests.size() == 1);
        v << copy{rret(0), dests[0]};
      }
      break;

    case DestType::SIMD:
      static_assert(offsetof(TypedValue, m_data) == 0, "");
      static_assert(offsetof(TypedValue, m_type) == 8, "");
      assertx(dests.size() == 1);

      pack2(v, rret(0), rret(1), dests[0]);
      break;

    case DestType::SSA:
    case DestType::Byte:
      assertx(dests.size() == 1);
      assertx(dests[0].isValid());

      // Copy the single-register result to dests[0].
      v << copy{rret(0), dests[0]};
      break;

    case DestType::Dbl:
      // Copy the single-register result to dests[0].
      assertx(dests.size() == 1);
      assertx(dests[0].isValid());
      v << copy{rret_simd(0), dests[0]};
      break;

    case DestType::Indirect:
      // Already asserted above
      break;

    case DestType::None:
      assertx(dests.empty());
      break;
  }

  if (vargs.stkArgs.size() > 0) {
    auto const delta = safe_cast<int32_t>(
      vargs.stkArgs.size() * sizeof(uintptr_t) + adjust
    );
    v << lea{rsp()[delta], rsp()};
  }

  // Insert new instructions to the appropriate block.
  if (is_vcall) {
    vector_splice(blocks[b].code, i, 1, blocks[scratch].code);
  } else {
    vector_splice(blocks[vinvoke.targets[0]].code, 0, 0,
                  blocks[scratch].code);
  }
}

///////////////////////////////////////////////////////////////////////////////

template<typename Inst>
void lower(Vunit& unit, Inst& inst, Vlabel b, size_t i) {}

void lower(Vunit& unit, vcall& inst, Vlabel b, size_t i) {
  lower_vcall(unit, inst, b, i);
}
void lower(Vunit& unit, vinvoke& inst, Vlabel b, size_t i) {
  lower_vcall(unit, inst, b, i);
}

void lower(Vunit& unit, vcallarray& inst, Vlabel b, size_t i) {
  // vcallarray can only appear at the end of a block.
  assertx(i == unit.blocks[b].code.size() - 1);

  lower_impl(unit, b, i, [&] (Vout& v) {
    auto const& srcs = unit.tuples[inst.extraArgs];
    auto args = inst.args;
    auto dsts = jit::vector<Vreg>{};

    for (auto i = 0; i < srcs.size(); ++i) {
      dsts.emplace_back(rarg(i));
      args |= rarg(i);
    }

    v << copyargs{unit.makeTuple(srcs), unit.makeTuple(std::move(dsts))};
    v << callarray{inst.target, args};
    v << unwind{{inst.targets[0], inst.targets[1]}};
  });
}

void lower(Vunit& unit, defvmsp& inst, Vlabel b, size_t i) {
  unit.blocks[b].code[i] = copy{rvmsp(), inst.d};
}
void lower(Vunit& unit, syncvmsp& inst, Vlabel b, size_t i) {
  unit.blocks[b].code[i] = copy{inst.s, rvmsp()};
}

void lower(Vunit& unit, defvmretdata& inst, Vlabel b, size_t i) {
  unit.blocks[b].code[i] = copy{rret_data(), inst.data};
}
void lower(Vunit& unit, defvmrettype& inst, Vlabel b, size_t i) {
  unit.blocks[b].code[i] = copy{rret_type(), inst.type};
}
void lower(Vunit& unit, syncvmret& inst, Vlabel b, size_t i) {
  unit.blocks[b].code[i] = copy2{inst.data,   inst.type,
                                 rret_data(), rret_type()};
}

///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////

void vlower(Vunit& unit, Vlabel b, size_t i) {
  auto& inst = unit.blocks[b].code[i];

  switch (inst.op) {
#define O(name, ...)                    \
    case Vinstr::name:                  \
      lower(unit, inst.name##_, b, i);  \
      break;

    VASM_OPCODES
#undef O
  }
}

///////////////////////////////////////////////////////////////////////////////

}}
