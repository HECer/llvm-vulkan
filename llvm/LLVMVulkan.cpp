// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Experimental bulk-compute transforms. LLVM owns IR and all mutation.
#include "lv/compute.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Plugins/PassPlugin.h")
#include "llvm/Plugins/PassPlugin.h"
#else
#include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace llvm;

namespace {
#if LV_LLVM_HAS_OPTIONAL_PASS_MIXIN
template <typename T> using LVPassInfoMixin = OptionalPassInfoMixin<T>;
#else
template <typename T> using LVPassInfoMixin = PassInfoMixin<T>;
#endif

std::string setting(const char *Name, const char *Default) {
  const char *V = std::getenv(Name);
  return V && *V ? V : Default;
}

uint64_t numberSetting(const char *Name, uint64_t Default) {
  const auto V = setting(Name, "");
  if (V.empty())
    return Default;
  size_t End = 0;
  if (V.front() == '-')
    throw std::invalid_argument(std::string(Name) + " cannot be negative");
  const auto Result = std::stoull(V, &End);
  if (End != V.size())
    throw std::invalid_argument(std::string(Name) + " must be an integer");
  return Result;
}

bool enabled(const char *Name) {
  const auto V = setting(Name, "0");
  if (V == "1")
    return true;
  if (V == "0")
    return false;
  throw std::invalid_argument(std::string(Name) + " must be 0 or 1");
}

bool eligible(const Function &F) {
  return !F.isDeclaration() && !F.hasOptNone();
}

uint32_t index32(size_t Size) {
  if (Size > std::numeric_limits<uint32_t>::max())
    throw std::overflow_error("LLVM Vulkan graph exceeds 32-bit indexing");
  return static_cast<uint32_t>(Size);
}

// Compiler workers retain their device and shader pipelines across passes and
// modules. The last configuration for each backend is cached per thread; no
// context or command pool is shared unsafely across compiler worker threads.
lv::Compute &cachedCompute(const lv::Options &O) {
  struct Slot {
    lv::Options Configuration;
    std::unique_ptr<lv::Compute> Instance;
  };
  static thread_local Slot Host, Device;
  auto &Cache = O.backend == lv::Backend::Cpu ? Host : Device;
  const auto &Old = Cache.Configuration;
  if (!Cache.Instance || O.backend != Old.backend || O.device != Old.device ||
      O.shader_dir != Old.shader_dir || O.device_name != Old.device_name ||
      O.memory_limit_bytes != Old.memory_limit_bytes) {
    auto New = std::make_unique<lv::Compute>(O);
    Cache.Configuration = O;
    Cache.Instance = std::move(New);
  }
  return *Cache.Instance;
}

// Auto defaults to the scalar reference until a caller explicitly sets a
// measured crossover; it never claims that lavapipe is a physical GPU.
class Session {
  lv::Compute *Host = nullptr;
  lv::Compute *Vulkan = nullptr;
  std::string Mode;
  lv::Options Options;
  uint64_t Threshold;
  bool Verify;
  bool Trace;
  uint64_t GPUCalls = 0, CPUCalls = 0, Dispatches = 0;
  uint64_t Uploaded = 0, Downloaded = 0;

  void record(const lv::Compute &C) {
    if (&C == Vulkan)
      ++GPUCalls;
    else
      ++CPUCalls;
    const auto &Stats = C.last_stats();
    Dispatches += Stats.dispatches;
    Uploaded += Stats.bytes_uploaded;
    Downloaded += Stats.bytes_downloaded;
  }

public:
  Session()
      : Mode(setting("LV_BACKEND", "cpu")),
        Threshold(numberSetting("LV_MIN_ITEMS", 65536)),
        Verify(enabled("LV_VERIFY")), Trace(enabled("LV_TRACE")) {
    if (Mode != "cpu" && Mode != "vulkan" && Mode != "auto")
      throw std::invalid_argument("LV_BACKEND must be cpu, vulkan, or auto");
    const auto Device = setting("LV_DEVICE", "any");
    if (Device == "cpu")
      Options.device = lv::DevicePreference::Cpu;
    else if (Device == "gpu")
      Options.device = lv::DevicePreference::Gpu;
    else if (Device == "any")
      Options.device = lv::DevicePreference::Any;
    else
      throw std::invalid_argument("LV_DEVICE must be cpu, gpu, or any");
    Options.shader_dir = setting("LV_SHADER_DIR", "");
    Options.device_name = setting("LV_DEVICE_NAME", "");
    Options.memory_limit_bytes =
        numberSetting("LV_MEMORY_LIMIT_BYTES", 512ull * 1024 * 1024);
  }

