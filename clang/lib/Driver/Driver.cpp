//===--- Driver.cpp - Clang GCC Compatible Driver -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#include "clang/Driver/Driver.h"
#include "InputInfo.h"
#include "ToolChains/AIX.h"
#include "ToolChains/AMDGPU.h"
#include "ToolChains/AMDGPUOpenMP.h"
#include "ToolChains/AVR.h"
#include "ToolChains/Ananas.h"
#include "ToolChains/BareMetal.h"
#include "ToolChains/Clang.h"
#include "ToolChains/CloudABI.h"
#include "ToolChains/Contiki.h"
#include "ToolChains/CrossWindows.h"
#include "ToolChains/Cuda.h"
#include "ToolChains/Darwin.h"
#include "ToolChains/DragonFly.h"
#include "ToolChains/FreeBSD.h"
#include "ToolChains/Fuchsia.h"
#include "ToolChains/Gnu.h"
#include "ToolChains/HIP.h"
#include "ToolChains/Haiku.h"
#include "ToolChains/Hexagon.h"
#include "ToolChains/Hurd.h"
#include "ToolChains/Lanai.h"
#include "ToolChains/Linux.h"
#include "ToolChains/MSP430.h"
#include "ToolChains/MSVC.h"
#include "ToolChains/MinGW.h"
#include "ToolChains/Minix.h"
#include "ToolChains/MipsLinux.h"
#include "ToolChains/Myriad.h"
#include "ToolChains/NaCl.h"
#include "ToolChains/NetBSD.h"
#include "ToolChains/OpenBSD.h"
#include "ToolChains/PPCLinux.h"
#include "ToolChains/PS4CPU.h"
#include "ToolChains/RISCVToolchain.h"
#include "ToolChains/SYCL.h"
#include "ToolChains/Solaris.h"
#include "ToolChains/TCE.h"
#include "ToolChains/VEToolchain.h"
#include "ToolChains/WebAssembly.h"
#include "ToolChains/XCore.h"
#include "ToolChains/ZOS.h"
#include "clang/Basic/TargetID.h"
#include "clang/Basic/Version.h"
#include "clang/Config/config.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptSpecifier.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ExitCodes.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <memory>
#include <utility>
#if LLVM_ON_UNIX
#include <unistd.h> // getpid
#endif

using namespace clang::driver;
using namespace clang;
using namespace llvm::opt;

static llvm::Triple getHIPOffloadTargetTriple() {
  static const llvm::Triple T("amdgcn-amd-amdhsa");
  return T;
}

// static
std::string Driver::GetResourcesPath(StringRef BinaryPath,
                                     StringRef CustomResourceDir) {
  // Since the resource directory is embedded in the module hash, it's important
  // that all places that need it call this function, so that they get the
  // exact same string ("a/../b/" and "b/" get different hashes, for example).

  // Dir is bin/ or lib/, depending on where BinaryPath is.
  std::string Dir = std::string(llvm::sys::path::parent_path(BinaryPath));

  SmallString<128> P(Dir);
  if (CustomResourceDir != "") {
    llvm::sys::path::append(P, CustomResourceDir);
  } else {
    // On Windows, libclang.dll is in bin/.
    // On non-Windows, libclang.so/.dylib is in lib/.
    // With a static-library build of libclang, LibClangPath will contain the
    // path of the embedding binary, which for LLVM binaries will be in bin/.
    // ../lib gets us to lib/ in both cases.
    P = llvm::sys::path::parent_path(Dir);
    llvm::sys::path::append(P, Twine("lib") + CLANG_LIBDIR_SUFFIX, "clang",
                            CLANG_VERSION_STRING);
  }

  return std::string(P.str());
}

Driver::Driver(StringRef ClangExecutable, StringRef TargetTriple,
               DiagnosticsEngine &Diags, std::string Title,
               IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS)
    : Diags(Diags), VFS(std::move(VFS)), Mode(GCCMode),
      SaveTemps(SaveTempsNone), BitcodeEmbed(EmbedNone), LTOMode(LTOK_None),
      ClangExecutable(ClangExecutable), SysRoot(DEFAULT_SYSROOT),
      DriverTitle(Title), CCPrintStatReportFilename(), CCPrintOptionsFilename(),
      CCPrintHeadersFilename(), CCLogDiagnosticsFilename(),
      CCCPrintBindings(false), CCPrintOptions(false), CCPrintHeaders(false),
      CCLogDiagnostics(false), CCGenDiagnostics(false),
      CCPrintProcessStats(false), TargetTriple(TargetTriple),
      CCCGenericGCCName(""), Saver(Alloc), CheckInputsExist(true),
      GenReproducer(false), SuppressMissingInputWarning(false) {
  // Provide a sane fallback if no VFS is specified.
  if (!this->VFS)
    this->VFS = llvm::vfs::getRealFileSystem();

  Name = std::string(llvm::sys::path::filename(ClangExecutable));
  Dir = std::string(llvm::sys::path::parent_path(ClangExecutable));
  InstalledDir = Dir; // Provide a sensible default installed dir.

  if ((!SysRoot.empty()) && llvm::sys::path::is_relative(SysRoot)) {
    // Prepend InstalledDir if SysRoot is relative
    SmallString<128> P(InstalledDir);
    llvm::sys::path::append(P, SysRoot);
    SysRoot = std::string(P);
  }

#if defined(CLANG_CONFIG_FILE_SYSTEM_DIR)
  SystemConfigDir = CLANG_CONFIG_FILE_SYSTEM_DIR;
#endif
#if defined(CLANG_CONFIG_FILE_USER_DIR)
  UserConfigDir = CLANG_CONFIG_FILE_USER_DIR;
#endif

  // Compute the path to the resource directory.
  ResourceDir = GetResourcesPath(ClangExecutable, CLANG_RESOURCE_DIR);
}

void Driver::ParseDriverMode(StringRef ProgramName,
                             ArrayRef<const char *> Args) {
  if (ClangNameParts.isEmpty())
    ClangNameParts = ToolChain::getTargetAndModeFromProgramName(ProgramName);
  setDriverModeFromOption(ClangNameParts.DriverMode);

  for (const char *ArgPtr : Args) {
    // Ignore nullptrs, they are the response file's EOL markers.
    if (ArgPtr == nullptr)
      continue;
    const StringRef Arg = ArgPtr;
    setDriverModeFromOption(Arg);
  }
}

void Driver::setDriverModeFromOption(StringRef Opt) {
  const std::string OptName =
      getOpts().getOption(options::OPT_driver_mode).getPrefixedName();
  if (!Opt.startswith(OptName))
    return;
  StringRef Value = Opt.drop_front(OptName.size());

  if (auto M = llvm::StringSwitch<llvm::Optional<DriverMode>>(Value)
                   .Case("gcc", GCCMode)
                   .Case("g++", GXXMode)
                   .Case("cpp", CPPMode)
                   .Case("cl", CLMode)
                   .Case("flang", FlangMode)
                   .Default(None))
    Mode = *M;
  else
    Diag(diag::err_drv_unsupported_option_argument) << OptName << Value;
}

InputArgList Driver::ParseArgStrings(ArrayRef<const char *> ArgStrings,
                                     bool IsClCompatMode,
                                     bool &ContainsError) {
  llvm::PrettyStackTraceString CrashInfo("Command line argument parsing");
  ContainsError = false;

  unsigned IncludedFlagsBitmask;
  unsigned ExcludedFlagsBitmask;
  std::tie(IncludedFlagsBitmask, ExcludedFlagsBitmask) =
      getIncludeExcludeOptionFlagMasks(IsClCompatMode);

  // Make sure that Flang-only options don't pollute the Clang output
  // TODO: Make sure that Clang-only options don't pollute Flang output
  if (!IsFlangMode())
    ExcludedFlagsBitmask |= options::FlangOnlyOption;

  unsigned MissingArgIndex, MissingArgCount;
  InputArgList Args =
      getOpts().ParseArgs(ArgStrings, MissingArgIndex, MissingArgCount,
                          IncludedFlagsBitmask, ExcludedFlagsBitmask);

  // Check for missing argument error.
  if (MissingArgCount) {
    Diag(diag::err_drv_missing_argument)
        << Args.getArgString(MissingArgIndex) << MissingArgCount;
    ContainsError |=
        Diags.getDiagnosticLevel(diag::err_drv_missing_argument,
                                 SourceLocation()) > DiagnosticsEngine::Warning;
  }

  // Check for unsupported options.
  for (const Arg *A : Args) {
    if (A->getOption().hasFlag(options::Unsupported)) {
      unsigned DiagID;
      auto ArgString = A->getAsString(Args);
      std::string Nearest;
      if (getOpts().findNearest(
            ArgString, Nearest, IncludedFlagsBitmask,
            ExcludedFlagsBitmask | options::Unsupported) > 1) {
        DiagID = diag::err_drv_unsupported_opt;
        Diag(DiagID) << ArgString;
      } else {
        DiagID = diag::err_drv_unsupported_opt_with_suggestion;
        Diag(DiagID) << ArgString << Nearest;
      }
      ContainsError |= Diags.getDiagnosticLevel(DiagID, SourceLocation()) >
                       DiagnosticsEngine::Warning;
      continue;
    }

    // Warn about -mcpu= without an argument.
    if (A->getOption().matches(options::OPT_mcpu_EQ) && A->containsValue("")) {
      Diag(diag::warn_drv_empty_joined_argument) << A->getAsString(Args);
      ContainsError |= Diags.getDiagnosticLevel(
                           diag::warn_drv_empty_joined_argument,
                           SourceLocation()) > DiagnosticsEngine::Warning;
    }
  }

  for (const Arg *A : Args.filtered(options::OPT_UNKNOWN)) {
    unsigned DiagID;
    auto ArgString = A->getAsString(Args);
    std::string Nearest;
    if (getOpts().findNearest(
          ArgString, Nearest, IncludedFlagsBitmask, ExcludedFlagsBitmask) > 1) {
      DiagID = IsCLMode() ? diag::warn_drv_unknown_argument_clang_cl
                          : diag::err_drv_unknown_argument;
      Diags.Report(DiagID) << ArgString;
    } else {
      DiagID = IsCLMode()
                   ? diag::warn_drv_unknown_argument_clang_cl_with_suggestion
                   : diag::err_drv_unknown_argument_with_suggestion;
      Diags.Report(DiagID) << ArgString << Nearest;
    }
    ContainsError |= Diags.getDiagnosticLevel(DiagID, SourceLocation()) >
                     DiagnosticsEngine::Warning;
  }

  return Args;
}

// Determine which compilation mode we are in. We look for options which
// affect the phase, starting with the earliest phases, and record which
// option we used to determine the final phase.
phases::ID Driver::getFinalPhase(const DerivedArgList &DAL,
                                 Arg **FinalPhaseArg) const {
  Arg *PhaseArg = nullptr;
  phases::ID FinalPhase;

  // -{E,EP,P,M,MM} only run the preprocessor.
  if (CCCIsCPP() || (PhaseArg = DAL.getLastArg(options::OPT_E)) ||
      (PhaseArg = DAL.getLastArg(options::OPT__SLASH_EP)) ||
      (PhaseArg = DAL.getLastArg(options::OPT_M, options::OPT_MM)) ||
      (PhaseArg = DAL.getLastArg(options::OPT__SLASH_P))) {
    FinalPhase = phases::Preprocess;

  // --precompile only runs up to precompilation.
  } else if ((PhaseArg = DAL.getLastArg(options::OPT__precompile))) {
    FinalPhase = phases::Precompile;

  // -{fsyntax-only,-analyze,emit-ast} only run up to the compiler.
  } else if ((PhaseArg = DAL.getLastArg(options::OPT_fsyntax_only)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_print_supported_cpus)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_module_file_info)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_verify_pch)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_rewrite_objc)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_rewrite_legacy_objc)) ||
             (PhaseArg = DAL.getLastArg(options::OPT__migrate)) ||
             (PhaseArg = DAL.getLastArg(options::OPT__analyze)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_emit_ast))) {
    FinalPhase = phases::Compile;

  // -S only runs up to the backend.
  } else if ((PhaseArg = DAL.getLastArg(options::OPT_S)) ||
             (PhaseArg = DAL.getLastArg(options::OPT_fsycl_device_only))) {
    FinalPhase = phases::Backend;

  // -c compilation only runs up to the assembler.
  } else if ((PhaseArg = DAL.getLastArg(options::OPT_c))) {
    FinalPhase = phases::Assemble;

  // Otherwise do everything.
  } else
    FinalPhase = phases::Link;

  if (FinalPhaseArg)
    *FinalPhaseArg = PhaseArg;

  return FinalPhase;
}

static Arg *MakeInputArg(DerivedArgList &Args, const OptTable &Opts,
                         StringRef Value, bool Claim = true) {
  Arg *A = new Arg(Opts.getOption(options::OPT_INPUT), Value,
                   Args.getBaseArgs().MakeIndex(Value), Value.data());
  Args.AddSynthesizedArg(A);
  if (Claim)
    A->claim();
  return A;
}

DerivedArgList *Driver::TranslateInputArgs(const InputArgList &Args) const {
  const llvm::opt::OptTable &Opts = getOpts();
  DerivedArgList *DAL = new DerivedArgList(Args);

  bool HasNostdlib = Args.hasArg(options::OPT_nostdlib);
  bool HasNostdlibxx = Args.hasArg(options::OPT_nostdlibxx);
  bool HasNodefaultlib = Args.hasArg(options::OPT_nodefaultlibs);
  for (Arg *A : Args) {
    // Unfortunately, we have to parse some forwarding options (-Xassembler,
    // -Xlinker, -Xpreprocessor) because we either integrate their functionality
    // (assembler and preprocessor), or bypass a previous driver ('collect2').

    // Rewrite linker options, to replace --no-demangle with a custom internal
    // option.
    if ((A->getOption().matches(options::OPT_Wl_COMMA) ||
         A->getOption().matches(options::OPT_Xlinker)) &&
        A->containsValue("--no-demangle")) {
      // Add the rewritten no-demangle argument.
      DAL->AddFlagArg(A, Opts.getOption(options::OPT_Z_Xlinker__no_demangle));

      // Add the remaining values as Xlinker arguments.
      for (StringRef Val : A->getValues())
        if (Val != "--no-demangle")
          DAL->AddSeparateArg(A, Opts.getOption(options::OPT_Xlinker), Val);

      continue;
    }

    // Rewrite preprocessor options, to replace -Wp,-MD,FOO which is used by
    // some build systems. We don't try to be complete here because we don't
    // care to encourage this usage model.
    if (A->getOption().matches(options::OPT_Wp_COMMA) &&
        (A->getValue(0) == StringRef("-MD") ||
         A->getValue(0) == StringRef("-MMD"))) {
      // Rewrite to -MD/-MMD along with -MF.
      if (A->getValue(0) == StringRef("-MD"))
        DAL->AddFlagArg(A, Opts.getOption(options::OPT_MD));
      else
        DAL->AddFlagArg(A, Opts.getOption(options::OPT_MMD));
      if (A->getNumValues() == 2)
        DAL->AddSeparateArg(A, Opts.getOption(options::OPT_MF), A->getValue(1));
      continue;
    }

    // Rewrite reserved library names.
    if (A->getOption().matches(options::OPT_l)) {
      StringRef Value = A->getValue();

      // Rewrite unless -nostdlib is present.
      if (!HasNostdlib && !HasNodefaultlib && !HasNostdlibxx &&
          Value == "stdc++") {
        DAL->AddFlagArg(A, Opts.getOption(options::OPT_Z_reserved_lib_stdcxx));
        continue;
      }

      // Rewrite unconditionally.
      if (Value == "cc_kext") {
        DAL->AddFlagArg(A, Opts.getOption(options::OPT_Z_reserved_lib_cckext));
        continue;
      }
    }

    // Pick up inputs via the -- option.
    if (A->getOption().matches(options::OPT__DASH_DASH)) {
      A->claim();
      for (StringRef Val : A->getValues())
        DAL->append(MakeInputArg(*DAL, Opts, Val, false));
      continue;
    }

    if (A->getOption().matches(options::OPT_offload_lib_Group)) {
      if (!A->getNumValues()) {
        Diag(clang::diag::warn_drv_unused_argument) << A->getSpelling();
        continue;
      }
    }

    DAL->append(A);
  }

  // Enforce -static if -miamcu is present.
  if (Args.hasFlag(options::OPT_miamcu, options::OPT_mno_iamcu, false))
    DAL->AddFlagArg(0, Opts.getOption(options::OPT_static));

  // Use of -fintelfpga implies -g
  if (Args.hasArg(options::OPT_fintelfpga)) {
    // if any -gN option is provided, use that.
    if (Arg *A = Args.getLastArg(options::OPT_gN_Group))
      DAL->append(A);
    else
      DAL->AddFlagArg(0, Opts.getOption(options::OPT_g_Flag));
  }

// Add a default value of -mlinker-version=, if one was given and the user
// didn't specify one.
#if defined(HOST_LINK_VERSION)
  if (!Args.hasArg(options::OPT_mlinker_version_EQ) &&
      strlen(HOST_LINK_VERSION) > 0) {
    DAL->AddJoinedArg(0, Opts.getOption(options::OPT_mlinker_version_EQ),
                      HOST_LINK_VERSION);
    DAL->getLastArg(options::OPT_mlinker_version_EQ)->claim();
  }
#endif

  return DAL;
}

/// Compute target triple from args.
///
/// This routine provides the logic to compute a target triple from various
/// args passed to the driver and the default triple string.
static llvm::Triple computeTargetTriple(const Driver &D,
                                        StringRef TargetTriple,
                                        const ArgList &Args,
                                        StringRef DarwinArchName = "") {
  // FIXME: Already done in Compilation *Driver::BuildCompilation
  if (const Arg *A = Args.getLastArg(options::OPT_target))
    TargetTriple = A->getValue();

  llvm::Triple Target(llvm::Triple::normalize(TargetTriple));

  // GNU/Hurd's triples should have been -hurd-gnu*, but were historically made
  // -gnu* only, and we can not change this, so we have to detect that case as
  // being the Hurd OS.
  if (TargetTriple.find("-unknown-gnu") != StringRef::npos ||
      TargetTriple.find("-pc-gnu") != StringRef::npos)
    Target.setOSName("hurd");

  // Handle Apple-specific options available here.
  if (Target.isOSBinFormatMachO()) {
    // If an explicit Darwin arch name is given, that trumps all.
    if (!DarwinArchName.empty()) {
      tools::darwin::setTripleTypeForMachOArchName(Target, DarwinArchName);
      return Target;
    }

    // Handle the Darwin '-arch' flag.
    if (Arg *A = Args.getLastArg(options::OPT_arch)) {
      StringRef ArchName = A->getValue();
      tools::darwin::setTripleTypeForMachOArchName(Target, ArchName);
    }
  }

  // Handle pseudo-target flags '-mlittle-endian'/'-EL' and
  // '-mbig-endian'/'-EB'.
  if (Arg *A = Args.getLastArg(options::OPT_mlittle_endian,
                               options::OPT_mbig_endian)) {
    if (A->getOption().matches(options::OPT_mlittle_endian)) {
      llvm::Triple LE = Target.getLittleEndianArchVariant();
      if (LE.getArch() != llvm::Triple::UnknownArch)
        Target = std::move(LE);
    } else {
      llvm::Triple BE = Target.getBigEndianArchVariant();
      if (BE.getArch() != llvm::Triple::UnknownArch)
        Target = std::move(BE);
    }
  }

  // Skip further flag support on OSes which don't support '-m32' or '-m64'.
  if (Target.getArch() == llvm::Triple::tce ||
      Target.getOS() == llvm::Triple::Minix)
    return Target;

  // On AIX, the env OBJECT_MODE may affect the resulting arch variant.
  if (Target.isOSAIX()) {
    if (Optional<std::string> ObjectModeValue =
            llvm::sys::Process::GetEnv("OBJECT_MODE")) {
      StringRef ObjectMode = *ObjectModeValue;
      llvm::Triple::ArchType AT = llvm::Triple::UnknownArch;

      if (ObjectMode.equals("64")) {
        AT = Target.get64BitArchVariant().getArch();
      } else if (ObjectMode.equals("32")) {
        AT = Target.get32BitArchVariant().getArch();
      } else {
        D.Diag(diag::err_drv_invalid_object_mode) << ObjectMode;
      }

      if (AT != llvm::Triple::UnknownArch && AT != Target.getArch())
        Target.setArch(AT);
    }
  }

  // Handle pseudo-target flags '-m64', '-mx32', '-m32' and '-m16'.
  Arg *A = Args.getLastArg(options::OPT_m64, options::OPT_mx32,
                           options::OPT_m32, options::OPT_m16);
  if (A) {
    llvm::Triple::ArchType AT = llvm::Triple::UnknownArch;

    if (A->getOption().matches(options::OPT_m64)) {
      AT = Target.get64BitArchVariant().getArch();
      if (Target.getEnvironment() == llvm::Triple::GNUX32)
        Target.setEnvironment(llvm::Triple::GNU);
      else if (Target.getEnvironment() == llvm::Triple::MuslX32)
        Target.setEnvironment(llvm::Triple::Musl);
    } else if (A->getOption().matches(options::OPT_mx32) &&
               Target.get64BitArchVariant().getArch() == llvm::Triple::x86_64) {
      AT = llvm::Triple::x86_64;
      if (Target.getEnvironment() == llvm::Triple::Musl)
        Target.setEnvironment(llvm::Triple::MuslX32);
      else
        Target.setEnvironment(llvm::Triple::GNUX32);
    } else if (A->getOption().matches(options::OPT_m32)) {
      AT = Target.get32BitArchVariant().getArch();
      if (Target.getEnvironment() == llvm::Triple::GNUX32)
        Target.setEnvironment(llvm::Triple::GNU);
      else if (Target.getEnvironment() == llvm::Triple::MuslX32)
        Target.setEnvironment(llvm::Triple::Musl);
    } else if (A->getOption().matches(options::OPT_m16) &&
               Target.get32BitArchVariant().getArch() == llvm::Triple::x86) {
      AT = llvm::Triple::x86;
      Target.setEnvironment(llvm::Triple::CODE16);
    }

    if (AT != llvm::Triple::UnknownArch && AT != Target.getArch())
      Target.setArch(AT);
  }

  // Handle -miamcu flag.
  if (Args.hasFlag(options::OPT_miamcu, options::OPT_mno_iamcu, false)) {
    if (Target.get32BitArchVariant().getArch() != llvm::Triple::x86)
      D.Diag(diag::err_drv_unsupported_opt_for_target) << "-miamcu"
                                                       << Target.str();

    if (A && !A->getOption().matches(options::OPT_m32))
      D.Diag(diag::err_drv_argument_not_allowed_with)
          << "-miamcu" << A->getBaseArg().getAsString(Args);

    Target.setArch(llvm::Triple::x86);
    Target.setArchName("i586");
    Target.setEnvironment(llvm::Triple::UnknownEnvironment);
    Target.setEnvironmentName("");
    Target.setOS(llvm::Triple::ELFIAMCU);
    Target.setVendor(llvm::Triple::UnknownVendor);
    Target.setVendorName("intel");
  }

  // If target is MIPS adjust the target triple
  // accordingly to provided ABI name.
  A = Args.getLastArg(options::OPT_mabi_EQ);
  if (A && Target.isMIPS()) {
    StringRef ABIName = A->getValue();
    if (ABIName == "32") {
      Target = Target.get32BitArchVariant();
      if (Target.getEnvironment() == llvm::Triple::GNUABI64 ||
          Target.getEnvironment() == llvm::Triple::GNUABIN32)
        Target.setEnvironment(llvm::Triple::GNU);
    } else if (ABIName == "n32") {
      Target = Target.get64BitArchVariant();
      if (Target.getEnvironment() == llvm::Triple::GNU ||
          Target.getEnvironment() == llvm::Triple::GNUABI64)
        Target.setEnvironment(llvm::Triple::GNUABIN32);
    } else if (ABIName == "64") {
      Target = Target.get64BitArchVariant();
      if (Target.getEnvironment() == llvm::Triple::GNU ||
          Target.getEnvironment() == llvm::Triple::GNUABIN32)
        Target.setEnvironment(llvm::Triple::GNUABI64);
    }
  }

  // If target is RISC-V adjust the target triple according to
  // provided architecture name
  A = Args.getLastArg(options::OPT_march_EQ);
  if (A && Target.isRISCV()) {
    StringRef ArchName = A->getValue();
    if (ArchName.startswith_insensitive("rv32"))
      Target.setArch(llvm::Triple::riscv32);
    else if (ArchName.startswith_insensitive("rv64"))
      Target.setArch(llvm::Triple::riscv64);
  }

  return Target;
}

// Parse the LTO options and record the type of LTO compilation
// based on which -f(no-)?lto(=.*)? or -f(no-)?offload-lto(=.*)?
// option occurs last.
static llvm::Optional<driver::LTOKind>
parseLTOMode(Driver &D, const llvm::opt::ArgList &Args, OptSpecifier OptPos,
             OptSpecifier OptNeg, OptSpecifier OptEq, bool IsOffload) {
  driver::LTOKind LTOMode = LTOK_None;
  // Non-offload LTO allows -flto=auto and -flto=jobserver. Offload LTO does
  // not support those options.
  if (!Args.hasFlag(OptPos, OptEq, OptNeg, false) &&
      (IsOffload ||
       (!Args.hasFlag(options::OPT_flto_EQ_auto, options::OPT_fno_lto, false) &&
        !Args.hasFlag(options::OPT_flto_EQ_jobserver, options::OPT_fno_lto,
                      false))))
    return None;

  StringRef LTOName("full");

  const Arg *A = Args.getLastArg(OptEq);
  if (A)
    LTOName = A->getValue();

  LTOMode = llvm::StringSwitch<LTOKind>(LTOName)
                .Case("full", LTOK_Full)
                .Case("thin", LTOK_Thin)
                .Default(LTOK_Unknown);

  if (LTOMode == LTOK_Unknown) {
    assert(A);
    D.Diag(diag::err_drv_unsupported_option_argument)
        << A->getOption().getName() << A->getValue();
    return None;
  }
  return LTOMode;
}

// Parse the LTO options.
void Driver::setLTOMode(const llvm::opt::ArgList &Args) {
  LTOMode = LTOK_None;
  if (auto M = parseLTOMode(*this, Args, options::OPT_flto,
                            options::OPT_fno_lto, options::OPT_flto_EQ,
                            /*IsOffload=*/false))
    LTOMode = M.getValue();

  OffloadLTOMode = LTOK_None;
  if (auto M = parseLTOMode(*this, Args, options::OPT_foffload_lto,
                            options::OPT_fno_offload_lto,
                            options::OPT_foffload_lto_EQ,
                            /*IsOffload=*/true))
    OffloadLTOMode = M.getValue();
}

/// Compute the desired OpenMP runtime from the flags provided.
Driver::OpenMPRuntimeKind Driver::getOpenMPRuntime(const ArgList &Args) const {
  StringRef RuntimeName(CLANG_DEFAULT_OPENMP_RUNTIME);

  const Arg *A = Args.getLastArg(options::OPT_fopenmp_EQ);
  if (A)
    RuntimeName = A->getValue();

  auto RT = llvm::StringSwitch<OpenMPRuntimeKind>(RuntimeName)
                .Case("libomp", OMPRT_OMP)
                .Case("libgomp", OMPRT_GOMP)
                .Case("libiomp5", OMPRT_IOMP5)
                .Default(OMPRT_Unknown);

  if (RT == OMPRT_Unknown) {
    if (A)
      Diag(diag::err_drv_unsupported_option_argument)
          << A->getOption().getName() << A->getValue();
    else
      // FIXME: We could use a nicer diagnostic here.
      Diag(diag::err_drv_unsupported_opt) << "-fopenmp";
  }

  return RT;
}

static bool isValidSYCLTriple(llvm::Triple T) {
  // NVPTX is valid for SYCL.
  if (T.isNVPTX())
    return true;

  // AMDGCN is valid for SYCL
  if (T.isAMDGCN())
    return true;

  // Check for invalid SYCL device triple values.
  // Non-SPIR arch.
  if (!T.isSPIR())
    return false;
  // SPIR arch, but has invalid SubArch for AOT.
  StringRef A(T.getArchName());
  if (T.getSubArch() == llvm::Triple::NoSubArch &&
      ((T.getArch() == llvm::Triple::spir && !A.equals("spir")) ||
       (T.getArch() == llvm::Triple::spir64 && !A.equals("spir64"))))
    return false;
  return true;
}

static void addSYCLDefaultTriple(Compilation &C,
                                 SmallVectorImpl<llvm::Triple> &SYCLTriples) {
  if (!C.getDriver().isSYCLDefaultTripleImplied())
    return;
  for (const auto &SYCLTriple : SYCLTriples) {
    if (SYCLTriple.getSubArch() == llvm::Triple::NoSubArch &&
        SYCLTriple.isSPIR())
      return;
    // If we encounter a known non-spir* target, do not add the default triple.
    if (SYCLTriple.isNVPTX() || SYCLTriple.isAMDGCN())
      return;
  }
  // Add the default triple as it was not found.
  llvm::Triple DefaultTriple = C.getDriver().MakeSYCLDeviceTriple("spir64");
  SYCLTriples.insert(SYCLTriples.begin(), DefaultTriple);
}

void Driver::CreateOffloadingDeviceToolChains(Compilation &C,
                                              InputList &Inputs) {

  //
  // CUDA/HIP
  //
  // We need to generate a CUDA/HIP toolchain if any of the inputs has a CUDA
  // or HIP type. However, mixed CUDA/HIP compilation is not supported.
  bool IsCuda =
      llvm::any_of(Inputs, [](std::pair<types::ID, const llvm::opt::Arg *> &I) {
        return types::isCuda(I.first);
      });
  bool IsHIP =
      llvm::any_of(Inputs,
                   [](std::pair<types::ID, const llvm::opt::Arg *> &I) {
                     return types::isHIP(I.first);
                   }) ||
      C.getInputArgs().hasArg(options::OPT_hip_link);
  if (IsCuda && IsHIP) {
    Diag(clang::diag::err_drv_mix_cuda_hip);
    return;
  }
  if (IsCuda) {
    const ToolChain *HostTC = C.getSingleOffloadToolChain<Action::OFK_Host>();
    const llvm::Triple &HostTriple = HostTC->getTriple();
    StringRef DeviceTripleStr;
    auto OFK = Action::OFK_Cuda;
    DeviceTripleStr =
        HostTriple.isArch64Bit() ? "nvptx64-nvidia-cuda" : "nvptx-nvidia-cuda";
    llvm::Triple CudaTriple(DeviceTripleStr);
    // Use the CUDA and host triples as the key into the
    // getOffloadingDeviceToolChain, because the device toolchain we
    // create depends on both.
    auto CudaTC = &getOffloadingDeviceToolChain(C.getInputArgs(), CudaTriple,
                                                *HostTC, OFK);
    C.addOffloadDeviceToolChain(CudaTC, OFK);
  } else if (IsHIP) {
    const ToolChain *HostTC = C.getSingleOffloadToolChain<Action::OFK_Host>();
    auto OFK = Action::OFK_HIP;
    llvm::Triple HIPTriple = getHIPOffloadTargetTriple();
    // Use the HIP and host triples as the key into
    // getOffloadingDeviceToolChain, because the device toolchain we create
    // depends on both.
    auto HIPTC = &getOffloadingDeviceToolChain(C.getInputArgs(), HIPTriple,
                                               *HostTC, OFK);
    C.addOffloadDeviceToolChain(HIPTC, OFK);
  }

  //
  // OpenMP
  //
  // We need to generate an OpenMP toolchain if the user specified targets with
  // the -fopenmp-targets option.
  if (Arg *OpenMPTargets =
          C.getInputArgs().getLastArg(options::OPT_fopenmp_targets_EQ)) {
    if (OpenMPTargets->getNumValues()) {
      // We expect that -fopenmp-targets is always used in conjunction with the
      // option -fopenmp specifying a valid runtime with offloading support,
      // i.e. libomp or libiomp.
      bool HasValidOpenMPRuntime = C.getInputArgs().hasFlag(
          options::OPT_fopenmp, options::OPT_fopenmp_EQ,
          options::OPT_fno_openmp, false);
      if (HasValidOpenMPRuntime) {
        OpenMPRuntimeKind OpenMPKind = getOpenMPRuntime(C.getInputArgs());
        HasValidOpenMPRuntime =
            OpenMPKind == OMPRT_OMP || OpenMPKind == OMPRT_IOMP5;
      }

      if (HasValidOpenMPRuntime) {
        llvm::StringMap<const char *> FoundNormalizedTriples;
        for (const char *Val : OpenMPTargets->getValues()) {
          llvm::Triple TT(Val);
          std::string NormalizedName = TT.normalize();

          // Make sure we don't have a duplicate triple.
          auto Duplicate = FoundNormalizedTriples.find(NormalizedName);
          if (Duplicate != FoundNormalizedTriples.end()) {
            Diag(clang::diag::warn_drv_omp_offload_target_duplicate)
                << Val << Duplicate->second;
            continue;
          }

          // Store the current triple so that we can check for duplicates in the
          // following iterations.
          FoundNormalizedTriples[NormalizedName] = Val;

          // If the specified target is invalid, emit a diagnostic.
          if (TT.getArch() == llvm::Triple::UnknownArch)
            Diag(clang::diag::err_drv_invalid_omp_target) << Val;
          else {
            const ToolChain *TC;
            // Device toolchains have to be selected differently. They pair host
            // and device in their implementation.
            if (TT.isNVPTX() || TT.isAMDGCN()) {
              const ToolChain *HostTC =
                  C.getSingleOffloadToolChain<Action::OFK_Host>();
              assert(HostTC && "Host toolchain should be always defined.");
              auto &DeviceTC =
                  ToolChains[TT.str() + "/" + HostTC->getTriple().normalize()];
              if (!DeviceTC) {
                if (TT.isNVPTX())
                  DeviceTC = std::make_unique<toolchains::CudaToolChain>(
                      *this, TT, *HostTC, C.getInputArgs(), Action::OFK_OpenMP);
                else if (TT.isAMDGCN())
                  DeviceTC =
                      std::make_unique<toolchains::AMDGPUOpenMPToolChain>(
                          *this, TT, *HostTC, C.getInputArgs());
                else
                  assert(DeviceTC && "Device toolchain not defined.");
              }

              TC = DeviceTC.get();
            } else
              TC = &getToolChain(C.getInputArgs(), TT);
            C.addOffloadDeviceToolChain(TC, Action::OFK_OpenMP);
          }
        }
      } else
        Diag(clang::diag::err_drv_expecting_fopenmp_with_fopenmp_targets);
    } else
      Diag(clang::diag::warn_drv_empty_joined_argument)
          << OpenMPTargets->getAsString(C.getInputArgs());
  }

  //
  // SYCL
  //
  // We need to generate a SYCL toolchain if the user specified targets with
  // the -fsycl-targets, -fsycl-add-targets or -fsycl-link-targets option.
  // If -fsycl is supplied without any of these we will assume SPIR-V.
  // Use of -fsycl-device-only overrides -fsycl.
  bool HasValidSYCLRuntime =
      C.getInputArgs().hasFlag(options::OPT_fsycl, options::OPT_fno_sycl,
                               false) ||
      C.getInputArgs().hasArg(options::OPT_fsycl_device_only);

  // A mechanism for retrieving SYCL-specific options, erroring out
  // if SYCL offloading wasn't enabled prior to that
  auto getArgRequiringSYCLRuntime = [&](OptSpecifier OptId) -> Arg * {
    Arg *SYCLArg = C.getInputArgs().getLastArg(OptId);
    if (SYCLArg && !HasValidSYCLRuntime) {
      Diag(clang::diag::err_drv_expecting_fsycl_with_sycl_opt)
          // Dropping the '=' symbol, which would otherwise pollute
          // the diagnostics for the most of options
          << SYCLArg->getSpelling().split('=').first;
      return nullptr;
    }
    return SYCLArg;
  };

  Arg *SYCLTargets = getArgRequiringSYCLRuntime(options::OPT_fsycl_targets_EQ);
  Arg *SYCLLinkTargets =
      getArgRequiringSYCLRuntime(options::OPT_fsycl_link_targets_EQ);
  Arg *SYCLAddTargets =
      getArgRequiringSYCLRuntime(options::OPT_fsycl_add_targets_EQ);
  Arg *SYCLLink = getArgRequiringSYCLRuntime(options::OPT_fsycl_link_EQ);
  Arg *SYCLfpga = getArgRequiringSYCLRuntime(options::OPT_fintelfpga);

  // -fsycl-targets cannot be used with -fsycl-link-targets
  if (SYCLTargets && SYCLLinkTargets)
    Diag(clang::diag::err_drv_option_conflict)
        << SYCLTargets->getSpelling() << SYCLLinkTargets->getSpelling();
  // -fsycl-link-targets and -fsycl-add-targets cannot be used together
  if (SYCLLinkTargets && SYCLAddTargets)
    Diag(clang::diag::err_drv_option_conflict)
        << SYCLLinkTargets->getSpelling() << SYCLAddTargets->getSpelling();
  // -fsycl-link-targets is not allowed with -fsycl-link
  if (SYCLLinkTargets && SYCLLink)
    Diag(clang::diag::err_drv_option_conflict)
        << SYCLLink->getSpelling() << SYCLLinkTargets->getSpelling();
  // -fsycl-targets cannot be used with -fintelfpga
  if (SYCLTargets && SYCLfpga)
    Diag(clang::diag::err_drv_option_conflict)
        << SYCLTargets->getSpelling() << SYCLfpga->getSpelling();
  // -ffreestanding cannot be used with -fsycl
  if (HasValidSYCLRuntime &&
      C.getInputArgs().hasArg(options::OPT_ffreestanding)) {
    Diag(clang::diag::err_drv_option_conflict) << "-fsycl"
                                               << "-ffreestanding";
  }

  bool HasSYCLTargetsOption = SYCLTargets || SYCLLinkTargets || SYCLAddTargets;
  llvm::StringMap<StringRef> FoundNormalizedTriples;
  llvm::SmallVector<llvm::Triple, 4> UniqueSYCLTriplesVec;
  if (HasSYCLTargetsOption) {
    // At this point, we know we have a valid combination
    // of -fsycl*target options passed
    Arg *SYCLTargetsValues = SYCLTargets ? SYCLTargets : SYCLLinkTargets;
    if (SYCLTargetsValues) {
      if (SYCLTargetsValues->getNumValues()) {
        for (StringRef Val : SYCLTargetsValues->getValues()) {
          llvm::Triple TT(MakeSYCLDeviceTriple(Val));
          if (!isValidSYCLTriple(TT)) {
            Diag(clang::diag::err_drv_invalid_sycl_target) << Val;
            continue;
          }
          std::string NormalizedName = TT.normalize();

          // Make sure we don't have a duplicate triple.
          auto Duplicate = FoundNormalizedTriples.find(NormalizedName);
          if (Duplicate != FoundNormalizedTriples.end()) {
            Diag(clang::diag::warn_drv_sycl_offload_target_duplicate)
                << Val << Duplicate->second;
            continue;
          }

          // Store the current triple so that we can check for duplicates in
          // the following iterations.
          FoundNormalizedTriples[NormalizedName] = Val;
          UniqueSYCLTriplesVec.push_back(TT);
        }
        addSYCLDefaultTriple(C, UniqueSYCLTriplesVec);
      } else
        Diag(clang::diag::warn_drv_empty_joined_argument)
            << SYCLTargetsValues->getAsString(C.getInputArgs());
    }
    // -fsycl-add-targets is a list of paired items (Triple and file) which are
    // gathered and used to be linked into the final device binary. This can
    // be used with -fsycl-targets to put together the final conglomerate binary
    if (SYCLAddTargets) {
      if (SYCLAddTargets->getNumValues()) {
        // Use of -fsycl-add-targets adds additional files to the SYCL device
        // link step.  Regular offload processing occurs below
        for (StringRef Val : SYCLAddTargets->getValues()) {
          // Parse out the Triple and Input (triple:binary) and create a
          // ToolChain for each entry.
          // The expected format is 'triple:file', any other format will
          // not be accepted.
          std::pair<StringRef, StringRef> I = Val.split(':');
          if (!I.first.empty() && !I.second.empty()) {
            llvm::Triple TT(I.first);
            if (!isValidSYCLTriple(TT)) {
              Diag(clang::diag::err_drv_invalid_sycl_target) << I.first;
              continue;
            }
            std::string NormalizedName = TT.normalize();

            // Make sure we don't have a duplicate triple.
            auto Duplicate = FoundNormalizedTriples.find(NormalizedName);
            if (Duplicate != FoundNormalizedTriples.end())
              // The toolchain for this triple was already created
              continue;

            // Store the current triple so that we can check for duplicates in
            // the following iterations.
            FoundNormalizedTriples[NormalizedName] = Val;
            UniqueSYCLTriplesVec.push_back(TT);
          } else {
            // No colon found, do not use the input
            C.getDriver().Diag(diag::err_drv_unsupported_option_argument)
                << SYCLAddTargets->getOption().getName() << Val;
          }
        }
      } else
        Diag(clang::diag::warn_drv_empty_joined_argument)
            << SYCLAddTargets->getAsString(C.getInputArgs());
    }
  } else {
    // If -fsycl is supplied without -fsycl-*targets we will assume SPIR-V
    // unless -fintelfpga is supplied, which uses SPIR-V with fpga AOT.
    // For -fsycl-device-only, we also setup the implied triple as needed.
    StringRef SYCLTargetArch;
    if (C.getInputArgs().hasArg(options::OPT_fsycl_device_only))
      if (C.getDefaultToolChain().getTriple().getArch() == llvm::Triple::x86)
        SYCLTargetArch = "spir";
      else
        SYCLTargetArch = "spir64";
    else if (HasValidSYCLRuntime)
      // Triple for -fintelfpga is spir64_fpga-unknown-unknown-sycldevice.
      SYCLTargetArch = SYCLfpga ? "spir64_fpga" : "spir64";
    if (!SYCLTargetArch.empty()) {
      UniqueSYCLTriplesVec.push_back(MakeSYCLDeviceTriple(SYCLTargetArch));
      addSYCLDefaultTriple(C, UniqueSYCLTriplesVec);
    }
  }
  // We'll need to use the SYCL and host triples as the key into
  // getOffloadingDeviceToolChain, because the device toolchains we're
  // going to create will depend on both.
  const ToolChain *HostTC = C.getSingleOffloadToolChain<Action::OFK_Host>();
  for (auto &TT : UniqueSYCLTriplesVec) {
    auto SYCLTC = &getOffloadingDeviceToolChain(C.getInputArgs(), TT, *HostTC,
                                                Action::OFK_SYCL);
    C.addOffloadDeviceToolChain(SYCLTC, Action::OFK_SYCL);
  }

  //
  // TODO: Add support for other offloading programming models here.
  //
}

