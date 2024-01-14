#pragma once

#include "CoreMinimal.h"
#include "IPlatformFilePak.h"
#include "IoDispatcher.h"
#include "KeyChainUtilities.h"

namespace uetools {
bool ExecutePakTools(const TCHAR *CmdLine);
bool ListFilesInPak(const TArray<FString> &pakFiles, const FKeyChain &keyChain);
bool ExtractFilesFromPak(const FKeyChain &keyChain, const FString &pakFile, const FString &outputDir);
TRefCountPtr<FPakFile> OpenPakFile(const FString &pakFilename, const FKeyChain &keyChain);
TUniquePtr<FIoStoreReader> CreateIoStoreReader(const FString &Path, const FKeyChain &KeyChain);
FString GetFileWithoutInitialDots(const FString &filename);
} // namespace uetools