  lv::Compute &cpu() {
    if (!Host) {
      auto O = Options;
      O.backend = lv::Backend::Cpu;
      Host = &cachedCompute(O);
    }
    return *Host;
  }

  lv::Compute &select(size_t Work) {
    // Auto deliberately needs an explicit policy opt-in in addition to size.
    if (Mode == "cpu" ||
        (Mode == "auto" && (!enabled("LV_AUTO_ENABLE") || Work < Threshold)))
      return cpu();
    if (!Vulkan) {
      Options.backend = lv::Backend::Vulkan;
      Vulkan = &cachedCompute(Options);
    }
    return *Vulkan;
  }

  std::vector<lv::FoldResult> fold(const std::vector<lv::FoldRequest> &R) {
    auto &C = select(R.size());
    auto Out = C.fold(R);
    record(C);
    if (Out.size() != R.size())
      throw std::runtime_error("fold result length mismatch");
    if (Verify && &C != &cpu()) {
      const auto Ref = cpu().fold(R);
      for (size_t I = 0; I < R.size(); ++I)
        if (Out[I].valid != Ref[I].valid ||
            (Out[I].valid && Out[I].value != Ref[I].value))
          throw std::runtime_error(
              "Vulkan fold disagrees with CPU before IR mutation");
    }
    return Out;
  }

  std::vector<lv::Fold64Result>
  fold64(const std::vector<lv::Fold64Request> &R) {
    auto &C = select(R.size());
    auto Out = C.fold64(R);
    record(C);
    if (Out.size() != R.size())
      throw std::runtime_error("64-bit fold result length mismatch");
    if (Verify && &C != &cpu()) {
      const auto Ref = cpu().fold64(R);
      for (size_t I = 0; I < R.size(); ++I)
        if (Out[I].valid != Ref[I].valid ||
            (Out[I].valid && Out[I].value != Ref[I].value))
          throw std::runtime_error(
              "Vulkan 64-bit fold disagrees with CPU before IR mutation");
    }
    return Out;
  }

  std::vector<uint32_t> reachable(const lv::Graph &G,
                                  const std::vector<uint32_t> &Seeds) {
    auto &C = select(G.edges.size() + Seeds.size());
    auto Out = C.reachable(G, Seeds);
    record(C);
    if (Out.size() != Seeds.size())
      throw std::runtime_error("reachability result length mismatch");
    if (Verify && &C != &cpu()) {
      if (Out != cpu().reachable(G, Seeds))
        throw std::runtime_error(
            "Vulkan reachability disagrees with CPU before IR mutation");
    }
    return Out;
  }

  std::vector<uint32_t> hash(const std::vector<uint32_t> &Words,
                             const std::vector<uint32_t> &Offsets) {
    auto &C = select(Words.size());
    auto Out = C.hash_records(Words, Offsets);
    record(C);
    if (Out.size() + 1 != Offsets.size())
      throw std::runtime_error("hash result length mismatch");
    if (Verify && &C != &cpu()) {
      if (Out != cpu().hash_records(Words, Offsets))
        throw std::runtime_error(
            "Vulkan hashing disagrees with CPU before IR mutation");
    }
    return Out;
  }