/// Looks the given directories for the specified file.
///
/// \param[out] FilePath File path, if the file was found.
/// \param[in]  Dirs Directories used for the search.
/// \param[in]  FileName Name of the file to search for.
/// \return True if file was found.
///
/// Looks for file specified by FileName sequentially in directories specified
/// by Dirs.
///
static bool searchForFile(SmallVectorImpl<char> &FilePath,
                          ArrayRef<StringRef> Dirs, StringRef FileName) {
  SmallString<128> WPath;
  for (const StringRef &Dir : Dirs) {
    if (Dir.empty())
      continue;
    WPath.clear();
    llvm::sys::path::append(WPath, Dir, FileName);
    llvm::sys::path::native(WPath);
    if (llvm::sys::fs::is_regular_file(WPath)) {
      FilePath = std::move(WPath);
      return true;
    }
  }
  return false;
}

bool Driver::readConfigFile(StringRef FileName) {
  // Try reading the given file.
  SmallVector<const char *, 32> NewCfgArgs;
  if (!llvm::cl::readConfigFile(FileName, Saver, NewCfgArgs)) {
    Diag(diag::err_drv_cannot_read_config_file) << FileName;
    return true;
  }

  // Read options from config file.
  llvm::SmallString<128> CfgFileName(FileName);
  llvm::sys::path::native(CfgFileName);
  ConfigFile = std::string(CfgFileName);
  bool ContainErrors;
  CfgOptions = std::make_unique<InputArgList>(
      ParseArgStrings(NewCfgArgs, IsCLMode(), ContainErrors));
  if (ContainErrors) {
    CfgOptions.reset();
    return true;
  }

  if (CfgOptions->hasArg(options::OPT_config)) {
    CfgOptions.reset();
    Diag(diag::err_drv_nested_config_file);
    return true;
  }

  // Claim all arguments that come from a configuration file so that the driver
  // does not warn on any that is unused.
  for (Arg *A : *CfgOptions)
    A->claim();
  return false;
}

bool Driver::loadConfigFile() {
  std::string CfgFileName;
  bool FileSpecifiedExplicitly = false;

  // Process options that change search path for config files.
  if (CLOptions) {
    if (CLOptions->hasArg(options::OPT_config_system_dir_EQ)) {
      SmallString<128> CfgDir;
      CfgDir.append(
          CLOptions->getLastArgValue(options::OPT_config_system_dir_EQ));
      if (!CfgDir.empty()) {
        if (llvm::sys::fs::make_absolute(CfgDir).value() != 0)
          SystemConfigDir.clear();
        else
          SystemConfigDir = std::string(CfgDir.begin(), CfgDir.end());
      }
    }
    if (CLOptions->hasArg(options::OPT_config_user_dir_EQ)) {
      SmallString<128> CfgDir;
      CfgDir.append(
          CLOptions->getLastArgValue(options::OPT_config_user_dir_EQ));
      if (!CfgDir.empty()) {
        if (llvm::sys::fs::make_absolute(CfgDir).value() != 0)
          UserConfigDir.clear();
        else
          UserConfigDir = std::string(CfgDir.begin(), CfgDir.end());
      }
    }
  }

  // First try to find config file specified in command line.
  if (CLOptions) {
    std::vector<std::string> ConfigFiles =
        CLOptions->getAllArgValues(options::OPT_config);
    if (ConfigFiles.size() > 1) {
      if (!std::all_of(ConfigFiles.begin(), ConfigFiles.end(),
                       [ConfigFiles](const std::string &s) {
                         return s == ConfigFiles[0];
                       })) {
        Diag(diag::err_drv_duplicate_config);
        return true;
      }
    }

    if (!ConfigFiles.empty()) {
      CfgFileName = ConfigFiles.front();
      assert(!CfgFileName.empty());

      // If argument contains directory separator, treat it as a path to
      // configuration file.
      if (llvm::sys::path::has_parent_path(CfgFileName)) {
        SmallString<128> CfgFilePath;
        if (llvm::sys::path::is_relative(CfgFileName))
          llvm::sys::fs::current_path(CfgFilePath);
        llvm::sys::path::append(CfgFilePath, CfgFileName);
        if (!llvm::sys::fs::is_regular_file(CfgFilePath)) {
          Diag(diag::err_drv_config_file_not_exist) << CfgFilePath;
          return true;
        }
        return readConfigFile(CfgFilePath);
      }

      FileSpecifiedExplicitly = true;
    }
  }

  // If config file is not specified explicitly, try to deduce configuration
  // from executable name. For instance, an executable 'armv7l-clang' will
  // search for config file 'armv7l-clang.cfg'.
  if (CfgFileName.empty() && !ClangNameParts.TargetPrefix.empty())
    CfgFileName = ClangNameParts.TargetPrefix + '-' + ClangNameParts.ModeSuffix;

  if (CfgFileName.empty())
    return false;

  // Determine architecture part of the file name, if it is present.
  StringRef CfgFileArch = CfgFileName;
  size_t ArchPrefixLen = CfgFileArch.find('-');
  if (ArchPrefixLen == StringRef::npos)
    ArchPrefixLen = CfgFileArch.size();
  llvm::Triple CfgTriple;
  CfgFileArch = CfgFileArch.take_front(ArchPrefixLen);
  CfgTriple = llvm::Triple(llvm::Triple::normalize(CfgFileArch));
  if (CfgTriple.getArch() == llvm::Triple::ArchType::UnknownArch)
    ArchPrefixLen = 0;

  if (!StringRef(CfgFileName).endswith(".cfg"))
    CfgFileName += ".cfg";

  // If config file starts with architecture name and command line options
  // redefine architecture (with options like -m32 -LE etc), try finding new
  // config file with that architecture.
  SmallString<128> FixedConfigFile;
  size_t FixedArchPrefixLen = 0;
  if (ArchPrefixLen) {
    // Get architecture name from config file name like 'i386.cfg' or
    // 'armv7l-clang.cfg'.
    // Check if command line options changes effective triple.
    llvm::Triple EffectiveTriple = computeTargetTriple(*this,
                                             CfgTriple.getTriple(), *CLOptions);
    if (CfgTriple.getArch() != EffectiveTriple.getArch()) {
      FixedConfigFile = EffectiveTriple.getArchName();
      FixedArchPrefixLen = FixedConfigFile.size();
      // Append the rest of original file name so that file name transforms
      // like: i386-clang.cfg -> x86_64-clang.cfg.
      if (ArchPrefixLen < CfgFileName.size())
        FixedConfigFile += CfgFileName.substr(ArchPrefixLen);
    }
  }

  // Prepare list of directories where config file is searched for.
  StringRef CfgFileSearchDirs[] = {UserConfigDir, SystemConfigDir, Dir};

  // Try to find config file. First try file with corrected architecture.
  llvm::SmallString<128> CfgFilePath;
  if (!FixedConfigFile.empty()) {
    if (searchForFile(CfgFilePath, CfgFileSearchDirs, FixedConfigFile))
      return readConfigFile(CfgFilePath);
    // If 'x86_64-clang.cfg' was not found, try 'x86_64.cfg'.
    FixedConfigFile.resize(FixedArchPrefixLen);
    FixedConfigFile.append(".cfg");
    if (searchForFile(CfgFilePath, CfgFileSearchDirs, FixedConfigFile))
      return readConfigFile(CfgFilePath);
  }

  // Then try original file name.
  if (searchForFile(CfgFilePath, CfgFileSearchDirs, CfgFileName))
    return readConfigFile(CfgFilePath);

  // Finally try removing driver mode part: 'x86_64-clang.cfg' -> 'x86_64.cfg'.
  if (!ClangNameParts.ModeSuffix.empty() &&
      !ClangNameParts.TargetPrefix.empty()) {
    CfgFileName.assign(ClangNameParts.TargetPrefix);
    CfgFileName.append(".cfg");
    if (searchForFile(CfgFilePath, CfgFileSearchDirs, CfgFileName))
      return readConfigFile(CfgFilePath);
  }

  // Report error but only if config file was specified explicitly, by option
  // --config. If it was deduced from executable name, it is not an error.
  if (FileSpecifiedExplicitly) {
    Diag(diag::err_drv_config_file_not_found) << CfgFileName;
    for (const StringRef &SearchDir : CfgFileSearchDirs)
      if (!SearchDir.empty())
        Diag(diag::note_drv_config_file_searched_in) << SearchDir;
    return true;
  }

  return false;
}

Compilation *Driver::BuildCompilation(ArrayRef<const char *> ArgList) {
  llvm::PrettyStackTraceString CrashInfo("Compilation construction");

  // FIXME: Handle environment options which affect driver behavior, somewhere
  // (client?). GCC_EXEC_PREFIX, LPATH, CC_PRINT_OPTIONS.

  // We look for the driver mode option early, because the mode can affect
  // how other options are parsed.
  ParseDriverMode(ClangExecutable, ArgList.slice(1));

  // FIXME: What are we going to do with -V and -b?

  // Arguments specified in command line.
  bool ContainsError;
  CLOptions = std::make_unique<InputArgList>(
      ParseArgStrings(ArgList.slice(1), IsCLMode(), ContainsError));

  // Try parsing configuration file.
  if (!ContainsError)
    ContainsError = loadConfigFile();
  bool HasConfigFile = !ContainsError && (CfgOptions.get() != nullptr);

  // All arguments, from both config file and command line.
  InputArgList Args = std::move(HasConfigFile ? std::move(*CfgOptions)
                                              : std::move(*CLOptions));

  // The args for config files or /clang: flags belong to different InputArgList
  // objects than Args. This copies an Arg from one of those other InputArgLists
  // to the ownership of Args.
  auto appendOneArg = [&Args](const Arg *Opt, const Arg *BaseArg) {
    unsigned Index = Args.MakeIndex(Opt->getSpelling());
    Arg *Copy = new llvm::opt::Arg(Opt->getOption(), Args.getArgString(Index),
                                   Index, BaseArg);
    Copy->getValues() = Opt->getValues();
    if (Opt->isClaimed())
      Copy->claim();
    Copy->setOwnsValues(Opt->getOwnsValues());
    Opt->setOwnsValues(false);
    Args.append(Copy);
  };

  if (HasConfigFile)
    for (auto *Opt : *CLOptions) {
      if (Opt->getOption().matches(options::OPT_config))
        continue;
      const Arg *BaseArg = &Opt->getBaseArg();
      if (BaseArg == Opt)
        BaseArg = nullptr;
      appendOneArg(Opt, BaseArg);
    }

  // In CL mode, look for any pass-through arguments
  if (IsCLMode() && !ContainsError) {
    SmallVector<const char *, 16> CLModePassThroughArgList;
    for (const auto *A : Args.filtered(options::OPT__SLASH_clang)) {
      A->claim();
      CLModePassThroughArgList.push_back(A->getValue());
    }

    if (!CLModePassThroughArgList.empty()) {
      // Parse any pass through args using default clang processing rather
      // than clang-cl processing.
      auto CLModePassThroughOptions = std::make_unique<InputArgList>(
          ParseArgStrings(CLModePassThroughArgList, false, ContainsError));

      if (!ContainsError)
        for (auto *Opt : *CLModePassThroughOptions) {
          appendOneArg(Opt, nullptr);
        }
    }
  }

  // Check for working directory option before accessing any files
  if (Arg *WD = Args.getLastArg(options::OPT_working_directory))
    if (VFS->setCurrentWorkingDirectory(WD->getValue()))
      Diag(diag::err_drv_unable_to_set_working_directory) << WD->getValue();

  // FIXME: This stuff needs to go into the Compilation, not the driver.
  bool CCCPrintPhases;

  // Silence driver warnings if requested
  Diags.setIgnoreAllWarnings(Args.hasArg(options::OPT_w));

  // -no-canonical-prefixes is used very early in main.
  Args.ClaimAllArgs(options::OPT_no_canonical_prefixes);

  // f(no-)integated-cc1 is also used very early in main.
  Args.ClaimAllArgs(options::OPT_fintegrated_cc1);
  Args.ClaimAllArgs(options::OPT_fno_integrated_cc1);

  // Ignore -pipe.
  Args.ClaimAllArgs(options::OPT_pipe);

  // Extract -ccc args.
  //
  // FIXME: We need to figure out where this behavior should live. Most of it
  // should be outside in the client; the parts that aren't should have proper
  // options, either by introducing new ones or by overloading gcc ones like -V
  // or -b.
  CCCPrintPhases = Args.hasArg(options::OPT_ccc_print_phases);
  CCCPrintBindings = Args.hasArg(options::OPT_ccc_print_bindings);
  if (const Arg *A = Args.getLastArg(options::OPT_ccc_gcc_name))
    CCCGenericGCCName = A->getValue();
  GenReproducer = Args.hasFlag(options::OPT_gen_reproducer,
                               options::OPT_fno_crash_diagnostics,
                               !!::getenv("FORCE_CLANG_DIAGNOSTICS_CRASH"));

  // Process -fproc-stat-report options.
  if (const Arg *A = Args.getLastArg(options::OPT_fproc_stat_report_EQ)) {
    CCPrintProcessStats = true;
    CCPrintStatReportFilename = A->getValue();
  }
  if (Args.hasArg(options::OPT_fproc_stat_report))
    CCPrintProcessStats = true;

  // FIXME: TargetTriple is used by the target-prefixed calls to as/ld
  // and getToolChain is const.
  if (IsCLMode()) {
    // clang-cl targets MSVC-style Win32.
    llvm::Triple T(TargetTriple);
    T.setOS(llvm::Triple::Win32);
    T.setVendor(llvm::Triple::PC);
    T.setEnvironment(llvm::Triple::MSVC);
    T.setObjectFormat(llvm::Triple::COFF);
    TargetTriple = T.str();
  }
  if (const Arg *A = Args.getLastArg(options::OPT_target))
    TargetTriple = A->getValue();
  if (const Arg *A = Args.getLastArg(options::OPT_ccc_install_dir))
    Dir = InstalledDir = A->getValue();
  for (const Arg *A : Args.filtered(options::OPT_B)) {
    A->claim();
    PrefixDirs.push_back(A->getValue(0));
  }
  if (Optional<std::string> CompilerPathValue =
          llvm::sys::Process::GetEnv("COMPILER_PATH")) {
    StringRef CompilerPath = *CompilerPathValue;
    while (!CompilerPath.empty()) {
      std::pair<StringRef, StringRef> Split =
          CompilerPath.split(llvm::sys::EnvPathSeparator);
      PrefixDirs.push_back(std::string(Split.first));
      CompilerPath = Split.second;
    }
  }
  if (const Arg *A = Args.getLastArg(options::OPT__sysroot_EQ))
    SysRoot = A->getValue();
  if (const Arg *A = Args.getLastArg(options::OPT__dyld_prefix_EQ))
    DyldPrefix = A->getValue();

  if (const Arg *A = Args.getLastArg(options::OPT_resource_dir))
    ResourceDir = A->getValue();

  if (const Arg *A = Args.getLastArg(options::OPT_save_temps_EQ)) {
    SaveTemps = llvm::StringSwitch<SaveTempsMode>(A->getValue())
                    .Case("cwd", SaveTempsCwd)
                    .Case("obj", SaveTempsObj)
                    .Default(SaveTempsCwd);
  }

  setLTOMode(Args);

  // Process -fembed-bitcode= flags.
  if (Arg *A = Args.getLastArg(options::OPT_fembed_bitcode_EQ)) {
    StringRef Name = A->getValue();
    unsigned Model = llvm::StringSwitch<unsigned>(Name)
        .Case("off", EmbedNone)
        .Case("all", EmbedBitcode)
        .Case("bitcode", EmbedBitcode)
        .Case("marker", EmbedMarker)
        .Default(~0U);
    if (Model == ~0U) {
      Diags.Report(diag::err_drv_invalid_value) << A->getAsString(Args)
                                                << Name;
    } else
      BitcodeEmbed = static_cast<BitcodeEmbedMode>(Model);
  }

  std::unique_ptr<llvm::opt::InputArgList> UArgs =
      std::make_unique<InputArgList>(std::move(Args));

  // Perform the default argument translations.
  DerivedArgList *TranslatedArgs = TranslateInputArgs(*UArgs);

  // Owned by the host.
  const ToolChain &TC = getToolChain(
      *UArgs, computeTargetTriple(*this, TargetTriple, *UArgs));

  // The compilation takes ownership of Args.
  Compilation *C = new Compilation(*this, TC, UArgs.release(), TranslatedArgs,
                                   ContainsError);

  if (!HandleImmediateArgs(*C))
    return C;

  // Construct the list of inputs.
  InputList Inputs;
  BuildInputs(C->getDefaultToolChain(), *TranslatedArgs, Inputs);

  // Determine if there are any offload static libraries.
  if (checkForOffloadStaticLib(*C, *TranslatedArgs))
    setOffloadStaticLibSeen();

  // Check for any objects/archives that need to be compiled with the default
  // triple.
  if (checkForSYCLDefaultDevice(*C, *TranslatedArgs))
    setSYCLDefaultTriple(true);

  // Populate the tool chains for the offloading devices, if any.
  CreateOffloadingDeviceToolChains(*C, Inputs);

  // Determine FPGA emulation status.
  if (C->hasOffloadToolChain<Action::OFK_SYCL>()) {
    auto SYCLTCRange = C->getOffloadToolChains<Action::OFK_SYCL>();
    ArgStringList TargetArgs;
    const ToolChain *TC = SYCLTCRange.first->second;
    const toolchains::SYCLToolChain *SYCLTC =
        static_cast<const toolchains::SYCLToolChain *>(TC);
    SYCLTC->TranslateBackendTargetArgs(SYCLTC->getTriple(), *TranslatedArgs,
                                       TargetArgs);
    for (StringRef ArgString : TargetArgs) {
      if (ArgString.equals("-hardware") || ArgString.equals("-simulation")) {
        setFPGAEmulationMode(false);
        break;
      }
    }
  }

  // Construct the list of abstract actions to perform for this compilation. On
  // MachO targets this uses the driver-driver and universal actions.
  if (TC.getTriple().isOSBinFormatMachO())
    BuildUniversalActions(*C, C->getDefaultToolChain(), Inputs);
  else
    BuildActions(*C, C->getArgs(), Inputs, C->getActions());

  if (CCCPrintPhases) {
    PrintActions(*C);
    return C;
  }

  BuildJobs(*C);

  return C;
}

static void printArgList(raw_ostream &OS, const llvm::opt::ArgList &Args) {
  llvm::opt::ArgStringList ASL;
  for (const auto *A : Args)
    A->render(Args, ASL);

  for (auto I = ASL.begin(), E = ASL.end(); I != E; ++I) {
    if (I != ASL.begin())
      OS << ' ';
    llvm::sys::printArg(OS, *I, true);
  }
  OS << '\n';
}

bool Driver::getCrashDiagnosticFile(StringRef ReproCrashFilename,
                                    SmallString<128> &CrashDiagDir) {
  using namespace llvm::sys;
  assert(llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin() &&
         "Only knows about .crash files on Darwin");

  // The .crash file can be found on at ~/Library/Logs/DiagnosticReports/
  // (or /Library/Logs/DiagnosticReports for root) and has the filename pattern
  // clang-<VERSION>_<YYYY-MM-DD-HHMMSS>_<hostname>.crash.
  path::home_directory(CrashDiagDir);
  if (CrashDiagDir.startswith("/var/root"))
    CrashDiagDir = "/";
  path::append(CrashDiagDir, "Library/Logs/DiagnosticReports");
  int PID =
#if LLVM_ON_UNIX
      getpid();
#else
      0;
#endif
  std::error_code EC;
  fs::file_status FileStatus;
  TimePoint<> LastAccessTime;
  SmallString<128> CrashFilePath;
  // Lookup the .crash files and get the one generated by a subprocess spawned
  // by this driver invocation.
  for (fs::directory_iterator File(CrashDiagDir, EC), FileEnd;
       File != FileEnd && !EC; File.increment(EC)) {
    StringRef FileName = path::filename(File->path());
    if (!FileName.startswith(Name))
      continue;
    if (fs::status(File->path(), FileStatus))
      continue;
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> CrashFile =
        llvm::MemoryBuffer::getFile(File->path());
    if (!CrashFile)
      continue;
    // The first line should start with "Process:", otherwise this isn't a real
    // .crash file.
    StringRef Data = CrashFile.get()->getBuffer();
    if (!Data.startswith("Process:"))
      continue;
    // Parse parent process pid line, e.g: "Parent Process: clang-4.0 [79141]"
    size_t ParentProcPos = Data.find("Parent Process:");
    if (ParentProcPos == StringRef::npos)
      continue;
    size_t LineEnd = Data.find_first_of("\n", ParentProcPos);
    if (LineEnd == StringRef::npos)
      continue;
    StringRef ParentProcess = Data.slice(ParentProcPos+15, LineEnd).trim();
    int OpenBracket = -1, CloseBracket = -1;
    for (size_t i = 0, e = ParentProcess.size(); i < e; ++i) {
      if (ParentProcess[i] == '[')
        OpenBracket = i;
      if (ParentProcess[i] == ']')
        CloseBracket = i;
    }
    // Extract the parent process PID from the .crash file and check whether
    // it matches this driver invocation pid.
    int CrashPID;
    if (OpenBracket < 0 || CloseBracket < 0 ||
        ParentProcess.slice(OpenBracket + 1, CloseBracket)
            .getAsInteger(10, CrashPID) || CrashPID != PID) {
      continue;
    }

    // Found a .crash file matching the driver pid. To avoid getting an older
    // and misleading crash file, continue looking for the most recent.
    // FIXME: the driver can dispatch multiple cc1 invocations, leading to
    // multiple crashes poiting to the same parent process. Since the driver
    // does not collect pid information for the dispatched invocation there's
    // currently no way to distinguish among them.
    const auto FileAccessTime = FileStatus.getLastModificationTime();
    if (FileAccessTime > LastAccessTime) {
      CrashFilePath.assign(File->path());
      LastAccessTime = FileAccessTime;
    }
  }

  // If found, copy it over to the location of other reproducer files.
  if (!CrashFilePath.empty()) {
    EC = fs::copy_file(CrashFilePath, ReproCrashFilename);
    if (EC)
      return false;
    return true;
  }

  return false;
}

// When clang crashes, produce diagnostic information including the fully
// preprocessed source file(s).  Request that the developer attach the
// diagnostic information to a bug report.
void Driver::generateCompilationDiagnostics(
    Compilation &C, const Command &FailingCommand,
    StringRef AdditionalInformation, CompilationDiagnosticReport *Report) {
  if (C.getArgs().hasArg(options::OPT_fno_crash_diagnostics))
    return;

  // Don't try to generate diagnostics for link or dsymutil jobs.
  if (FailingCommand.getCreator().isLinkJob() ||
      FailingCommand.getCreator().isDsymutilJob())
    return;

  // Print the version of the compiler.
  PrintVersion(C, llvm::errs());

  // Suppress driver output and emit preprocessor output to temp file.
  Mode = CPPMode;
  CCGenDiagnostics = true;

  // Save the original job command(s).
  Command Cmd = FailingCommand;

  // Keep track of whether we produce any errors while trying to produce
  // preprocessed sources.
  DiagnosticErrorTrap Trap(Diags);

  // Suppress tool output.
  C.initCompilationForDiagnostics();

  // Construct the list of inputs.
  InputList Inputs;
  BuildInputs(C.getDefaultToolChain(), C.getArgs(), Inputs);

  for (InputList::iterator it = Inputs.begin(), ie = Inputs.end(); it != ie;) {
    bool IgnoreInput = false;

    // Ignore input from stdin or any inputs that cannot be preprocessed.
    // Check type first as not all linker inputs have a value.
    if (types::getPreprocessedType(it->first) == types::TY_INVALID) {
      IgnoreInput = true;
    } else if (!strcmp(it->second->getValue(), "-")) {
      Diag(clang::diag::note_drv_command_failed_diag_msg)
          << "Error generating preprocessed source(s) - "
             "ignoring input from stdin.";
      IgnoreInput = true;
    }

    if (IgnoreInput) {
      it = Inputs.erase(it);
      ie = Inputs.end();
    } else {
      ++it;
    }
  }

  if (Inputs.empty()) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating preprocessed source(s) - "
           "no preprocessable inputs.";
    return;
  }

  // Don't attempt to generate preprocessed files if multiple -arch options are
  // used, unless they're all duplicates.
  llvm::StringSet<> ArchNames;
  for (const Arg *A : C.getArgs()) {
    if (A->getOption().matches(options::OPT_arch)) {
      StringRef ArchName = A->getValue();
      ArchNames.insert(ArchName);
    }
  }
  if (ArchNames.size() > 1) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating preprocessed source(s) - cannot generate "
           "preprocessed source with multiple -arch options.";
    return;
  }

  // Construct the list of abstract actions to perform for this compilation. On
  // Darwin OSes this uses the driver-driver and builds universal actions.
  const ToolChain &TC = C.getDefaultToolChain();
  if (TC.getTriple().isOSBinFormatMachO())
    BuildUniversalActions(C, TC, Inputs);
  else
    BuildActions(C, C.getArgs(), Inputs, C.getActions());

  BuildJobs(C);

  // If there were errors building the compilation, quit now.
  if (Trap.hasErrorOccurred()) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating preprocessed source(s).";
    return;
  }

  // Generate preprocessed output.
  SmallVector<std::pair<int, const Command *>, 4> FailingCommands;
  C.ExecuteJobs(C.getJobs(), FailingCommands);

  // If any of the preprocessing commands failed, clean up and exit.
  if (!FailingCommands.empty()) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating preprocessed source(s).";
    return;
  }

  const TempFileList &TempFiles = C.getTempFiles();
  if (TempFiles.empty()) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating preprocessed source(s).";
    return;
  }

  Diag(clang::diag::note_drv_command_failed_diag_msg)
      << "\n********************\n\n"
         "PLEASE ATTACH THE FOLLOWING FILES TO THE BUG REPORT:\n"
         "Preprocessed source(s) and associated run script(s) are located at:";

  SmallString<128> VFS;
  SmallString<128> ReproCrashFilename;
  for (auto &TempFile : TempFiles) {
    Diag(clang::diag::note_drv_command_failed_diag_msg) << TempFile.first;
    if (Report)
      Report->TemporaryFiles.push_back(TempFile.first);
    if (ReproCrashFilename.empty()) {
      ReproCrashFilename = TempFile.first;
      llvm::sys::path::replace_extension(ReproCrashFilename, ".crash");
    }
    if (StringRef(TempFile.first).endswith(".cache")) {
      // In some cases (modules) we'll dump extra data to help with reproducing
      // the crash into a directory next to the output.
      VFS = llvm::sys::path::filename(TempFile.first);
      llvm::sys::path::append(VFS, "vfs", "vfs.yaml");
    }
  }

  // Assume associated files are based off of the first temporary file.
  CrashReportInfo CrashInfo(TempFiles[0].first, VFS);

  llvm::SmallString<128> Script(CrashInfo.Filename);
  llvm::sys::path::replace_extension(Script, "sh");
  std::error_code EC;
  llvm::raw_fd_ostream ScriptOS(Script, EC, llvm::sys::fs::CD_CreateNew,
                                llvm::sys::fs::FA_Write,
                                llvm::sys::fs::OF_Text);
  if (EC) {
    Diag(clang::diag::note_drv_command_failed_diag_msg)
        << "Error generating run script: " << Script << " " << EC.message();
  } else {
    ScriptOS << "# Crash reproducer for " << getClangFullVersion() << "\n"
             << "# Driver args: ";
    printArgList(ScriptOS, C.getInputArgs());
    ScriptOS << "# Original command: ";
    Cmd.Print(ScriptOS, "\n", /*Quote=*/true);
    Cmd.Print(ScriptOS, "\n", /*Quote=*/true, &CrashInfo);
    if (!AdditionalInformation.empty())
      ScriptOS << "\n# Additional information: " << AdditionalInformation
               << "\n";
    if (Report)
      Report->TemporaryFiles.push_back(std::string(Script.str()));
    Diag(clang::diag::note_drv_command_failed_diag_msg) << Script;
  }

  // On darwin, provide information about the .crash diagnostic report.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSDarwin()) {
    SmallString<128> CrashDiagDir;
    if (getCrashDiagnosticFile(ReproCrashFilename, CrashDiagDir)) {
      Diag(clang::diag::note_drv_command_failed_diag_msg)
          << ReproCrashFilename.str();
    } else { // Suggest a directory for the user to look for .crash files.
      llvm::sys::path::append(CrashDiagDir, Name);
      CrashDiagDir += "_<YYYY-MM-DD-HHMMSS>_<hostname>.crash";
      Diag(clang::diag::note_drv_command_failed_diag_msg)
          << "Crash backtrace is located in";
      Diag(clang::diag::note_drv_command_failed_diag_msg)
          << CrashDiagDir.str();
      Diag(clang::diag::note_drv_command_failed_diag_msg)
          << "(choose the .crash file that corresponds to your crash)";
    }
  }

  for (const auto &A : C.getArgs().filtered(options::OPT_frewrite_map_file_EQ))
    Diag(clang::diag::note_drv_command_failed_diag_msg) << A->getValue();

  Diag(clang::diag::note_drv_command_failed_diag_msg)
      << "\n\n********************";
}

void Driver::setUpResponseFiles(Compilation &C, Command &Cmd) {
  // Since commandLineFitsWithinSystemLimits() may underestimate system's
  // capacity if the tool does not support response files, there is a chance/
  // that things will just work without a response file, so we silently just
  // skip it.
  if (Cmd.getResponseFileSupport().ResponseKind ==
          ResponseFileSupport::RF_None ||
      llvm::sys::commandLineFitsWithinSystemLimits(Cmd.getExecutable(),
                                                   Cmd.getArguments()))
    return;

  std::string TmpName = GetTemporaryPath("response", "txt");
  Cmd.setResponseFile(C.addTempFile(C.getArgs().MakeArgString(TmpName)));
}

int Driver::ExecuteCompilation(
    Compilation &C,
    SmallVectorImpl<std::pair<int, const Command *>> &FailingCommands) {
  // Just print if -### was present.
  if (C.getArgs().hasArg(options::OPT__HASH_HASH_HASH)) {
    C.getJobs().Print(llvm::errs(), "\n", true);
    return 0;
  }

  // If there were errors building the compilation, quit now.
  if (Diags.hasErrorOccurred())
    return 1;

  // Set up response file names for each command, if necessary
  for (auto &Job : C.getJobs())
    setUpResponseFiles(C, Job);

  C.ExecuteJobs(C.getJobs(), FailingCommands);

  // If the command succeeded, we are done.
  if (FailingCommands.empty())
    return 0;

  // Otherwise, remove result files and print extra information about abnormal
  // failures.
  int Res = 0;
  for (const auto &CmdPair : FailingCommands) {
    int CommandRes = CmdPair.first;
    const Command *FailingCommand = CmdPair.second;

    // Remove result files if we're not saving temps.
    if (!isSaveTempsEnabled()) {
      const JobAction *JA = cast<JobAction>(&FailingCommand->getSource());
      C.CleanupFileMap(C.getResultFiles(), JA, true);

      // Failure result files are valid unless we crashed.
      if (CommandRes < 0)
        C.CleanupFileMap(C.getFailureResultFiles(), JA, true);
    }

#if LLVM_ON_UNIX
    // llvm/lib/Support/Unix/Signals.inc will exit with a special return code
    // for SIGPIPE. Do not print diagnostics for this case.
    if (CommandRes == EX_IOERR) {
      Res = CommandRes;
      continue;
    }
#endif

    // Print extra information about abnormal failures, if possible.
    //
    // This is ad-hoc, but we don't want to be excessively noisy. If the result
    // status was 1, assume the command failed normally. In particular, if it
    // was the compiler then assume it gave a reasonable error code. Failures
    // in other tools are less common, and they generally have worse
    // diagnostics, so always print the diagnostic there.
    const Tool &FailingTool = FailingCommand->getCreator();

    if (!FailingTool.hasGoodDiagnostics() || CommandRes != 1) {
      // FIXME: See FIXME above regarding result code interpretation.
      if (CommandRes < 0)
        Diag(clang::diag::err_drv_command_signalled)
            << FailingTool.getShortName();
      else
        Diag(clang::diag::err_drv_command_failed)
            << FailingTool.getShortName() << CommandRes;
    }

    auto CustomDiag = FailingCommand->getDiagForErrorCode(CommandRes);
    if (!CustomDiag.empty())
      Diag(clang::diag::note_drv_command_failed_diag_msg) << CustomDiag;
  }
  return Res;
}

void Driver::PrintHelp(bool ShowHidden) const {
  unsigned IncludedFlagsBitmask;
  unsigned ExcludedFlagsBitmask;
  std::tie(IncludedFlagsBitmask, ExcludedFlagsBitmask) =
      getIncludeExcludeOptionFlagMasks(IsCLMode());

  ExcludedFlagsBitmask |= options::NoDriverOption;
  if (!ShowHidden)
    ExcludedFlagsBitmask |= HelpHidden;

  if (IsFlangMode())
    IncludedFlagsBitmask |= options::FlangOption;
  else
    ExcludedFlagsBitmask |= options::FlangOnlyOption;

  std::string Usage = llvm::formatv("{0} [options] file...", Name).str();
  getOpts().printHelp(llvm::outs(), Usage.c_str(), DriverTitle.c_str(),
                      IncludedFlagsBitmask, ExcludedFlagsBitmask,
                      /*ShowAllAliases=*/false);
}

llvm::Triple Driver::MakeSYCLDeviceTriple(StringRef TargetArch) const {
  SmallVector<StringRef, 5> SYCLAlias = {"spir", "spir64", "spir64_fpga",
                                         "spir64_x86_64", "spir64_gen"};
  if (std::find(SYCLAlias.begin(), SYCLAlias.end(), TargetArch) !=
      SYCLAlias.end()) {
    llvm::Triple TT;
    TT.setArchName(TargetArch);
    TT.setVendor(llvm::Triple::UnknownVendor);
    TT.setOS(llvm::Triple::UnknownOS);
    TT.setEnvironment(llvm::Triple::SYCLDevice);
    return TT;
  }
  return llvm::Triple(TargetArch);
}

// Print the help from any of the given tools which are used for AOT
// compilation for SYCL
void Driver::PrintSYCLToolHelp(const Compilation &C) const {
  SmallVector<std::tuple<llvm::Triple, StringRef, StringRef, StringRef>, 4>
      HelpArgs;
  // Populate the vector with the tools and help options
  if (Arg *A = C.getArgs().getLastArg(options::OPT_fsycl_help_EQ)) {
    StringRef AV(A->getValue());
    llvm::Triple T;
    if (AV == "gen" || AV == "all")
      HelpArgs.push_back(std::make_tuple(MakeSYCLDeviceTriple("spir64_gen"),
                                         "ocloc", "--help", ""));
    if (AV == "fpga" || AV == "all")
      HelpArgs.push_back(std::make_tuple(MakeSYCLDeviceTriple("spir64_fpga"),
                                         "aoc", "-help", "-sycl"));
    if (AV == "x86_64" || AV == "all")
      HelpArgs.push_back(std::make_tuple(MakeSYCLDeviceTriple("spir64_x86_64"),
                                         "opencl-aot", "--help", ""));
    if (HelpArgs.empty()) {
      C.getDriver().Diag(diag::err_drv_unsupported_option_argument)
                         << A->getOption().getName() << AV;
      return;
    }
  }

  // Go through the args and emit the help information for each.
  for (auto &HA : HelpArgs) {
    llvm::outs() << "Emitting help information for " << std::get<1>(HA) << '\n'
        << "Use triple of '" << std::get<0>(HA).normalize() <<
        "' to enable ahead of time compilation\n";
    std::vector<StringRef> ToolArgs = {std::get<1>(HA), std::get<2>(HA),
                                       std::get<3>(HA)};
    SmallString<128> ExecPath(
        C.getDefaultToolChain().GetProgramPath(std::get<1>(HA).data()));
    auto ToolBinary = llvm::sys::findProgramByName(ExecPath);
    if (ToolBinary.getError()) {
      C.getDriver().Diag(diag::err_drv_command_failure) << ExecPath;
      continue;
    }
    // do not run the tools with -###.
    if (C.getArgs().hasArg(options::OPT__HASH_HASH_HASH)) {
      llvm::errs() << "\"" << ExecPath << "\" \"" << ToolArgs[1] << "\"";
      if (!ToolArgs[2].empty())
        llvm::errs() << " \"" << ToolArgs[2] << "\"";
      llvm::errs() << "\n";
      continue;
    }
    // Run the Tool.
    llvm::sys::ExecuteAndWait(ToolBinary.get(), ToolArgs);
  }
}

