#include "PakTools.h"
#include "CommandLine.h"
#include "IPlatformFilePak.h"
#include "KeyChainUtilities.h"
#include "Runtime/Launch/Public/RequiredProgramMainCPPInclude.h"
#include "StableSort.h"

#include <filesystem>

IMPLEMENT_APPLICATION(PakTools, "PakTools");

// Defined in PakFileUtilities.cpp
void LoadKeyChain(const TCHAR *CmdLine, FKeyChain &OutCryptoSettings);

INT32_MAIN_INT32_ARGC_TCHAR_ARGV() {
    FTaskTagScope scope(ETaskTag::EGameThread);

    // start up the main loop
    GEngineLoop.PreInit(ArgC, ArgV, TEXT("-UseIoStore"));

    const double startTime = FPlatformTime::Seconds();

    const int32 result = uetools::ExecutePakTools(FCommandLine::Get()) ? 0 : 1;

    UE_LOG(LogPakFile, Display, TEXT("PakTools executed in %f seconds"), FPlatformTime::Seconds() - startTime);

    GLog->Flush();

    RequestEngineExit(TEXT("PakTools Exiting"));

    FEngineLoop::AppPreExit();
    FModuleManager::Get().UnloadModulesAtShutdown();
    FEngineLoop::AppExit();

    return result;
}

namespace uetools {
bool ExecutePakTools(const TCHAR *CmdLine) {
    // Parse CLI (see PakFileUtilities.cpp)
    TArray<FString> nonOptionArguments;
    for (const TCHAR *cmdLineEnd = CmdLine; *cmdLineEnd != 0;) {
        const FString argument = FParse::Token(cmdLineEnd, false);
        if (argument.Len() > 0 && !argument.StartsWith(TEXT("-"))) {
            nonOptionArguments.Add(argument);
        }
    }

    FKeyChain KeyChain;
    LoadKeyChain(CmdLine, KeyChain);
    KeyChainUtilities::ApplyEncryptionKeys(KeyChain);

    if (FParse::Param(CmdLine, TEXT("List"))) {
        if (nonOptionArguments.Num() == 0) {
            UE_LOG(LogPakFile, Error, TEXT("Incorrect arguments. Expected: -List <pak_or_utoc> ..."));
            return false;
        }

        return ListFilesInPak(nonOptionArguments, KeyChain);
    }

    if (FParse::Param(CmdLine, TEXT("Extract"))) {
        if (nonOptionArguments.Num() != 2) {
            UE_LOG(LogPakFile, Error, TEXT("Incorrect arguments. Expected: -Extract <pak_or_utoc> <output_directory>"));
            return false;
        }

        return ExtractFilesFromPak(KeyChain, nonOptionArguments[0], nonOptionArguments[1]);
    }

    UE_LOG(LogPakFile, Error, TEXT("No command specified. Usage:"));
    UE_LOG(LogPakFile, Error, TEXT("  PakTools -List <pak_or_utoc> ..."));
    UE_LOG(LogPakFile, Error, TEXT("  PakTools -Extract <pak_or_utoc> <output_directory>"));

    return true;
}
} // namespace uetools