  void report(StringRef Pass, size_t Changed, size_t Work,
              std::chrono::steady_clock::time_point Start) const {
    if (!Trace)
      return;
    const auto Ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - Start)
                        .count();
    errs() << "[llvm-vulkan] pass=" << Pass << " requested=" << Mode
           << " device="
           << (Vulkan     ? Vulkan->device_info().name
               : CPUCalls ? "scalar-cpu"
                          : "none-no-work")
           << " work=" << Work << " changed=" << Changed << " total_ms=" << Ms
           << " vulkan_calls=" << GPUCalls << " cpu_calls=" << CPUCalls
           << " dispatches=" << Dispatches << " uploaded=" << Uploaded
           << " downloaded=" << Downloaded
           << " verification=" << (Verify ? "enabled" : "disabled") << '\n';
  }
};

bool makeRequest(Instruction &I, lv::Fold64Request &R) {
  // Integers wider than 64 bits, vectors, poison/undef, FP, pointers, and all
  // annotations with extra poison semantics remain with upstream LLVM.
  if (I.getNumOperands() != 2 || I.hasPoisonGeneratingFlags() ||
      I.hasPoisonGeneratingMetadata())
    return false;
  const auto *A = dyn_cast<ConstantInt>(I.getOperand(0));
  const auto *B = dyn_cast<ConstantInt>(I.getOperand(1));
  if (!A || !B || A->getType() != B->getType())
    return false;
  const auto Width = A->getBitWidth();
  if (Width == 0 || Width > 64)
    return false;

  lv::FoldOp Op;
  switch (I.getOpcode()) {
  case Instruction::Add:
    Op = lv::FoldOp::Add;
    break;
  case Instruction::Sub:
    Op = lv::FoldOp::Sub;
    break;
  case Instruction::Mul:
    Op = lv::FoldOp::Mul;
    break;
  case Instruction::And:
    Op = lv::FoldOp::And;
    break;
  case Instruction::Or:
    Op = lv::FoldOp::Or;
    break;
  case Instruction::Xor:
    Op = lv::FoldOp::Xor;
    break;
  case Instruction::Shl:
    Op = lv::FoldOp::Shl;
    break;
  case Instruction::LShr:
    Op = lv::FoldOp::LShr;
    break;
  case Instruction::AShr:
    Op = lv::FoldOp::AShr;
    break;
  case Instruction::UDiv:
    Op = lv::FoldOp::UDiv;
    break;
  case Instruction::SDiv:
    Op = lv::FoldOp::SDiv;
    break;
  case Instruction::URem:
    Op = lv::FoldOp::URem;
    break;
  case Instruction::SRem:
    Op = lv::FoldOp::SRem;
    break;
  case Instruction::ICmp:
    switch (cast<ICmpInst>(I).getPredicate()) {
    case ICmpInst::ICMP_EQ:
      Op = lv::FoldOp::Eq;
      break;
    case ICmpInst::ICMP_NE:
      Op = lv::FoldOp::Ne;
      break;
    case ICmpInst::ICMP_ULT:
      Op = lv::FoldOp::Ult;
      break;
    case ICmpInst::ICMP_ULE:
      Op = lv::FoldOp::Ule;
      break;
    case ICmpInst::ICMP_UGT:
      Op = lv::FoldOp::Ugt;
      break;
    case ICmpInst::ICMP_UGE:
      Op = lv::FoldOp::Uge;
      break;
    case ICmpInst::ICMP_SLT:
      Op = lv::FoldOp::Slt;
      break;
    case ICmpInst::ICMP_SLE:
      Op = lv::FoldOp::Sle;
      break;
    case ICmpInst::ICMP_SGT:
      Op = lv::FoldOp::Sgt;
      break;
    case ICmpInst::ICMP_SGE:
      Op = lv::FoldOp::Sge;
      break;
    default:
      return false;
    }
    break;
  default:
    return false;
  }
  R = {static_cast<uint32_t>(Op), Width, A->getZExtValue(), B->getZExtValue()};
  return true;
}