void Driver::PrintVersion(const Compilation &C, raw_ostream &OS) const {
  if (IsFlangMode()) {
    OS << getClangToolFullVersion("flang-new") << '\n';
  } else {
    // FIXME: The following handlers should use a callback mechanism, we don't
    // know what the client would like to do.
    OS << getClangFullVersion() << '\n';
  }
  const ToolChain &TC = C.getDefaultToolChain();
  OS << "Target: " << TC.getTripleString() << '\n';

  // Print the threading model.
  if (Arg *A = C.getArgs().getLastArg(options::OPT_mthread_model)) {
    // Don't print if the ToolChain would have barfed on it already
    if (TC.isThreadModelSupported(A->getValue()))
      OS << "Thread model: " << A->getValue();
  } else
    OS << "Thread model: " << TC.getThreadModel();
  OS << '\n';

  // Print out the install directory.
  OS << "InstalledDir: " << InstalledDir << '\n';

  // If configuration file was used, print its path.
  if (!ConfigFile.empty())
    OS << "Configuration file: " << ConfigFile << '\n';
}

/// PrintDiagnosticCategories - Implement the --print-diagnostic-categories
/// option.
static void PrintDiagnosticCategories(raw_ostream &OS) {
  // Skip the empty category.
  for (unsigned i = 1, max = DiagnosticIDs::getNumberOfCategories(); i != max;
       ++i)
    OS << i << ',' << DiagnosticIDs::getCategoryNameFromID(i) << '\n';
}

void Driver::HandleAutocompletions(StringRef PassedFlags) const {
  if (PassedFlags == "")
    return;
  // Print out all options that start with a given argument. This is used for
  // shell autocompletion.
  std::vector<std::string> SuggestedCompletions;
  std::vector<std::string> Flags;

  unsigned int DisableFlags =
      options::NoDriverOption | options::Unsupported | options::Ignored;

  // Make sure that Flang-only options don't pollute the Clang output
  // TODO: Make sure that Clang-only options don't pollute Flang output
  if (!IsFlangMode())
    DisableFlags |= options::FlangOnlyOption;

  // Distinguish "--autocomplete=-someflag" and "--autocomplete=-someflag,"
  // because the latter indicates that the user put space before pushing tab
  // which should end up in a file completion.
  const bool HasSpace = PassedFlags.endswith(",");

  // Parse PassedFlags by "," as all the command-line flags are passed to this
  // function separated by ","
  StringRef TargetFlags = PassedFlags;
  while (TargetFlags != "") {
    StringRef CurFlag;
    std::tie(CurFlag, TargetFlags) = TargetFlags.split(",");
    Flags.push_back(std::string(CurFlag));
  }

  // We want to show cc1-only options only when clang is invoked with -cc1 or
  // -Xclang.
  if (llvm::is_contained(Flags, "-Xclang") || llvm::is_contained(Flags, "-cc1"))
    DisableFlags &= ~options::NoDriverOption;

  const llvm::opt::OptTable &Opts = getOpts();
  StringRef Cur;
  Cur = Flags.at(Flags.size() - 1);
  StringRef Prev;
  if (Flags.size() >= 2) {
    Prev = Flags.at(Flags.size() - 2);
    SuggestedCompletions = Opts.suggestValueCompletions(Prev, Cur);
  }

  if (SuggestedCompletions.empty())
    SuggestedCompletions = Opts.suggestValueCompletions(Cur, "");

  // If Flags were empty, it means the user typed `clang [tab]` where we should
  // list all possible flags. If there was no value completion and the user
  // pressed tab after a space, we should fall back to a file completion.
  // We're printing a newline to be consistent with what we print at the end of
  // this function.
  if (SuggestedCompletions.empty() && HasSpace && !Flags.empty()) {
    llvm::outs() << '\n';
    return;
  }

  // When flag ends with '=' and there was no value completion, return empty
  // string and fall back to the file autocompletion.
  if (SuggestedCompletions.empty() && !Cur.endswith("=")) {
    // If the flag is in the form of "--autocomplete=-foo",
    // we were requested to print out all option names that start with "-foo".
    // For example, "--autocomplete=-fsyn" is expanded to "-fsyntax-only".
    SuggestedCompletions = Opts.findByPrefix(Cur, DisableFlags);

    // We have to query the -W flags manually as they're not in the OptTable.
    // TODO: Find a good way to add them to OptTable instead and them remove
    // this code.
    for (StringRef S : DiagnosticIDs::getDiagnosticFlags())
      if (S.startswith(Cur))
        SuggestedCompletions.push_back(std::string(S));
  }

  // Sort the autocomplete candidates so that shells print them out in a
  // deterministic order. We could sort in any way, but we chose
  // case-insensitive sorting for consistency with the -help option
  // which prints out options in the case-insensitive alphabetical order.
  llvm::sort(SuggestedCompletions, [](StringRef A, StringRef B) {
    if (int X = A.compare_insensitive(B))
      return X < 0;
    return A.compare(B) > 0;
  });

  llvm::outs() << llvm::join(SuggestedCompletions, "\n") << '\n';
}

bool Driver::HandleImmediateArgs(const Compilation &C) {
  // The order these options are handled in gcc is all over the place, but we
  // don't expect inconsistencies w.r.t. that to matter in practice.

  if (C.getArgs().hasArg(options::OPT_dumpmachine)) {
    llvm::outs() << C.getDefaultToolChain().getTripleString() << '\n';
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_dumpversion)) {
    // Since -dumpversion is only implemented for pedantic GCC compatibility, we
    // return an answer which matches our definition of __VERSION__.
    llvm::outs() << CLANG_VERSION_STRING << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT__print_diagnostic_categories)) {
    PrintDiagnosticCategories(llvm::outs());
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_help) ||
      C.getArgs().hasArg(options::OPT__help_hidden)) {
    PrintHelp(C.getArgs().hasArg(options::OPT__help_hidden));
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_fsycl_help_EQ)) {
    PrintSYCLToolHelp(C);
    return false;
  }

  if (C.getArgs().hasArg(options::OPT__version)) {
    // Follow gcc behavior and use stdout for --version and stderr for -v.
    PrintVersion(C, llvm::outs());
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_v) ||
      C.getArgs().hasArg(options::OPT__HASH_HASH_HASH) ||
      C.getArgs().hasArg(options::OPT_print_supported_cpus)) {
    PrintVersion(C, llvm::errs());
    SuppressMissingInputWarning = true;
  }

  if (C.getArgs().hasArg(options::OPT_v)) {
    if (!SystemConfigDir.empty())
      llvm::errs() << "System configuration file directory: "
                   << SystemConfigDir << "\n";
    if (!UserConfigDir.empty())
      llvm::errs() << "User configuration file directory: "
                   << UserConfigDir << "\n";
  }

  const ToolChain &TC = C.getDefaultToolChain();

  if (C.getArgs().hasArg(options::OPT_v))
    TC.printVerboseInfo(llvm::errs());

  if (C.getArgs().hasArg(options::OPT_print_resource_dir)) {
    llvm::outs() << ResourceDir << '\n';
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_search_dirs)) {
    llvm::outs() << "programs: =";
    bool separator = false;
    // Print -B and COMPILER_PATH.
    for (const std::string &Path : PrefixDirs) {
      if (separator)
        llvm::outs() << llvm::sys::EnvPathSeparator;
      llvm::outs() << Path;
      separator = true;
    }
    for (const std::string &Path : TC.getProgramPaths()) {
      if (separator)
        llvm::outs() << llvm::sys::EnvPathSeparator;
      llvm::outs() << Path;
      separator = true;
    }
    llvm::outs() << "\n";
    llvm::outs() << "libraries: =" << ResourceDir;

    StringRef sysroot = C.getSysRoot();

    for (const std::string &Path : TC.getFilePaths()) {
      // Always print a separator. ResourceDir was the first item shown.
      llvm::outs() << llvm::sys::EnvPathSeparator;
      // Interpretation of leading '=' is needed only for NetBSD.
      if (Path[0] == '=')
        llvm::outs() << sysroot << Path.substr(1);
      else
        llvm::outs() << Path;
    }
    llvm::outs() << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_runtime_dir)) {
    std::string CandidateRuntimePath = TC.getRuntimePath();
    if (getVFS().exists(CandidateRuntimePath))
      llvm::outs() << CandidateRuntimePath << '\n';
    else
      llvm::outs() << TC.getCompilerRTPath() << '\n';
    return false;
  }

  // FIXME: The following handlers should use a callback mechanism, we don't
  // know what the client would like to do.
  if (Arg *A = C.getArgs().getLastArg(options::OPT_print_file_name_EQ)) {
    llvm::outs() << GetFilePath(A->getValue(), TC) << "\n";
    return false;
  }

  if (Arg *A = C.getArgs().getLastArg(options::OPT_print_prog_name_EQ)) {
    StringRef ProgName = A->getValue();

    // Null program name cannot have a path.
    if (! ProgName.empty())
      llvm::outs() << GetProgramPath(ProgName, TC);

    llvm::outs() << "\n";
    return false;
  }

  if (Arg *A = C.getArgs().getLastArg(options::OPT_autocomplete)) {
    StringRef PassedFlags = A->getValue();
    HandleAutocompletions(PassedFlags);
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_libgcc_file_name)) {
    ToolChain::RuntimeLibType RLT = TC.GetRuntimeLibType(C.getArgs());
    const llvm::Triple Triple(TC.ComputeEffectiveClangTriple(C.getArgs()));
    RegisterEffectiveTriple TripleRAII(TC, Triple);
    switch (RLT) {
    case ToolChain::RLT_CompilerRT:
      llvm::outs() << TC.getCompilerRT(C.getArgs(), "builtins") << "\n";
      break;
    case ToolChain::RLT_Libgcc:
      llvm::outs() << GetFilePath("libgcc.a", TC) << "\n";
      break;
    }
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_multi_lib)) {
    for (const Multilib &Multilib : TC.getMultilibs())
      llvm::outs() << Multilib << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_multi_directory)) {
    const Multilib &Multilib = TC.getMultilib();
    if (Multilib.gccSuffix().empty())
      llvm::outs() << ".\n";
    else {
      StringRef Suffix(Multilib.gccSuffix());
      assert(Suffix.front() == '/');
      llvm::outs() << Suffix.substr(1) << "\n";
    }
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_target_triple)) {
    llvm::outs() << TC.getTripleString() << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_effective_triple)) {
    const llvm::Triple Triple(TC.ComputeEffectiveClangTriple(C.getArgs()));
    llvm::outs() << Triple.getTriple() << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_multiarch)) {
    llvm::outs() << TC.getMultiarchTriple(*this, TC.getTriple(), SysRoot)
                 << "\n";
    return false;
  }

  if (C.getArgs().hasArg(options::OPT_print_targets)) {
    llvm::TargetRegistry::printRegisteredTargetsForVersion(llvm::outs());
    return false;
  }

  return true;
}

enum {
  TopLevelAction = 0,
  HeadSibAction = 1,
  OtherSibAction = 2,
};

// Display an action graph human-readably.  Action A is the "sink" node
// and latest-occuring action. Traversal is in pre-order, visiting the
// inputs to each action before printing the action itself.
static unsigned PrintActions1(const Compilation &C, Action *A,
                              std::map<Action *, unsigned> &Ids,
                              Twine Indent = {}, int Kind = TopLevelAction) {
  if (Ids.count(A)) // A was already visited.
    return Ids[A];

  std::string str;
  llvm::raw_string_ostream os(str);

  auto getSibIndent = [](int K) -> Twine {
    return (K == HeadSibAction) ? "   " : (K == OtherSibAction) ? "|  " : "";
  };

  Twine SibIndent = Indent + getSibIndent(Kind);
  int SibKind = HeadSibAction;
  os << Action::getClassName(A->getKind()) << ", ";
  if (InputAction *IA = dyn_cast<InputAction>(A)) {
    os << "\"" << IA->getInputArg().getValue() << "\"";
  } else if (BindArchAction *BIA = dyn_cast<BindArchAction>(A)) {
    os << '"' << BIA->getArchName() << '"' << ", {"
       << PrintActions1(C, *BIA->input_begin(), Ids, SibIndent, SibKind) << "}";
  } else if (OffloadAction *OA = dyn_cast<OffloadAction>(A)) {
    bool IsFirst = true;
    OA->doOnEachDependence(
        [&](Action *A, const ToolChain *TC, const char *BoundArch) {
          assert(TC && "Unknown host toolchain");
          // E.g. for two CUDA device dependences whose bound arch is sm_20 and
          // sm_35 this will generate:
          // "cuda-device" (nvptx64-nvidia-cuda:sm_20) {#ID}, "cuda-device"
          // (nvptx64-nvidia-cuda:sm_35) {#ID}
          if (!IsFirst)
            os << ", ";
          os << '"';
          os << A->getOffloadingKindPrefix();
          os << " (";
          os << TC->getTriple().normalize();
          if (BoundArch)
            os << ":" << BoundArch;
          os << ")";
          os << '"';
          os << " {" << PrintActions1(C, A, Ids, SibIndent, SibKind) << "}";
          IsFirst = false;
          SibKind = OtherSibAction;
        });
  } else {
    const ActionList *AL = &A->getInputs();

    if (AL->size()) {
      const char *Prefix = "{";
      for (Action *PreRequisite : *AL) {
        os << Prefix << PrintActions1(C, PreRequisite, Ids, SibIndent, SibKind);
        Prefix = ", ";
        SibKind = OtherSibAction;
      }
      os << "}";
    } else
      os << "{}";
  }

  // Append offload info for all options other than the offloading action
  // itself (e.g. (cuda-device, sm_20) or (cuda-host)).
  std::string offload_str;
  llvm::raw_string_ostream offload_os(offload_str);
  if (!isa<OffloadAction>(A)) {
    auto S = A->getOffloadingKindPrefix();
    if (!S.empty()) {
      offload_os << ", (" << S;
      if (A->getOffloadingArch())
        offload_os << ", " << A->getOffloadingArch();
      offload_os << ")";
    }
  }

  auto getSelfIndent = [](int K) -> Twine {
    return (K == HeadSibAction) ? "+- " : (K == OtherSibAction) ? "|- " : "";
  };

  unsigned Id = Ids.size();
  Ids[A] = Id;
  llvm::errs() << Indent + getSelfIndent(Kind) << Id << ": " << os.str() << ", "
               << types::getTypeName(A->getType()) << offload_os.str() << "\n";

  return Id;
}

// Print the action graphs in a compilation C.
// For example "clang -c file1.c file2.c" is composed of two subgraphs.
void Driver::PrintActions(const Compilation &C) const {
  std::map<Action *, unsigned> Ids;
  for (Action *A : C.getActions())
    PrintActions1(C, A, Ids);
}

/// Check whether the given input tree contains any compilation or
/// assembly actions.
static bool ContainsCompileOrAssembleAction(const Action *A) {
  if (isa<CompileJobAction>(A) || isa<BackendJobAction>(A) ||
      isa<AssembleJobAction>(A))
    return true;

  for (const Action *Input : A->inputs())
    if (ContainsCompileOrAssembleAction(Input))
      return true;

  return false;
}

void Driver::BuildUniversalActions(Compilation &C, const ToolChain &TC,
                                   const InputList &BAInputs) const {
  DerivedArgList &Args = C.getArgs();
  ActionList &Actions = C.getActions();
  llvm::PrettyStackTraceString CrashInfo("Building universal build actions");
  // Collect the list of architectures. Duplicates are allowed, but should only
  // be handled once (in the order seen).
  llvm::StringSet<> ArchNames;
  SmallVector<const char *, 4> Archs;
  for (Arg *A : Args) {
    if (A->getOption().matches(options::OPT_arch)) {
      // Validate the option here; we don't save the type here because its
      // particular spelling may participate in other driver choices.
      llvm::Triple::ArchType Arch =
          tools::darwin::getArchTypeForMachOArchName(A->getValue());
      if (Arch == llvm::Triple::UnknownArch) {
        Diag(clang::diag::err_drv_invalid_arch_name) << A->getAsString(Args);
        continue;
      }

      A->claim();
      if (ArchNames.insert(A->getValue()).second)
        Archs.push_back(A->getValue());
    }
  }

  // When there is no explicit arch for this platform, make sure we still bind
  // the architecture (to the default) so that -Xarch_ is handled correctly.
  if (!Archs.size())
    Archs.push_back(Args.MakeArgString(TC.getDefaultUniversalArchName()));

  ActionList SingleActions;
  BuildActions(C, Args, BAInputs, SingleActions);

  // Add in arch bindings for every top level action, as well as lipo and
  // dsymutil steps if needed.
  for (Action* Act : SingleActions) {
    // Make sure we can lipo this kind of output. If not (and it is an actual
    // output) then we disallow, since we can't create an output file with the
    // right name without overwriting it. We could remove this oddity by just
    // changing the output names to include the arch, which would also fix
    // -save-temps. Compatibility wins for now.

    if (Archs.size() > 1 && !types::canLipoType(Act->getType()))
      Diag(clang::diag::err_drv_invalid_output_with_multiple_archs)
          << types::getTypeName(Act->getType());

    ActionList Inputs;
    for (unsigned i = 0, e = Archs.size(); i != e; ++i)
      Inputs.push_back(C.MakeAction<BindArchAction>(Act, Archs[i]));

    // Lipo if necessary, we do it this way because we need to set the arch flag
    // so that -Xarch_ gets overwritten.
    if (Inputs.size() == 1 || Act->getType() == types::TY_Nothing)
      Actions.append(Inputs.begin(), Inputs.end());
    else
      Actions.push_back(C.MakeAction<LipoJobAction>(Inputs, Act->getType()));

    // Handle debug info queries.
    Arg *A = Args.getLastArg(options::OPT_g_Group);
    bool enablesDebugInfo = A && !A->getOption().matches(options::OPT_g0) &&
                            !A->getOption().matches(options::OPT_gstabs);
    if ((enablesDebugInfo || willEmitRemarks(Args)) &&
        ContainsCompileOrAssembleAction(Actions.back())) {

      // Add a 'dsymutil' step if necessary, when debug info is enabled and we
      // have a compile input. We need to run 'dsymutil' ourselves in such cases
      // because the debug info will refer to a temporary object file which
      // will be removed at the end of the compilation process.
      if (Act->getType() == types::TY_Image) {
        ActionList Inputs;
        Inputs.push_back(Actions.back());
        Actions.pop_back();
        Actions.push_back(
            C.MakeAction<DsymutilJobAction>(Inputs, types::TY_dSYM));
      }

      // Verify the debug info output.
      if (Args.hasArg(options::OPT_verify_debug_info)) {
        Action* LastAction = Actions.back();
        Actions.pop_back();
        Actions.push_back(C.MakeAction<VerifyDebugInfoJobAction>(
            LastAction, types::TY_Nothing));
      }
    }
  }
}

bool Driver::DiagnoseInputExistence(const DerivedArgList &Args, StringRef Value,
                                    types::ID Ty, bool TypoCorrect) const {
  if (!getCheckInputsExist())
    return true;

  // stdin always exists.
  if (Value == "-")
    return true;

  if (getVFS().exists(Value))
    return true;

  if (IsCLMode()) {
    if (!llvm::sys::path::is_absolute(Twine(Value)) &&
        llvm::sys::Process::FindInEnvPath("LIB", Value, ';'))
      return true;

    if (Args.hasArg(options::OPT__SLASH_link) && Ty == types::TY_Object) {
      // Arguments to the /link flag might cause the linker to search for object
      // and library files in paths we don't know about. Don't error in such
      // cases.
      return true;
    }
  }

  if (TypoCorrect) {
    // Check if the filename is a typo for an option flag. OptTable thinks
    // that all args that are not known options and that start with / are
    // filenames, but e.g. `/diagnostic:caret` is more likely a typo for
    // the option `/diagnostics:caret` than a reference to a file in the root
    // directory.
    unsigned IncludedFlagsBitmask;
    unsigned ExcludedFlagsBitmask;
    std::tie(IncludedFlagsBitmask, ExcludedFlagsBitmask) =
        getIncludeExcludeOptionFlagMasks(IsCLMode());
    std::string Nearest;
    if (getOpts().findNearest(Value, Nearest, IncludedFlagsBitmask,
                              ExcludedFlagsBitmask) <= 1) {
      Diag(clang::diag::err_drv_no_such_file_with_suggestion)
          << Value << Nearest;
      return false;
    }
  }

  Diag(clang::diag::err_drv_no_such_file) << Value;
  return false;
}

// Construct a the list of inputs and their types.
void Driver::BuildInputs(const ToolChain &TC, DerivedArgList &Args,
                         InputList &Inputs) const {
  const llvm::opt::OptTable &Opts = getOpts();
  // Track the current user specified (-x) input. We also explicitly track the
  // argument used to set the type; we only want to claim the type when we
  // actually use it, so we warn about unused -x arguments.
  types::ID InputType = types::TY_Nothing;
  Arg *InputTypeArg = nullptr;
  bool IsSYCL =
      Args.hasFlag(options::OPT_fsycl, options::OPT_fno_sycl, false) ||
      Args.hasArg(options::OPT_fsycl_device_only);

  // The last /TC or /TP option sets the input type to C or C++ globally.
  if (Arg *TCTP = Args.getLastArgNoClaim(options::OPT__SLASH_TC,
                                         options::OPT__SLASH_TP)) {
    InputTypeArg = TCTP;
    InputType = TCTP->getOption().matches(options::OPT__SLASH_TC) && !IsSYCL
                    ? types::TY_C
                    : types::TY_CXX;

    Arg *Previous = nullptr;
    bool ShowNote = false;
    for (Arg *A :
         Args.filtered(options::OPT__SLASH_TC, options::OPT__SLASH_TP)) {
      if (Previous) {
        Diag(clang::diag::warn_drv_overriding_flag_option)
          << Previous->getSpelling() << A->getSpelling();
        ShowNote = true;
      }
      Previous = A;
    }
    if (ShowNote)
      Diag(clang::diag::note_drv_t_option_is_global);

    // No driver mode exposes -x and /TC or /TP; we don't support mixing them.
    assert(!Args.hasArg(options::OPT_x) && "-x and /TC or /TP is not allowed");
  }

  for (Arg *A : Args) {
    if (A->getOption().getKind() == Option::InputClass) {
      const char *Value = A->getValue();
      types::ID Ty = types::TY_INVALID;

      // Infer the input type if necessary.
      if (InputType == types::TY_Nothing) {
        // If there was an explicit arg for this, claim it.
        if (InputTypeArg)
          InputTypeArg->claim();

        types::ID CType = types::TY_C;
        // For SYCL, all source file inputs are considered C++.
        if (IsSYCL)
          CType = types::TY_CXX;

        // stdin must be handled specially.
        if (memcmp(Value, "-", 2) == 0) {
          if (IsFlangMode()) {
            Ty = types::TY_Fortran;
          } else {
            // If running with -E, treat as a C input (this changes the
            // builtin macros, for example). This may be overridden by -ObjC
            // below.
            //
            // Otherwise emit an error but still use a valid type to avoid
            // spurious errors (e.g., no inputs).
            if (!Args.hasArgNoClaim(options::OPT_E) && !CCCIsCPP())
              Diag(IsCLMode() ? clang::diag::err_drv_unknown_stdin_type_clang_cl
                              : clang::diag::err_drv_unknown_stdin_type);
            Ty = types::TY_C;
          }
        } else {
          // Otherwise lookup by extension.
          // Fallback is C if invoked as C preprocessor, C++ if invoked with
          // clang-cl /E, or Object otherwise.
          // We use a host hook here because Darwin at least has its own
          // idea of what .s is.
          if (const char *Ext = strrchr(Value, '.'))
            Ty = TC.LookupTypeForExtension(Ext + 1);

          // For SYCL, convert C-type sources to C++-type sources.
          if (IsSYCL) {
            switch (Ty) {
            case types::TY_C:
              Ty = types::TY_CXX;
              break;
            case types::TY_CHeader:
              Ty = types::TY_CXXHeader;
              break;
            case types::TY_PP_C:
              Ty = types::TY_PP_CXX;
              break;
            case types::TY_PP_CHeader:
              Ty = types::TY_PP_CXXHeader;
              break;
            default:
              break;
            }
          }

          if (Ty == types::TY_INVALID) {
            if (CCCIsCPP())
              Ty = CType;
            else if (IsCLMode() && Args.hasArgNoClaim(options::OPT_E))
              Ty = types::TY_CXX;
            else
              Ty = types::TY_Object;
          }

          // If the driver is invoked as C++ compiler (like clang++ or c++) it
          // should autodetect some input files as C++ for g++ compatibility.
          if (CCCIsCXX()) {
            types::ID OldTy = Ty;
            Ty = types::lookupCXXTypeForCType(Ty);

            if (Ty != OldTy)
              Diag(clang::diag::warn_drv_treating_input_as_cxx)
                  << getTypeName(OldTy) << getTypeName(Ty);
          }

          // If running with -fthinlto-index=, extensions that normally identify
          // native object files actually identify LLVM bitcode files.
          if (Args.hasArgNoClaim(options::OPT_fthinlto_index_EQ) &&
              Ty == types::TY_Object)
            Ty = types::TY_LLVM_BC;
        }

        // -ObjC and -ObjC++ override the default language, but only for "source
        // files". We just treat everything that isn't a linker input as a
        // source file.
        //
        // FIXME: Clean this up if we move the phase sequence into the type.
        if (Ty != types::TY_Object) {
          if (Args.hasArg(options::OPT_ObjC))
            Ty = types::TY_ObjC;
          else if (Args.hasArg(options::OPT_ObjCXX))
            Ty = types::TY_ObjCXX;
        }
      } else {
        assert(InputTypeArg && "InputType set w/o InputTypeArg");
        if (!InputTypeArg->getOption().matches(options::OPT_x)) {
          // If emulating cl.exe, make sure that /TC and /TP don't affect input
          // object files.
          const char *Ext = strrchr(Value, '.');
          if (Ext && TC.LookupTypeForExtension(Ext + 1) == types::TY_Object)
            Ty = types::TY_Object;
        }
        if (Ty == types::TY_INVALID) {
          Ty = InputType;
          InputTypeArg->claim();
        }
      }

      if (DiagnoseInputExistence(Args, Value, Ty, /*TypoCorrect=*/true))
        Inputs.push_back(std::make_pair(Ty, A));

    } else if (A->getOption().matches(options::OPT__SLASH_Tc)) {
      StringRef Value = A->getValue();
      if (DiagnoseInputExistence(Args, Value, types::TY_C,
                                 /*TypoCorrect=*/false)) {
        Arg *InputArg = MakeInputArg(Args, Opts, A->getValue());
        Inputs.push_back(
            std::make_pair(IsSYCL ? types::TY_CXX : types::TY_C, InputArg));
      }
      A->claim();
    } else if (A->getOption().matches(options::OPT__SLASH_Tp)) {
      StringRef Value = A->getValue();
      if (DiagnoseInputExistence(Args, Value, types::TY_CXX,
                                 /*TypoCorrect=*/false)) {
        Arg *InputArg = MakeInputArg(Args, Opts, A->getValue());
        Inputs.push_back(std::make_pair(types::TY_CXX, InputArg));
      }
      A->claim();
    } else if (A->getOption().hasFlag(options::LinkerInput)) {
      // Just treat as object type, we could make a special type for this if
      // necessary.
      Inputs.push_back(std::make_pair(types::TY_Object, A));

    } else if (A->getOption().matches(options::OPT_x)) {
      InputTypeArg = A;
      InputType = types::lookupTypeForTypeSpecifier(A->getValue());
      A->claim();

      // Follow gcc behavior and treat as linker input for invalid -x
      // options. Its not clear why we shouldn't just revert to unknown; but
      // this isn't very important, we might as well be bug compatible.
      if (!InputType) {
        Diag(clang::diag::err_drv_unknown_language) << A->getValue();
        InputType = types::TY_Object;
      }
      // Emit an error if c-compilation is forced in -fsycl mode
      if (IsSYCL && (InputType == types::TY_C || InputType == types::TY_PP_C ||
                     InputType == types::TY_CHeader))
        Diag(clang::diag::err_drv_fsycl_with_c_type) << A->getAsString(Args);

    } else if (A->getOption().getID() == options::OPT_U) {
      assert(A->getNumValues() == 1 && "The /U option has one value.");
      StringRef Val = A->getValue(0);
      if (Val.find_first_of("/\\") != StringRef::npos) {
        // Warn about e.g. "/Users/me/myfile.c".
        Diag(diag::warn_slash_u_filename) << Val;
        Diag(diag::note_use_dashdash);
      }
    }
    // TODO: remove when -foffload-static-lib support is dropped.
    else if (A->getOption().matches(options::OPT_offload_lib_Group)) {
      // Add the foffload-static-lib library to the command line to allow
      // processing when no source or object is supplied as well as proper
      // host link.
      Arg *InputArg = MakeInputArg(Args, Opts, A->getValue());
      Inputs.push_back(std::make_pair(types::TY_Object, InputArg));
      A->claim();
      // Use of -foffload-static-lib and -foffload-whole-static-lib are
      // deprecated with the updated functionality to scan the static libs.
      Diag(clang::diag::warn_drv_deprecated_option)
          << A->getAsString(Args) << A->getValue();
    }
  }
  if (CCCIsCPP() && Inputs.empty()) {
    // If called as standalone preprocessor, stdin is processed
    // if no other input is present.
    Arg *A = MakeInputArg(Args, Opts, "-");
    Inputs.push_back(std::make_pair(types::TY_C, A));
  }
}

static bool runBundler(const SmallVectorImpl<StringRef> &BundlerArgs,
                       Compilation &C) {
  // Find bundler.
  StringRef ExecPath(C.getArgs().MakeArgString(C.getDriver().Dir));
  llvm::ErrorOr<std::string> BundlerBinary =
      llvm::sys::findProgramByName("clang-offload-bundler", ExecPath);
  // Since this is run in real time and not in the toolchain, output the
  // command line if requested.
  bool OutputOnly = C.getArgs().hasArg(options::OPT__HASH_HASH_HASH);
  if (C.getArgs().hasArg(options::OPT_v) || OutputOnly) {
    for (StringRef A : BundlerArgs)
      if (OutputOnly)
        llvm::errs() << "\"" << A << "\" ";
      else
        llvm::errs() << A << " ";
    llvm::errs() << '\n';
  }
  if (BundlerBinary.getError())
    return false;

  return !llvm::sys::ExecuteAndWait(BundlerBinary.get(), BundlerArgs);
}

bool hasFPGABinary(Compilation &C, std::string Object, types::ID Type) {
  assert(types::isFPGA(Type) && "unexpected Type for FPGA binary check");
  // Do not do the check if the file doesn't exist
  if (!llvm::sys::fs::exists(Object))
    return false;

  // Temporary names for the output.
  llvm::Triple TT;
  TT.setArchName(types::getTypeName(Type));
  TT.setVendorName("intel");
  TT.setOS(llvm::Triple::UnknownOS);
  TT.setEnvironment(llvm::Triple::SYCLDevice);

  // Checking uses -check-section option with the input file, no output
  // file and the target triple being looked for.
  const char *Targets =
      C.getArgs().MakeArgString(Twine("-targets=sycl-") + TT.str());
  const char *Inputs = C.getArgs().MakeArgString(Twine("-inputs=") + Object);
  // Always use -type=ao for aocx/aocr bundle checking.  The 'bundles' are
  // actually archives.
  SmallVector<StringRef, 6> BundlerArgs = {"clang-offload-bundler", "-type=ao",
                                           Targets, Inputs, "-check-section"};
  return runBundler(BundlerArgs, C);
}

static bool hasSYCLDefaultSection(Compilation &C, const StringRef &File) {
  // Do not do the check if the file doesn't exist
  if (!llvm::sys::fs::exists(File))
    return false;

  bool IsArchive = isStaticArchiveFile(File);
  if (!(IsArchive || isObjectFile(File.str())))
    return false;

  llvm::Triple TT(C.getDriver().MakeSYCLDeviceTriple("spir64"));
  // Checking uses -check-section option with the input file, no output
  // file and the target triple being looked for.
  const char *Targets =
      C.getArgs().MakeArgString(Twine("-targets=sycl-") + TT.str());
  const char *Inputs =
      C.getArgs().MakeArgString(Twine("-inputs=") + File.str());
  // Always use -type=ao for bundle checking.  The 'bundles' are
  // actually archives.
  SmallVector<StringRef, 6> BundlerArgs = {"clang-offload-bundler",
                                           IsArchive ? "-type=ao" : "-type=o",
                                           Targets, Inputs, "-check-section"};
  return runBundler(BundlerArgs, C);
}

static bool hasOffloadSections(Compilation &C, const StringRef &Archive,
                               DerivedArgList &Args) {
  // Do not do the check if the file doesn't exist
  if (!llvm::sys::fs::exists(Archive))
    return false;

  llvm::Triple TT(C.getDefaultToolChain().getTriple());
  // Checking uses -check-section option with the input file, no output
  // file and the target triple being looked for.
  // TODO - Improve checking to check for explicit offload target instead
  // of the generic host availability.
  const char *Targets = Args.MakeArgString(Twine("-targets=host-") + TT.str());
  const char *Inputs = Args.MakeArgString(Twine("-inputs=") + Archive.str());
  // Always use -type=ao for bundle checking.  The 'bundles' are
  // actually archives.
  SmallVector<StringRef, 6> BundlerArgs = {"clang-offload-bundler", "-type=ao",
                                           Targets, Inputs, "-check-section"};
  return runBundler(BundlerArgs, C);
}

// Simple helper function for Linker options, where the option is valid if
// it has '-' or '--' as the designator.
static bool optionMatches(const std::string &Option,
                          const std::string &OptCheck) {
  return (Option == OptCheck || ("-" + Option) == OptCheck);
}

// Process linker inputs for use with offload static libraries.  We are only
// handling options and explicitly named static archives as these need to be
// partially linked.
static SmallVector<const char *, 16>
getLinkerArgs(Compilation &C, DerivedArgList &Args, bool IncludeObj = false) {
  SmallVector<const char *, 16> LibArgs;
  for (const auto *A : Args) {
    std::string FileName = A->getAsString(Args);
    if (A->getOption().getKind() == Option::InputClass) {
      StringRef Value(A->getValue());
      if (isStaticArchiveFile(Value) ||
          (IncludeObj && isObjectFile(Value.str()))) {
        LibArgs.push_back(Args.MakeArgString(FileName));
        continue;
      }
    }
    if (A->getOption().matches(options::OPT_Wl_COMMA) ||
        A->getOption().matches(options::OPT_Xlinker)) {
      // Parse through additional linker arguments that are meant to go
      // directly to the linker.
      // Keep the previous arg even if it is a new argument, for example:
      //   -Xlinker -rpath -Xlinker <dir>.
      // Without this history, we do not know that <dir> was assocated with
      // -rpath and is processed incorrectly.
      static std::string PrevArg;
      for (const std::string &Value : A->getValues()) {
        auto addKnownValues = [&](const StringRef &V) {
          // Only add named static libs objects and --whole-archive options.
          if (optionMatches("-whole-archive", V.str()) ||
              optionMatches("-no-whole-archive", V.str()) ||
              isStaticArchiveFile(V) || (IncludeObj && isObjectFile(V.str()))) {
            LibArgs.push_back(Args.MakeArgString(V));
            return;
          }
          // Probably not the best way to handle this, but there are options
          // that take arguments which we should not add to the known values.
          // Handle -z and -rpath for now - can be expanded if/when usage shows
          // the need.
          if (PrevArg != "-z" && PrevArg != "-rpath" && V[0] != '-' &&
              isObjectFile(V.str())) {
            LibArgs.push_back(Args.MakeArgString(V));
            return;
          }
        };
        if (Value[0] == '@') {
          // Found a response file, we want to expand contents to try and
          // discover more libraries and options.
          SmallVector<const char *, 20> ExpandArgs;
          ExpandArgs.push_back(Value.c_str());

          llvm::BumpPtrAllocator A;
          llvm::StringSaver S(A);
          llvm::cl::ExpandResponseFiles(
              S,
              C.getDefaultToolChain().getTriple().isWindowsMSVCEnvironment()
                  ? llvm::cl::TokenizeWindowsCommandLine
                  : llvm::cl::TokenizeGNUCommandLine,
              ExpandArgs);
          for (StringRef EA : ExpandArgs)
            addKnownValues(EA);
        } else
          addKnownValues(Value);
        PrevArg = Value;
      }
      continue;
    }
    // Use of -foffload-static-lib and -foffload-whole-static-lib is
    // considered deprecated.  Usage should move to passing in the static
    // library name on the command line, encapsulating with
    // -Wl,--whole-archive <lib> -Wl,--no-whole-archive as needed.
    if (A->getOption().matches(options::OPT_foffload_static_lib_EQ)) {
      LibArgs.push_back(Args.MakeArgString(A->getValue()));
      continue;
    }
    if (A->getOption().matches(options::OPT_foffload_whole_static_lib_EQ)) {
      // For -foffload-whole-static-lib, we add the --whole-archive wrap
      // around the library which will be used during the partial link step.
      LibArgs.push_back("--whole-archive");
      LibArgs.push_back(Args.MakeArgString(A->getValue()));
      LibArgs.push_back("--no-whole-archive");
      continue;
    }
  }
  return LibArgs;
}

static bool IsSYCLDeviceLibObj(std::string ObjFilePath, bool isMSVCEnv) {
  StringRef ObjFileName = llvm::sys::path::filename(ObjFilePath);
  StringRef ObjSuffix = isMSVCEnv ? ".obj" : ".o";
  bool Ret =
      (ObjFileName.startswith("libsycl-") && ObjFileName.endswith(ObjSuffix))
          ? true
          : false;
  return Ret;
}

