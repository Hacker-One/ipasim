// SysTranslator.hpp

#ifndef IPASIM_SYS_TRANSLATOR_HPP
#define IPASIM_SYS_TRANSLATOR_HPP

#include "ipasim/DynamicLoader.hpp"
#include "ipasim/Emulator.hpp"
#include "ipasim/LoadedLibrary.hpp"

namespace ipasim {

class SysTranslator {
public:
  SysTranslator(DynamicLoader &Dyld, Emulator &Emu)
      : Dyld(Dyld), Emu(Emu), Running(false), Restart(false), Continue(false) {}
  void execute(LoadedLibrary *Lib);
  void execute(uint64_t Addr);
  void *translate(void *Addr);
  void handleTrampoline(void *Ret, void **Args, void *Data);
  template <typename... ArgTypes> void callBack(void *FP, ArgTypes... Args);
  template <typename... ArgTypes> void *callBackR(void *FP, ArgTypes... Args);

private:
  template <typename... Args>
  void call(const std::string &Lib, const std::string &Func,
            Args &&... Params) {
    LoadedLibrary *L = Dyld.load(Lib);
    uint64_t Addr = L->findSymbol(Dyld, Func);
    auto *Ptr = reinterpret_cast<void (*)(Args...)>(Addr);
    Ptr(std::forward<Args>(Params)...);
  }
  bool handleFetchProtMem(uc_mem_type Type, uint64_t Addr, int Size,
                          int64_t Value);
  void handleCode(uint64_t Addr, uint32_t Size);
  bool handleMemWrite(uc_mem_type Type, uint64_t Addr, int Size, int64_t Value);
  bool handleMemUnmapped(uc_mem_type Type, uint64_t Addr, int Size,
                         int64_t Value);
  void returnToKernel();
  void returnToEmulation();
  void continueOutsideEmulation(std::function<void()> Cont);

  DynamicLoader &Dyld;
  Emulator &Emu;
  std::stack<uint32_t> LRs; // stack of return addresses
  bool Running; // `true` iff the Unicorn Engine is emulating some code
  bool Restart, Continue;
  std::function<void()> Continuation;
};

class DynamicCaller {
public:
  DynamicCaller(Emulator &Emu) : Emu(Emu), RegId(UC_ARM_REG_R0) {}
  void loadArg(size_t Size);
  bool call(bool Returns, uint32_t Addr);
  template <size_t N> void call0(bool Returns, uint32_t Addr) {
    if (Returns)
      call1<N, true>(Addr);
    else
      call1<N, false>(Addr);
  }
  template <size_t N, bool Returns> void call1(uint32_t Addr) {
    call2<N, Returns>(Addr);
  }
  template <size_t N, bool Returns, typename... ArgTypes>
  void call2(uint32_t Addr, ArgTypes... Params) {
    if constexpr (N > 0)
      call2<N - 1, Returns, ArgTypes..., uint32_t>(Addr, Params...,
                                                   Args[Args.size() - N]);
    else
      call3<Returns, ArgTypes...>(Addr, Params...);
  }
  template <bool Returns, typename... ArgTypes>
  void call3(uint32_t Addr, ArgTypes... Params) {
    if constexpr (Returns) {
      uint32_t RetVal =
          reinterpret_cast<uint32_t (*)(ArgTypes...)>(Addr)(Params...);
      Emu.writeReg(UC_ARM_REG_R0, RetVal);
    } else
      reinterpret_cast<void (*)(ArgTypes...)>(Addr)(Params...);
  }

private:
  Emulator &Emu;
  uc_arm_reg RegId;
  std::vector<uint32_t> Args;
};

class DynamicBackCaller {
public:
  DynamicBackCaller(DynamicLoader &Dyld, Emulator &Emu, SysTranslator &Sys)
      : Dyld(Dyld), Emu(Emu), Sys(Sys) {}

  template <uc_arm_reg RegId> void pushArgs() {}
  template <uc_arm_reg RegId, typename... ArgTypes>
  void pushArgs(void *Arg, ArgTypes... Args) {
    using namespace ipasim;

    static_assert(UC_ARM_REG_R0 <= RegId && RegId <= UC_ARM_REG_R3,
                  "Callback has too many arguments.");
    Emu.writeReg(RegId, reinterpret_cast<uint32_t>(Arg));
    pushArgs<RegId + 1>(Args...);
  }
  template <typename... ArgTypes> void callBack(void *FP, ArgTypes... Args) {
    uint64_t Addr = reinterpret_cast<uint64_t>(FP);
    AddrInfo AI(Dyld.lookup(Addr));
    if (!dynamic_cast<LoadedDylib *>(AI.Lib)) {
      // Target load method is not inside any emulated Dylib, so it must be
      // native executable code and we can simply call it.
      reinterpret_cast<void (*)(ArgTypes...)>(FP)(Args...);
    } else {
      // Target load method is inside some emulated library.
      pushArgs<UC_ARM_REG_R0>(Args...);
      Sys.execute(Addr);
    }
  }
  template <typename... ArgTypes> void *callBackR(void *FP, ArgTypes... Args) {
    callBack(FP, Args...);

    // Fetch return value.
    return reinterpret_cast<void *>(Emu.readReg(UC_ARM_REG_R0));
  }

private:
  DynamicLoader &Dyld;
  Emulator &Emu;
  SysTranslator &Sys;
  std::vector<uint32_t> Args;
};

class TypeDecoder {
public:
  TypeDecoder(const char *T) : T(T) {}
  size_t getNextTypeSize();
  bool hasNext() { return *T; }

  static const size_t InvalidSize = -1;

private:
  const char *T;

  size_t getNextTypeSizeImpl();
};

template <typename... ArgTypes>
inline void SysTranslator::callBack(void *FP, ArgTypes... Args) {
  DynamicBackCaller(Dyld, Emu, *this).callBack<ArgTypes...>(FP, Args...);
}
template <typename... ArgTypes>
inline void *SysTranslator::callBackR(void *FP, ArgTypes... Args) {
  return DynamicBackCaller(Dyld, Emu, *this)
      .callBackR<ArgTypes...>(FP, Args...);
}

} // namespace ipasim

// !defined(IPASIM_SYS_TRANSLATOR_HPP)
#endif