class FoldPass : public LVPassInfoMixin<FoldPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    const auto Start = std::chrono::steady_clock::now();
    Session S;
    std::vector<Instruction *> Frontier;
    for (auto &F : M)
      if (eligible(F))
        for (auto &I : instructions(F)) {
          lv::Fold64Request R{};
          if (makeRequest(I, R))
            Frontier.push_back(&I);
        }
    size_t Changed = 0, Work = 0;
    const auto MaxRounds = numberSetting("LV_FOLD_MAX_ROUNDS", 64);
    for (uint64_t Round = 0; !Frontier.empty() && Round < MaxRounds; ++Round) {
      std::vector<lv::FoldRequest> Requests;
      std::vector<lv::Fold64Request> WideRequests;
      std::vector<Instruction *> Selected;
      std::vector<Instruction *> WideSelected;
      for (auto *I : Frontier) {
        lv::Fold64Request R{};
        if (makeRequest(*I, R)) {
          if (R.width <= 32) {
            Requests.push_back({R.op, R.width, static_cast<uint32_t>(R.lhs),
                                static_cast<uint32_t>(R.rhs)});
            Selected.push_back(I);
          } else {
            WideRequests.push_back(R);
            WideSelected.push_back(I);
          }
        }
      }
      if (Requests.empty() && WideRequests.empty())
        break;
      Work += Requests.size() + WideRequests.size();
      // Finish and verify both batches before consuming either. The 32-bit
      // kernel remains usable on devices without shaderInt64 when no wider
      // candidates exist. Explicit nonempty Vulkan i64 never falls back.
      const auto Results =
          Requests.empty() ? std::vector<lv::FoldResult>() : S.fold(Requests);
      const auto WideResults = WideRequests.empty()
                                   ? std::vector<lv::Fold64Result>()
                                   : S.fold64(WideRequests);
      SmallPtrSet<Instruction *, 32> Queued;
      std::vector<Instruction *> Next;
      auto Replace = [&](const std::vector<Instruction *> &Instructions,
                         const auto &Computed) {
        for (size_t J = 0; J < Instructions.size(); ++J) {
          if (!Computed[J].valid)
            continue;
          auto *I = Instructions[J];
          for (auto *U : I->users())
            if (auto *UI = dyn_cast<Instruction>(U))
              if (Queued.insert(UI).second)
                Next.push_back(UI);
          I->replaceAllUsesWith(
              ConstantInt::get(I->getType(), Computed[J].value));
          I->eraseFromParent();
          ++Changed;
        }
      };
      Replace(Selected, Results);
      Replace(WideSelected, WideResults);
      Frontier = std::move(Next);
    }
    S.report("fold", Changed, Work, Start);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

// This whitelist deliberately excludes memory, calls (including apparently
// pure calls), EH pads, atomics, fences and terminators. It handles dead cycles
// of pure SSA values without touching control dependence or LLVM's UB rules.
bool removablePure(const Instruction &I) {
  return isa<BinaryOperator>(I) || isa<UnaryOperator>(I) || isa<CastInst>(I) ||
         isa<CmpInst>(I) || isa<SelectInst>(I) || isa<PHINode>(I) ||
         isa<GetElementPtrInst>(I) || isa<ExtractElementInst>(I) ||
         isa<InsertElementInst>(I) || isa<ShuffleVectorInst>(I) ||
         isa<ExtractValueInst>(I) || isa<InsertValueInst>(I) ||
         isa<FreezeInst>(I);
}