// Goes through all of the arguments, including inputs expected for the
// linker directly, to determine if we need to potentially add the SYCL
// default triple.
bool Driver::checkForSYCLDefaultDevice(Compilation &C,
                                       DerivedArgList &Args) const {
  // Check only if enabled with -fsycl
  if (!Args.hasFlag(options::OPT_fsycl, options::OPT_fno_sycl, false))
    return false;

  if (Args.hasArg(options::OPT_fno_sycl_link_spirv))
    return false;

  SmallVector<const char *, 16> AllArgs(getLinkerArgs(C, Args, true));
  for (StringRef Arg : AllArgs) {
    if (hasSYCLDefaultSection(C, Arg))
      return true;
  }
  return false;
}

// Goes through all of the arguments, including inputs expected for the
// linker directly, to determine if we need to perform additional work for
// static offload libraries.
bool Driver::checkForOffloadStaticLib(Compilation &C,
                                      DerivedArgList &Args) const {
  // Check only if enabled with -fsycl or -fopenmp-targets
  if (!Args.hasFlag(options::OPT_fsycl, options::OPT_fno_sycl, false) &&
      !Args.hasArg(options::OPT_fopenmp_targets_EQ))
    return false;

  // Right off the bat, assume the presence of -foffload-static-lib means
  // the need to perform linking steps for fat static archive offloading.
  // TODO: remove when -foffload-static-lib support is dropped.
  if (Args.hasArg(options::OPT_offload_lib_Group))
    return true;
  SmallVector<const char *, 16> OffloadLibArgs(getLinkerArgs(C, Args));
  for (StringRef OLArg : OffloadLibArgs)
    if (isStaticArchiveFile(OLArg) && hasOffloadSections(C, OLArg, Args)) {
      // FPGA binaries with AOCX or AOCR sections are not considered fat
      // static archives.
      return !(hasFPGABinary(C, OLArg.str(), types::TY_FPGA_AOCR) ||
               hasFPGABinary(C, OLArg.str(), types::TY_FPGA_AOCX));
    }
  return false;
}

namespace {
/// Provides a convenient interface for different programming models to generate
/// the required device actions.
class OffloadingActionBuilder final {
  /// Flag used to trace errors in the builder.
  bool IsValid = false;

  /// The compilation that is using this builder.
  Compilation &C;

  /// Map between an input argument and the offload kinds used to process it.
  std::map<const Arg *, unsigned> InputArgToOffloadKindMap;

  /// Builder interface. It doesn't build anything or keep any state.
  class DeviceActionBuilder {
  public:
    typedef const llvm::SmallVectorImpl<phases::ID> PhasesTy;

    enum ActionBuilderReturnCode {
      // The builder acted successfully on the current action.
      ABRT_Success,
      // The builder didn't have to act on the current action.
      ABRT_Inactive,
      // The builder was successful and requested the host action to not be
      // generated.
      ABRT_Ignore_Host,
    };

  protected:
    /// Compilation associated with this builder.
    Compilation &C;

    /// Tool chains associated with this builder. The same programming
    /// model may have associated one or more tool chains.
    SmallVector<const ToolChain *, 2> ToolChains;

    /// The derived arguments associated with this builder.
    DerivedArgList &Args;

    /// The inputs associated with this builder.
    const Driver::InputList &Inputs;

    /// The associated offload kind.
    Action::OffloadKind AssociatedOffloadKind = Action::OFK_None;

  public:
    DeviceActionBuilder(Compilation &C, DerivedArgList &Args,
                        const Driver::InputList &Inputs,
                        Action::OffloadKind AssociatedOffloadKind)
        : C(C), Args(Args), Inputs(Inputs),
          AssociatedOffloadKind(AssociatedOffloadKind) {}
    virtual ~DeviceActionBuilder() {}

    /// Fill up the array \a DA with all the device dependences that should be
    /// added to the provided host action \a HostAction. By default it is
    /// inactive.
    virtual ActionBuilderReturnCode
    getDeviceDependences(OffloadAction::DeviceDependences &DA,
                         phases::ID CurPhase, phases::ID FinalPhase,
                         PhasesTy &Phases) {
      return ABRT_Inactive;
    }

    /// Update the state to include the provided host action \a HostAction as a
    /// dependency of the current device action. By default it is inactive.
    virtual ActionBuilderReturnCode addDeviceDepences(Action *HostAction) {
      return ABRT_Inactive;
    }

    /// Append top level actions generated by the builder.
    virtual void appendTopLevelActions(ActionList &AL) {}

    /// Append top level actions specific for certain link situations.
    virtual void appendTopLevelLinkAction(ActionList &AL) {}

    /// Append linker device actions generated by the builder.
    virtual void appendLinkDeviceActions(ActionList &AL) {}

    /// Append linker host action generated by the builder.
    virtual Action* appendLinkHostActions(ActionList &AL) { return nullptr; }

    /// Append linker actions generated by the builder.
    virtual void appendLinkDependences(OffloadAction::DeviceDependences &DA) {}

    /// Append linker actions generated by the builder.
    virtual void addDeviceLinkDependencies(OffloadDepsJobAction *DA) {}

    /// Initialize the builder. Return true if any initialization errors are
    /// found.
    virtual bool initialize() { return false; }

    /// Return true if the builder can use bundling/unbundling.
    virtual bool canUseBundlerUnbundler() const { return false; }

    /// Return true if this builder is valid. We have a valid builder if we have
    /// associated device tool chains.
    bool isValid() { return !ToolChains.empty(); }

    /// Return the associated offload kind.
    Action::OffloadKind getAssociatedOffloadKind() {
      return AssociatedOffloadKind;
    }
  };

  /// Base class for CUDA/HIP action builder. It injects device code in
  /// the host backend action.
  class CudaActionBuilderBase : public DeviceActionBuilder {
  protected:
    /// Flags to signal if the user requested host-only or device-only
    /// compilation.
    bool CompileHostOnly = false;
    bool CompileDeviceOnly = false;
    bool EmitLLVM = false;
    bool EmitAsm = false;

    /// ID to identify each device compilation. For CUDA it is simply the
    /// GPU arch string. For HIP it is either the GPU arch string or GPU
    /// arch string plus feature strings delimited by a plus sign, e.g.
    /// gfx906+xnack.
    struct TargetID {
      /// Target ID string which is persistent throughout the compilation.
      const char *ID;
      TargetID(CudaArch Arch) { ID = CudaArchToString(Arch); }
      TargetID(const char *ID) : ID(ID) {}
      operator const char *() { return ID; }
      operator StringRef() { return StringRef(ID); }
    };
    /// List of GPU architectures to use in this compilation.
    SmallVector<TargetID, 4> GpuArchList;

    /// The CUDA actions for the current input.
    ActionList CudaDeviceActions;

    /// The CUDA fat binary if it was generated for the current input.
    Action *CudaFatBinary = nullptr;

    /// Flag that is set to true if this builder acted on the current input.
    bool IsActive = false;

    /// Flag for -fgpu-rdc.
    bool Relocatable = false;

    /// Default GPU architecture if there's no one specified.
    CudaArch DefaultCudaArch = CudaArch::UNKNOWN;

    /// Method to generate compilation unit ID specified by option
    /// '-fuse-cuid='.
    enum UseCUIDKind { CUID_Hash, CUID_Random, CUID_None, CUID_Invalid };
    UseCUIDKind UseCUID = CUID_Hash;

    /// Compilation unit ID specified by option '-cuid='.
    StringRef FixedCUID;

  public:
    CudaActionBuilderBase(Compilation &C, DerivedArgList &Args,
                          const Driver::InputList &Inputs,
                          Action::OffloadKind OFKind)
        : DeviceActionBuilder(C, Args, Inputs, OFKind) {}

    ActionBuilderReturnCode addDeviceDepences(Action *HostAction) override {
      // While generating code for CUDA, we only depend on the host input action
      // to trigger the creation of all the CUDA device actions.

      // If we are dealing with an input action, replicate it for each GPU
      // architecture. If we are in host-only mode we return 'success' so that
      // the host uses the CUDA offload kind.
      if (auto *IA = dyn_cast<InputAction>(HostAction)) {
        assert(!GpuArchList.empty() &&
               "We should have at least one GPU architecture.");

        // If the host input is not CUDA or HIP, we don't need to bother about
        // this input.
        if (!(IA->getType() == types::TY_CUDA ||
              IA->getType() == types::TY_HIP ||
              IA->getType() == types::TY_PP_HIP)) {
          // The builder will ignore this input.
          IsActive = false;
          return ABRT_Inactive;
        }

        // Set the flag to true, so that the builder acts on the current input.
        IsActive = true;

        if (CompileHostOnly)
          return ABRT_Success;

        // Replicate inputs for each GPU architecture.
        auto Ty = IA->getType() == types::TY_HIP ? types::TY_HIP_DEVICE
                                                 : types::TY_CUDA_DEVICE;
        std::string CUID = FixedCUID.str();
        if (CUID.empty()) {
          if (UseCUID == CUID_Random)
            CUID = llvm::utohexstr(llvm::sys::Process::GetRandomNumber(),
                                   /*LowerCase=*/true);
          else if (UseCUID == CUID_Hash) {
            llvm::MD5 Hasher;
            llvm::MD5::MD5Result Hash;
            SmallString<256> RealPath;
            llvm::sys::fs::real_path(IA->getInputArg().getValue(), RealPath,
                                     /*expand_tilde=*/true);
            Hasher.update(RealPath);
            for (auto *A : Args) {
              if (A->getOption().matches(options::OPT_INPUT))
                continue;
              Hasher.update(A->getAsString(Args));
            }
            Hasher.final(Hash);
            CUID = llvm::utohexstr(Hash.low(), /*LowerCase=*/true);
          }
        }
        IA->setId(CUID);

        for (unsigned I = 0, E = GpuArchList.size(); I != E; ++I) {
          CudaDeviceActions.push_back(
              C.MakeAction<InputAction>(IA->getInputArg(), Ty, IA->getId()));
        }

        return ABRT_Success;
      }

      // If this is an unbundling action use it as is for each CUDA toolchain.
      if (auto *UA = dyn_cast<OffloadUnbundlingJobAction>(HostAction)) {

        // If -fgpu-rdc is disabled, should not unbundle since there is no
        // device code to link.
        if (UA->getType() == types::TY_Object && !Relocatable)
          return ABRT_Inactive;

        CudaDeviceActions.clear();
        if (auto *IA = dyn_cast<InputAction>(UA->getInputs().back())) {
          std::string FileName = IA->getInputArg().getAsString(Args);
          // Check if the type of the file is the same as the action. Do not
          // unbundle it if it is not. Do not unbundle .so files, for example,
          // which are not object files.
          if (IA->getType() == types::TY_Object &&
              (!llvm::sys::path::has_extension(FileName) ||
               types::lookupTypeForExtension(
                   llvm::sys::path::extension(FileName).drop_front()) !=
                   types::TY_Object))
            return ABRT_Inactive;
        }

        for (auto Arch : GpuArchList) {
          CudaDeviceActions.push_back(UA);
          UA->registerDependentActionInfo(ToolChains[0], Arch,
                                          AssociatedOffloadKind);
        }
        return ABRT_Success;
      }

      return IsActive ? ABRT_Success : ABRT_Inactive;
    }

    void appendTopLevelActions(ActionList &AL) override {
      // Utility to append actions to the top level list.
      auto AddTopLevel = [&](Action *A, TargetID TargetID) {
        OffloadAction::DeviceDependences Dep;
        Dep.add(*A, *ToolChains.front(), TargetID, AssociatedOffloadKind);
        AL.push_back(C.MakeAction<OffloadAction>(Dep, A->getType()));
      };

      // If we have a fat binary, add it to the list.
      if (CudaFatBinary) {
        AddTopLevel(CudaFatBinary, CudaArch::UNUSED);
        CudaDeviceActions.clear();
        CudaFatBinary = nullptr;
        return;
      }

      if (CudaDeviceActions.empty())
        return;

      // If we have CUDA actions at this point, that's because we have a have
      // partial compilation, so we should have an action for each GPU
      // architecture.
      assert(CudaDeviceActions.size() == GpuArchList.size() &&
             "Expecting one action per GPU architecture.");
      assert(ToolChains.size() == 1 &&
             "Expecting to have a sing CUDA toolchain.");
      for (unsigned I = 0, E = GpuArchList.size(); I != E; ++I)
        AddTopLevel(CudaDeviceActions[I], GpuArchList[I]);

      CudaDeviceActions.clear();
    }

    /// Get canonicalized offload arch option. \returns empty StringRef if the
    /// option is invalid.
    virtual StringRef getCanonicalOffloadArch(StringRef Arch) = 0;

    virtual llvm::Optional<std::pair<llvm::StringRef, llvm::StringRef>>
    getConflictOffloadArchCombination(const std::set<StringRef> &GpuArchs) = 0;

    bool initialize() override {
      assert(AssociatedOffloadKind == Action::OFK_Cuda ||
             AssociatedOffloadKind == Action::OFK_HIP);

      // We don't need to support CUDA.
      if (AssociatedOffloadKind == Action::OFK_Cuda &&
          !C.hasOffloadToolChain<Action::OFK_Cuda>())
        return false;

      // We don't need to support HIP.
      if (AssociatedOffloadKind == Action::OFK_HIP &&
          !C.hasOffloadToolChain<Action::OFK_HIP>())
        return false;

      Relocatable = Args.hasFlag(options::OPT_fgpu_rdc,
          options::OPT_fno_gpu_rdc, /*Default=*/false);

      const ToolChain *HostTC = C.getSingleOffloadToolChain<Action::OFK_Host>();
      assert(HostTC && "No toolchain for host compilation.");
      if (HostTC->getTriple().isNVPTX() ||
          HostTC->getTriple().getArch() == llvm::Triple::amdgcn) {
        // We do not support targeting NVPTX/AMDGCN for host compilation. Throw
        // an error and abort pipeline construction early so we don't trip
        // asserts that assume device-side compilation.
        C.getDriver().Diag(diag::err_drv_cuda_host_arch)
            << HostTC->getTriple().getArchName();
        return true;
      }

      ToolChains.push_back(
          AssociatedOffloadKind == Action::OFK_Cuda
              ? C.getSingleOffloadToolChain<Action::OFK_Cuda>()
              : C.getSingleOffloadToolChain<Action::OFK_HIP>());

      Arg *PartialCompilationArg = Args.getLastArg(
          options::OPT_cuda_host_only, options::OPT_cuda_device_only,
          options::OPT_cuda_compile_host_device);
      CompileHostOnly = PartialCompilationArg &&
                        PartialCompilationArg->getOption().matches(
                            options::OPT_cuda_host_only);
      CompileDeviceOnly = PartialCompilationArg &&
                          PartialCompilationArg->getOption().matches(
                              options::OPT_cuda_device_only);
      EmitLLVM = Args.getLastArg(options::OPT_emit_llvm);
      EmitAsm = Args.getLastArg(options::OPT_S);
      FixedCUID = Args.getLastArgValue(options::OPT_cuid_EQ);
      if (Arg *A = Args.getLastArg(options::OPT_fuse_cuid_EQ)) {
        StringRef UseCUIDStr = A->getValue();
        UseCUID = llvm::StringSwitch<UseCUIDKind>(UseCUIDStr)
                      .Case("hash", CUID_Hash)
                      .Case("random", CUID_Random)
                      .Case("none", CUID_None)
                      .Default(CUID_Invalid);
        if (UseCUID == CUID_Invalid) {
          C.getDriver().Diag(diag::err_drv_invalid_value)
              << A->getAsString(Args) << UseCUIDStr;
          C.setContainsError();
          return true;
        }
      }

      // Collect all cuda_gpu_arch parameters, removing duplicates.
      std::set<StringRef> GpuArchs;
      bool Error = false;
      for (Arg *A : Args) {
        if (!(A->getOption().matches(options::OPT_offload_arch_EQ) ||
              A->getOption().matches(options::OPT_no_offload_arch_EQ)))
          continue;
        A->claim();

        StringRef ArchStr = A->getValue();
        if (A->getOption().matches(options::OPT_no_offload_arch_EQ) &&
            ArchStr == "all") {
          GpuArchs.clear();
          continue;
        }
        ArchStr = getCanonicalOffloadArch(ArchStr);
        if (ArchStr.empty()) {
          Error = true;
        } else if (A->getOption().matches(options::OPT_offload_arch_EQ))
          GpuArchs.insert(ArchStr);
        else if (A->getOption().matches(options::OPT_no_offload_arch_EQ))
          GpuArchs.erase(ArchStr);
        else
          llvm_unreachable("Unexpected option.");
      }

      auto &&ConflictingArchs = getConflictOffloadArchCombination(GpuArchs);
      if (ConflictingArchs) {
        C.getDriver().Diag(clang::diag::err_drv_bad_offload_arch_combo)
            << ConflictingArchs.getValue().first
            << ConflictingArchs.getValue().second;
        C.setContainsError();
        return true;
      }

      // Collect list of GPUs remaining in the set.
      for (auto Arch : GpuArchs)
        GpuArchList.push_back(Arch.data());

      // Default to sm_20 which is the lowest common denominator for
      // supported GPUs.  sm_20 code should work correctly, if
      // suboptimally, on all newer GPUs.
      if (GpuArchList.empty())
        GpuArchList.push_back(DefaultCudaArch);

      return Error;
    }
  };

  /// \brief CUDA action builder. It injects device code in the host backend
  /// action.
  class CudaActionBuilder final : public CudaActionBuilderBase {
  public:
    CudaActionBuilder(Compilation &C, DerivedArgList &Args,
                      const Driver::InputList &Inputs)
        : CudaActionBuilderBase(C, Args, Inputs, Action::OFK_Cuda) {
      DefaultCudaArch = CudaArch::SM_20;
    }

    StringRef getCanonicalOffloadArch(StringRef ArchStr) override {
      CudaArch Arch = StringToCudaArch(ArchStr);
      if (Arch == CudaArch::UNKNOWN || !IsNVIDIAGpuArch(Arch)) {
        C.getDriver().Diag(clang::diag::err_drv_cuda_bad_gpu_arch) << ArchStr;
        return StringRef();
      }
      return CudaArchToString(Arch);
    }

    llvm::Optional<std::pair<llvm::StringRef, llvm::StringRef>>
    getConflictOffloadArchCombination(
        const std::set<StringRef> &GpuArchs) override {
      return llvm::None;
    }

    ActionBuilderReturnCode
    getDeviceDependences(OffloadAction::DeviceDependences &DA,
                         phases::ID CurPhase, phases::ID FinalPhase,
                         PhasesTy &Phases) override {
      if (!IsActive)
        return ABRT_Inactive;

      // If we don't have more CUDA actions, we don't have any dependences to
      // create for the host.
      if (CudaDeviceActions.empty())
        return ABRT_Success;

      assert(CudaDeviceActions.size() == GpuArchList.size() &&
             "Expecting one action per GPU architecture.");
      assert(!CompileHostOnly &&
             "Not expecting CUDA actions in host-only compilation.");

      // If we are generating code for the device or we are in a backend phase,
      // we attempt to generate the fat binary. We compile each arch to ptx and
      // assemble to cubin, then feed the cubin *and* the ptx into a device
      // "link" action, which uses fatbinary to combine these cubins into one
      // fatbin.  The fatbin is then an input to the host action if not in
      // device-only mode.
      if (CompileDeviceOnly || CurPhase == phases::Backend) {
        ActionList DeviceActions;
        for (unsigned I = 0, E = GpuArchList.size(); I != E; ++I) {
          // Produce the device action from the current phase up to the assemble
          // phase.
          for (auto Ph : Phases) {
            // Skip the phases that were already dealt with.
            if (Ph < CurPhase)
              continue;
            // We have to be consistent with the host final phase.
            if (Ph > FinalPhase)
              break;

            CudaDeviceActions[I] = C.getDriver().ConstructPhaseAction(
                C, Args, Ph, CudaDeviceActions[I], Action::OFK_Cuda);

            if (Ph == phases::Assemble)
              break;
          }

          // If we didn't reach the assemble phase, we can't generate the fat
          // binary. We don't need to generate the fat binary if we are not in
          // device-only mode.
          if (!isa<AssembleJobAction>(CudaDeviceActions[I]) ||
              CompileDeviceOnly)
            continue;

          Action *AssembleAction = CudaDeviceActions[I];
          assert(AssembleAction->getType() == types::TY_Object);
          assert(AssembleAction->getInputs().size() == 1);

          Action *BackendAction = AssembleAction->getInputs()[0];
          assert(BackendAction->getType() == types::TY_PP_Asm);

          for (auto &A : {AssembleAction, BackendAction}) {
            OffloadAction::DeviceDependences DDep;
            DDep.add(*A, *ToolChains.front(), GpuArchList[I], Action::OFK_Cuda);
            DeviceActions.push_back(
                C.MakeAction<OffloadAction>(DDep, A->getType()));
          }
        }

        // We generate the fat binary if we have device input actions.
        if (!DeviceActions.empty()) {
          CudaFatBinary =
              C.MakeAction<LinkJobAction>(DeviceActions, types::TY_CUDA_FATBIN);

          if (!CompileDeviceOnly) {
            DA.add(*CudaFatBinary, *ToolChains.front(), /*BoundArch=*/nullptr,
                   Action::OFK_Cuda);
            // Clear the fat binary, it is already a dependence to an host
            // action.
            CudaFatBinary = nullptr;
          }

          // Remove the CUDA actions as they are already connected to an host
          // action or fat binary.
          CudaDeviceActions.clear();
        }

        // We avoid creating host action in device-only mode.
        return CompileDeviceOnly ? ABRT_Ignore_Host : ABRT_Success;
      } else if (CurPhase > phases::Backend) {
        // If we are past the backend phase and still have a device action, we
        // don't have to do anything as this action is already a device
        // top-level action.
        return ABRT_Success;
      }

      assert(CurPhase < phases::Backend && "Generating single CUDA "
                                           "instructions should only occur "
                                           "before the backend phase!");

      // By default, we produce an action for each device arch.
      for (Action *&A : CudaDeviceActions)
        A = C.getDriver().ConstructPhaseAction(C, Args, CurPhase, A);

      return ABRT_Success;
    }
  };
  /// \brief HIP action builder. It injects device code in the host backend
  /// action.
  class HIPActionBuilder final : public CudaActionBuilderBase {
    /// The linker inputs obtained for each device arch.
    SmallVector<ActionList, 8> DeviceLinkerInputs;
    bool GPUSanitize;
    // The default bundling behavior depends on the type of output, therefore
    // BundleOutput needs to be tri-value: None, true, or false.
    // Bundle code objects except --no-gpu-output is specified for device
    // only compilation. Bundle other type of output files only if
    // --gpu-bundle-output is specified for device only compilation.
    Optional<bool> BundleOutput;

  public:
    HIPActionBuilder(Compilation &C, DerivedArgList &Args,
                     const Driver::InputList &Inputs)
        : CudaActionBuilderBase(C, Args, Inputs, Action::OFK_HIP) {
      DefaultCudaArch = CudaArch::GFX803;
      GPUSanitize = Args.hasFlag(options::OPT_fgpu_sanitize,
                                 options::OPT_fno_gpu_sanitize, false);
      if (Args.hasArg(options::OPT_gpu_bundle_output,
                      options::OPT_no_gpu_bundle_output))
        BundleOutput = Args.hasFlag(options::OPT_gpu_bundle_output,
                                    options::OPT_no_gpu_bundle_output);
    }

    bool canUseBundlerUnbundler() const override { return true; }

    StringRef getCanonicalOffloadArch(StringRef IdStr) override {
      llvm::StringMap<bool> Features;
      auto ArchStr =
          parseTargetID(getHIPOffloadTargetTriple(), IdStr, &Features);
      if (!ArchStr) {
        C.getDriver().Diag(clang::diag::err_drv_bad_target_id) << IdStr;
        C.setContainsError();
        return StringRef();
      }
      auto CanId = getCanonicalTargetID(ArchStr.getValue(), Features);
      return Args.MakeArgStringRef(CanId);
    };

    llvm::Optional<std::pair<llvm::StringRef, llvm::StringRef>>
    getConflictOffloadArchCombination(
        const std::set<StringRef> &GpuArchs) override {
      return getConflictTargetIDCombination(GpuArchs);
    }

    ActionBuilderReturnCode
    getDeviceDependences(OffloadAction::DeviceDependences &DA,
                         phases::ID CurPhase, phases::ID FinalPhase,
                         PhasesTy &Phases) override {
      // amdgcn does not support linking of object files, therefore we skip
      // backend and assemble phases to output LLVM IR. Except for generating
      // non-relocatable device coee, where we generate fat binary for device
      // code and pass to host in Backend phase.
      if (CudaDeviceActions.empty())
        return ABRT_Success;

      assert(((CurPhase == phases::Link && Relocatable) ||
              CudaDeviceActions.size() == GpuArchList.size()) &&
             "Expecting one action per GPU architecture.");
      assert(!CompileHostOnly &&
             "Not expecting CUDA actions in host-only compilation.");

      if (!Relocatable && CurPhase == phases::Backend && !EmitLLVM &&
          !EmitAsm) {
        // If we are in backend phase, we attempt to generate the fat binary.
        // We compile each arch to IR and use a link action to generate code
        // object containing ISA. Then we use a special "link" action to create
        // a fat binary containing all the code objects for different GPU's.
        // The fat binary is then an input to the host action.
        for (unsigned I = 0, E = GpuArchList.size(); I != E; ++I) {
          if (C.getDriver().isUsingLTO(/*IsOffload=*/true)) {
            // When LTO is enabled, skip the backend and assemble phases and
            // use lld to link the bitcode.
            ActionList AL;
            AL.push_back(CudaDeviceActions[I]);
            // Create a link action to link device IR with device library
            // and generate ISA.
            CudaDeviceActions[I] =
                C.MakeAction<LinkJobAction>(AL, types::TY_Image);
          } else {
            // When LTO is not enabled, we follow the conventional
            // compiler phases, including backend and assemble phases.
            ActionList AL;
            auto BackendAction = C.getDriver().ConstructPhaseAction(
                C, Args, phases::Backend, CudaDeviceActions[I],
                AssociatedOffloadKind);
            auto AssembleAction = C.getDriver().ConstructPhaseAction(
                C, Args, phases::Assemble, BackendAction,
                AssociatedOffloadKind);
            AL.push_back(AssembleAction);
            // Create a link action to link device IR with device library
            // and generate ISA.
            CudaDeviceActions[I] =
                C.MakeAction<LinkJobAction>(AL, types::TY_Image);
          }

          // OffloadingActionBuilder propagates device arch until an offload
          // action. Since the next action for creating fatbin does
          // not have device arch, whereas the above link action and its input
          // have device arch, an offload action is needed to stop the null
          // device arch of the next action being propagated to the above link
          // action.
          OffloadAction::DeviceDependences DDep;
          DDep.add(*CudaDeviceActions[I], *ToolChains.front(), GpuArchList[I],
                   AssociatedOffloadKind);
          CudaDeviceActions[I] = C.MakeAction<OffloadAction>(
              DDep, CudaDeviceActions[I]->getType());
        }

        if (!CompileDeviceOnly || !BundleOutput.hasValue() ||
            BundleOutput.getValue()) {
          // Create HIP fat binary with a special "link" action.
          CudaFatBinary = C.MakeAction<LinkJobAction>(CudaDeviceActions,
                                                      types::TY_HIP_FATBIN);

          if (!CompileDeviceOnly) {
            DA.add(*CudaFatBinary, *ToolChains.front(), /*BoundArch=*/nullptr,
                   AssociatedOffloadKind);
            // Clear the fat binary, it is already a dependence to an host
            // action.
            CudaFatBinary = nullptr;
          }

          // Remove the CUDA actions as they are already connected to an host
          // action or fat binary.
          CudaDeviceActions.clear();
        }

        return CompileDeviceOnly ? ABRT_Ignore_Host : ABRT_Success;
      } else if (CurPhase == phases::Link) {
        // Save CudaDeviceActions to DeviceLinkerInputs for each GPU subarch.
        // This happens to each device action originated from each input file.
        // Later on, device actions in DeviceLinkerInputs are used to create
        // device link actions in appendLinkDependences and the created device
        // link actions are passed to the offload action as device dependence.
        DeviceLinkerInputs.resize(CudaDeviceActions.size());
        auto LI = DeviceLinkerInputs.begin();
        for (auto *A : CudaDeviceActions) {
          LI->push_back(A);
          ++LI;
        }

        // We will pass the device action as a host dependence, so we don't
        // need to do anything else with them.
        CudaDeviceActions.clear();
        return ABRT_Success;
      }

      // By default, we produce an action for each device arch.
      for (Action *&A : CudaDeviceActions)
        A = C.getDriver().ConstructPhaseAction(C, Args, CurPhase, A,
                                               AssociatedOffloadKind);

      if (CompileDeviceOnly && CurPhase == FinalPhase &&
          BundleOutput.hasValue() && BundleOutput.getValue()) {
        for (unsigned I = 0, E = GpuArchList.size(); I != E; ++I) {
          OffloadAction::DeviceDependences DDep;
          DDep.add(*CudaDeviceActions[I], *ToolChains.front(), GpuArchList[I],
                   AssociatedOffloadKind);
          CudaDeviceActions[I] = C.MakeAction<OffloadAction>(
              DDep, CudaDeviceActions[I]->getType());
        }
        CudaFatBinary =
            C.MakeAction<OffloadBundlingJobAction>(CudaDeviceActions);
        CudaDeviceActions.clear();
      }

      return (CompileDeviceOnly && CurPhase == FinalPhase) ? ABRT_Ignore_Host
                                                           : ABRT_Success;
    }

    void appendLinkDeviceActions(ActionList &AL) override {
      if (DeviceLinkerInputs.size() == 0)
        return;

      assert(DeviceLinkerInputs.size() == GpuArchList.size() &&
             "Linker inputs and GPU arch list sizes do not match.");

      // Append a new link action for each device.
      unsigned I = 0;
      for (auto &LI : DeviceLinkerInputs) {
        // Each entry in DeviceLinkerInputs corresponds to a GPU arch.
        auto *DeviceLinkAction =
            C.MakeAction<LinkJobAction>(LI, types::TY_Image);
        // Linking all inputs for the current GPU arch.
        // LI contains all the inputs for the linker.
        OffloadAction::DeviceDependences DeviceLinkDeps;
        DeviceLinkDeps.add(*DeviceLinkAction, *ToolChains[0],
            GpuArchList[I], AssociatedOffloadKind);
        AL.push_back(C.MakeAction<OffloadAction>(DeviceLinkDeps,
            DeviceLinkAction->getType()));
        ++I;
      }
      DeviceLinkerInputs.clear();

      // Create a host object from all the device images by embedding them
      // in a fat binary.
      OffloadAction::DeviceDependences DDeps;
      auto *TopDeviceLinkAction =
          C.MakeAction<LinkJobAction>(AL, types::TY_Object);
      DDeps.add(*TopDeviceLinkAction, *ToolChains[0],
          nullptr, AssociatedOffloadKind);

      // Offload the host object to the host linker.
      AL.push_back(C.MakeAction<OffloadAction>(DDeps, TopDeviceLinkAction->getType()));
    }

    Action* appendLinkHostActions(ActionList &AL) override { return AL.back(); }

    void appendLinkDependences(OffloadAction::DeviceDependences &DA) override {}
  };

  /// OpenMP action builder. The host bitcode is passed to the device frontend
  /// and all the device linked images are passed to the host link phase.
  class OpenMPActionBuilder final : public DeviceActionBuilder {
    /// The OpenMP actions for the current input.
    ActionList OpenMPDeviceActions;

    /// The linker inputs obtained for each toolchain.
    SmallVector<ActionList, 8> DeviceLinkerInputs;

  public:
    OpenMPActionBuilder(Compilation &C, DerivedArgList &Args,
                        const Driver::InputList &Inputs)
        : DeviceActionBuilder(C, Args, Inputs, Action::OFK_OpenMP) {}

    ActionBuilderReturnCode
    getDeviceDependences(OffloadAction::DeviceDependences &DA,
                         phases::ID CurPhase, phases::ID FinalPhase,
                         PhasesTy &Phases) override {
      if (OpenMPDeviceActions.empty())
        return ABRT_Inactive;

      // We should always have an action for each input.
      assert(OpenMPDeviceActions.size() == ToolChains.size() &&
             "Number of OpenMP actions and toolchains do not match.");

      // The host only depends on device action in the linking phase, when all
      // the device images have to be embedded in the host image.
      if (CurPhase == phases::Link) {
        assert(ToolChains.size() == DeviceLinkerInputs.size() &&
               "Toolchains and linker inputs sizes do not match.");
        auto LI = DeviceLinkerInputs.begin();
        for (auto *A : OpenMPDeviceActions) {
          LI->push_back(A);
          ++LI;
        }

        // We passed the device action as a host dependence, so we don't need to
        // do anything else with them.
        OpenMPDeviceActions.clear();
        return ABRT_Success;
      }

      // By default, we produce an action for each device arch.
      for (Action *&A : OpenMPDeviceActions)
        A = C.getDriver().ConstructPhaseAction(C, Args, CurPhase, A);

      return ABRT_Success;
    }

    ActionBuilderReturnCode addDeviceDepences(Action *HostAction) override {

      // If this is an input action replicate it for each OpenMP toolchain.
      if (auto *IA = dyn_cast<InputAction>(HostAction)) {
        OpenMPDeviceActions.clear();
        for (unsigned I = 0; I < ToolChains.size(); ++I)
          OpenMPDeviceActions.push_back(
              C.MakeAction<InputAction>(IA->getInputArg(), IA->getType()));
        return ABRT_Success;
      }

      // If this is an unbundling action use it as is for each OpenMP toolchain.
      if (auto *UA = dyn_cast<OffloadUnbundlingJobAction>(HostAction)) {
        OpenMPDeviceActions.clear();
        if (auto *IA = dyn_cast<InputAction>(UA->getInputs().back())) {
          std::string FileName = IA->getInputArg().getAsString(Args);
          // Check if the type of the file is the same as the action. Do not
          // unbundle it if it is not. Do not unbundle .so files, for example,
          // which are not object files.
          if (IA->getType() == types::TY_Object &&
              (!llvm::sys::path::has_extension(FileName) ||
               types::lookupTypeForExtension(
                   llvm::sys::path::extension(FileName).drop_front()) !=
                   types::TY_Object))
            return ABRT_Inactive;
        }
        for (unsigned I = 0; I < ToolChains.size(); ++I) {
          OpenMPDeviceActions.push_back(UA);
          UA->registerDependentActionInfo(
              ToolChains[I], /*BoundArch=*/StringRef(), Action::OFK_OpenMP);
        }
        return ABRT_Success;
      }

      // When generating code for OpenMP we use the host compile phase result as
      // a dependence to the device compile phase so that it can learn what
      // declarations should be emitted. However, this is not the only use for
      // the host action, so we prevent it from being collapsed.
      if (isa<CompileJobAction>(HostAction)) {
        HostAction->setCannotBeCollapsedWithNextDependentAction();
        assert(ToolChains.size() == OpenMPDeviceActions.size() &&
               "Toolchains and device action sizes do not match.");
        OffloadAction::HostDependence HDep(
            *HostAction, *C.getSingleOffloadToolChain<Action::OFK_Host>(),
            /*BoundArch=*/nullptr, Action::OFK_OpenMP);
        auto TC = ToolChains.begin();
        for (Action *&A : OpenMPDeviceActions) {
          assert(isa<CompileJobAction>(A));
          OffloadAction::DeviceDependences DDep;
          DDep.add(*A, **TC, /*BoundArch=*/nullptr, Action::OFK_OpenMP);
          A = C.MakeAction<OffloadAction>(HDep, DDep);
          ++TC;
        }
      }
      return ABRT_Success;
    }

    void appendTopLevelActions(ActionList &AL) override {
      if (OpenMPDeviceActions.empty())
        return;

      // We should always have an action for each input.
      assert(OpenMPDeviceActions.size() == ToolChains.size() &&
             "Number of OpenMP actions and toolchains do not match.");

      // Append all device actions followed by the proper offload action.
      auto TI = ToolChains.begin();
      for (auto *A : OpenMPDeviceActions) {
        OffloadAction::DeviceDependences Dep;
        Dep.add(*A, **TI, /*BoundArch=*/nullptr, Action::OFK_OpenMP);
        AL.push_back(C.MakeAction<OffloadAction>(Dep, A->getType()));
        ++TI;
      }
      // We no longer need the action stored in this builder.
      OpenMPDeviceActions.clear();
    }

    void appendLinkDeviceActions(ActionList &AL) override {
      assert(ToolChains.size() == DeviceLinkerInputs.size() &&
             "Toolchains and linker inputs sizes do not match.");

      // Append a new link action for each device.
      auto TC = ToolChains.begin();
      for (auto &LI : DeviceLinkerInputs) {
        auto *DeviceLinkAction =
            C.MakeAction<LinkJobAction>(LI, types::TY_Image);
        OffloadAction::DeviceDependences DeviceLinkDeps;
        DeviceLinkDeps.add(*DeviceLinkAction, **TC, /*BoundArch=*/nullptr,
		        Action::OFK_OpenMP);
        AL.push_back(C.MakeAction<OffloadAction>(DeviceLinkDeps,
            DeviceLinkAction->getType()));
        ++TC;
      }
      DeviceLinkerInputs.clear();
    }

    Action* appendLinkHostActions(ActionList &AL) override {
      // Create wrapper bitcode from the result of device link actions and compile
      // it to an object which will be added to the host link command.
      auto *BC = C.MakeAction<OffloadWrapperJobAction>(AL, types::TY_LLVM_BC);
      auto *ASM = C.MakeAction<BackendJobAction>(BC, types::TY_PP_Asm);
      return C.MakeAction<AssembleJobAction>(ASM, types::TY_Object);
    }

    void appendLinkDependences(OffloadAction::DeviceDependences &DA) override {}

    void addDeviceLinkDependencies(OffloadDepsJobAction *DA) override {
      for (unsigned I = 0; I < ToolChains.size(); ++I) {
        // Register dependent toolchain.
        DA->registerDependentActionInfo(
            ToolChains[I], /*BoundArch=*/StringRef(), Action::OFK_OpenMP);

        if (!ToolChains[I]->getTriple().isSPIR()) {
          // Create object from the deps bitcode.
          auto *BA = C.MakeAction<BackendJobAction>(DA, types::TY_PP_Asm);
          auto *AA = C.MakeAction<AssembleJobAction>(BA, types::TY_Object);

          // Add deps object to linker inputs.
          DeviceLinkerInputs[I].push_back(AA);
        } else
          DeviceLinkerInputs[I].push_back(DA);
      }
    }

    bool initialize() override {
      // Get the OpenMP toolchains. If we don't get any, the action builder will
      // know there is nothing to do related to OpenMP offloading.
      auto OpenMPTCRange = C.getOffloadToolChains<Action::OFK_OpenMP>();
      for (auto TI = OpenMPTCRange.first, TE = OpenMPTCRange.second; TI != TE;
           ++TI)
        ToolChains.push_back(TI->second);

      DeviceLinkerInputs.resize(ToolChains.size());
      return false;
    }

    bool canUseBundlerUnbundler() const override {
      // OpenMP should use bundled files whenever possible.
      return true;
    }
  };

  /// SYCL action builder. The host bitcode is passed to the device frontend
  /// and all the device linked images are passed to the host link phase.
  /// SPIR related are wrapped before added to the fat binary
  class SYCLActionBuilder final : public DeviceActionBuilder {
    /// Flag to signal if the user requested device-only compilation.
    bool CompileDeviceOnly = false;

    /// Flag to signal if the user requested the device object to be wrapped.
    bool WrapDeviceOnlyBinary = false;

    /// Flag to signal if the user requested device code split.
    bool DeviceCodeSplit = false;

    /// The SYCL actions for the current input.
    ActionList SYCLDeviceActions;

    /// The SYCL link binary if it was generated for the current input.
    Action *SYCLLinkBinary = nullptr;

    /// Running list of SYCL actions specific for device linking.
    ActionList SYCLLinkBinaryList;

    /// SYCL ahead of time compilation inputs
    SmallVector<std::pair<llvm::Triple, const char *>, 8> SYCLAOTInputs;

    /// The linker inputs obtained for each toolchain.
    SmallVector<ActionList, 8> DeviceLinkerInputs;

    /// The compiler inputs obtained for each toolchain
    Action * DeviceCompilerInput = nullptr;

    /// List of offload device triples needed to track for different toolchain
    /// construction. Does not track AOT binary inputs triples.
    SmallVector<llvm::Triple, 4> SYCLTripleList;

    /// Type of output file for FPGA device compilation.
    types::ID FPGAOutType = types::TY_FPGA_AOCX;

    /// List of objects to extract FPGA dependency info from
    ActionList FPGAObjectInputs;

    /// List of static archives to extract FPGA dependency info from
    ActionList FPGAArchiveInputs;

    /// List of CUDA architectures to use in this compilation with NVPTX targets.
    SmallVector<CudaArch, 8> GpuArchList;

    /// Build the last steps for CUDA after all BC files have been linked.
    JobAction *finalizeNVPTXDependences(Action *Input, const llvm::Triple &TT) {
      auto *BA = C.getDriver().ConstructPhaseAction(
          C, Args, phases::Backend, Input, AssociatedOffloadKind);
      if (TT.getOS() != llvm::Triple::NVCL) {
        auto *AA = C.getDriver().ConstructPhaseAction(
            C, Args, phases::Assemble, BA, AssociatedOffloadKind);
        ActionList DeviceActions = {BA, AA};
        return C.MakeAction<LinkJobAction>(DeviceActions,
                                           types::TY_CUDA_FATBIN);
      }
      return cast<JobAction>(BA);
    }

    JobAction *finalizeAMDGCNDependences(Action *Input,
                                         const llvm::Triple &TT) {
      auto *BA = C.getDriver().ConstructPhaseAction(
          C, Args, phases::Backend, Input, AssociatedOffloadKind);

      auto *AA = C.getDriver().ConstructPhaseAction(C, Args, phases::Assemble,
                                                    BA, AssociatedOffloadKind);

      ActionList AL = {AA};
      Action *LinkAction = C.MakeAction<LinkJobAction>(AL, types::TY_Image);
      ActionList HIPActions = {LinkAction};
      JobAction *HIPFatBinary =
          C.MakeAction<LinkJobAction>(HIPActions, types::TY_HIP_FATBIN);
      return HIPFatBinary;
    }

  public:
    SYCLActionBuilder(Compilation &C, DerivedArgList &Args,
                      const Driver::InputList &Inputs)
        : DeviceActionBuilder(C, Args, Inputs, Action::OFK_SYCL) {}

    void withBoundArchForToolChain(const ToolChain* TC,
                                   llvm::function_ref<void(const char *)> Op) {
      if (TC->getTriple().isNVPTX())
        for (CudaArch A : GpuArchList)
          Op(CudaArchToString(A));
      else
        Op(nullptr);
    }

    ActionBuilderReturnCode
    getDeviceDependences(OffloadAction::DeviceDependences &DA,
                         phases::ID CurPhase, phases::ID FinalPhase,
                         PhasesTy &Phases) override {
      bool SYCLDeviceOnly = Args.hasArg(options::OPT_fsycl_device_only);
      if (CurPhase == phases::Preprocess) {
        // Do not perform the host compilation when doing preprocessing only
        // with -fsycl-device-only.
        bool IsPreprocessOnly =
            Args.getLastArg(options::OPT_E) ||
            Args.getLastArg(options::OPT__SLASH_EP, options::OPT__SLASH_P) ||
            Args.getLastArg(options::OPT_M, options::OPT_MM);
        if (IsPreprocessOnly) {
          for (Action *&A : SYCLDeviceActions) {
            A = C.getDriver().ConstructPhaseAction(C, Args, CurPhase, A,
                                                   AssociatedOffloadKind);
            if (SYCLDeviceOnly)
              continue;
            // Add an additional compile action to generate the integration
            // header.
            Action *CompileAction =
                C.MakeAction<CompileJobAction>(A, types::TY_Nothing);
            DA.add(*CompileAction, *ToolChains.front(), nullptr,
                   Action::OFK_SYCL);
          }
          return SYCLDeviceOnly ? ABRT_Ignore_Host : ABRT_Success;
        }
      }

      // Device compilation generates LLVM BC.
      if (CurPhase == phases::Compile) {
        for (Action *&A : SYCLDeviceActions) {
          types::ID OutputType = types::TY_LLVM_BC;
          if ((SYCLDeviceOnly || Args.hasArg(options::OPT_emit_llvm)) &&
              Args.hasArg(options::OPT_S))
            OutputType = types::TY_LLVM_IR;
          if (SYCLDeviceOnly) {
            if (Args.hasFlag(options::OPT_fno_sycl_use_bitcode,
                             options::OPT_fsycl_use_bitcode, false)) {
              auto *CompileAction =
                  C.MakeAction<CompileJobAction>(A, types::TY_LLVM_BC);
              A = C.MakeAction<SPIRVTranslatorJobAction>(CompileAction,
                                                         types::TY_SPIRV);
              continue;
            }
          }
          A = C.MakeAction<CompileJobAction>(A, OutputType);
          DeviceCompilerInput = A;
        }
        const auto *TC = ToolChains.front();
        const char *BoundArch = nullptr;
        if (TC->getTriple().isNVPTX())
          BoundArch = CudaArchToString(GpuArchList.front());
        DA.add(*DeviceCompilerInput, *TC, BoundArch, Action::OFK_SYCL);
        // Clear the input file, it is already a dependence to a host
        // action.
        DeviceCompilerInput = nullptr;
        return SYCLDeviceOnly ? ABRT_Ignore_Host : ABRT_Success;
      }

      // Backend/Assemble actions are obsolete for the SYCL device side
      if (CurPhase == phases::Backend || CurPhase == phases::Assemble)
        return ABRT_Inactive;

      // The host only depends on device action in the linking phase, when all
      // the device images have to be embedded in the host image.
      if (CurPhase == phases::Link) {
        assert(ToolChains.size() == DeviceLinkerInputs.size() &&
               "Toolchains and linker inputs sizes do not match.");
        auto LI = DeviceLinkerInputs.begin();
        for (auto *A : SYCLDeviceActions) {
          LI->push_back(A);
          ++LI;
        }

        // With -fsycl-link-targets, we will take the unbundled binaries
        // for each device and link them together to a single binary that will
        // be used in a split compilation step.
        if (CompileDeviceOnly && !SYCLDeviceActions.empty()) {
          for (auto SDA : SYCLDeviceActions)
            SYCLLinkBinaryList.push_back(SDA);
          if (WrapDeviceOnlyBinary) {
            // -fsycl-link behavior does the following to the unbundled device
            // binaries:
            //   1) Link them together using llvm-link
            //   2) Pass the linked binary through sycl-post-link
            //   3) Translate final .bc file to .spv
            //   4) Wrap the binary with the offload wrapper which can be used
            //      by any compilation link step.
            auto *DeviceLinkAction = C.MakeAction<LinkJobAction>(
                SYCLLinkBinaryList, types::TY_Image);
            ActionList FullSYCLLinkBinaryList;
            bool SYCLDeviceLibLinked = false;
            FullSYCLLinkBinaryList.push_back(DeviceLinkAction);
            // If used without the FPGA target, -fsycl-link is used to wrap
            // device objects for future host link. Device libraries should
            // be linked by default to resolve any undefined reference.
            const auto *TC = ToolChains.front();
            if (TC->getTriple().getSubArch() !=
                llvm::Triple::SPIRSubArch_fpga) {
              SYCLDeviceLibLinked =
                  addSYCLDeviceLibs(TC, FullSYCLLinkBinaryList, true,
                                    C.getDefaultToolChain()
                                        .getTriple()
                                        .isWindowsMSVCEnvironment());
            }

            Action *FullDeviceLinkAction = nullptr;
            if (SYCLDeviceLibLinked)
              FullDeviceLinkAction = C.MakeAction<LinkJobAction>(
                  FullSYCLLinkBinaryList, types::TY_LLVM_BC);
            else
              FullDeviceLinkAction = DeviceLinkAction;
            auto *PostLinkAction = C.MakeAction<SYCLPostLinkJobAction>(
                FullDeviceLinkAction, types::TY_LLVM_BC, types::TY_LLVM_BC);
            auto *TranslateAction = C.MakeAction<SPIRVTranslatorJobAction>(
                PostLinkAction, types::TY_Image);
            SYCLLinkBinary = C.MakeAction<OffloadWrapperJobAction>(
                TranslateAction, types::TY_Object);
          } else {
            auto *Link = C.MakeAction<LinkJobAction>(SYCLLinkBinaryList,
                                                         types::TY_Image);
            SYCLLinkBinary = C.MakeAction<SPIRVTranslatorJobAction>(
                Link, types::TY_Image);
          }

          // Remove the SYCL actions as they are already connected to an host
          // action or fat binary.
          SYCLDeviceActions.clear();
          // We avoid creating host action in device-only mode.
          return ABRT_Ignore_Host;
        }

        // We passed the device action as a host dependence, so we don't need to
        // do anything else with them.
        SYCLDeviceActions.clear();
        return ABRT_Success;
      }

      // By default, we produce an action for each device arch.
      auto TC = ToolChains.begin();
      for (Action *&A : SYCLDeviceActions) {
        if ((*TC)->getTriple().isNVPTX() && CurPhase >= phases::Backend) {
          // For CUDA, stop to emit LLVM IR so it can be linked later on.
          ++TC;
          continue;
        }

        A = C.getDriver().ConstructPhaseAction(C, Args, CurPhase, A,
                                               AssociatedOffloadKind);
        ++TC;
      }

      return ABRT_Success;
    }

    ActionBuilderReturnCode addDeviceDepences(Action *HostAction) override {

      // If this is an input action replicate it for each SYCL toolchain.
      if (auto *IA = dyn_cast<InputAction>(HostAction)) {
        SYCLDeviceActions.clear();

        // Options that are considered LinkerInput are not valid input actions
        // to the device tool chain.
        if (IA->getInputArg().getOption().hasFlag(options::LinkerInput))
          return ABRT_Inactive;

        std::string InputName = IA->getInputArg().getAsString(Args);
        // Objects will be consumed as part of the partial link step when
        // dealing with offload static libraries
        if (C.getDriver().getOffloadStaticLibSeen() &&
            IA->getType() == types::TY_Object && isObjectFile(InputName))
          return ABRT_Inactive;

        // Libraries are not processed in the SYCL toolchain
        if (IA->getType() == types::TY_Object && !isObjectFile(InputName))
          return ABRT_Inactive;

        for (unsigned I = 0; I < ToolChains.size(); ++I)
          SYCLDeviceActions.push_back(
              C.MakeAction<InputAction>(IA->getInputArg(), IA->getType()));
        return ABRT_Success;
      }

      // If this is an unbundling action use it as is for each SYCL toolchain.
      if (auto *UA = dyn_cast<OffloadUnbundlingJobAction>(HostAction)) {
        SYCLDeviceActions.clear();
        if (auto *IA = dyn_cast<InputAction>(UA->getInputs().back())) {
          // Options that are considered LinkerInput are not valid input actions
          // to the device tool chain.
          if (IA->getInputArg().getOption().hasFlag(options::LinkerInput))
            return ABRT_Inactive;

          std::string FileName = IA->getInputArg().getAsString(Args);
          // Check if the type of the file is the same as the action. Do not
          // unbundle it if it is not. Do not unbundle .so files, for example,
          // which are not object files.
          if (IA->getType() == types::TY_Object) {
            if (!isObjectFile(FileName))
              return ABRT_Inactive;
            // For SYCL device libraries, don't need to add them to
            // FPGAObjectInputs as there is no FPGA dep files inside.
            const auto *TC = ToolChains.front();
            if (TC->getTriple().getSubArch() ==
                    llvm::Triple::SPIRSubArch_fpga &&
                !IsSYCLDeviceLibObj(FileName, C.getDefaultToolChain()
                                                  .getTriple()
                                                  .isWindowsMSVCEnvironment()))
              FPGAObjectInputs.push_back(IA);
          }
        }
        for (unsigned I = 0; I < ToolChains.size(); ++I) {
          SYCLDeviceActions.push_back(UA);
          withBoundArchForToolChain(ToolChains[I], [&](const char *BoundArch) {
            UA->registerDependentActionInfo(ToolChains[I], BoundArch,
                                            Action::OFK_SYCL);
          });
        }
        return ABRT_Success;
      }
      return ABRT_Success;
    }

    // Actions that can only be appended after all Inputs have been processed
    // occur here.  Not all offload actions are against single files.
    void appendTopLevelLinkAction(ActionList &AL) override {
      if (!SYCLLinkBinary)
        return;

      OffloadAction::DeviceDependences Dep;
      Dep.add(*SYCLLinkBinary, *ToolChains.front(), /*BoundArch=*/nullptr,
              Action::OFK_SYCL);
      AL.push_back(C.MakeAction<OffloadAction>(Dep,
                                               SYCLLinkBinary->getType()));
      SYCLLinkBinary = nullptr;
    }

    void appendTopLevelActions(ActionList &AL) override {
      if (SYCLDeviceActions.empty())
        return;

      // We should always have an action for each input.
      assert(SYCLDeviceActions.size() == ToolChains.size() &&
             "Number of SYCL actions and toolchains do not match.");

      // Append all device actions followed by the proper offload action.
      auto TI = ToolChains.begin();
      for (auto *A : SYCLDeviceActions) {
        OffloadAction::DeviceDependences Dep;
        withBoundArchForToolChain(*TI, [&](const char *BoundArch) {
          Dep.add(*A, **TI, BoundArch, Action::OFK_SYCL);
        });
        AL.push_back(C.MakeAction<OffloadAction>(Dep, A->getType()));
        ++TI;
      }
      // We no longer need the action stored in this builder.
      SYCLDeviceActions.clear();
    }

    bool addSYCLDeviceLibs(const ToolChain *TC, ActionList &DeviceLinkObjects,
                           bool isSpirvAOT, bool isMSVCEnv) {
      enum SYCLDeviceLibType {
        sycl_devicelib_wrapper,
        sycl_devicelib_fallback
      };
      struct DeviceLibOptInfo {
        StringRef devicelib_name;
        StringRef devicelib_option;
      };

      bool NoDeviceLibs = false;
      int NumOfDeviceLibLinked = 0;
      // Currently, all SYCL device libraries will be linked by default
      llvm::StringMap<bool> devicelib_link_info = {
          {"libc", true}, {"libm-fp32", true}, {"libm-fp64", true}};
      if (Arg *A = Args.getLastArg(options::OPT_fsycl_device_lib_EQ,
                                   options::OPT_fno_sycl_device_lib_EQ)) {
        if (A->getValues().size() == 0)
          C.getDriver().Diag(diag::warn_drv_empty_joined_argument)
              << A->getAsString(Args);
        else {
          if (A->getOption().matches(options::OPT_fno_sycl_device_lib_EQ))
            NoDeviceLibs = true;

          for (StringRef Val : A->getValues()) {
            if (Val == "all") {
              for (auto &K : devicelib_link_info.keys())
                devicelib_link_info[K] = true && !NoDeviceLibs;
              break;
            }
            auto LinkInfoIter = devicelib_link_info.find(Val);
            if (LinkInfoIter == devicelib_link_info.end()) {
              C.getDriver().Diag(diag::err_drv_unsupported_option_argument)
                  << A->getOption().getName() << Val;
            }
            devicelib_link_info[Val] = true && !NoDeviceLibs;
          }
        }
      }

      const toolchains::SYCLToolChain *SYCLTC =
          static_cast<const toolchains::SYCLToolChain *>(TC);
      SmallVector<SmallString<128>, 4> LibLocCandidates;
      SYCLTC->SYCLInstallation.getSYCLDeviceLibPath(LibLocCandidates);
      StringRef LibSuffix = isMSVCEnv ? ".obj" : ".o";
      SmallVector<DeviceLibOptInfo, 5> sycl_device_wrapper_libs = {
          {"libsycl-crt", "libc"},
          {"libsycl-complex", "libm-fp32"},
          {"libsycl-complex-fp64", "libm-fp64"},
          {"libsycl-cmath", "libm-fp32"},
          {"libsycl-cmath-fp64", "libm-fp64"}};
      // For AOT compilation, we need to link sycl_device_fallback_libs as
      // default too.
      SmallVector<DeviceLibOptInfo, 5> sycl_device_fallback_libs = {
          {"libsycl-fallback-cassert", "libc"},
          {"libsycl-fallback-cstring", "libc"},
          {"libsycl-fallback-complex", "libm-fp32"},
          {"libsycl-fallback-complex-fp64", "libm-fp64"},
          {"libsycl-fallback-cmath", "libm-fp32"},
          {"libsycl-fallback-cmath-fp64", "libm-fp64"}};
      auto addInputs = [&](SYCLDeviceLibType t) {
        auto sycl_libs = (t == sycl_devicelib_wrapper)
                             ? sycl_device_wrapper_libs
                             : sycl_device_fallback_libs;
        bool LibLocSelected = false;
        for (const auto &LLCandidate : LibLocCandidates) {
          if (LibLocSelected)
            break;
          for (const DeviceLibOptInfo &Lib : sycl_libs) {
            if (!devicelib_link_info[Lib.devicelib_option])
              continue;
            SmallString<128> LibName(LLCandidate);
            llvm::sys::path::append(LibName, Lib.devicelib_name);
            llvm::sys::path::replace_extension(LibName, LibSuffix);
            if (llvm::sys::fs::exists(LibName)) {
              ++NumOfDeviceLibLinked;
              Arg *InputArg = MakeInputArg(Args, C.getDriver().getOpts(),
                                           Args.MakeArgString(LibName));
              auto *SYCLDeviceLibsInputAction =
                  C.MakeAction<InputAction>(*InputArg, types::TY_Object);
              auto *SYCLDeviceLibsUnbundleAction =
                  C.MakeAction<OffloadUnbundlingJobAction>(
                      SYCLDeviceLibsInputAction);
              addDeviceDepences(SYCLDeviceLibsUnbundleAction);
              DeviceLinkObjects.push_back(SYCLDeviceLibsUnbundleAction);
              if (!LibLocSelected)
                LibLocSelected = !LibLocSelected;
            }
          }
        }
      };
      addInputs(sycl_devicelib_wrapper);
      if (isSpirvAOT)
        addInputs(sycl_devicelib_fallback);
      return NumOfDeviceLibLinked != 0;
    }

    void appendLinkDependences(OffloadAction::DeviceDependences &DA) override {
      assert(ToolChains.size() == DeviceLinkerInputs.size() &&
             "Toolchains and linker inputs sizes do not match.");

      // Append a new link action for each device.
      auto TC = ToolChains.begin();

      unsigned I = 0;
      for (auto &LI : DeviceLinkerInputs) {

        auto TripleIt = llvm::find_if(SYCLTripleList, [&](auto &SYCLTriple) {
          return SYCLTriple == (*TC)->getTriple();
        });
        if (TripleIt == SYCLTripleList.end()) {
          // If the toolchain's triple is absent in this "main" triple
          // collection, this means it was created specifically for one of
          // the SYCL AOT inputs. Those will be handled separately.
          ++TC;
          continue;
        }
        if (LI.empty())
          // Current list is empty, nothing to process.
          continue;

        ActionList DeviceLibObjects;
        ActionList LinkObjects;
        auto TT = SYCLTripleList[I];
        auto isNVPTX = (*TC)->getTriple().isNVPTX();
        auto isAMDGCN = (*TC)->getTriple().isAMDGCN();
        auto isSPIR = (*TC)->getTriple().isSPIR();
        bool isSpirvAOT = TT.getSubArch() == llvm::Triple::SPIRSubArch_fpga ||
                          TT.getSubArch() == llvm::Triple::SPIRSubArch_gen ||
                          TT.getSubArch() == llvm::Triple::SPIRSubArch_x86_64;
        for (const auto &Input : LI) {
          // FPGA aoco does not go through the link, everything else does.
          if (Input->getType() == types::TY_FPGA_AOCO)
            DeviceLibObjects.push_back(Input);
          // FPGA aocr/aocx does not go through the link and is passed
          // directly to the backend compilation step (aocr) or wrapper (aocx)
          else if (types::isFPGA(Input->getType())) {
            Action *FPGAAOTAction;
            if (Input->getType() == types::TY_FPGA_AOCR ||
                Input->getType() == types::TY_FPGA_AOCR_EMU)
              // Generate AOCX/AOCR
              FPGAAOTAction =
                  C.MakeAction<BackendCompileJobAction>(Input, FPGAOutType);
            else if (Input->getType() == types::TY_FPGA_AOCX)
              FPGAAOTAction = Input;
            else
              llvm_unreachable("Unexpected FPGA input type.");
            auto *RenameAction = C.MakeAction<FileTableTformJobAction>(
                FPGAAOTAction, types::TY_Tempfilelist, types::TY_Tempfilelist);
            RenameAction->addRenameColumnTform(
                FileTableTformJobAction::COL_ZERO,
                FileTableTformJobAction::COL_CODE);
            auto *DeviceWrappingAction = C.MakeAction<OffloadWrapperJobAction>(
                RenameAction, types::TY_Object);
            DA.add(*DeviceWrappingAction, **TC, /*BoundArch=*/nullptr,
                   Action::OFK_SYCL);
          } else
            LinkObjects.push_back(Input);
        }
        if (LinkObjects.empty())
          continue;

        // The linkage actions subgraph leading to the offload wrapper.
        // [cond] Means incoming/outgoing dependence is created only when cond
        //        is true. A function of:
        //   n - target is NVPTX/AMDGCN
        //   a - SPIRV AOT compilation is requested
        //   s - device code split requested
        //   * - "all other cases"
        //     - no condition means output/input is "always" present
        // First symbol indicates output/input type
        //   . - single file output (TY_SPIRV, TY_LLVM_BC,...)
        //   - - TY_Tempfilelist
        //   + - TY_Tempfiletable
        //
        //                   .-----------------.
        //                   |Link(LinkObjects)|
        //                   .-----------------.
        //                            |
        //         .--------------------------------------.
        //         |               PostLink               |
        //         .--------------------------------------.
        //                             [+*]            [+]
        //                               |              |
        //                      .-----------------.     |
        //                      | FileTableTform  |     |
        //                      | (extract "Code")|     |
        //                      .-----------------.     |
        //                              [-]             |
        //           --------------------|              |
        //          [.]                [-*]             |
        //   .---------------.  .-------------------.   |
        //   | finalizeNVPTX  | |  SPIRVTranslator  |   |
        //   | finalizeAMDGCN | |                   |   |
        //   .---------------.  .-------------------.   |
        //          [.]             [-as]      [-!a]    |
        //           |                |          |      |
        //           |              [-s]         |      |
        //           |       .----------------.  |      |
        //           |       | BackendCompile |  |      |
        //           |       .----------------.  |      |
        //           |              [-s]         |      |
        //           |                |          |      |
        //           |              [-a]      [-!a]    [+]
        //           |              .--------------------.
        //           -----------[-n]|   FileTableTform   |
        //                          |  (replace "Code")  |
        //                          .--------------------.
        //                                      |
        //                                    [+*]
        //         .--------------------------------------.
        //         |            OffloadWrapper            |
        //         .--------------------------------------.
        //
        Action *DeviceLinkAction =
            C.MakeAction<LinkJobAction>(LinkObjects, types::TY_LLVM_BC);
        ActionList FullLinkObjects;
        bool SYCLDeviceLibLinked = false;
        FullLinkObjects.push_back(DeviceLinkAction);

        // FIXME: Link all wrapper and fallback device libraries as default,
        // When spv online link is supported by all backends, the fallback
        // device libraries are only needed when current toolchain is using
        // AOT compilation.
        if (isSPIR) {
          SYCLDeviceLibLinked = addSYCLDeviceLibs(
              *TC, FullLinkObjects, true,
              C.getDefaultToolChain().getTriple().isWindowsMSVCEnvironment());
        }

        Action *FullDeviceLinkAction = nullptr;
        if (SYCLDeviceLibLinked)
          FullDeviceLinkAction =
              C.MakeAction<LinkJobAction>(FullLinkObjects, types::TY_LLVM_BC);
        else
          FullDeviceLinkAction = DeviceLinkAction;

        // reflects whether current target is ahead-of-time and can't support
        // runtime setting of specialization constants
        bool isAOT = isNVPTX || isAMDGCN || isSpirvAOT;
        // TODO support device code split for NVPTX target

        ActionList WrapperInputs;
        // post link is not optional - even if not splitting, always need to
        // process specialization constants

        types::ID PostLinkOutType =
            isSPIR ? types::TY_Tempfiletable : FullDeviceLinkAction->getType();
        // For SPIR-V targets, force TY_Tempfiletable.
        auto *PostLinkAction = C.MakeAction<SYCLPostLinkJobAction>(
            FullDeviceLinkAction, PostLinkOutType, types::TY_Tempfiletable);
        PostLinkAction->setRTSetsSpecConstants(!isAOT);

        auto *ExtractIRFilesAction = C.MakeAction<FileTableTformJobAction>(
            PostLinkAction,
            isSPIR ? types::TY_Tempfilelist : PostLinkAction->getType(),
            types::TY_Tempfilelist);
        // single column w/o title fits TY_Tempfilelist format
        ExtractIRFilesAction->addExtractColumnTform(
            FileTableTformJobAction::COL_CODE, false /*drop titles*/);

        if (isNVPTX || isAMDGCN) {
          JobAction *FinAction =
              isNVPTX ? finalizeNVPTXDependences(ExtractIRFilesAction,
                                                 (*TC)->getTriple())
                      : finalizeAMDGCNDependences(ExtractIRFilesAction,
                                                  (*TC)->getTriple());
          auto *ForEachWrapping = C.MakeAction<ForEachWrappingAction>(
              ExtractIRFilesAction, FinAction);

          ActionList TformInputs{PostLinkAction, ForEachWrapping};
          auto *ReplaceFilesAction = C.MakeAction<FileTableTformJobAction>(
              TformInputs, types::TY_Tempfiletable, types::TY_Tempfiletable);
          ReplaceFilesAction->addReplaceColumnTform(
              FileTableTformJobAction::COL_CODE,
              FileTableTformJobAction::COL_CODE);

          WrapperInputs.push_back(ReplaceFilesAction);
        } else {
          // For SPIRV-based targets - translate to SPIRV then optionally
          // compile ahead-of-time to native architecture
          Action *BuildCodeAction =
              (Action *)C.MakeAction<SPIRVTranslatorJobAction>(
                  ExtractIRFilesAction, types::TY_Tempfilelist);

          // After the Link, wrap the files before the final host link
          if (isAOT) {
            types::ID OutType = types::TY_Tempfilelist;
            if (!DeviceCodeSplit) {
              OutType = (TT.getSubArch() == llvm::Triple::SPIRSubArch_fpga)
                            ? FPGAOutType
                            : types::TY_Image;
            }
            // Do the additional Ahead of Time compilation when the specific
            // triple calls for it (provided a valid subarch).
            ActionList BEInputs;
            BEInputs.push_back(BuildCodeAction);
            auto unbundleAdd = [&](Action *A, types::ID T) {
              ActionList AL;
              AL.push_back(A);
              Action *UnbundleAction =
                  C.MakeAction<OffloadUnbundlingJobAction>(AL, T);
              BEInputs.push_back(UnbundleAction);
            };
            // Send any known objects/archives through the unbundler to grab the
            // dependency file associated.  This is only done for -fintelfpga.
            for (Action *A : FPGAObjectInputs)
              unbundleAdd(A, types::TY_FPGA_Dependencies);
            for (Action *A : FPGAArchiveInputs)
              unbundleAdd(A, types::TY_FPGA_Dependencies_List);
            for (const auto &A : DeviceLibObjects)
              BEInputs.push_back(A);
            BuildCodeAction =
                C.MakeAction<BackendCompileJobAction>(BEInputs, OutType);
          }
          ActionList TformInputs{PostLinkAction, BuildCodeAction};
          auto *ReplaceFilesAction = C.MakeAction<FileTableTformJobAction>(
              TformInputs, types::TY_Tempfiletable, types::TY_Tempfiletable);
          ReplaceFilesAction->addReplaceColumnTform(
              FileTableTformJobAction::COL_CODE,
              FileTableTformJobAction::COL_CODE);
          WrapperInputs.push_back(ReplaceFilesAction);
        }

        // After the Link, wrap the files before the final host link
        auto *DeviceWrappingAction = C.MakeAction<OffloadWrapperJobAction>(
            WrapperInputs, types::TY_Object);

        if (isSpirvAOT)
          DA.add(*DeviceWrappingAction, **TC, /*BoundArch=*/nullptr,
                 Action::OFK_SYCL);
        else
          withBoundArchForToolChain(*TC, [&](const char *BoundArch) {
            DA.add(*DeviceWrappingAction, **TC, BoundArch, Action::OFK_SYCL);
          });
        ++TC;
        ++I;
      }

      for (auto &SAI : SYCLAOTInputs) {
        // Extract binary file name
        std::string FN(SAI.second);
        const char *FNStr = Args.MakeArgString(FN);
        Arg *myArg = Args.MakeSeparateArg(
            nullptr, C.getDriver().getOpts().getOption(options::OPT_INPUT),
            FNStr);
        auto *SYCLAdd =
            C.MakeAction<InputAction>(*myArg, types::TY_SYCL_FATBIN);
        auto *DeviceWrappingAction =
            C.MakeAction<OffloadWrapperJobAction>(SYCLAdd, types::TY_Object);

        // Extract the target triple for this binary
        llvm::Triple TT(SAI.first);
        // Extract the toolchain for this target triple
        auto SYCLDeviceTC = llvm::find_if(
            ToolChains, [&](auto &TC) { return TC->getTriple() == TT; });
        assert(SYCLDeviceTC != ToolChains.end() &&
               "No toolchain found for this AOT input");

        DA.add(*DeviceWrappingAction, **SYCLDeviceTC,
               /*BoundArch=*/nullptr, Action::OFK_SYCL);
      }
    }

    void addDeviceLinkDependencies(OffloadDepsJobAction *DA) override {
      for (unsigned I = 0; I < ToolChains.size(); ++I) {
        // Register dependent toolchain.
        DA->registerDependentActionInfo(
            ToolChains[I], /*BoundArch=*/StringRef(), Action::OFK_SYCL);

        // Add deps output to linker inputs.
        DeviceLinkerInputs[I].push_back(DA);
      }
    }

    /// Initialize the GPU architecture list from arguments - this populates `GpuArchList` from
    /// `--cuda-gpu-arch` flags. Only relevant if compiling to CUDA. Return true if any
    /// initialization errors are found.
    bool initializeGpuArchMap() {
      const OptTable &Opts = C.getDriver().getOpts();
      for (auto *A : Args) {
        unsigned Index;

        if (A->getOption().matches(options::OPT_Xsycl_backend_EQ))
          // Passing device args: -Xsycl-target-backend=<triple> -opt=val.
          if (llvm::Triple(A->getValue(0)).isNVPTX())
            Index = Args.getBaseArgs().MakeIndex(A->getValue(1));
          else
            continue;
        else if (A->getOption().matches(options::OPT_Xsycl_backend))
          // Passing device args: -Xsycl-target-backend -opt=val.
          Index = Args.getBaseArgs().MakeIndex(A->getValue(0));
        else
          continue;

        A->claim();
        auto ParsedArg = Opts.ParseOneArg(Args, Index);
        // TODO: Support --no-cuda-gpu-arch, --{,no-}cuda-gpu-arch=all.
        if (ParsedArg &&
            ParsedArg->getOption().matches(options::OPT_offload_arch_EQ)) {
          ParsedArg->claim();
          GpuArchList.push_back(StringToCudaArch(ParsedArg->getValue(0)));
        }
      }

      // If there are no CUDA architectures provided then default to SM_50.
      if (GpuArchList.empty()) {
        GpuArchList.push_back(CudaArch::SM_50);
      }

      return false;
    }

    bool initialize() override {
      // Get the SYCL toolchains. If we don't get any, the action builder will
      // know there is nothing to do related to SYCL offloading.
      auto SYCLTCRange = C.getOffloadToolChains<Action::OFK_SYCL>();
      for (auto TI = SYCLTCRange.first, TE = SYCLTCRange.second; TI != TE;
           ++TI)
        ToolChains.push_back(TI->second);

      Arg *SYCLLinkTargets = Args.getLastArg(
                                  options::OPT_fsycl_link_targets_EQ);
      WrapDeviceOnlyBinary = Args.hasArg(options::OPT_fsycl_link_EQ);
      auto *DeviceCodeSplitArg =
          Args.getLastArg(options::OPT_fsycl_device_code_split_EQ);
      // -fsycl-device-code-split is an alias to
      // -fsycl-device-code-split=per_source
      DeviceCodeSplit = DeviceCodeSplitArg &&
                        DeviceCodeSplitArg->getValue() != StringRef("off");
      // Gather information about the SYCL Ahead of Time targets.  The targets
      // are determined on the SubArch values passed along in the triple.
      Arg *SYCLTargets =
              C.getInputArgs().getLastArg(options::OPT_fsycl_targets_EQ);
      Arg *SYCLAddTargets = Args.getLastArg(options::OPT_fsycl_add_targets_EQ);
      bool HasValidSYCLRuntime = C.getInputArgs().hasFlag(
          options::OPT_fsycl, options::OPT_fno_sycl, false);
      bool SYCLfpgaTriple = false;
      if (SYCLTargets || SYCLAddTargets) {
        if (SYCLTargets) {
          llvm::StringMap<StringRef> FoundNormalizedTriples;
          for (const char *Val : SYCLTargets->getValues()) {
            llvm::Triple TT(C.getDriver().MakeSYCLDeviceTriple(Val));
            std::string NormalizedName = TT.normalize();

            // Make sure we don't have a duplicate triple.
            auto Duplicate = FoundNormalizedTriples.find(NormalizedName);
            if (Duplicate != FoundNormalizedTriples.end())
              continue;

            // Store the current triple so that we can check for duplicates in
            // the following iterations.
            FoundNormalizedTriples[NormalizedName] = Val;

            SYCLTripleList.push_back(TT);
            if (TT.getSubArch() == llvm::Triple::SPIRSubArch_fpga)
              SYCLfpgaTriple = true;
          }
          addSYCLDefaultTriple(C, SYCLTripleList);
        }
        if (SYCLAddTargets) {
          for (StringRef Val : SYCLAddTargets->getValues()) {
            // Parse out the Triple and Input (triple:binary). At this point,
            // the format has already been validated at the Driver level.
            // Populate the pairs. Each of these will be wrapped and fed
            // into the final binary.
            std::pair<StringRef, StringRef> I = Val.split(':');
            llvm::Triple TT(I.first);
            const char *TF = C.getArgs().MakeArgString(I.second);

            // populate the AOT binary inputs vector.
            SYCLAOTInputs.push_back(std::make_pair(TT, TF));
          }
        }
      } else if (HasValidSYCLRuntime) {
        // -fsycl is provided without -fsycl-*targets.
        bool SYCLfpga = C.getInputArgs().hasArg(options::OPT_fintelfpga);
        // -fsycl -fintelfpga implies spir64_fpga
        const char *SYCLTargetArch = SYCLfpga ? "spir64_fpga" : "spir64";
        SYCLTripleList.push_back(
            C.getDriver().MakeSYCLDeviceTriple(SYCLTargetArch));
        addSYCLDefaultTriple(C, SYCLTripleList);
        if (SYCLfpga)
          SYCLfpgaTriple = true;
      }

      // Device only compilation for -fsycl-link (no FPGA) and
      // -fsycl-link-targets
      CompileDeviceOnly =
          (SYCLLinkTargets || (WrapDeviceOnlyBinary && !SYCLfpgaTriple));

      // Set the FPGA output type based on command line (-fsycl-link).
      if (auto *A = C.getInputArgs().getLastArg(options::OPT_fsycl_link_EQ)) {
        FPGAOutType = (A->getValue() == StringRef("early"))
                          ? types::TY_FPGA_AOCR
                          : types::TY_FPGA_AOCX;
        if (C.getDriver().isFPGAEmulationMode())
          FPGAOutType = (A->getValue() == StringRef("early"))
                            ? types::TY_FPGA_AOCR_EMU
                            : types::TY_FPGA_AOCX;
      }

      // Populate FPGA static archives that could contain dep files to be
      // incorporated into the aoc compilation
      if (SYCLfpgaTriple && Args.hasArg(options::OPT_fintelfpga)) {
        SmallVector<const char *, 16> LinkArgs(getLinkerArgs(C, Args));
        for (StringRef LA : LinkArgs) {
          if (isStaticArchiveFile(LA) && hasOffloadSections(C, LA, Args)) {
            const llvm::opt::OptTable &Opts = C.getDriver().getOpts();
            Arg *InputArg = MakeInputArg(Args, Opts, Args.MakeArgString(LA));
            Action *Current =
                C.MakeAction<InputAction>(*InputArg, types::TY_Archive);
            FPGAArchiveInputs.push_back(Current);
          }
        }
      }

      DeviceLinkerInputs.resize(ToolChains.size());
      return initializeGpuArchMap();
    }

    bool canUseBundlerUnbundler() const override {
      // SYCL should use bundled files whenever possible.
      return true;
    }
  };

  ///
  /// TODO: Add the implementation for other specialized builders here.
  ///

  /// Specialized builders being used by this offloading action builder.
  SmallVector<DeviceActionBuilder *, 4> SpecializedBuilders;

  /// Flag set to true if all valid builders allow file bundling/unbundling.
  bool CanUseBundler;

public:
  OffloadingActionBuilder(Compilation &C, DerivedArgList &Args,
                          const Driver::InputList &Inputs)
      : C(C) {
    // Create a specialized builder for each device toolchain.

    IsValid = true;

    // Create a specialized builder for CUDA.
    SpecializedBuilders.push_back(new CudaActionBuilder(C, Args, Inputs));

    // Create a specialized builder for HIP.
    SpecializedBuilders.push_back(new HIPActionBuilder(C, Args, Inputs));

    // Create a specialized builder for OpenMP.
    SpecializedBuilders.push_back(new OpenMPActionBuilder(C, Args, Inputs));

    // Create a specialized builder for SYCL.
    SpecializedBuilders.push_back(new SYCLActionBuilder(C, Args, Inputs));

    //
    // TODO: Build other specialized builders here.
    //

    // Initialize all the builders, keeping track of errors. If all valid
    // builders agree that we can use bundling, set the flag to true.
    unsigned ValidBuilders = 0u;
    unsigned ValidBuildersSupportingBundling = 0u;
    for (auto *SB : SpecializedBuilders) {
      IsValid = IsValid && !SB->initialize();

      // Update the counters if the builder is valid.
      if (SB->isValid()) {
        ++ValidBuilders;
        if (SB->canUseBundlerUnbundler())
          ++ValidBuildersSupportingBundling;
      }
    }
    CanUseBundler =
        ValidBuilders && ValidBuilders == ValidBuildersSupportingBundling;
  }