class DCEPass : public LVPassInfoMixin<DCEPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    const auto Start = std::chrono::steady_clock::now();
    Session S;
    std::vector<Instruction *> Nodes;
    DenseMap<const Instruction *, uint32_t> Index;
    for (auto &F : M)
      if (eligible(F))
        for (auto &I : instructions(F)) {
          Index[&I] = index32(Nodes.size());
          Nodes.push_back(&I);
        }
    if (Nodes.empty())
      return PreservedAnalyses::all();
    lv::Graph G;
    std::vector<uint32_t> Roots;
    G.offsets.reserve(Nodes.size() + 1);
    for (auto *I : Nodes) {
      G.offsets.push_back(index32(G.edges.size()));
      Roots.push_back(removablePure(*I) ? 0u : 1u);
      for (const auto &Op : I->operands())
        if (auto *Def = dyn_cast<Instruction>(Op.get())) {
          auto It = Index.find(Def);
          if (It != Index.end())
            G.edges.push_back(It->second);
        }
    }
    G.offsets.push_back(index32(G.edges.size()));
    const auto Live = S.reachable(G, Roots);
    std::vector<Instruction *> Dead;
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (!Live[I])
        Dead.push_back(Nodes[I]);
    // Before dropping cyclic uses, retain debug information where LLVM can
    // express it. LLVM's own utility handles old and new debug records.
    for (auto *I : Dead)
      salvageDebugInfo(*I);
    for (auto *I : Dead)
      I->dropAllReferences();
    for (auto *I : Dead)
      I->eraseFromParent();
    S.report("dce", Dead.size(), Nodes.size() + G.edges.size(), Start);
    return Dead.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
  }
};

class CFGPass : public LVPassInfoMixin<CFGPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    const auto Start = std::chrono::steady_clock::now();
    Session S;
    std::vector<BasicBlock *> Nodes;
    DenseMap<const BasicBlock *, uint32_t> Index;
    size_t TerminatorsFolded = 0;
    for (auto &F : M)
      if (eligible(F))
        for (auto &BB : F) {
          // The device-evaluated integer predicates may now make a branch or
          // switch constant. LLVM performs the small, semantic CFG rewrite and
          // PHI repair; the bulk reachability analysis below runs on Vulkan.
          TerminatorsFolded += ConstantFoldTerminator(&BB, false) ? 1 : 0;
          Index[&BB] = index32(Nodes.size());
          Nodes.push_back(&BB);
        }
    if (Nodes.empty())
      return PreservedAnalyses::all();
    lv::Graph G;
    std::vector<uint32_t> Roots;
    for (auto *BB : Nodes) {
      G.offsets.push_back(index32(G.edges.size()));
      // Block addresses can escape the local CFG. Keeping them and their
      // successors is a conservative rule for indirectbr and callbr.
      Roots.push_back(BB->isEntryBlock() || BB->hasAddressTaken() ? 1u : 0u);
      for (auto *Succ : successors(BB))
        G.edges.push_back(Index.lookup(Succ));
    }
    G.offsets.push_back(index32(G.edges.size()));
    const auto Live = S.reachable(G, Roots);
    SmallVector<BasicBlock *, 32> Dead;
    for (size_t I = 0; I < Nodes.size(); ++I)
      if (!Live[I])
        Dead.push_back(Nodes[I]);
    const auto Count = Dead.size();
    // LLVM repairs successor PHIs and debug state; no custom IR surgery.
    if (!Dead.empty())
      DeleteDeadBlocks(Dead);
    S.report("cfg", Count + TerminatorsFolded, Nodes.size() + G.edges.size(),
             Start);
    return Count || TerminatorsFolded ? PreservedAnalyses::none()
                                      : PreservedAnalyses::all();
  }
};