  ~OffloadingActionBuilder() {
    for (auto *SB : SpecializedBuilders)
      delete SB;
  }

  /// Generate an action that adds device dependences (if any) to a host action.
  /// If no device dependence actions exist, just return the host action \a
  /// HostAction. If an error is found or if no builder requires the host action
  /// to be generated, return nullptr.
  Action *
  addDeviceDependencesToHostAction(Action *HostAction, const Arg *InputArg,
                                   phases::ID CurPhase, phases::ID FinalPhase,
                                   DeviceActionBuilder::PhasesTy &Phases) {
    if (!IsValid)
      return nullptr;

    if (SpecializedBuilders.empty())
      return HostAction;

    assert(HostAction && "Invalid host action!");

    OffloadAction::DeviceDependences DDeps;
    // Check if all the programming models agree we should not emit the host
    // action. Also, keep track of the offloading kinds employed.
    auto &OffloadKind = InputArgToOffloadKindMap[InputArg];
    unsigned InactiveBuilders = 0u;
    unsigned IgnoringBuilders = 0u;
    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid()) {
        ++InactiveBuilders;
        continue;
      }

      auto RetCode =
          SB->getDeviceDependences(DDeps, CurPhase, FinalPhase, Phases);

      // If the builder explicitly says the host action should be ignored,
      // we need to increment the variable that tracks the builders that request
      // the host object to be ignored.
      if (RetCode == DeviceActionBuilder::ABRT_Ignore_Host)
        ++IgnoringBuilders;

      // Unless the builder was inactive for this action, we have to record the
      // offload kind because the host will have to use it.
      if (RetCode != DeviceActionBuilder::ABRT_Inactive)
        OffloadKind |= SB->getAssociatedOffloadKind();
    }

    // If all builders agree that the host object should be ignored, just return
    // nullptr.
    if (IgnoringBuilders &&
        SpecializedBuilders.size() == (InactiveBuilders + IgnoringBuilders))
      return nullptr;

    if (DDeps.getActions().empty())
      return HostAction;

    // We have dependences we need to bundle together. We use an offload action
    // for that.
    OffloadAction::HostDependence HDep(
        *HostAction, *C.getSingleOffloadToolChain<Action::OFK_Host>(),
        /*BoundArch=*/nullptr, DDeps);
    return C.MakeAction<OffloadAction>(HDep, DDeps);
  }

  // Update Input action to reflect FPGA device archive specifics based
  // on archive contents.
  bool updateInputForFPGA(Action *&A, const Arg *InputArg,
                          DerivedArgList &Args) {
    std::string InputName = InputArg->getAsString(Args);
    const Driver &D = C.getDriver();
    bool IsFPGAEmulation = D.isFPGAEmulationMode();
    // Only check for FPGA device information when using fpga SubArch.
    if (A->getType() == types::TY_Object && isObjectFile(InputName))
      return true;

    auto ArchiveTypeMismatch = [&D, &InputName](bool EmitDiag) {
      if (EmitDiag)
        D.Diag(clang::diag::warn_drv_mismatch_fpga_archive) << InputName;
    };
    // Type FPGA aoco is a special case for static archives
    if (A->getType() == types::TY_FPGA_AOCO) {
      if (!hasFPGABinary(C, InputName, types::TY_FPGA_AOCO))
        return false;
      A = C.MakeAction<InputAction>(*InputArg, types::TY_FPGA_AOCO);
      return true;
    }

    // Type FPGA aocx is considered the same way for Hardware and Emulation.
    if (hasFPGABinary(C, InputName, types::TY_FPGA_AOCX)) {
      A = C.MakeAction<InputAction>(*InputArg, types::TY_FPGA_AOCX);
      return true;
    }

    SmallVector<std::pair<types::ID, bool>, 4> FPGAAOCTypes = {
        {types::TY_FPGA_AOCR, false},
        {types::TY_FPGA_AOCR_EMU, true}};
    for (const auto &ArchiveType : FPGAAOCTypes) {
      bool BinaryFound = hasFPGABinary(C, InputName, ArchiveType.first);
      if (BinaryFound && ArchiveType.second == IsFPGAEmulation) {
        // Binary matches check and emulation type, we keep this one.
        A = C.MakeAction<InputAction>(*InputArg, ArchiveType.first);
        return true;
      }
      ArchiveTypeMismatch(BinaryFound && ArchiveType.second != IsFPGAEmulation);
    }
    return true;
  }

  /// Generate an action that adds a host dependence to a device action. The
  /// results will be kept in this action builder. Return true if an error was
  /// found.
  bool addHostDependenceToDeviceActions(Action *&HostAction,
                                        const Arg *InputArg,
                                        DerivedArgList &Args) {
    if (!IsValid)
      return true;

    // An FPGA AOCX input does not have a host dependence to the unbundler
    if (HostAction->getType() == types::TY_FPGA_AOCX)
      return false;

    // If we are supporting bundling/unbundling and the current action is an
    // input action of non-source file, we replace the host action by the
    // unbundling action. The bundler tool has the logic to detect if an input
    // is a bundle or not and if the input is not a bundle it assumes it is a
    // host file. Therefore it is safe to create an unbundling action even if
    // the input is not a bundle.
    bool HasFPGATarget = false;
    if (CanUseBundler && isa<InputAction>(HostAction) &&
        InputArg->getOption().getKind() == llvm::opt::Option::InputClass &&
        !InputArg->getOption().hasFlag(options::LinkerInput) &&
        (!types::isSrcFile(HostAction->getType()) ||
         HostAction->getType() == types::TY_PP_HIP)) {
      ActionList HostActionList;
      Action *A(HostAction);
      // Only check for FPGA device information when using fpga SubArch.
      auto SYCLTCRange = C.getOffloadToolChains<Action::OFK_SYCL>();
      for (auto TI = SYCLTCRange.first, TE = SYCLTCRange.second; TI != TE; ++TI)
        HasFPGATarget |= TI->second->getTriple().getSubArch() ==
                         llvm::Triple::SPIRSubArch_fpga;
      bool isArchive = !(HostAction->getType() == types::TY_Object &&
                         isObjectFile(InputArg->getAsString(Args)));
      if (!HasFPGATarget && isArchive &&
          HostAction->getType() == types::TY_FPGA_AOCO)
        // Archive with Non-FPGA target with AOCO type should not be unbundled.
        return false;
      if (HasFPGATarget && !updateInputForFPGA(A, InputArg, Args))
        return false;
      auto UnbundlingHostAction = C.MakeAction<OffloadUnbundlingJobAction>(A);
      UnbundlingHostAction->registerDependentActionInfo(
          C.getSingleOffloadToolChain<Action::OFK_Host>(),
          /*BoundArch=*/StringRef(), Action::OFK_Host);
      HostAction = UnbundlingHostAction;
    }

    assert(HostAction && "Invalid host action!");

    // Register the offload kinds that are used.
    auto &OffloadKind = InputArgToOffloadKindMap[InputArg];
    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;

      auto RetCode = SB->addDeviceDepences(HostAction);

      // Host dependences for device actions are not compatible with that same
      // action being ignored.
      assert(RetCode != DeviceActionBuilder::ABRT_Ignore_Host &&
             "Host dependence not expected to be ignored.!");

      // Unless the builder was inactive for this action, we have to record the
      // offload kind because the host will have to use it.
      if (RetCode != DeviceActionBuilder::ABRT_Inactive)
        OffloadKind |= SB->getAssociatedOffloadKind();
    }

    // Do not use unbundler if the Host does not depend on device action.
    // Now that we have unbundled the object, when doing -fsycl-link we
    // want to continue the host link with the input object.
    // For unbundling of an FPGA AOCX binary, we want to link with the original
    // FPGA device archive.
    if ((OffloadKind == Action::OFK_None && CanUseBundler) ||
        (HasFPGATarget && ((Args.hasArg(options::OPT_fsycl_link_EQ) &&
                            HostAction->getType() == types::TY_Object) ||
                           HostAction->getType() == types::TY_FPGA_AOCX)))
      if (auto *UA = dyn_cast<OffloadUnbundlingJobAction>(HostAction))
        HostAction = UA->getInputs().back();

    return false;
  }

  /// Add the offloading top level actions that are specific for unique
  /// linking situations where objects are used at only the device link
  /// with no intermedate steps.
  bool appendTopLevelLinkAction(ActionList &AL) {
    // Get the device actions to be appended.
    ActionList OffloadAL;
    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;
      SB->appendTopLevelLinkAction(OffloadAL);
    }
    // Append the device actions.
    AL.append(OffloadAL.begin(), OffloadAL.end());
    return false;
  }

  /// Add the offloading top level actions to the provided action list. This
  /// function can replace the host action by a bundling action if the
  /// programming models allow it.
  bool appendTopLevelActions(ActionList &AL, Action *HostAction,
                             const Arg *InputArg) {
    // Get the device actions to be appended.
    ActionList OffloadAL;
    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;
      SB->appendTopLevelActions(OffloadAL);
    }

    // If we can use the bundler, replace the host action by the bundling one in
    // the resulting list. Otherwise, just append the device actions. For
    // device only compilation, HostAction is a null pointer, therefore only do
    // this when HostAction is not a null pointer.
    if (CanUseBundler && HostAction &&
        HostAction->getType() != types::TY_Nothing && !OffloadAL.empty()) {
      // Add the host action to the list in order to create the bundling action.
      OffloadAL.push_back(HostAction);

      // We expect that the host action was just appended to the action list
      // before this method was called.
      assert(HostAction == AL.back() && "Host action not in the list??");
      HostAction = C.MakeAction<OffloadBundlingJobAction>(OffloadAL);
      AL.back() = HostAction;
    } else
      AL.append(OffloadAL.begin(), OffloadAL.end());

    // Propagate to the current host action (if any) the offload information
    // associated with the current input.
    if (HostAction)
      HostAction->propagateHostOffloadInfo(InputArgToOffloadKindMap[InputArg],
                                           /*BoundArch=*/nullptr);
    return false;
  }

  /// Create link job from the given host inputs and feed the result to offload
  /// deps job which fetches device dependencies from the linked host image.
  /// Offload deps output is then forwarded to active device action builders so
  /// they can add it to the device linker inputs.
  void addDeviceLinkDependenciesFromHost(ActionList &LinkerInputs) {
    // Link image for reading dependencies from it.
    auto *LA = C.MakeAction<LinkJobAction>(LinkerInputs,
                                           types::TY_Host_Dependencies_Image);

    // Calculate all the offload kinds used in the current compilation.
    unsigned ActiveOffloadKinds = 0u;
    for (auto &I : InputArgToOffloadKindMap)
      ActiveOffloadKinds |= I.second;

    OffloadAction::HostDependence HDep(
        *LA, *C.getSingleOffloadToolChain<Action::OFK_Host>(),
        /*BoundArch*/ nullptr, ActiveOffloadKinds);

    auto *DA = C.MakeAction<OffloadDepsJobAction>(HDep, types::TY_LLVM_BC);

    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;
      SB->addDeviceLinkDependencies(DA);
    }
  }

  void makeHostLinkAction(ActionList &LinkerInputs) {
    // Build a list of device linking actions.
    ActionList DeviceAL;
    for (DeviceActionBuilder *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;
      SB->appendLinkDeviceActions(DeviceAL);
    }

    if (DeviceAL.empty())
      return;

    // Let builders add host linking actions.
    for (DeviceActionBuilder *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;
      if (Action *HA = SB->appendLinkHostActions(DeviceAL))
        LinkerInputs.push_back(HA);
    }
  }

  /// Processes the host linker action. This currently consists of replacing it
  /// with an offload action if there are device link objects and propagate to
  /// the host action all the offload kinds used in the current compilation. The
  /// resulting action is returned.
  Action *processHostLinkAction(Action *HostAction) {
    // Add all the dependences from the device linking actions.
    OffloadAction::DeviceDependences DDeps;
    for (auto *SB : SpecializedBuilders) {
      if (!SB->isValid())
        continue;

      SB->appendLinkDependences(DDeps);
    }

    // Calculate all the offload kinds used in the current compilation.
    unsigned ActiveOffloadKinds = 0u;
    for (auto &I : InputArgToOffloadKindMap)
      ActiveOffloadKinds |= I.second;

    // If we don't have device dependencies, we don't have to create an offload
    // action.
    if (DDeps.getActions().empty()) {
      // Propagate all the active kinds to host action. Given that it is a link
      // action it is assumed to depend on all actions generated so far.
      HostAction->propagateHostOffloadInfo(ActiveOffloadKinds,
                                           /*BoundArch=*/nullptr);
      return HostAction;
    }

    // Create the offload action with all dependences. When an offload action
    // is created the kinds are propagated to the host action, so we don't have
    // to do that explicitly here.
    OffloadAction::HostDependence HDep(
        *HostAction, *C.getSingleOffloadToolChain<Action::OFK_Host>(),
        /*BoundArch*/ nullptr, ActiveOffloadKinds);
    return C.MakeAction<OffloadAction>(HDep, DDeps);
  }
};
} // anonymous namespace.

void Driver::handleArguments(Compilation &C, DerivedArgList &Args,
                             const InputList &Inputs,
                             ActionList &Actions) const {

  // Ignore /Yc/Yu if both /Yc and /Yu passed but with different filenames.
  Arg *YcArg = Args.getLastArg(options::OPT__SLASH_Yc);
  Arg *YuArg = Args.getLastArg(options::OPT__SLASH_Yu);
  if (YcArg && YuArg && strcmp(YcArg->getValue(), YuArg->getValue()) != 0) {
    Diag(clang::diag::warn_drv_ycyu_different_arg_clang_cl);
    Args.eraseArg(options::OPT__SLASH_Yc);
    Args.eraseArg(options::OPT__SLASH_Yu);
    YcArg = YuArg = nullptr;
  }
  if (YcArg && Inputs.size() > 1) {
    Diag(clang::diag::warn_drv_yc_multiple_inputs_clang_cl);
    Args.eraseArg(options::OPT__SLASH_Yc);
    YcArg = nullptr;
  }

  Arg *FinalPhaseArg;
  phases::ID FinalPhase = getFinalPhase(Args, &FinalPhaseArg);

  if (FinalPhase == phases::Link) {
    if (Args.hasArg(options::OPT_emit_llvm))
      Diag(clang::diag::err_drv_emit_llvm_link);
    if (IsCLMode() && LTOMode != LTOK_None &&
        !Args.getLastArgValue(options::OPT_fuse_ld_EQ)
             .equals_insensitive("lld"))
      Diag(clang::diag::err_drv_lto_without_lld);
  }

  if (FinalPhase == phases::Preprocess || Args.hasArg(options::OPT__SLASH_Y_)) {
    // If only preprocessing or /Y- is used, all pch handling is disabled.
    // Rather than check for it everywhere, just remove clang-cl pch-related
    // flags here.
    Args.eraseArg(options::OPT__SLASH_Fp);
    Args.eraseArg(options::OPT__SLASH_Yc);
    Args.eraseArg(options::OPT__SLASH_Yu);
    YcArg = YuArg = nullptr;
  }

  unsigned LastPLSize = 0;
  for (auto &I : Inputs) {
    types::ID InputType = I.first;
    const Arg *InputArg = I.second;

    auto PL = types::getCompilationPhases(InputType);
    LastPLSize = PL.size();

    // If the first step comes after the final phase we are doing as part of
    // this compilation, warn the user about it.
    phases::ID InitialPhase = PL[0];
    if (InitialPhase > FinalPhase) {
      if (InputArg->isClaimed())
        continue;

      // Claim here to avoid the more general unused warning.
      InputArg->claim();

      // Suppress all unused style warnings with -Qunused-arguments
      if (Args.hasArg(options::OPT_Qunused_arguments))
        continue;

      // Special case when final phase determined by binary name, rather than
      // by a command-line argument with a corresponding Arg.
      if (CCCIsCPP())
        Diag(clang::diag::warn_drv_input_file_unused_by_cpp)
            << InputArg->getAsString(Args) << getPhaseName(InitialPhase);
      // Special case '-E' warning on a previously preprocessed file to make
      // more sense.
      else if (InitialPhase == phases::Compile &&
               (Args.getLastArg(options::OPT__SLASH_EP,
                                options::OPT__SLASH_P) ||
                Args.getLastArg(options::OPT_E) ||
                Args.getLastArg(options::OPT_M, options::OPT_MM)) &&
               getPreprocessedType(InputType) == types::TY_INVALID)
        Diag(clang::diag::warn_drv_preprocessed_input_file_unused)
            << InputArg->getAsString(Args) << !!FinalPhaseArg
            << (FinalPhaseArg ? FinalPhaseArg->getOption().getName() : "");
      else
        Diag(clang::diag::warn_drv_input_file_unused)
            << InputArg->getAsString(Args) << getPhaseName(InitialPhase)
            << !!FinalPhaseArg
            << (FinalPhaseArg ? FinalPhaseArg->getOption().getName() : "");
      continue;
    }

    if (YcArg) {
      // Add a separate precompile phase for the compile phase.
      if (FinalPhase >= phases::Compile) {
        const types::ID HeaderType = lookupHeaderTypeForSourceType(InputType);
        // Build the pipeline for the pch file.
        Action *ClangClPch = C.MakeAction<InputAction>(*InputArg, HeaderType);
        for (phases::ID Phase : types::getCompilationPhases(HeaderType))
          ClangClPch = ConstructPhaseAction(C, Args, Phase, ClangClPch);
        assert(ClangClPch);
        Actions.push_back(ClangClPch);
        // The driver currently exits after the first failed command.  This
        // relies on that behavior, to make sure if the pch generation fails,
        // the main compilation won't run.
        // FIXME: If the main compilation fails, the PCH generation should
        // probably not be considered successful either.
      }
    }
  }

  // If we are linking, claim any options which are obviously only used for
  // compilation.
  // FIXME: Understand why the last Phase List length is used here.
  if (FinalPhase == phases::Link && LastPLSize == 1) {
    Args.ClaimAllArgs(options::OPT_CompileOnly_Group);
    Args.ClaimAllArgs(options::OPT_cl_compile_Group);
  }
}

void Driver::BuildActions(Compilation &C, DerivedArgList &Args,
                          const InputList &Inputs, ActionList &Actions) const {
  llvm::PrettyStackTraceString CrashInfo("Building compilation actions");

  if (!SuppressMissingInputWarning && Inputs.empty()) {
    Diag(clang::diag::err_drv_no_input_files);
    return;
  }

  // Reject -Z* at the top level, these options should never have been exposed
  // by gcc.
  if (Arg *A = Args.getLastArg(options::OPT_Z_Joined))
    Diag(clang::diag::err_drv_use_of_Z_option) << A->getAsString(Args);

  // Diagnose misuse of /Fo.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_Fo)) {
    StringRef V = A->getValue();
    if (Inputs.size() > 1 && !V.empty() &&
        !llvm::sys::path::is_separator(V.back())) {
      // Check whether /Fo tries to name an output file for multiple inputs.
      Diag(clang::diag::err_drv_out_file_argument_with_multiple_sources)
          << A->getSpelling() << V;
      Args.eraseArg(options::OPT__SLASH_Fo);
    }
  }

  // Diagnose misuse of /Fa.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_Fa)) {
    StringRef V = A->getValue();
    if (Inputs.size() > 1 && !V.empty() &&
        !llvm::sys::path::is_separator(V.back())) {
      // Check whether /Fa tries to name an asm file for multiple inputs.
      Diag(clang::diag::err_drv_out_file_argument_with_multiple_sources)
          << A->getSpelling() << V;
      Args.eraseArg(options::OPT__SLASH_Fa);
    }
  }

  // Diagnose misuse of /o.
  if (Arg *A = Args.getLastArg(options::OPT__SLASH_o)) {
    if (A->getValue()[0] == '\0') {
      // It has to have a value.
      Diag(clang::diag::err_drv_missing_argument) << A->getSpelling() << 1;
      Args.eraseArg(options::OPT__SLASH_o);
    }
  }

  handleArguments(C, Args, Inputs, Actions);

  // When compiling for -fsycl, generate the integration header files and the
  // Unique ID that will be used during the compilation.
  if (Args.hasFlag(options::OPT_fsycl, options::OPT_fno_sycl, false)) {
    for (auto &I : Inputs) {
      std::string SrcFileName(I.second->getAsString(Args));
      if (I.first == types::TY_PP_C || I.first == types::TY_PP_CXX ||
          types::isSrcFile(I.first)) {
        // Unique ID is generated for source files and preprocessed files.
        SmallString<128> ResultID;
        llvm::sys::fs::createUniquePath("%%%%%%%%%%%%%%%%", ResultID, false);
        addSYCLUniqueID(Args.MakeArgString(ResultID.str()), SrcFileName);
      }
      if (!types::isSrcFile(I.first))
        continue;
      std::string TmpFileNameHeader = C.getDriver().GetTemporaryPath(
          llvm::sys::path::stem(SrcFileName).str() + "-header", "h");
      StringRef TmpFileHeader =
          C.addTempFile(C.getArgs().MakeArgString(TmpFileNameHeader));
      std::string TmpFileNameFooter = C.getDriver().GetTemporaryPath(
          llvm::sys::path::stem(SrcFileName).str() + "-footer", "h");
      StringRef TmpFileFooter =
          C.addTempFile(C.getArgs().MakeArgString(TmpFileNameFooter));
      addIntegrationFiles(TmpFileHeader, TmpFileFooter, SrcFileName);
    }
  }

  // Builder to be used to build offloading actions.
  OffloadingActionBuilder OffloadBuilder(C, Args, Inputs);

  // Construct the actions to perform.
  HeaderModulePrecompileJobAction *HeaderModuleAction = nullptr;
  ActionList LinkerInputs;
  ActionList MergerInputs;

  llvm::SmallVector<phases::ID, phases::MaxNumberOfPhases> PL;
  for (auto &I : Inputs) {
    types::ID InputType = I.first;
    const Arg *InputArg = I.second;

    PL = types::getCompilationPhases(*this, Args, InputType);
    if (PL.empty())
      continue;

    auto FullPL = types::getCompilationPhases(InputType);

    // Build the pipeline for this file.
    Action *Current = C.MakeAction<InputAction>(*InputArg, InputType);

    // Use the current host action in any of the offloading actions, if
    // required.
    if (OffloadBuilder.addHostDependenceToDeviceActions(Current, InputArg,
                                                        Args))
      break;

    for (phases::ID Phase : PL) {

      // Add any offload action the host action depends on.
      Current = OffloadBuilder.addDeviceDependencesToHostAction(
          Current, InputArg, Phase, PL.back(), FullPL);
      if (!Current)
        break;

      // Queue linker inputs.
      if (Phase == phases::Link) {
        assert(Phase == PL.back() && "linking must be final compilation step.");
        LinkerInputs.push_back(Current);
        Current = nullptr;
        break;
      }

      // TODO: Consider removing this because the merged may not end up being
      // the final Phase in the pipeline. Perhaps the merged could just merge
      // and then pass an artifact of some sort to the Link Phase.
      // Queue merger inputs.
      if (Phase == phases::IfsMerge) {
        assert(Phase == PL.back() && "merging must be final compilation step.");
        MergerInputs.push_back(Current);
        Current = nullptr;
        break;
      }

      // Each precompiled header file after a module file action is a module
      // header of that same module file, rather than being compiled to a
      // separate PCH.
      if (Phase == phases::Precompile && HeaderModuleAction &&
          getPrecompiledType(InputType) == types::TY_PCH) {
        HeaderModuleAction->addModuleHeaderInput(Current);
        Current = nullptr;
        break;
      }

      // FIXME: Should we include any prior module file outputs as inputs of
      // later actions in the same command line?

      // Otherwise construct the appropriate action.
      Action *NewCurrent = ConstructPhaseAction(C, Args, Phase, Current);

      // We didn't create a new action, so we will just move to the next phase.
      if (NewCurrent == Current)
        continue;

      if (auto *HMA = dyn_cast<HeaderModulePrecompileJobAction>(NewCurrent))
        HeaderModuleAction = HMA;

      Current = NewCurrent;

      // Use the current host action in any of the offloading actions, if
      // required.
      if (OffloadBuilder.addHostDependenceToDeviceActions(Current, InputArg,
                                                          Args))
        break;

      if (Current->getType() == types::TY_Nothing)
        break;
    }

    // If we ended with something, add to the output list.
    if (Current)
      Actions.push_back(Current);

    // Add any top level actions generated for offloading.
    OffloadBuilder.appendTopLevelActions(Actions, Current, InputArg);
  }

  OffloadBuilder.appendTopLevelLinkAction(Actions);

  // With static fat archives we need to create additional steps for
  // generating dependence objects for device link actions.
  if (!LinkerInputs.empty() && C.getDriver().getOffloadStaticLibSeen())
    OffloadBuilder.addDeviceLinkDependenciesFromHost(LinkerInputs);

  // Go through all of the args, and create a Linker specific argument list.
  // When dealing with fat static archives each archive is individually
  // unbundled.
  SmallVector<const char *, 16> LinkArgs(getLinkerArgs(C, Args));
  const llvm::opt::OptTable &Opts = getOpts();
  auto unbundleStaticLib = [&](types::ID T, const StringRef &A) {
    Arg *InputArg = MakeInputArg(Args, Opts, Args.MakeArgString(A));
    Action *Current = C.MakeAction<InputAction>(*InputArg, T);
    OffloadBuilder.addHostDependenceToDeviceActions(Current, InputArg, Args);
    OffloadBuilder.addDeviceDependencesToHostAction(
        Current, InputArg, phases::Link, PL.back(), PL);
  };
  for (StringRef LA : LinkArgs) {
    // At this point, we will process the archives for FPGA AOCO and individual
    // archive unbundling for Windows.
    if (!isStaticArchiveFile(LA))
      continue;
    // FPGA AOCX/AOCR files are archives, but we do not want to unbundle them
    // here as they have already been unbundled and processed for linking.
    // TODO: The multiple binary checks for FPGA types getting a little out
    // of hand. Improve this by doing a single scan of the args and holding
    // that in a data structure for reference.
    if (hasFPGABinary(C, LA.str(), types::TY_FPGA_AOCX) ||
        hasFPGABinary(C, LA.str(), types::TY_FPGA_AOCR) ||
        hasFPGABinary(C, LA.str(), types::TY_FPGA_AOCR_EMU))
      continue;
    // For offload-static-libs we add an unbundling action for each static
    // archive which produces list files with extracted objects. Device lists
    // are then added to the appropriate device link actions and host list is
    // ignored since we are adding offload-static-libs as normal libraries to
    // the host link command.
    if (hasOffloadSections(C, LA, Args)) {
      unbundleStaticLib(types::TY_Archive, LA);
      // Pass along the static libraries to check if we need to add them for
      // unbundling for FPGA AOT static lib usage.  Uses FPGA aoco type to
      // differentiate if aoco unbundling is needed.  Unbundling of aoco is not
      // needed for emulation, as these are treated as regular archives.
      if (!C.getDriver().isFPGAEmulationMode())
        unbundleStaticLib(types::TY_FPGA_AOCO, LA);
    }
  }

  // For an FPGA archive, we add the unbundling step above to take care of
  // the device side, but also unbundle here to extract the host side
  bool EarlyLink = false;
  if (const Arg *A = Args.getLastArg(options::OPT_fsycl_link_EQ))
    EarlyLink = A->getValue() == StringRef("early");
  for (auto &LI : LinkerInputs) {
    Action *UnbundlerInput = nullptr;
    auto wrapObject = [&] {
      if (EarlyLink && Args.hasArg(options::OPT_fintelfpga)) {
        // Only wrap the object with -fsycl-link=early
        auto *BC = C.MakeAction<OffloadWrapperJobAction>(LI, types::TY_LLVM_BC);
        auto *ASM = C.MakeAction<BackendJobAction>(BC, types::TY_PP_Asm);
        LI = C.MakeAction<AssembleJobAction>(ASM, types::TY_Object);
      }
    };
    if (auto *IA = dyn_cast<InputAction>(LI)) {
      if (IA->getType() == types::TY_FPGA_AOCR ||
          IA->getType() == types::TY_FPGA_AOCX ||
          IA->getType() == types::TY_FPGA_AOCR_EMU) {
        // Add to unbundler.
        UnbundlerInput = LI;
      } else {
        std::string FileName = IA->getInputArg().getAsString(Args);
        if ((IA->getType() == types::TY_Object && !isObjectFile(FileName)) ||
            IA->getInputArg().getOption().hasFlag(options::LinkerInput))
          continue;
        wrapObject();
      }
    } else {
      wrapObject();
    }
    if (UnbundlerInput && !PL.empty()) {
      if (auto *IA = dyn_cast<InputAction>(UnbundlerInput)) {
        std::string FileName = IA->getInputArg().getAsString(Args);
        Arg *InputArg = MakeInputArg(Args, Opts, FileName);
        OffloadBuilder.addHostDependenceToDeviceActions(UnbundlerInput,
                                                        InputArg, Args);
        OffloadBuilder.addDeviceDependencesToHostAction(
            UnbundlerInput, InputArg, phases::Link, PL.back(), PL);
      }
    }
  }

  // Add a link action if necessary.
  if (!LinkerInputs.empty()) {
    OffloadBuilder.makeHostLinkAction(LinkerInputs);
    types::ID LinkType(types::TY_Image);
    if (Args.hasArg(options::OPT_fsycl_link_EQ))
      LinkType = types::TY_Archive;
    Action *LA;
    // Check if this Linker Job should emit a static library.
    if (ShouldEmitStaticLibrary(Args)) {
      LA = C.MakeAction<StaticLibJobAction>(LinkerInputs, LinkType);
    } else {
      LA = C.MakeAction<LinkJobAction>(LinkerInputs, LinkType);
    }
    LA = OffloadBuilder.processHostLinkAction(LA);
    Actions.push_back(LA);
  }

  // Add an interface stubs merge action if necessary.
  if (!MergerInputs.empty())
    Actions.push_back(
        C.MakeAction<IfsMergeJobAction>(MergerInputs, types::TY_Image));

  if (Args.hasArg(options::OPT_emit_interface_stubs)) {
    auto PhaseList = types::getCompilationPhases(
        types::TY_IFS_CPP,
        Args.hasArg(options::OPT_c) ? phases::Compile : phases::LastPhase);

    ActionList MergerInputs;

    for (auto &I : Inputs) {
      types::ID InputType = I.first;
      const Arg *InputArg = I.second;

      // Currently clang and the llvm assembler do not support generating symbol
      // stubs from assembly, so we skip the input on asm files. For ifs files
      // we rely on the normal pipeline setup in the pipeline setup code above.
      if (InputType == types::TY_IFS || InputType == types::TY_PP_Asm ||
          InputType == types::TY_Asm)
        continue;

      Action *Current = C.MakeAction<InputAction>(*InputArg, InputType);

      for (auto Phase : PhaseList) {
        switch (Phase) {
        default:
          llvm_unreachable(
              "IFS Pipeline can only consist of Compile followed by IfsMerge.");
        case phases::Compile: {
          // Only IfsMerge (llvm-ifs) can handle .o files by looking for ifs
          // files where the .o file is located. The compile action can not
          // handle this.
          if (InputType == types::TY_Object)
            break;

          Current = C.MakeAction<CompileJobAction>(Current, types::TY_IFS_CPP);
          break;
        }
        case phases::IfsMerge: {
          assert(Phase == PhaseList.back() &&
                 "merging must be final compilation step.");
          MergerInputs.push_back(Current);
          Current = nullptr;
          break;
        }
        }
      }

      // If we ended with something, add to the output list.
      if (Current)
        Actions.push_back(Current);
    }

    // Add an interface stubs merge action if necessary.
    if (!MergerInputs.empty())
      Actions.push_back(
          C.MakeAction<IfsMergeJobAction>(MergerInputs, types::TY_Image));
  }

  // If --print-supported-cpus, -mcpu=? or -mtune=? is specified, build a custom
  // Compile phase that prints out supported cpu models and quits.
  if (Arg *A = Args.getLastArg(options::OPT_print_supported_cpus)) {
    // Use the -mcpu=? flag as the dummy input to cc1.
    Actions.clear();
    Action *InputAc = C.MakeAction<InputAction>(*A, types::TY_C);
    Actions.push_back(
        C.MakeAction<PrecompileJobAction>(InputAc, types::TY_Nothing));
    for (auto &I : Inputs)
      I.second->claim();
  }

  // Claim ignored clang-cl options.
  Args.ClaimAllArgs(options::OPT_cl_ignored_Group);

  // Claim --cuda-host-only and --cuda-compile-host-device, which may be passed
  // to non-CUDA compilations and should not trigger warnings there.
  Args.ClaimAllArgs(options::OPT_cuda_host_only);
  Args.ClaimAllArgs(options::OPT_cuda_compile_host_device);
}

Action *Driver::ConstructPhaseAction(
    Compilation &C, const ArgList &Args, phases::ID Phase, Action *Input,
    Action::OffloadKind TargetDeviceOffloadKind) const {
  llvm::PrettyStackTraceString CrashInfo("Constructing phase actions");

  // Some types skip the assembler phase (e.g., llvm-bc), but we can't
  // encode this in the steps because the intermediate type depends on
  // arguments. Just special case here.
  if (Phase == phases::Assemble && Input->getType() != types::TY_PP_Asm)
    return Input;

  // Build the appropriate action.
  switch (Phase) {
  case phases::Link:
    llvm_unreachable("link action invalid here.");
  case phases::IfsMerge:
    llvm_unreachable("ifsmerge action invalid here.");
  case phases::Preprocess: {
    types::ID OutputTy;
    // -M and -MM specify the dependency file name by altering the output type,
    // -if -MD and -MMD are not specified.
    if (Args.hasArg(options::OPT_M, options::OPT_MM) &&
        !Args.hasArg(options::OPT_MD, options::OPT_MMD)) {
      OutputTy = types::TY_Dependencies;
    } else {
      OutputTy = Input->getType();
      if (!Args.hasFlag(options::OPT_frewrite_includes,
                        options::OPT_fno_rewrite_includes, false) &&
          !Args.hasFlag(options::OPT_frewrite_imports,
                        options::OPT_fno_rewrite_imports, false) &&
          !CCGenDiagnostics)
        OutputTy = types::getPreprocessedType(OutputTy);
      assert(OutputTy != types::TY_INVALID &&
             "Cannot preprocess this input type!");
    }
    types::ID HostPPType = types::getPreprocessedType(Input->getType());
    if (Args.hasArg(options::OPT_fsycl) && HostPPType != types::TY_INVALID &&
        !Args.hasArg(options::OPT_fno_sycl_use_footer) &&
        TargetDeviceOffloadKind == Action::OFK_None) {
      // Performing a host compilation with -fsycl.  Append the integration
      // footer to the source file.
      auto *AppendFooter =
          C.MakeAction<AppendFooterJobAction>(Input, types::TY_CXX);
      // FIXME: There are 2 issues with dependency generation in regards to
      // the integration footer that need to be addressed.
      // 1) Input file referenced on the RHS of a dependency is based on the
      //    input src, which is a temporary.  We want this to be the true
      //    user input src file.
      // 2) When generating dependencies against a preprocessed file, header
      //    file information (using -MD or-MMD) is not provided.
      return C.MakeAction<PreprocessJobAction>(AppendFooter, OutputTy);
    }
    return C.MakeAction<PreprocessJobAction>(Input, OutputTy);
  }
  case phases::Precompile: {
    types::ID OutputTy = getPrecompiledType(Input->getType());
    assert(OutputTy != types::TY_INVALID &&
           "Cannot precompile this input type!");

    // If we're given a module name, precompile header file inputs as a
    // module, not as a precompiled header.
    const char *ModName = nullptr;
    if (OutputTy == types::TY_PCH) {
      if (Arg *A = Args.getLastArg(options::OPT_fmodule_name_EQ))
        ModName = A->getValue();
      if (ModName)
        OutputTy = types::TY_ModuleFile;
    }

    if (Args.hasArg(options::OPT_fsyntax_only)) {
      // Syntax checks should not emit a PCH file
      OutputTy = types::TY_Nothing;
    }

    if (ModName)
      return C.MakeAction<HeaderModulePrecompileJobAction>(Input, OutputTy,
                                                           ModName);
    return C.MakeAction<PrecompileJobAction>(Input, OutputTy);
  }
  case phases::Compile: {
    if (Args.hasArg(options::OPT_fsyntax_only))
      return C.MakeAction<CompileJobAction>(Input, types::TY_Nothing);
    if (Args.hasArg(options::OPT_rewrite_objc))
      return C.MakeAction<CompileJobAction>(Input, types::TY_RewrittenObjC);
    if (Args.hasArg(options::OPT_rewrite_legacy_objc))
      return C.MakeAction<CompileJobAction>(Input,
                                            types::TY_RewrittenLegacyObjC);
    if (Args.hasArg(options::OPT__analyze))
      return C.MakeAction<AnalyzeJobAction>(Input, types::TY_Plist);
    if (Args.hasArg(options::OPT__migrate))
      return C.MakeAction<MigrateJobAction>(Input, types::TY_Remap);
    if (Args.hasArg(options::OPT_emit_ast))
      return C.MakeAction<CompileJobAction>(Input, types::TY_AST);
    if (Args.hasArg(options::OPT_module_file_info))
      return C.MakeAction<CompileJobAction>(Input, types::TY_ModuleFile);
    if (Args.hasArg(options::OPT_verify_pch))
      return C.MakeAction<VerifyPCHJobAction>(Input, types::TY_Nothing);
    return C.MakeAction<CompileJobAction>(Input, types::TY_LLVM_BC);
  }
  case phases::Backend: {
    if (isUsingLTO() && TargetDeviceOffloadKind == Action::OFK_None) {
      types::ID Output =
          Args.hasArg(options::OPT_S) ? types::TY_LTO_IR : types::TY_LTO_BC;
      return C.MakeAction<BackendJobAction>(Input, Output);
    }
    if (Args.hasArg(options::OPT_emit_llvm) ||
        (TargetDeviceOffloadKind == Action::OFK_HIP &&
         Args.hasFlag(options::OPT_fgpu_rdc, options::OPT_fno_gpu_rdc,
                      false))) {
      types::ID Output =
          Args.hasArg(options::OPT_S) ? types::TY_LLVM_IR : types::TY_LLVM_BC;
      return C.MakeAction<BackendJobAction>(Input, Output);
    }
    return C.MakeAction<BackendJobAction>(Input, types::TY_PP_Asm);
  }
  case phases::Assemble:
    return C.MakeAction<AssembleJobAction>(std::move(Input), types::TY_Object);
  }

  llvm_unreachable("invalid phase in ConstructPhaseAction");
}

void Driver::BuildJobs(Compilation &C) const {
  llvm::PrettyStackTraceString CrashInfo("Building compilation jobs");

  Arg *FinalOutput = C.getArgs().getLastArg(options::OPT_o);

  // It is an error to provide a -o option if we are making multiple output
  // files. There are exceptions:
  //
  // IfsMergeJob: when generating interface stubs enabled we want to be able to
  // generate the stub file at the same time that we generate the real
  // library/a.out. So when a .o, .so, etc are the output, with clang interface
  // stubs there will also be a .ifs and .ifso at the same location.
  //
  // CompileJob of type TY_IFS_CPP: when generating interface stubs is enabled
  // and -c is passed, we still want to be able to generate a .ifs file while
  // we are also generating .o files. So we allow more than one output file in
  // this case as well.
  //
  if (FinalOutput) {
    unsigned NumOutputs = 0;
    unsigned NumIfsOutputs = 0;
    for (const Action *A : C.getActions())
      if (A->getType() != types::TY_Nothing &&
          !(A->getKind() == Action::IfsMergeJobClass ||
            (A->getType() == clang::driver::types::TY_IFS_CPP &&
             A->getKind() == clang::driver::Action::CompileJobClass &&
             0 == NumIfsOutputs++) ||
            (A->getKind() == Action::BindArchClass && A->getInputs().size() &&
             A->getInputs().front()->getKind() == Action::IfsMergeJobClass)))
        ++NumOutputs;

    if (NumOutputs > 1) {
      Diag(clang::diag::err_drv_output_argument_with_multiple_files);
      FinalOutput = nullptr;
    }
  }

  const llvm::Triple &RawTriple = C.getDefaultToolChain().getTriple();
  if (RawTriple.isOSAIX()) {
    if (Arg *A = C.getArgs().getLastArg(options::OPT_G))
      Diag(diag::err_drv_unsupported_opt_for_target)
          << A->getSpelling() << RawTriple.str();
    if (LTOMode == LTOK_Thin)
      Diag(diag::err_drv_clang_unsupported) << "thinLTO on AIX";
  }

  // Collect the list of architectures.
  llvm::StringSet<> ArchNames;
  if (RawTriple.isOSBinFormatMachO())
    for (const Arg *A : C.getArgs())
      if (A->getOption().matches(options::OPT_arch))
        ArchNames.insert(A->getValue());

  // Set of (Action, canonical ToolChain triple) pairs we've built jobs for.
  std::map<std::pair<const Action *, std::string>, InputInfo> CachedResults;
  for (Action *A : C.getActions()) {
    // If we are linking an image for multiple archs then the linker wants
    // -arch_multiple and -final_output <final image name>. Unfortunately, this
    // doesn't fit in cleanly because we have to pass this information down.
    //
    // FIXME: This is a hack; find a cleaner way to integrate this into the
    // process.
    const char *LinkingOutput = nullptr;
    if (isa<LipoJobAction>(A)) {
      if (FinalOutput)
        LinkingOutput = FinalOutput->getValue();
      else
        LinkingOutput = getDefaultImageName();
    }

    BuildJobsForAction(C, A, &C.getDefaultToolChain(),
                       /*BoundArch*/ StringRef(),
                       /*AtTopLevel*/ true,
                       /*MultipleArchs*/ ArchNames.size() > 1,
                       /*LinkingOutput*/ LinkingOutput, CachedResults,
                       /*TargetDeviceOffloadKind*/ Action::OFK_None);
  }

  // If we have more than one job, then disable integrated-cc1 for now. Do this
  // also when we need to report process execution statistics.
  if (C.getJobs().size() > 1 || CCPrintProcessStats)
    for (auto &J : C.getJobs())
      J.InProcess = false;

  if (CCPrintProcessStats) {
    C.setPostCallback([=](const Command &Cmd, int Res) {
      Optional<llvm::sys::ProcessStatistics> ProcStat =
          Cmd.getProcessStatistics();
      if (!ProcStat)
        return;

      const char *LinkingOutput = nullptr;
      if (FinalOutput)
        LinkingOutput = FinalOutput->getValue();
      else if (!Cmd.getOutputFilenames().empty())
        LinkingOutput = Cmd.getOutputFilenames().front().c_str();
      else
        LinkingOutput = getDefaultImageName();

      if (CCPrintStatReportFilename.empty()) {
        using namespace llvm;
        // Human readable output.
        outs() << sys::path::filename(Cmd.getExecutable()) << ": "
               << "output=" << LinkingOutput;
        outs() << ", total="
               << format("%.3f", ProcStat->TotalTime.count() / 1000.) << " ms"
               << ", user="
               << format("%.3f", ProcStat->UserTime.count() / 1000.) << " ms"
               << ", mem=" << ProcStat->PeakMemory << " Kb\n";
      } else {
        // CSV format.
        std::string Buffer;
        llvm::raw_string_ostream Out(Buffer);
        llvm::sys::printArg(Out, llvm::sys::path::filename(Cmd.getExecutable()),
                            /*Quote*/ true);
        Out << ',';
        llvm::sys::printArg(Out, LinkingOutput, true);
        Out << ',' << ProcStat->TotalTime.count() << ','
            << ProcStat->UserTime.count() << ',' << ProcStat->PeakMemory
            << '\n';
        Out.flush();
        std::error_code EC;
        llvm::raw_fd_ostream OS(CCPrintStatReportFilename.c_str(), EC,
                                llvm::sys::fs::OF_Append |
                                    llvm::sys::fs::OF_Text);
        if (EC)
          return;
        auto L = OS.lock();
        if (!L) {
          llvm::errs() << "ERROR: Cannot lock file "
                       << CCPrintStatReportFilename << ": "
                       << toString(L.takeError()) << "\n";
          return;
        }
        OS << Buffer;
        OS.flush();
      }
    });
  }

  // If the user passed -Qunused-arguments or there were errors, don't warn
  // about any unused arguments.
  if (Diags.hasErrorOccurred() ||
      C.getArgs().hasArg(options::OPT_Qunused_arguments))
    return;

  // Claim -### here.
  (void)C.getArgs().hasArg(options::OPT__HASH_HASH_HASH);

  // Claim --driver-mode, --rsp-quoting, it was handled earlier.
  (void)C.getArgs().hasArg(options::OPT_driver_mode);
  (void)C.getArgs().hasArg(options::OPT_rsp_quoting);

  for (Arg *A : C.getArgs()) {
    // FIXME: It would be nice to be able to send the argument to the
    // DiagnosticsEngine, so that extra values, position, and so on could be
    // printed.
    if (!A->isClaimed()) {
      if (A->getOption().hasFlag(options::NoArgumentUnused))
        continue;

      // Suppress the warning automatically if this is just a flag, and it is an
      // instance of an argument we already claimed.
      const Option &Opt = A->getOption();
      if (Opt.getKind() == Option::FlagClass) {
        bool DuplicateClaimed = false;

        for (const Arg *AA : C.getArgs().filtered(&Opt)) {
          if (AA->isClaimed()) {
            DuplicateClaimed = true;
            break;
          }
        }

        if (DuplicateClaimed)
          continue;
      }

      // In clang-cl, don't mention unknown arguments here since they have
      // already been warned about.
      if (!IsCLMode() || !A->getOption().matches(options::OPT_UNKNOWN))
        Diag(clang::diag::warn_drv_unused_argument)
            << A->getAsString(C.getArgs());
    }
  }
}

namespace {
/// Utility class to control the collapse of dependent actions and select the
/// tools accordingly.
class ToolSelector final {
  /// The tool chain this selector refers to.
  const ToolChain &TC;

  /// The compilation this selector refers to.
  const Compilation &C;

  /// The base action this selector refers to.
  const JobAction *BaseAction;

  /// Set to true if the current toolchain refers to host actions.
  bool IsHostSelector;

  /// Set to true if save-temps and embed-bitcode functionalities are active.
  bool SaveTemps;
  bool EmbedBitcode;

  /// Get previous dependent action or null if that does not exist. If
  /// \a CanBeCollapsed is false, that action must be legal to collapse or
  /// null will be returned.
  const JobAction *getPrevDependentAction(const ActionList &Inputs,
                                          ActionList &SavedOffloadAction,
                                          bool CanBeCollapsed = true) {
    // An option can be collapsed only if it has a single input.
    if (Inputs.size() != 1)
      return nullptr;

    Action *CurAction = *Inputs.begin();
    if (CanBeCollapsed &&
        !CurAction->isCollapsingWithNextDependentActionLegal())
      return nullptr;

    // If the input action is an offload action. Look through it and save any
    // offload action that can be dropped in the event of a collapse.
    if (auto *OA = dyn_cast<OffloadAction>(CurAction)) {
      // If the dependent action is a device action, we will attempt to collapse
      // only with other device actions. Otherwise, we would do the same but
      // with host actions only.
      if (!IsHostSelector) {
        if (OA->hasSingleDeviceDependence(/*DoNotConsiderHostActions=*/true)) {
          CurAction =
              OA->getSingleDeviceDependence(/*DoNotConsiderHostActions=*/true);
          if (CanBeCollapsed &&
              !CurAction->isCollapsingWithNextDependentActionLegal())
            return nullptr;
          SavedOffloadAction.push_back(OA);
          return dyn_cast<JobAction>(CurAction);
        }
      } else if (OA->hasHostDependence()) {
        CurAction = OA->getHostDependence();
        if (CanBeCollapsed &&
            !CurAction->isCollapsingWithNextDependentActionLegal())
          return nullptr;
        SavedOffloadAction.push_back(OA);
        return dyn_cast<JobAction>(CurAction);
      }
      return nullptr;
    }

    return dyn_cast<JobAction>(CurAction);
  }

  /// Return true if an assemble action can be collapsed.
  bool canCollapseAssembleAction() const {
    return TC.useIntegratedAs() && !SaveTemps &&
           !C.getArgs().hasArg(options::OPT_via_file_asm) &&
           !C.getArgs().hasArg(options::OPT__SLASH_FA) &&
           !C.getArgs().hasArg(options::OPT__SLASH_Fa);
  }

  /// Return true if a preprocessor action can be collapsed.
  bool canCollapsePreprocessorAction() const {
    return !C.getArgs().hasArg(options::OPT_no_integrated_cpp) &&
           !C.getArgs().hasArg(options::OPT_traditional_cpp) && !SaveTemps &&
           !C.getArgs().hasArg(options::OPT_rewrite_objc);
  }

  /// Struct that relates an action with the offload actions that would be
  /// collapsed with it.
  struct JobActionInfo final {
    /// The action this info refers to.
    const JobAction *JA = nullptr;
    /// The offload actions we need to take care off if this action is
    /// collapsed.
    ActionList SavedOffloadAction;
  };

  /// Append collapsed offload actions from the give nnumber of elements in the
  /// action info array.
  static void AppendCollapsedOffloadAction(ActionList &CollapsedOffloadAction,
                                           ArrayRef<JobActionInfo> &ActionInfo,
                                           unsigned ElementNum) {
    assert(ElementNum <= ActionInfo.size() && "Invalid number of elements.");
    for (unsigned I = 0; I < ElementNum; ++I)
      CollapsedOffloadAction.append(ActionInfo[I].SavedOffloadAction.begin(),
                                    ActionInfo[I].SavedOffloadAction.end());
  }

  /// Functions that attempt to perform the combining. They detect if that is
  /// legal, and if so they update the inputs \a Inputs and the offload action
  /// that were collapsed in \a CollapsedOffloadAction. A tool that deals with
  /// the combined action is returned. If the combining is not legal or if the
  /// tool does not exist, null is returned.
  /// Currently three kinds of collapsing are supported:
  ///  - Assemble + Backend + Compile;
  ///  - Assemble + Backend ;
  ///  - Backend + Compile.
  const Tool *
  combineAssembleBackendCompile(ArrayRef<JobActionInfo> ActionInfo,
                                ActionList &Inputs,
                                ActionList &CollapsedOffloadAction) {
    if (ActionInfo.size() < 3 || !canCollapseAssembleAction())
      return nullptr;
    auto *AJ = dyn_cast<AssembleJobAction>(ActionInfo[0].JA);
    auto *BJ = dyn_cast<BackendJobAction>(ActionInfo[1].JA);
    auto *CJ = dyn_cast<CompileJobAction>(ActionInfo[2].JA);
    if (!AJ || !BJ || !CJ)
      return nullptr;

    // Get compiler tool.
    const Tool *T = TC.SelectTool(*CJ);
    if (!T)
      return nullptr;

    // When using -fembed-bitcode, it is required to have the same tool (clang)
    // for both CompilerJA and BackendJA. Otherwise, combine two stages.
    if (EmbedBitcode) {
      const Tool *BT = TC.SelectTool(*BJ);
      if (BT == T)
        return nullptr;
    }

    if (!T->hasIntegratedAssembler())
      return nullptr;

    Inputs = CJ->getInputs();
    AppendCollapsedOffloadAction(CollapsedOffloadAction, ActionInfo,
                                 /*NumElements=*/3);
    return T;
  }
  const Tool *combineAssembleBackend(ArrayRef<JobActionInfo> ActionInfo,
                                     ActionList &Inputs,
                                     ActionList &CollapsedOffloadAction) {
    if (ActionInfo.size() < 2 || !canCollapseAssembleAction())
      return nullptr;
    auto *AJ = dyn_cast<AssembleJobAction>(ActionInfo[0].JA);
    auto *BJ = dyn_cast<BackendJobAction>(ActionInfo[1].JA);
    if (!AJ || !BJ)
      return nullptr;

    // Get backend tool.
    const Tool *T = TC.SelectTool(*BJ);
    if (!T)
      return nullptr;

    if (!T->hasIntegratedAssembler())
      return nullptr;

    Inputs = BJ->getInputs();
    AppendCollapsedOffloadAction(CollapsedOffloadAction, ActionInfo,
                                 /*NumElements=*/2);
    return T;
  }
  const Tool *combineBackendCompile(ArrayRef<JobActionInfo> ActionInfo,
                                    ActionList &Inputs,
                                    ActionList &CollapsedOffloadAction) {
    if (ActionInfo.size() < 2)
      return nullptr;
    auto *BJ = dyn_cast<BackendJobAction>(ActionInfo[0].JA);
    auto *CJ = dyn_cast<CompileJobAction>(ActionInfo[1].JA);
    if (!BJ || !CJ)
      return nullptr;

    // Check if the initial input (to the compile job or its predessor if one
    // exists) is LLVM bitcode. In that case, no preprocessor step is required
    // and we can still collapse the compile and backend jobs when we have
    // -save-temps. I.e. there is no need for a separate compile job just to
    // emit unoptimized bitcode.
    bool InputIsBitcode = true;
    for (size_t i = 1; i < ActionInfo.size(); i++)
      if (ActionInfo[i].JA->getType() != types::TY_LLVM_BC &&
          ActionInfo[i].JA->getType() != types::TY_LTO_BC) {
        InputIsBitcode = false;
        break;
      }
    if (!InputIsBitcode && !canCollapsePreprocessorAction())
      return nullptr;

    // Get compiler tool.
    const Tool *T = TC.SelectTool(*CJ);
    if (!T)
      return nullptr;

    if (T->canEmitIR() && ((SaveTemps && !InputIsBitcode) || EmbedBitcode))
      return nullptr;

    Inputs = CJ->getInputs();
    AppendCollapsedOffloadAction(CollapsedOffloadAction, ActionInfo,
                                 /*NumElements=*/2);
    return T;
  }

  /// Updates the inputs if the obtained tool supports combining with
  /// preprocessor action, and the current input is indeed a preprocessor
  /// action. If combining results in the collapse of offloading actions, those
  /// are appended to \a CollapsedOffloadAction.
  void combineWithPreprocessor(const Tool *T, ActionList &Inputs,
                               ActionList &CollapsedOffloadAction) {
    if (!T || !canCollapsePreprocessorAction() || !T->hasIntegratedCPP())
      return;

    // Attempt to get a preprocessor action dependence.
    ActionList PreprocessJobOffloadActions;
    ActionList NewInputs;
    for (Action *A : Inputs) {
      auto *PJ = getPrevDependentAction({A}, PreprocessJobOffloadActions);
      if (!PJ || !isa<PreprocessJobAction>(PJ)) {
        NewInputs.push_back(A);
        continue;
      }

      // This is legal to combine. Append any offload action we found and add the
      // current input to preprocessor inputs.
      CollapsedOffloadAction.append(PreprocessJobOffloadActions.begin(),
                                    PreprocessJobOffloadActions.end());
      NewInputs.append(PJ->input_begin(), PJ->input_end());
    }
    Inputs = NewInputs;
  }

public:
  ToolSelector(const JobAction *BaseAction, const ToolChain &TC,
               const Compilation &C, bool SaveTemps, bool EmbedBitcode)
      : TC(TC), C(C), BaseAction(BaseAction), SaveTemps(SaveTemps),
        EmbedBitcode(EmbedBitcode) {
    assert(BaseAction && "Invalid base action.");
    IsHostSelector = BaseAction->getOffloadingDeviceKind() == Action::OFK_None;
  }

  /// Check if a chain of actions can be combined and return the tool that can
  /// handle the combination of actions. The pointer to the current inputs \a
  /// Inputs and the list of offload actions \a CollapsedOffloadActions
  /// connected to collapsed actions are updated accordingly. The latter enables
  /// the caller of the selector to process them afterwards instead of just
  /// dropping them. If no suitable tool is found, null will be returned.
  const Tool *getTool(ActionList &Inputs,
                      ActionList &CollapsedOffloadAction) {
    //
    // Get the largest chain of actions that we could combine.
    //

    SmallVector<JobActionInfo, 5> ActionChain(1);
    ActionChain.back().JA = BaseAction;
    while (ActionChain.back().JA) {
      const Action *CurAction = ActionChain.back().JA;

      // Grow the chain by one element.
      ActionChain.resize(ActionChain.size() + 1);
      JobActionInfo &AI = ActionChain.back();

      // Attempt to fill it with the
      AI.JA =
          getPrevDependentAction(CurAction->getInputs(), AI.SavedOffloadAction);
    }

    // Pop the last action info as it could not be filled.
    ActionChain.pop_back();

    //
    // Attempt to combine actions. If all combining attempts failed, just return
    // the tool of the provided action. At the end we attempt to combine the
    // action with any preprocessor action it may depend on.
    //

    const Tool *T = combineAssembleBackendCompile(ActionChain, Inputs,
                                                  CollapsedOffloadAction);
    if (!T)
      T = combineAssembleBackend(ActionChain, Inputs, CollapsedOffloadAction);
    if (!T)
      T = combineBackendCompile(ActionChain, Inputs, CollapsedOffloadAction);
    if (!T) {
      Inputs = BaseAction->getInputs();
      T = TC.SelectTool(*BaseAction);
    }

    combineWithPreprocessor(T, Inputs, CollapsedOffloadAction);
    return T;
  }
};
}

/// Return a string that uniquely identifies the result of a job. The bound arch
/// is not necessarily represented in the toolchain's triple -- for example,
/// armv7 and armv7s both map to the same triple -- so we need both in our map.
/// Also, we need to add the offloading device kind, as the same tool chain can
/// be used for host and device for some programming models, e.g. OpenMP.
static std::string GetTriplePlusArchString(const ToolChain *TC,
                                           StringRef BoundArch,
                                           Action::OffloadKind OffloadKind) {
  std::string TriplePlusArch = TC->getTriple().normalize();
  if (!BoundArch.empty()) {
    TriplePlusArch += "-";
    TriplePlusArch += BoundArch;
  }
  TriplePlusArch += "-";
  TriplePlusArch += Action::GetOffloadKindName(OffloadKind);
  return TriplePlusArch;
}

static void CollectForEachInputs(
    InputInfoList &InputInfos, const Action *SourceAction, const ToolChain *TC,
    StringRef BoundArch, Action::OffloadKind TargetDeviceOffloadKind,
    const std::map<std::pair<const Action *, std::string>, InputInfo>
        &CachedResults) {
  for (const Action *Input : SourceAction->getInputs()) {
    // Search for the Input, if not in the cache assume actions were collapsed
    // so recurse.
    auto Lookup = CachedResults.find(
        {Input,
         GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)});
    if (Lookup != CachedResults.end()) {
      InputInfos.push_back(Lookup->second);
    } else {
      CollectForEachInputs(InputInfos, Input, TC, BoundArch,
                           TargetDeviceOffloadKind, CachedResults);
    }
  }
}

InputInfo Driver::BuildJobsForAction(
    Compilation &C, const Action *A, const ToolChain *TC, StringRef BoundArch,
    bool AtTopLevel, bool MultipleArchs, const char *LinkingOutput,
    std::map<std::pair<const Action *, std::string>, InputInfo> &CachedResults,
    Action::OffloadKind TargetDeviceOffloadKind) const {
  std::pair<const Action *, std::string> ActionTC = {
      A, GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)};
  auto CachedResult = CachedResults.find(ActionTC);
  if (CachedResult != CachedResults.end()) {
    return CachedResult->second;
  }
  InputInfo Result = BuildJobsForActionNoCache(
      C, A, TC, BoundArch, AtTopLevel, MultipleArchs, LinkingOutput,
      CachedResults, TargetDeviceOffloadKind);
  CachedResults[ActionTC] = Result;
  return Result;
}

InputInfo Driver::BuildJobsForActionNoCache(
    Compilation &C, const Action *A, const ToolChain *TC, StringRef BoundArch,
    bool AtTopLevel, bool MultipleArchs, const char *LinkingOutput,
    std::map<std::pair<const Action *, std::string>, InputInfo> &CachedResults,
    Action::OffloadKind TargetDeviceOffloadKind) const {
  llvm::PrettyStackTraceString CrashInfo("Building compilation jobs");

  InputInfoList OffloadDependencesInputInfo;
  bool BuildingForOffloadDevice = TargetDeviceOffloadKind != Action::OFK_None;
  if (const OffloadAction *OA = dyn_cast<OffloadAction>(A)) {
    // The 'Darwin' toolchain is initialized only when its arguments are
    // computed. Get the default arguments for OFK_None to ensure that
    // initialization is performed before processing the offload action.
    // FIXME: Remove when darwin's toolchain is initialized during construction.
    C.getArgsForToolChain(TC, BoundArch, Action::OFK_None);

    // The offload action is expected to be used in four different situations.
    //
    // a) Set a toolchain/architecture/kind for a host action:
    //    Host Action 1 -> OffloadAction -> Host Action 2
    //
    // b) Set a toolchain/architecture/kind for a device action;
    //    Device Action 1 -> OffloadAction -> Device Action 2
    //
    // c) Specify a device dependence to a host action;
    //    Device Action 1  _
    //                      \
    //      Host Action 1  ---> OffloadAction -> Host Action 2
    //
    // d) Specify a host dependence to a device action.
    //      Host Action 1  _
    //                      \
    //    Device Action 1  ---> OffloadAction -> Device Action 2
    //
    // For a) and b), we just return the job generated for the dependence. For
    // c) and d) we override the current action with the host/device dependence
    // if the current toolchain is host/device and set the offload dependences
    // info with the jobs obtained from the device/host dependence(s).

    // If there is a single device option, just generate the job for it.
    if (OA->hasSingleDeviceDependence()) {
      InputInfo DevA;
      OA->doOnEachDeviceDependence([&](Action *DepA, const ToolChain *DepTC,
                                       const char *DepBoundArch) {
        DevA =
            BuildJobsForAction(C, DepA, DepTC, DepBoundArch, AtTopLevel,
                               /*MultipleArchs*/ !!DepBoundArch, LinkingOutput,
                               CachedResults, DepA->getOffloadingDeviceKind());
      });
      return DevA;
    }

    // If 'Action 2' is host, we generate jobs for the device dependences and
    // override the current action with the host dependence. Otherwise, we
    // generate the host dependences and override the action with the device
    // dependence. The dependences can't therefore be a top-level action.
    OA->doOnEachDependence(
        /*IsHostDependence=*/BuildingForOffloadDevice,
        [&](Action *DepA, const ToolChain *DepTC, const char *DepBoundArch) {
          OffloadDependencesInputInfo.push_back(BuildJobsForAction(
              C, DepA, DepTC, DepBoundArch, /*AtTopLevel=*/false,
              /*MultipleArchs*/ !!DepBoundArch, LinkingOutput, CachedResults,
              DepA->getOffloadingDeviceKind()));
        });

    A = BuildingForOffloadDevice
            ? OA->getSingleDeviceDependence(/*DoNotConsiderHostActions=*/true)
            : OA->getHostDependence();
  }

  if (const InputAction *IA = dyn_cast<InputAction>(A)) {
    // FIXME: It would be nice to not claim this here; maybe the old scheme of
    // just using Args was better?
    const Arg &Input = IA->getInputArg();
    Input.claim();
    if (Input.getOption().matches(options::OPT_INPUT)) {
      const char *Name = Input.getValue();
      return InputInfo(A, Name, /* _BaseInput = */ Name);
    }
    return InputInfo(A, &Input, /* _BaseInput = */ "");
  }
  if (const BindArchAction *BAA = dyn_cast<BindArchAction>(A)) {
    const ToolChain *TC;
    StringRef ArchName = BAA->getArchName();

    if (!ArchName.empty())
      TC = &getToolChain(C.getArgs(),
                         computeTargetTriple(*this, TargetTriple,
                                             C.getArgs(), ArchName));
    else
      TC = &C.getDefaultToolChain();

    return BuildJobsForAction(C, *BAA->input_begin(), TC, ArchName, AtTopLevel,
                              MultipleArchs, LinkingOutput, CachedResults,
                              TargetDeviceOffloadKind);
  }

  if (const ForEachWrappingAction *FEA = dyn_cast<ForEachWrappingAction>(A)) {
    // Check that the main action wasn't already processed.
    auto MainActionOutput = CachedResults.find(
        {FEA->getJobAction(),
         GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)});
    if (MainActionOutput != CachedResults.end()) {
      // The input was processed on behalf of another foreach.
      // Add entry in cache and return.
      CachedResults[{FEA, GetTriplePlusArchString(TC, BoundArch,
                                                  TargetDeviceOffloadKind)}] =
          MainActionOutput->second;
      return MainActionOutput->second;
    }

    // Build commands for the TFormInput then take any command added after as
    // needing a llvm-foreach wrapping.
    BuildJobsForAction(C, FEA->getTFormInput(), TC, BoundArch,
                       /*AtTopLevel=*/false, MultipleArchs, LinkingOutput,
                       CachedResults, TargetDeviceOffloadKind);
    unsigned OffsetIdx = C.getJobs().size();
    BuildJobsForAction(C, FEA->getJobAction(), TC, BoundArch,
                       /*AtTopLevel=*/false, MultipleArchs, LinkingOutput,
                       CachedResults, TargetDeviceOffloadKind);

    auto begin = C.getJobs().getJobsForOverride().begin() + OffsetIdx;
    auto end = C.getJobs().getJobsForOverride().end();

    // Steal the commands.
    llvm::SmallVector<std::unique_ptr<Command>, 4> JobsToWrap(
        std::make_move_iterator(begin), std::make_move_iterator(end));
    C.getJobs().getJobsForOverride().erase(begin, end);

    InputInfo ActionResult;
    for (std::unique_ptr<Command> Cmd :
         llvm::make_range(std::make_move_iterator(JobsToWrap.begin()),
                          std::make_move_iterator(JobsToWrap.end()))) {
      const JobAction *SourceAction = cast<JobAction>(&Cmd->getSource());

      ActionResult = CachedResults.at(
          {SourceAction,
           GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)});
      InputInfoList InputInfos;
      CollectForEachInputs(InputInfos, SourceAction, TC, BoundArch,
                           TargetDeviceOffloadKind, CachedResults);
      const Tool *Creator = &Cmd->getCreator();

      tools::SYCL::constructLLVMForeachCommand(
          C, *SourceAction, std::move(Cmd), InputInfos, ActionResult, Creator,
          "", types::getTypeTempSuffix(ActionResult.getType()));
    }
    return ActionResult;
  }

  ActionList Inputs = A->getInputs();

  const JobAction *JA = cast<JobAction>(A);
  ActionList CollapsedOffloadActions;

  auto *DA = dyn_cast<OffloadDepsJobAction>(JA);
  const ToolChain *JATC = DA ? DA->getHostTC() : TC;

  ToolSelector TS(JA, *JATC, C, isSaveTempsEnabled(),
                  embedBitcodeInObject() && !isUsingLTO());
  const Tool *T = TS.getTool(Inputs, CollapsedOffloadActions);

  if (!T)
    return InputInfo();

  if (BuildingForOffloadDevice &&
      A->getOffloadingDeviceKind() == Action::OFK_OpenMP) {
    if (TC->getTriple().isAMDGCN()) {
      // AMDGCN treats backend and assemble actions as no-op because
      // linker does not support object files.
      if (const BackendJobAction *BA = dyn_cast<BackendJobAction>(A)) {
        return BuildJobsForAction(C, *BA->input_begin(), TC, BoundArch,
                                  AtTopLevel, MultipleArchs, LinkingOutput,
                                  CachedResults, TargetDeviceOffloadKind);
      }

      if (const AssembleJobAction *AA = dyn_cast<AssembleJobAction>(A)) {
        return BuildJobsForAction(C, *AA->input_begin(), TC, BoundArch,
                                  AtTopLevel, MultipleArchs, LinkingOutput,
                                  CachedResults, TargetDeviceOffloadKind);
      }
    }
  }

  // If we've collapsed action list that contained OffloadAction we
  // need to build jobs for host/device-side inputs it may have held.
  for (const auto *OA : CollapsedOffloadActions)
    cast<OffloadAction>(OA)->doOnEachDependence(
        /*IsHostDependence=*/BuildingForOffloadDevice,
        [&](Action *DepA, const ToolChain *DepTC, const char *DepBoundArch) {
          OffloadDependencesInputInfo.push_back(BuildJobsForAction(
              C, DepA, DepTC, DepBoundArch, /* AtTopLevel */ false,
              /*MultipleArchs=*/!!DepBoundArch, LinkingOutput, CachedResults,
              DepA->getOffloadingDeviceKind()));
        });

  // Only use pipes when there is exactly one input.
  InputInfoList InputInfos;
  for (const Action *Input : Inputs) {
    // Treat dsymutil and verify sub-jobs as being at the top-level too, they
    // shouldn't get temporary output names.
    // FIXME: Clean this up.
    bool SubJobAtTopLevel =
        AtTopLevel && (isa<DsymutilJobAction>(A) || isa<VerifyJobAction>(A));
    InputInfos.push_back(BuildJobsForAction(
        C, Input, JATC, DA ? DA->getOffloadingArch() : BoundArch,
        SubJobAtTopLevel, MultipleArchs, LinkingOutput, CachedResults,
        A->getOffloadingDeviceKind()));
  }

  // Always use the first input as the base input.
  const char *BaseInput = InputInfos[0].getBaseInput();

  // ... except dsymutil actions, which use their actual input as the base
  // input.
  if (JA->getType() == types::TY_dSYM)
    BaseInput = InputInfos[0].getFilename();

  // ... and in header module compilations, which use the module name.
  if (auto *ModuleJA = dyn_cast<HeaderModulePrecompileJobAction>(JA))
    BaseInput = ModuleJA->getModuleName();

  // Append outputs of offload device jobs to the input list
  if (!OffloadDependencesInputInfo.empty())
    InputInfos.append(OffloadDependencesInputInfo.begin(),
                      OffloadDependencesInputInfo.end());

  // Set the effective triple of the toolchain for the duration of this job.
  llvm::Triple EffectiveTriple;
  const ToolChain &ToolTC = T->getToolChain();
  const ArgList &Args =
      C.getArgsForToolChain(TC, BoundArch, A->getOffloadingDeviceKind());
  if (InputInfos.size() != 1) {
    EffectiveTriple = llvm::Triple(ToolTC.ComputeEffectiveClangTriple(Args));
  } else {
    // Pass along the input type if it can be unambiguously determined.
    EffectiveTriple = llvm::Triple(
        ToolTC.ComputeEffectiveClangTriple(Args, InputInfos[0].getType()));
  }
  RegisterEffectiveTriple TripleRAII(ToolTC, EffectiveTriple);

  // Determine the place to write output to, if any.
  InputInfo Result;
  InputInfoList UnbundlingResults;
  if (auto *UA = dyn_cast<OffloadUnbundlingJobAction>(JA)) {
    // If we have an unbundling job, we need to create results for all the
    // outputs. We also update the results cache so that other actions using
    // this unbundling action can get the right results.
    for (auto &UI : UA->getDependentActionsInfo()) {
      assert(UI.DependentOffloadKind != Action::OFK_None &&
             "Unbundling with no offloading??");

      // Unbundling actions are never at the top level. When we generate the
      // offloading prefix, we also do that for the host file because the
      // unbundling action does not change the type of the output which can
      // cause a overwrite.
      InputInfo CurI;
      bool IsFPGAObjLink =
          (JA->getType() == types::TY_Object &&
           EffectiveTriple.getSubArch() == llvm::Triple::SPIRSubArch_fpga &&
           C.getInputArgs().hasArg(options::OPT_fsycl_link_EQ));
      if (C.getDriver().getOffloadStaticLibSeen() &&
          JA->getType() == types::TY_Archive) {
        // Host part of the unbundled static archive is not used.
        if (UI.DependentOffloadKind == Action::OFK_Host)
          continue;
        // Host part of the unbundled object is not used when using the
        // FPGA target and -fsycl-link is enabled.
        if (UI.DependentOffloadKind == Action::OFK_Host && IsFPGAObjLink)
          continue;
        std::string TmpFileName = C.getDriver().GetTemporaryPath(
            llvm::sys::path::stem(BaseInput), "a");
        const char *TmpFile =
            C.addTempFile(C.getArgs().MakeArgString(TmpFileName));
        CurI = InputInfo(types::TY_Archive, TmpFile, TmpFile);
      } else if (types::isFPGA(JA->getType())) {
        std::string Ext(types::getTypeTempSuffix(JA->getType()));
        types::ID TI = types::TY_Object;
        if (EffectiveTriple.getSubArch() == llvm::Triple::SPIRSubArch_fpga) {
          // Output file from unbundle is FPGA device. Name the file
          // accordingly.
          if (UI.DependentOffloadKind == Action::OFK_Host) {
            // Do not add the current info for Host with FPGA device.  The host
            // side isn't used
            continue;
          }
          if (JA->getType() == types::TY_FPGA_AOCO) {
            TI = types::TY_TempAOCOfilelist;
            Ext = "txt";
          }
          if (JA->getType() == types::TY_FPGA_AOCR ||
              JA->getType() == types::TY_FPGA_AOCR_EMU)
            // AOCR files are always unbundled into a list file.
            TI = types::TY_Tempfilelist;
        } else if (EffectiveTriple.getSubArch() !=
                   llvm::Triple::SPIRSubArch_fpga) {
          if (UI.DependentOffloadKind == Action::OFK_SYCL) {
            // Do not add the current info for device with FPGA device.  The
            // device side isn't used
            continue;
          }
          TI = types::TY_Tempfilelist;
          Ext = "txt";
        }
        std::string TmpFileName = C.getDriver().GetTemporaryPath(
            llvm::sys::path::stem(BaseInput), Ext);
        const char *TmpFile =
                        C.addTempFile(C.getArgs().MakeArgString(TmpFileName));
        CurI = InputInfo(TI, TmpFile, TmpFile);
      } else {
        // Host part of the unbundled object is not used when -fsycl-link is
        // enabled with FPGA target
        if (UI.DependentOffloadKind == Action::OFK_Host && IsFPGAObjLink)
          continue;
        std::string OffloadingPrefix = Action::GetOffloadingFileNamePrefix(
          UI.DependentOffloadKind,
          UI.DependentToolChain->getTriple().normalize(),
          /*CreatePrefixForHost=*/true);
        CurI = InputInfo(
          UA,
          GetNamedOutputPath(C, *UA, BaseInput, UI.DependentBoundArch,
                             /*AtTopLevel=*/false,
                             MultipleArchs ||
                                 UI.DependentOffloadKind == Action::OFK_HIP,
                             OffloadingPrefix),
          BaseInput);
      }
      // Save the unbundling result.
      UnbundlingResults.push_back(CurI);

      // Get the unique string identifier for this dependence and cache the
      // result.
      StringRef Arch;
      if (TargetDeviceOffloadKind == Action::OFK_HIP) {
        if (UI.DependentOffloadKind == Action::OFK_Host)
          Arch = StringRef();
        else
          Arch = UI.DependentBoundArch;
      } else
        Arch = BoundArch;
      // When unbundling for SYCL and there is no Target offload, assume
      // Host as the dependent offload, as the host path has been stripped
      // in this instance
      Action::OffloadKind DependentOffloadKind;
      if (UI.DependentOffloadKind == Action::OFK_SYCL &&
          TargetDeviceOffloadKind == Action::OFK_None)
        DependentOffloadKind = Action::OFK_Host;
      else
        DependentOffloadKind = UI.DependentOffloadKind;

      CachedResults[{A, GetTriplePlusArchString(UI.DependentToolChain, Arch,
                                                DependentOffloadKind)}] =
          CurI;
    }
    // Do a check for a dependency file unbundle for FPGA.  This is out of line
    // from a regular unbundle, so just create and return the name of the
    // unbundled file.
    if (JA->getType() == types::TY_FPGA_Dependencies ||
        JA->getType() == types::TY_FPGA_Dependencies_List) {
      std::string Ext(types::getTypeTempSuffix(JA->getType()));
      std::string TmpFileName =
          C.getDriver().GetTemporaryPath(llvm::sys::path::stem(BaseInput), Ext);
      const char *TmpFile =
          C.addTempFile(C.getArgs().MakeArgString(TmpFileName));
      Result = InputInfo(JA->getType(), TmpFile, TmpFile);
      UnbundlingResults.push_back(Result);
    } else {
      // Now that we have all the results generated, select the one that should
      // be returned for the current depending action.
      std::pair<const Action *, std::string> ActionTC = {
          A, GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)};
      assert(CachedResults.find(ActionTC) != CachedResults.end() &&
             "Result does not exist??");
      Result = CachedResults[ActionTC];
    }
  } else if (auto *DA = dyn_cast<OffloadDepsJobAction>(JA)) {
    for (auto &DI : DA->getDependentActionsInfo()) {
      assert(DI.DependentOffloadKind != Action::OFK_None &&
             "Deps job with no offloading");

      std::string OffloadingPrefix = Action::GetOffloadingFileNamePrefix(
          DI.DependentOffloadKind,
          DI.DependentToolChain->getTriple().normalize(),
          /*CreatePrefixForHost=*/true);
      auto CurI = InputInfo(
          DA,
          GetNamedOutputPath(C, *DA, BaseInput, DI.DependentBoundArch,
                             /*AtTopLevel=*/false,
                             MultipleArchs ||
                                 DI.DependentOffloadKind == Action::OFK_HIP,
                             OffloadingPrefix),
          BaseInput);
      // Save the result.
      UnbundlingResults.push_back(CurI);

      // Get the unique string identifier for this dependence and cache the
      // result.
      StringRef Arch = TargetDeviceOffloadKind == Action::OFK_HIP
                           ? DI.DependentOffloadKind == Action::OFK_Host
                                 ? StringRef()
                                 : DI.DependentBoundArch
                           : BoundArch;

      CachedResults[{A, GetTriplePlusArchString(DI.DependentToolChain, Arch,
                                                DI.DependentOffloadKind)}] =
          CurI;
    }

    // Now that we have all the results generated, select the one that should be
    // returned for the current depending action.
    std::pair<const Action *, std::string> ActionTC = {
        A, GetTriplePlusArchString(TC, BoundArch, TargetDeviceOffloadKind)};
    auto It = CachedResults.find(ActionTC);
    assert(It != CachedResults.end() && "Result does not exist??");
    Result = It->second;
  } else if (JA->getType() == types::TY_Nothing)
    Result = InputInfo(A, BaseInput);
  else {
    std::string OffloadingPrefix;
    // When generating binaries with -fsycl-link-target or -fsycl-link, the
    // output file prefix is the triple arch only.  Do not add the arch when
    // compiling for host.
    if (!A->getOffloadingHostActiveKinds() &&
        (Args.getLastArg(options::OPT_fsycl_link_targets_EQ) ||
         Args.hasArg(options::OPT_fsycl_link_EQ))) {
      OffloadingPrefix = "-";
      OffloadingPrefix += TC->getTriple().getArchName();
    } else {
      // We only have to generate a prefix for the host if this is not a
      // top-level action.
      OffloadingPrefix = Action::GetOffloadingFileNamePrefix(
        A->getOffloadingDeviceKind(), TC->getTriple().normalize(),
        /*CreatePrefixForHost=*/!!A->getOffloadingHostActiveKinds() &&
            !AtTopLevel);
    }
    if (isa<OffloadWrapperJobAction>(JA)) {
      if (Arg *FinalOutput = C.getArgs().getLastArg(options::OPT_o))
        BaseInput = FinalOutput->getValue();
      else
        BaseInput = getDefaultImageName();
      BaseInput =
          C.getArgs().MakeArgString(std::string(BaseInput) + "-wrapper");
    }
    Result = InputInfo(A, GetNamedOutputPath(C, *JA, BaseInput, BoundArch,
                                             AtTopLevel, MultipleArchs,
                                             OffloadingPrefix),
                       BaseInput);
  }

  if (CCCPrintBindings && !CCGenDiagnostics) {
    llvm::errs() << "# \"" << T->getToolChain().getTripleString() << '"'
                 << " - \"" << T->getName() << "\", inputs: [";
    for (unsigned i = 0, e = InputInfos.size(); i != e; ++i) {
      llvm::errs() << InputInfos[i].getAsString();
      if (i + 1 != e)
        llvm::errs() << ", ";
    }
    if (UnbundlingResults.empty())
      llvm::errs() << "], output: " << Result.getAsString() << "\n";
    else {
      llvm::errs() << "], outputs: [";
      for (unsigned i = 0, e = UnbundlingResults.size(); i != e; ++i) {
        llvm::errs() << UnbundlingResults[i].getAsString();
        if (i + 1 != e)
          llvm::errs() << ", ";
      }
      llvm::errs() << "] \n";
    }
  } else {
    if (UnbundlingResults.empty())
      T->ConstructJob(
          C, *JA, Result, InputInfos,
          C.getArgsForToolChain(TC, BoundArch, JA->getOffloadingDeviceKind()),
          LinkingOutput);
    else
      T->ConstructJobMultipleOutputs(
          C, *JA, UnbundlingResults, InputInfos,
          C.getArgsForToolChain(TC, BoundArch, JA->getOffloadingDeviceKind()),
          LinkingOutput);
  }
  return Result;
}