// Hashes only nominate candidates. LLVM's exact semantic comparison and
// same-basic-block ordering are the authority for every replacement.
class CSEPass : public LVPassInfoMixin<CSEPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    const auto Start = std::chrono::steady_clock::now();
    Session S;
    std::vector<Instruction *> Nodes;
    std::vector<uint32_t> Words, Offsets;
    DenseMap<const Value *, uint32_t> ValueIDs;
    DenseMap<const Type *, uint32_t> TypeIDs;
    auto ValueID = [&](const Value *V) {
      auto It = ValueIDs.find(V);
      if (It != ValueIDs.end())
        return It->second;
      const auto ID = index32(ValueIDs.size());
      ValueIDs[V] = ID;
      return ID;
    };
    auto TypeID = [&](const Type *T) {
      auto It = TypeIDs.find(T);
      if (It != TypeIDs.end())
        return It->second;
      const auto ID = index32(TypeIDs.size());
      TypeIDs[T] = ID;
      return ID;
    };
    for (auto &F : M)
      if (eligible(F))
        for (auto &I : instructions(F)) {
          if (!removablePure(I) || isa<PHINode>(I) || isa<FreezeInst>(I) ||
              I.hasMetadataOtherThanDebugLoc() || I.hasPoisonGeneratingFlags())
            continue;
          Nodes.push_back(&I);
          Offsets.push_back(index32(Words.size()));
          Words.push_back(I.getOpcode());
          Words.push_back(TypeID(I.getType()));
          Words.push_back(I.getNumOperands());
          for (const auto &Op : I.operands())
            Words.push_back(ValueID(Op.get()));
        }
    if (Nodes.empty())
      return PreservedAnalyses::all();
    Offsets.push_back(index32(Words.size()));
    const auto Hashes = S.hash(Words, Offsets);
    const auto BucketLimit = numberSetting("LV_CSE_BUCKET_LIMIT", 64);
    BasicBlock *Current = nullptr;
    // LLVM DenseMap reserves two uint32_t key values; a hash can equal either.
    std::unordered_map<uint32_t, SmallVector<Instruction *, 4>> Buckets;
    size_t Changed = 0;
    for (size_t J = 0; J < Nodes.size(); ++J) {
      auto *I = Nodes[J];
      if (Current != I->getParent()) {
        Current = I->getParent();
        Buckets.clear();
      }
      auto &Candidates = Buckets[Hashes[J]];
      Instruction *Replacement = nullptr;
      for (auto *Previous : Candidates)
        if (I->isIdenticalTo(Previous)) {
          Replacement = Previous;
          break;
        }
      if (Replacement) {
        I->replaceAllUsesWith(Replacement);
        I->eraseFromParent();
        ++Changed;
      } else if (Candidates.size() < BucketLimit) {
        Candidates.push_back(I);
      }
    }
    S.report("cse", Changed, Words.size(), Start);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

// A boundary around every public pass converts runtime exceptions to clear,
// fatal compiler diagnostics. Explicit Vulkan requests never silently fall
// back.
template <typename P>
class CheckedPass : public LVPassInfoMixin<CheckedPass<P>> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    try {
      return P().run(M, AM);
    } catch (const std::exception &E) {
      report_fatal_error(Twine("LLVM Vulkan: ") + E.what(), false);
    }
  }
};

void addPipeline(ModulePassManager &MPM) {
  MPM.addPass(CheckedPass<FoldPass>());
  MPM.addPass(CheckedPass<CSEPass>());
  MPM.addPass(CheckedPass<CFGPass>());
  MPM.addPass(CheckedPass<DCEPass>());
}
} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "LLVMVulkan", "0.1.0-experimental",
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement> Inner) {
                  if (!Inner.empty())
                    return false;
                  if (Name == "lv-fold")
                    MPM.addPass(CheckedPass<FoldPass>());
                  else if (Name == "lv-cse")
                    MPM.addPass(CheckedPass<CSEPass>());
                  else if (Name == "lv-dce")
                    MPM.addPass(CheckedPass<DCEPass>());
                  else if (Name == "lv-cfg")
                    MPM.addPass(CheckedPass<CFGPass>());
                  else if (Name == "lv-pipeline")
                    addPipeline(MPM);
                  else
                    return false;
                  return true;
                });
            // Loading a plugin alone does not alter a normal compilation. This
            // opt-in hook is useful for -fpass-plugin in clang and other LLVM
            // frontends.
            if (setting("LV_AUTO_INSERT", "0") == "1") {
              PB.registerPipelineStartEPCallback(
                  [](ModulePassManager &MPM, OptimizationLevel) {
                    addPipeline(MPM);
                  });
            }
          }};
}