const char *Driver::getDefaultImageName() const {
  llvm::Triple Target(llvm::Triple::normalize(TargetTriple));
  return Target.isOSWindows() ? "a.exe" : "a.out";
}

/// Create output filename based on ArgValue, which could either be a
/// full filename, filename without extension, or a directory. If ArgValue
/// does not provide a filename, then use BaseName, and use the extension
/// suitable for FileType.
static const char *MakeCLOutputFilename(const ArgList &Args, StringRef ArgValue,
                                        StringRef BaseName,
                                        types::ID FileType) {
  SmallString<128> Filename = ArgValue;

  if (ArgValue.empty()) {
    // If the argument is empty, output to BaseName in the current dir.
    Filename = BaseName;
  } else if (llvm::sys::path::is_separator(Filename.back())) {
    // If the argument is a directory, output to BaseName in that dir.
    llvm::sys::path::append(Filename, BaseName);
  }

  if (!llvm::sys::path::has_extension(ArgValue)) {
    // If the argument didn't provide an extension, then set it.
    const char *Extension = types::getTypeTempSuffix(FileType, true);

    if (FileType == types::TY_Image &&
        Args.hasArg(options::OPT__SLASH_LD, options::OPT__SLASH_LDd)) {
      // The output file is a dll.
      Extension = "dll";
    }

    llvm::sys::path::replace_extension(Filename, Extension);
  }

  return Args.MakeArgString(Filename.c_str());
}

static bool HasPreprocessOutput(const Action &JA) {
  if (isa<PreprocessJobAction>(JA))
    return true;
  if (isa<OffloadAction>(JA) && isa<PreprocessJobAction>(JA.getInputs()[0]))
    return true;
  if (isa<OffloadBundlingJobAction>(JA) &&
      HasPreprocessOutput(*(JA.getInputs()[0])))
    return true;
  return false;
}

const char *Driver::GetNamedOutputPath(Compilation &C, const JobAction &JA,
                                       const char *BaseInput,
                                       StringRef OrigBoundArch, bool AtTopLevel,
                                       bool MultipleArchs,
                                       StringRef OffloadingPrefix) const {
  std::string BoundArch = OrigBoundArch.str();
#if defined(_WIN32)
  // BoundArch may contains ':', which is invalid in file names on Windows,
  // therefore replace it with '%'.
  std::replace(BoundArch.begin(), BoundArch.end(), ':', '@');
#endif

  llvm::PrettyStackTraceString CrashInfo("Computing output path");
  // Output to a user requested destination?
  if (AtTopLevel && !isa<DsymutilJobAction>(JA) && !isa<VerifyJobAction>(JA)) {
    if (Arg *FinalOutput = C.getArgs().getLastArg(options::OPT_o))
      return C.addResultFile(FinalOutput->getValue(), &JA);
    // Output to destination for -fsycl-device-only and Windows -o
    if (C.getArgs().hasArg(options::OPT_fsycl_device_only))
      if (Arg *FinalOutput = C.getArgs().getLastArg(options::OPT__SLASH_o))
        return C.addResultFile(FinalOutput->getValue(), &JA);
  }

  // For /P, preprocess to file named after BaseInput.
  if (C.getArgs().hasArg(options::OPT__SLASH_P) &&
      ((AtTopLevel && isa<PreprocessJobAction>(JA)) ||
       isa<OffloadBundlingJobAction>(JA))) {
    StringRef BaseName = llvm::sys::path::filename(BaseInput);
    StringRef NameArg;
    if (Arg *A = C.getArgs().getLastArg(options::OPT__SLASH_Fi))
      NameArg = A->getValue();
    return C.addResultFile(
        MakeCLOutputFilename(C.getArgs(), NameArg, BaseName, types::TY_PP_C),
        &JA);
  }

  // Redirect output for the generated source + integration footer.
  if (isa<AppendFooterJobAction>(JA)) {
    if (Arg *A = C.getArgs().getLastArg(options::OPT_fsycl_footer_path_EQ)) {
      SmallString<128> OutName(A->getValue());
      StringRef BaseName = llvm::sys::path::filename(BaseInput);
      if (isSaveTempsEnabled()) {
        // Retain the location specified by the user with -save-temps.
        const char *Suffix = types::getTypeTempSuffix(JA.getType());
        std::string::size_type End = std::string::npos;
        if (!types::appendSuffixForType(JA.getType()))
          End = BaseName.rfind('.');
        SmallString<128> Suffixed(BaseName.substr(0, End));
        Suffixed += OffloadingPrefix;
        Suffixed += '.';
        Suffixed += Suffix;
        llvm::sys::path::append(OutName, Suffixed.c_str());
      } else {
        std::string TmpName =
            GetTemporaryPath(llvm::sys::path::stem(BaseName),
                             types::getTypeTempSuffix(JA.getType()));
        llvm::sys::path::append(OutName, llvm::sys::path::filename(TmpName));
      }
      return C.addTempFile(C.getArgs().MakeArgString(OutName));
    }
  }

  // Default to writing to stdout?
  if (AtTopLevel && !CCGenDiagnostics && HasPreprocessOutput(JA)) {
    return "-";
  }

  if (JA.getType() == types::TY_ModuleFile &&
      C.getArgs().getLastArg(options::OPT_module_file_info)) {
    return "-";
  }

  // Is this the assembly listing for /FA?
  if (JA.getType() == types::TY_PP_Asm &&
      (C.getArgs().hasArg(options::OPT__SLASH_FA) ||
       C.getArgs().hasArg(options::OPT__SLASH_Fa))) {
    // Use /Fa and the input filename to determine the asm file name.
    StringRef BaseName = llvm::sys::path::filename(BaseInput);
    StringRef FaValue = C.getArgs().getLastArgValue(options::OPT__SLASH_Fa);
    return C.addResultFile(
        MakeCLOutputFilename(C.getArgs(), FaValue, BaseName, JA.getType()),
        &JA);
  }

  // Output to a temporary file?
  if ((!AtTopLevel && !isSaveTempsEnabled() &&
       (!C.getArgs().hasArg(options::OPT__SLASH_Fo) ||
        // FIXME - The use of /Fo is limited when offloading is enabled.  When
        // compiling to exe use of /Fo does not produce the named obj.  We also
        // should not use the named output when performing unbundling.
        (C.getArgs().hasArg(options::OPT__SLASH_Fo) &&
         (!JA.isOffloading(Action::OFK_None) ||
          isa<OffloadUnbundlingJobAction>(JA) ||
          JA.getOffloadingHostActiveKinds() > Action::OFK_Host)))) ||
      CCGenDiagnostics) {
    StringRef Name = llvm::sys::path::filename(BaseInput);
    std::pair<StringRef, StringRef> Split = Name.split('.');
    SmallString<128> TmpName;
    const char *Suffix = types::getTypeTempSuffix(JA.getType(), IsCLMode());
    Arg *A = C.getArgs().getLastArg(options::OPT_fcrash_diagnostics_dir);
    if (CCGenDiagnostics && A) {
      SmallString<128> CrashDirectory(A->getValue());
      if (!getVFS().exists(CrashDirectory))
        llvm::sys::fs::create_directories(CrashDirectory);
      llvm::sys::path::append(CrashDirectory, Split.first);
      const char *Middle = Suffix ? "-%%%%%%." : "-%%%%%%";
      std::error_code EC = llvm::sys::fs::createUniqueFile(
          CrashDirectory + Middle + Suffix, TmpName);
      if (EC) {
        Diag(clang::diag::err_unable_to_make_temp) << EC.message();
        return "";
      }
    } else {
      TmpName = GetTemporaryPath(Split.first, Suffix);
    }
    return C.addTempFile(C.getArgs().MakeArgString(TmpName), JA.getType());
  }

  SmallString<128> BasePath(BaseInput);
  SmallString<128> ExternalPath("");
  StringRef BaseName;

  // Dsymutil actions should use the full path.
  if (isa<DsymutilJobAction>(JA) && C.getArgs().hasArg(options::OPT_dsym_dir)) {
    ExternalPath += C.getArgs().getLastArg(options::OPT_dsym_dir)->getValue();
    // We use posix style here because the tests (specifically
    // darwin-dsymutil.c) demonstrate that posix style paths are acceptable
    // even on Windows and if we don't then the similar test covering this
    // fails.
    llvm::sys::path::append(ExternalPath, llvm::sys::path::Style::posix,
                            llvm::sys::path::filename(BasePath));
    BaseName = ExternalPath;
  } else if (isa<DsymutilJobAction>(JA) || isa<VerifyJobAction>(JA))
    BaseName = BasePath;
  else
    BaseName = llvm::sys::path::filename(BasePath);

  // Determine what the derived output name should be.
  const char *NamedOutput;

  if ((JA.getType() == types::TY_Object || JA.getType() == types::TY_LTO_BC ||
       JA.getType() == types::TY_Archive) &&
      C.getArgs().hasArg(options::OPT__SLASH_Fo, options::OPT__SLASH_o)) {
    // The /Fo or /o flag decides the object filename.
    StringRef Val =
        C.getArgs()
            .getLastArg(options::OPT__SLASH_Fo, options::OPT__SLASH_o)
            ->getValue();
    NamedOutput =
        MakeCLOutputFilename(C.getArgs(), Val, BaseName, types::TY_Object);
  } else if (JA.getType() == types::TY_Image &&
             C.getArgs().hasArg(options::OPT__SLASH_Fe,
                                options::OPT__SLASH_o)) {
    // The /Fe or /o flag names the linked file.
    StringRef Val =
        C.getArgs()
            .getLastArg(options::OPT__SLASH_Fe, options::OPT__SLASH_o)
            ->getValue();
    NamedOutput =
        MakeCLOutputFilename(C.getArgs(), Val, BaseName, types::TY_Image);
  } else if (JA.getType() == types::TY_Image) {
    if (IsCLMode()) {
      // clang-cl uses BaseName for the executable name.
      NamedOutput =
          MakeCLOutputFilename(C.getArgs(), "", BaseName, types::TY_Image);
    } else {
      SmallString<128> Output(getDefaultImageName());
      // HIP image for device compilation with -fno-gpu-rdc is per compilation
      // unit.
      bool IsHIPNoRDC = JA.getOffloadingDeviceKind() == Action::OFK_HIP &&
                        !C.getArgs().hasFlag(options::OPT_fgpu_rdc,
                                             options::OPT_fno_gpu_rdc, false);
      if (IsHIPNoRDC) {
        Output = BaseName;
        llvm::sys::path::replace_extension(Output, "");
      }
      Output += OffloadingPrefix;
      if (MultipleArchs && !BoundArch.empty()) {
        Output += "-";
        Output.append(BoundArch);
      }
      if (IsHIPNoRDC)
        Output += ".out";
      NamedOutput = C.getArgs().MakeArgString(Output.c_str());
    }
  } else if (JA.getType() == types::TY_PCH && IsCLMode()) {
    NamedOutput = C.getArgs().MakeArgString(GetClPchPath(C, BaseName));
  } else {
    const char *Suffix = types::getTypeTempSuffix(JA.getType(), IsCLMode());
    assert(Suffix && "All types used for output should have a suffix.");

    std::string::size_type End = std::string::npos;
    if (!types::appendSuffixForType(JA.getType()))
      End = BaseName.rfind('.');
    SmallString<128> Suffixed(BaseName.substr(0, End));
    Suffixed += OffloadingPrefix;
    if (MultipleArchs && !BoundArch.empty()) {
      Suffixed += "-";
      Suffixed.append(BoundArch);
    }
    // When using both -save-temps and -emit-llvm, use a ".tmp.bc" suffix for
    // the unoptimized bitcode so that it does not get overwritten by the ".bc"
    // optimized bitcode output.
    auto IsHIPRDCInCompilePhase = [](const JobAction &JA,
                                     const llvm::opt::DerivedArgList &Args) {
      // The relocatable compilation in HIP implies -emit-llvm. Similarly, use a
      // ".tmp.bc" suffix for the unoptimized bitcode (generated in the compile
      // phase.)
      return isa<CompileJobAction>(JA) &&
             JA.getOffloadingDeviceKind() == Action::OFK_HIP &&
             Args.hasFlag(options::OPT_fgpu_rdc, options::OPT_fno_gpu_rdc,
                          false);
    };
    if (!AtTopLevel && JA.getType() == types::TY_LLVM_BC &&
        (C.getArgs().hasArg(options::OPT_emit_llvm) ||
         IsHIPRDCInCompilePhase(JA, C.getArgs())))
      Suffixed += ".tmp";
    Suffixed += '.';
    Suffixed += Suffix;
    NamedOutput = C.getArgs().MakeArgString(Suffixed.c_str());
  }

  // Prepend object file path if -save-temps=obj
  if (!AtTopLevel && isSaveTempsObj() && C.getArgs().hasArg(options::OPT_o) &&
      JA.getType() != types::TY_PCH) {
    Arg *FinalOutput = C.getArgs().getLastArg(options::OPT_o);
    SmallString<128> TempPath(FinalOutput->getValue());
    llvm::sys::path::remove_filename(TempPath);
    StringRef OutputFileName = llvm::sys::path::filename(NamedOutput);
    llvm::sys::path::append(TempPath, OutputFileName);
    NamedOutput = C.getArgs().MakeArgString(TempPath.c_str());
  }

  if (isSaveTempsEnabled()) {
    // If we're saving temps and the temp file conflicts with any
    // input/resulting file, then avoid overwriting.
    if (!AtTopLevel) {
      bool SameFile = false;
      SmallString<256> Result;
      llvm::sys::fs::current_path(Result);
      llvm::sys::path::append(Result, BaseName);
      llvm::sys::fs::equivalent(BaseInput, Result.c_str(), SameFile);
      // Must share the same path to conflict.
      if (SameFile) {
        StringRef Name = llvm::sys::path::filename(BaseInput);
        std::pair<StringRef, StringRef> Split = Name.split('.');
        std::string TmpName = GetTemporaryPath(
            Split.first, types::getTypeTempSuffix(JA.getType(), IsCLMode()));
        return C.addTempFile(C.getArgs().MakeArgString(TmpName));
      }
    }

    const auto &ResultFiles = C.getResultFiles();
    const auto CollidingFilenameIt =
        llvm::find_if(ResultFiles, [NamedOutput](const auto &It) {
          return StringRef(NamedOutput).equals(It.second);
        });
    if (CollidingFilenameIt != ResultFiles.end()) {
      // Upon any collision, a unique hash will be appended to the filename,
      // similar to what is done for temporary files in the regular flow.
      StringRef CollidingName(CollidingFilenameIt->second);
      std::pair<StringRef, StringRef> Split = CollidingName.split('.');
      std::string UniqueName = GetUniquePath(
          Split.first, types::getTypeTempSuffix(JA.getType(), IsCLMode()));
      return C.addResultFile(C.getArgs().MakeArgString(UniqueName), &JA);
    }
  }

  // As an annoying special case, PCH generation doesn't strip the pathname.
  if (JA.getType() == types::TY_PCH && !IsCLMode()) {
    llvm::sys::path::remove_filename(BasePath);
    if (BasePath.empty())
      BasePath = NamedOutput;
    else
      llvm::sys::path::append(BasePath, NamedOutput);
    return C.addResultFile(C.getArgs().MakeArgString(BasePath.c_str()), &JA);
  } else {
    return C.addResultFile(NamedOutput, &JA);
  }
}

std::string Driver::GetFilePath(StringRef Name, const ToolChain &TC) const {
  // Search for Name in a list of paths.
  auto SearchPaths = [&](const llvm::SmallVectorImpl<std::string> &P)
      -> llvm::Optional<std::string> {
    // Respect a limited subset of the '-Bprefix' functionality in GCC by
    // attempting to use this prefix when looking for file paths.
    for (const auto &Dir : P) {
      if (Dir.empty())
        continue;
      SmallString<128> P(Dir[0] == '=' ? SysRoot + Dir.substr(1) : Dir);
      llvm::sys::path::append(P, Name);
      if (llvm::sys::fs::exists(Twine(P)))
        return std::string(P);
    }
    return None;
  };

  if (auto P = SearchPaths(PrefixDirs))
    return *P;

  SmallString<128> R(ResourceDir);
  llvm::sys::path::append(R, Name);
  if (llvm::sys::fs::exists(Twine(R)))
    return std::string(R.str());

  SmallString<128> P(TC.getCompilerRTPath());
  llvm::sys::path::append(P, Name);
  if (llvm::sys::fs::exists(Twine(P)))
    return std::string(P.str());

  SmallString<128> D(Dir);
  llvm::sys::path::append(D, "..", Name);
  if (llvm::sys::fs::exists(Twine(D)))
    return std::string(D.str());

  if (auto P = SearchPaths(TC.getLibraryPaths()))
    return *P;

  if (auto P = SearchPaths(TC.getFilePaths()))
    return *P;

  return std::string(Name);
}

void Driver::generatePrefixedToolNames(
    StringRef Tool, const ToolChain &TC,
    SmallVectorImpl<std::string> &Names) const {
  // FIXME: Needs a better variable than TargetTriple
  Names.emplace_back((TargetTriple + "-" + Tool).str());
  Names.emplace_back(Tool);
}

static bool ScanDirForExecutable(SmallString<128> &Dir, StringRef Name) {
  llvm::sys::path::append(Dir, Name);
  if (llvm::sys::fs::can_execute(Twine(Dir)))
    return true;
  llvm::sys::path::remove_filename(Dir);
  return false;
}

std::string Driver::GetProgramPath(StringRef Name, const ToolChain &TC) const {
  SmallVector<std::string, 2> TargetSpecificExecutables;
  generatePrefixedToolNames(Name, TC, TargetSpecificExecutables);

  // Respect a limited subset of the '-Bprefix' functionality in GCC by
  // attempting to use this prefix when looking for program paths.
  for (const auto &PrefixDir : PrefixDirs) {
    if (llvm::sys::fs::is_directory(PrefixDir)) {
      SmallString<128> P(PrefixDir);
      if (ScanDirForExecutable(P, Name))
        return std::string(P.str());
    } else {
      SmallString<128> P((PrefixDir + Name).str());
      if (llvm::sys::fs::can_execute(Twine(P)))
        return std::string(P.str());
    }
  }

  const ToolChain::path_list &List = TC.getProgramPaths();
  for (const auto &TargetSpecificExecutable : TargetSpecificExecutables) {
    // For each possible name of the tool look for it in
    // program paths first, then the path.
    // Higher priority names will be first, meaning that
    // a higher priority name in the path will be found
    // instead of a lower priority name in the program path.
    // E.g. <triple>-gcc on the path will be found instead
    // of gcc in the program path
    for (const auto &Path : List) {
      SmallString<128> P(Path);
      if (ScanDirForExecutable(P, TargetSpecificExecutable))
        return std::string(P.str());
    }

    // Fall back to the path
    if (llvm::ErrorOr<std::string> P =
            llvm::sys::findProgramByName(TargetSpecificExecutable))
      return *P;
  }

  return std::string(Name);
}

std::string Driver::GetTemporaryPath(StringRef Prefix, StringRef Suffix) const {
  SmallString<128> Path;
  std::error_code EC = llvm::sys::fs::createTemporaryFile(Prefix, Suffix, Path);
  if (EC) {
    Diag(clang::diag::err_unable_to_make_temp) << EC.message();
    return "";
  }

  return std::string(Path.str());
}

std::string Driver::GetUniquePath(StringRef BaseName, StringRef Ext) const {
  SmallString<128> Path;
  std::error_code EC = llvm::sys::fs::createUniqueFile(
      Twine(BaseName) + Twine("-%%%%%%.") + Ext, Path);
  if (EC) {
    Diag(clang::diag::err_unable_to_make_temp) << EC.message();
    return "";
  }

  return std::string(Path.str());
}

std::string Driver::GetTemporaryDirectory(StringRef Prefix) const {
  SmallString<128> Path;
  std::error_code EC = llvm::sys::fs::createUniqueDirectory(Prefix, Path);
  if (EC) {
    Diag(clang::diag::err_unable_to_make_temp) << EC.message();
    return "";
  }

  return std::string(Path.str());
}

std::string Driver::GetClPchPath(Compilation &C, StringRef BaseName) const {
  SmallString<128> Output;
  if (Arg *FpArg = C.getArgs().getLastArg(options::OPT__SLASH_Fp)) {
    // FIXME: If anybody needs it, implement this obscure rule:
    // "If you specify a directory without a file name, the default file name
    // is VCx0.pch., where x is the major version of Visual C++ in use."
    Output = FpArg->getValue();

    // "If you do not specify an extension as part of the path name, an
    // extension of .pch is assumed. "
    if (!llvm::sys::path::has_extension(Output))
      Output += ".pch";
  } else {
    if (Arg *YcArg = C.getArgs().getLastArg(options::OPT__SLASH_Yc))
      Output = YcArg->getValue();
    if (Output.empty())
      Output = BaseName;
    llvm::sys::path::replace_extension(Output, ".pch");
  }
  return std::string(Output.str());
}

const ToolChain &Driver::getToolChain(const ArgList &Args,
                                      const llvm::Triple &Target) const {

  auto &TC = ToolChains[Target.str()];
  if (!TC) {
    switch (Target.getOS()) {
    case llvm::Triple::AIX:
      TC = std::make_unique<toolchains::AIX>(*this, Target, Args);
      break;
    case llvm::Triple::Haiku:
      TC = std::make_unique<toolchains::Haiku>(*this, Target, Args);
      break;
    case llvm::Triple::Ananas:
      TC = std::make_unique<toolchains::Ananas>(*this, Target, Args);
      break;
    case llvm::Triple::CloudABI:
      TC = std::make_unique<toolchains::CloudABI>(*this, Target, Args);
      break;
    case llvm::Triple::Darwin:
    case llvm::Triple::MacOSX:
    case llvm::Triple::IOS:
    case llvm::Triple::TvOS:
    case llvm::Triple::WatchOS:
      TC = std::make_unique<toolchains::DarwinClang>(*this, Target, Args);
      break;
    case llvm::Triple::DragonFly:
      TC = std::make_unique<toolchains::DragonFly>(*this, Target, Args);
      break;
    case llvm::Triple::OpenBSD:
      TC = std::make_unique<toolchains::OpenBSD>(*this, Target, Args);
      break;
    case llvm::Triple::NetBSD:
      TC = std::make_unique<toolchains::NetBSD>(*this, Target, Args);
      break;
    case llvm::Triple::FreeBSD:
      TC = std::make_unique<toolchains::FreeBSD>(*this, Target, Args);
      break;
    case llvm::Triple::Minix:
      TC = std::make_unique<toolchains::Minix>(*this, Target, Args);
      break;
    case llvm::Triple::Linux:
    case llvm::Triple::ELFIAMCU:
      if (Target.getArch() == llvm::Triple::hexagon)
        TC = std::make_unique<toolchains::HexagonToolChain>(*this, Target,
                                                             Args);
      else if ((Target.getVendor() == llvm::Triple::MipsTechnologies) &&
               !Target.hasEnvironment())
        TC = std::make_unique<toolchains::MipsLLVMToolChain>(*this, Target,
                                                              Args);
      else if (Target.isPPC())
        TC = std::make_unique<toolchains::PPCLinuxToolChain>(*this, Target,
                                                              Args);
      else if (Target.getArch() == llvm::Triple::ve)
        TC = std::make_unique<toolchains::VEToolChain>(*this, Target, Args);

      else
        TC = std::make_unique<toolchains::Linux>(*this, Target, Args);
      break;
    case llvm::Triple::NaCl:
      TC = std::make_unique<toolchains::NaClToolChain>(*this, Target, Args);
      break;
    case llvm::Triple::Fuchsia:
      TC = std::make_unique<toolchains::Fuchsia>(*this, Target, Args);
      break;
    case llvm::Triple::Solaris:
      TC = std::make_unique<toolchains::Solaris>(*this, Target, Args);
      break;
    case llvm::Triple::AMDHSA:
      TC = std::make_unique<toolchains::ROCMToolChain>(*this, Target, Args);
      break;
    case llvm::Triple::AMDPAL:
    case llvm::Triple::Mesa3D:
      TC = std::make_unique<toolchains::AMDGPUToolChain>(*this, Target, Args);
      break;
    case llvm::Triple::Win32:
      switch (Target.getEnvironment()) {
      default:
        if (Target.isOSBinFormatELF())
          TC = std::make_unique<toolchains::Generic_ELF>(*this, Target, Args);
        else if (Target.isOSBinFormatMachO())
          TC = std::make_unique<toolchains::MachO>(*this, Target, Args);
        else
          TC = std::make_unique<toolchains::Generic_GCC>(*this, Target, Args);
        break;
      case llvm::Triple::GNU:
        TC = std::make_unique<toolchains::MinGW>(*this, Target, Args);
        break;
      case llvm::Triple::Itanium:
        TC = std::make_unique<toolchains::CrossWindowsToolChain>(*this, Target,
                                                                  Args);
        break;
      case llvm::Triple::MSVC:
      case llvm::Triple::UnknownEnvironment:
      case llvm::Triple::SYCLDevice:
        if (Args.getLastArgValue(options::OPT_fuse_ld_EQ)
                .startswith_insensitive("bfd"))
          TC = std::make_unique<toolchains::CrossWindowsToolChain>(
              *this, Target, Args);
        else
          TC =
              std::make_unique<toolchains::MSVCToolChain>(*this, Target, Args);
        break;
      }
      break;
    case llvm::Triple::PS4:
      TC = std::make_unique<toolchains::PS4CPU>(*this, Target, Args);
      break;
    case llvm::Triple::Contiki:
      TC = std::make_unique<toolchains::Contiki>(*this, Target, Args);
      break;
    case llvm::Triple::Hurd:
      TC = std::make_unique<toolchains::Hurd>(*this, Target, Args);
      break;
    case llvm::Triple::ZOS:
      TC = std::make_unique<toolchains::ZOS>(*this, Target, Args);
      break;
    default:
      // Of these targets, Hexagon is the only one that might have
      // an OS of Linux, in which case it got handled above already.
      switch (Target.getArch()) {
      case llvm::Triple::tce:
        TC = std::make_unique<toolchains::TCEToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::tcele:
        TC = std::make_unique<toolchains::TCELEToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::hexagon:
        TC = std::make_unique<toolchains::HexagonToolChain>(*this, Target,
                                                             Args);
        break;
      case llvm::Triple::lanai:
        TC = std::make_unique<toolchains::LanaiToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::xcore:
        TC = std::make_unique<toolchains::XCoreToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::wasm32:
      case llvm::Triple::wasm64:
        TC = std::make_unique<toolchains::WebAssembly>(*this, Target, Args);
        break;
      case llvm::Triple::avr:
        TC = std::make_unique<toolchains::AVRToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::msp430:
        TC =
            std::make_unique<toolchains::MSP430ToolChain>(*this, Target, Args);
        break;
      case llvm::Triple::riscv32:
      case llvm::Triple::riscv64:
        if (toolchains::RISCVToolChain::hasGCCToolchain(*this, Args))
          TC =
              std::make_unique<toolchains::RISCVToolChain>(*this, Target, Args);
        else
          TC = std::make_unique<toolchains::BareMetal>(*this, Target, Args);
        break;
      case llvm::Triple::ve:
        TC = std::make_unique<toolchains::VEToolChain>(*this, Target, Args);
        break;
      default:
        if (Target.getVendor() == llvm::Triple::Myriad)
          TC = std::make_unique<toolchains::MyriadToolChain>(*this, Target,
                                                              Args);
        else if (toolchains::BareMetal::handlesTarget(Target))
          TC = std::make_unique<toolchains::BareMetal>(*this, Target, Args);
        else if (Target.isOSBinFormatELF())
          TC = std::make_unique<toolchains::Generic_ELF>(*this, Target, Args);
        else if (Target.isOSBinFormatMachO())
          TC = std::make_unique<toolchains::MachO>(*this, Target, Args);
        else
          TC = std::make_unique<toolchains::Generic_GCC>(*this, Target, Args);
      }
    }
  }

  // Intentionally omitted from the switch above: llvm::Triple::CUDA.  CUDA
  // compiles always need two toolchains, the CUDA toolchain and the host
  // toolchain.  So the only valid way to create a CUDA toolchain is via
  // CreateOffloadingDeviceToolChains.

  return *TC;
}

const ToolChain &Driver::getOffloadingDeviceToolChain(const ArgList &Args,
                  const llvm::Triple &Target, const ToolChain &HostTC,
                  const Action::OffloadKind &TargetDeviceOffloadKind) const {
  // Use device / host triples as the key into the ToolChains map because the
  // device ToolChain we create depends on both.
  auto &TC = ToolChains[Target.str() + "/" + HostTC.getTriple().str()];
  if (!TC) {
    // Categorized by offload kind > arch rather than OS > arch like
    // the normal getToolChain call, as it seems a reasonable way to categorize
    // things.
    switch (TargetDeviceOffloadKind) {
      case Action::OFK_Cuda:
        TC = std::make_unique<toolchains::CudaToolChain>(
          *this, Target, HostTC, Args, TargetDeviceOffloadKind);
        break;
      case Action::OFK_HIP:
        TC = std::make_unique<toolchains::HIPToolChain>(
            *this, Target, HostTC, Args, TargetDeviceOffloadKind);
        break;
      case Action::OFK_OpenMP:
        // omp + nvptx
        TC = std::make_unique<toolchains::CudaToolChain>(
          *this, Target, HostTC, Args, TargetDeviceOffloadKind);
        break;
      case Action::OFK_SYCL:
        switch (Target.getArch()) {
          case llvm::Triple::spir:
          case llvm::Triple::spir64:
            TC = std::make_unique<toolchains::SYCLToolChain>(
              *this, Target, HostTC, Args);
            break;
          case llvm::Triple::nvptx:
          case llvm::Triple::nvptx64:
            TC = std::make_unique<toolchains::CudaToolChain>(
              *this, Target, HostTC, Args, TargetDeviceOffloadKind);
            break;
          case llvm::Triple::amdgcn:
            TC = std::make_unique<toolchains::HIPToolChain>(
                *this, Target, HostTC, Args, TargetDeviceOffloadKind);
            break;
          default:
          break;
        }
      break;
      default:
      break;
    }
  }

  return *TC;
}

bool Driver::ShouldUseClangCompiler(const JobAction &JA) const {
  // Say "no" if there is not exactly one input of a type clang understands.
  if (JA.size() != 1 ||
      !types::isAcceptedByClang((*JA.input_begin())->getType()))
    return false;

  // And say "no" if this is not a kind of action clang understands.
  if (!isa<PreprocessJobAction>(JA) && !isa<PrecompileJobAction>(JA) &&
      !isa<CompileJobAction>(JA) && !isa<BackendJobAction>(JA))
    return false;

  return true;
}

bool Driver::ShouldUseFlangCompiler(const JobAction &JA) const {
  // Say "no" if there is not exactly one input of a type flang understands.
  if (JA.size() != 1 ||
      !types::isFortran((*JA.input_begin())->getType()))
    return false;

  // And say "no" if this is not a kind of action flang understands.
  if (!isa<PreprocessJobAction>(JA) && !isa<CompileJobAction>(JA) && !isa<BackendJobAction>(JA))
    return false;

  return true;
}

bool Driver::ShouldEmitStaticLibrary(const ArgList &Args) const {
  // Only emit static library if the flag is set explicitly.
  if (Args.hasArg(options::OPT_emit_static_lib))
    return true;
  return false;
}

/// GetReleaseVersion - Parse (([0-9]+)(.([0-9]+)(.([0-9]+)?))?)? and return the
/// grouped values as integers. Numbers which are not provided are set to 0.
///
/// \return True if the entire string was parsed (9.2), or all groups were
/// parsed (10.3.5extrastuff).
bool Driver::GetReleaseVersion(StringRef Str, unsigned &Major, unsigned &Minor,
                               unsigned &Micro, bool &HadExtra) {
  HadExtra = false;

  Major = Minor = Micro = 0;
  if (Str.empty())
    return false;

  if (Str.consumeInteger(10, Major))
    return false;
  if (Str.empty())
    return true;
  if (Str[0] != '.')
    return false;

  Str = Str.drop_front(1);

  if (Str.consumeInteger(10, Minor))
    return false;
  if (Str.empty())
    return true;
  if (Str[0] != '.')
    return false;
  Str = Str.drop_front(1);

  if (Str.consumeInteger(10, Micro))
    return false;
  if (!Str.empty())
    HadExtra = true;
  return true;
}

/// Parse digits from a string \p Str and fulfill \p Digits with
/// the parsed numbers. This method assumes that the max number of
/// digits to look for is equal to Digits.size().
///
/// \return True if the entire string was parsed and there are
/// no extra characters remaining at the end.
bool Driver::GetReleaseVersion(StringRef Str,
                               MutableArrayRef<unsigned> Digits) {
  if (Str.empty())
    return false;

  unsigned CurDigit = 0;
  while (CurDigit < Digits.size()) {
    unsigned Digit;
    if (Str.consumeInteger(10, Digit))
      return false;
    Digits[CurDigit] = Digit;
    if (Str.empty())
      return true;
    if (Str[0] != '.')
      return false;
    Str = Str.drop_front(1);
    CurDigit++;
  }

  // More digits than requested, bail out...
  return false;
}

std::pair<unsigned, unsigned>
Driver::getIncludeExcludeOptionFlagMasks(bool IsClCompatMode) const {
  unsigned IncludedFlagsBitmask = 0;
  unsigned ExcludedFlagsBitmask = options::NoDriverOption;

  if (IsClCompatMode) {
    // Include CL and Core options.
    IncludedFlagsBitmask |= options::CLOption;
    IncludedFlagsBitmask |= options::CoreOption;
  } else {
    ExcludedFlagsBitmask |= options::CLOption;
  }

  return std::make_pair(IncludedFlagsBitmask, ExcludedFlagsBitmask);
}

bool clang::driver::isOptimizationLevelFast(const ArgList &Args) {
  return Args.hasFlag(options::OPT_Ofast, options::OPT_O_Group, false);
}

bool clang::driver::isObjectFile(std::string FileName) {
  if (!llvm::sys::path::has_extension(FileName))
    // Any file with no extension should be considered an Object. Take into
    // account -lsomelib library filenames.
    return FileName.rfind("-l", 0) != 0;
  std::string Ext(llvm::sys::path::extension(FileName).drop_front());
  // We cannot rely on lookupTypeForExtension solely as that has 'lib'
  // marked as an object.
  return (Ext != "lib" &&
          types::lookupTypeForExtension(Ext) == types::TY_Object);
}

bool clang::driver::isStaticArchiveFile(const StringRef &FileName) {
  if (!llvm::sys::path::has_extension(FileName))
    // Any file with no extension should not be considered an Archive.
    return false;
  StringRef Ext(llvm::sys::path::extension(FileName).drop_front());
  llvm::file_magic Magic;
  llvm::identify_magic(FileName, Magic);
  // Only .lib and archive files are to be considered.
  return (Ext == "lib" || Magic == llvm::file_magic::archive);
}

bool clang::driver::willEmitRemarks(const ArgList &Args) {
  // -fsave-optimization-record enables it.
  if (Args.hasFlag(options::OPT_fsave_optimization_record,
                   options::OPT_fno_save_optimization_record, false))
    return true;

  // -fsave-optimization-record=<format> enables it as well.
  if (Args.hasFlag(options::OPT_fsave_optimization_record_EQ,
                   options::OPT_fno_save_optimization_record, false))
    return true;

  // -foptimization-record-file alone enables it too.
  if (Args.hasFlag(options::OPT_foptimization_record_file_EQ,
                   options::OPT_fno_save_optimization_record, false))
    return true;

  // -foptimization-record-passes alone enables it too.
  if (Args.hasFlag(options::OPT_foptimization_record_passes_EQ,
                   options::OPT_fno_save_optimization_record, false))
    return true;
  return false;
}
