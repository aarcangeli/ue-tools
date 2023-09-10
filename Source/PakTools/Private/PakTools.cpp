// Copyright Epic Games, Inc. All Rights Reserved.

#include "PakTools.h"
#include "CommandLine.h"
#include "IO/IoContainerHeader.h"
#include "IPlatformFilePak.h"
#include "KeyChainUtilities.h"
#include "Private/IoDispatcherFileBackend.h"
#include "Runtime/Launch/Public/RequiredProgramMainCPPInclude.h"
#include "StableSort.h"

IMPLEMENT_APPLICATION(PakTools, "PakTools");

// Defined in PakFileUtilities.cpp
FString GetPakPath(const TCHAR *SpecifiedPath, bool bIsForCreation);
void LoadKeyChain(const TCHAR *CmdLine, FKeyChain &OutCryptoSettings);

// Defined in IoDispatcherFileBackend.cpp
TSharedRef<FFileIoStore> CreateIoDispatcherFileBackend();

struct FToolFileEntry {
    FString Filename;
    int64 UncompressedSize = 0;
    int64 CompressedSize = 0;
    const TCHAR *Source; // "Pak" or "IoStore"
};

INT32_MAIN_INT32_ARGC_TCHAR_ARGV() {
    FTaskTagScope Scope(ETaskTag::EGameThread);

    // start up the main loop
    GEngineLoop.PreInit(ArgC, ArgV, TEXT("-UseIoStore"));

    const double startTime = FPlatformTime::Seconds();

    const int32 result = ExecutePakTools(FCommandLine::Get()) ? 0 : 1;

    UE_LOG(LogPakFile, Display, TEXT("PakTools executed in %f seconds"), FPlatformTime::Seconds() - startTime);

    GLog->Flush();

    RequestEngineExit(TEXT("PakTools Exiting"));

    FEngineLoop::AppPreExit();
    FModuleManager::Get().UnloadModulesAtShutdown();
    FEngineLoop::AppExit();

    return result;
}

FString HumanSize(int64 size) {
    const FString Units[] = {TEXT("B"), TEXT("KB"), TEXT("MB"), TEXT("GB"), TEXT("TB"), TEXT("PB")};
    int32 UnitIndex = 0;
    double sizef = size;

    while (sizef >= 1024.0 && UnitIndex < 5) {
        sizef /= 1024.0;
        UnitIndex++;
    }

    return FString::Printf(TEXT("%.2f %s"), sizef, *Units[UnitIndex]);
}

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

    UE_LOG(LogPakFile, Error, TEXT("No command specified. Usage:"));
    UE_LOG(LogPakFile, Error, TEXT("  PakTools -List <pak_or_utoc> ..."));

    return true;
}

static TUniquePtr<FIoStoreReader> CreateIoStoreReader(const FString &Path, const FKeyChain &KeyChain) {
    TUniquePtr<FIoStoreReader> IoStoreReader(new FIoStoreReader());

    TMap<FGuid, FAES::FAESKey> DecryptionKeys;
    for (const auto &KV : KeyChain.GetEncryptionKeys()) {
        DecryptionKeys.Add(KV.Key, KV.Value.Key);
    }
    const FIoStatus Status = IoStoreReader->Initialize(*FPaths::ChangeExtension(*Path, TEXT("")), DecryptionKeys);
    if (Status.IsOk()) {
        return IoStoreReader;
    } else {
        UE_LOG(LogPakFile, Warning, TEXT("Failed creating IoStore reader '%s' [%s]"), *Path, *Status.ToString())
        return nullptr;
    }
}

using FFullFileVisitor = TFunctionRef<bool(FString, const FIoDirectoryIndexHandle &)>;

TOptional<TArray<FToolFileEntry>> ReadFileListFromPak(const FString &pakFilename, const FKeyChain &keyChain) {

    IPlatformFile *lowerLevelPlatformFile = &FPlatformFileManager::Get().GetPlatformFile();
    const TRefCountPtr pakFile = new FPakFile(lowerLevelPlatformFile, *pakFilename, false);

    UE_LOG(LogPakFile, Display, TEXT("Reading from Pak '%s'"), *pakFilename);
    if (!pakFile->IsValid()) {
        if (pakFile->GetInfo().Magic != 0 && pakFile->GetInfo().EncryptionKeyGuid.IsValid() && !keyChain.GetEncryptionKeys().Contains(pakFile->GetInfo().EncryptionKeyGuid)) {
            UE_LOG(LogPakFile, Fatal, TEXT("Missing encryption key %s for pak file \"%s\"."), *pakFile->GetInfo().EncryptionKeyGuid.ToString(), *pakFilename);
        } else {
            UE_LOG(LogPakFile, Fatal, TEXT("Unable to open pak file \"%s\"."), *pakFilename);
        }

        return NullOpt;
    }

    TArray<FToolFileEntry> result;

    // Iterate over all files in the pak file
    for (FPakFile::FPakEntryIterator It(*pakFile, true); It; ++It) {
        const FString *filename = It.TryGetFilename();
        if (filename == nullptr) {
            UE_LOG(LogPakFile, Error, TEXT("Unable to get filename for pak file entry."));
            continue;
        }
        FString fullPath = pakFile->GetMountPoint() / *filename;
        result.Add({fullPath, It.Info().UncompressedSize, It.Info().Size, TEXT("Pak")});
    }

    return result;
}

TOptional<TArray<FToolFileEntry>> ReadFileListFromToc(const FString &pakFilename, const FKeyChain &keyChain) {
    // Iterate over all files in the utoc/ucas files
    auto ioStoreReader = CreateIoStoreReader(pakFilename, keyChain);
    if (!ioStoreReader) {
        return NullOpt;
    }
    UE_LOG(LogPakFile, Display, TEXT("Reading from IoStore"));
    UE_LOG(LogPakFile, Display, TEXT("  Mount Point: %s"), *ioStoreReader->GetDirectoryIndexReader().GetMountPoint());
    TArray<TPair<FString, FIoDirectoryIndexHandle>> entries;

    // Populate ChunkFileNamesMap (unused for now)
    TMap<FIoChunkId, FString> chunkFileNamesMap;
    auto visitor = [&chunkFileNamesMap, &ioStoreReader](const FString &filename, uint32 TocEntryIndex) -> bool {
        TIoStatusOr<FIoStoreTocChunkInfo> chunkInfo = ioStoreReader->GetChunkInfo(TocEntryIndex);
        if (chunkInfo.IsOk()) {
            chunkFileNamesMap.Add(chunkInfo.ValueOrDie().Id, filename);
        }
        return true;
    };
    ioStoreReader->GetDirectoryIndexReader().IterateDirectoryIndex(FIoDirectoryIndexHandle::RootDirectory(), TEXT(""), visitor);

    // Populate SourceHashByChunkId
    TMap<FIoChunkId, FIoStoreTocChunkInfo> sourceHashByChunkId;
    TMap<FString, FToolFileEntry> fileEntriesByFilename;
    ioStoreReader->EnumerateChunks([&](const FIoStoreTocChunkInfo &chunkInfo) {
        FToolFileEntry &currentEntry = fileEntriesByFilename.FindOrAdd(chunkInfo.FileName);
        currentEntry.Filename = chunkInfo.FileName;
        currentEntry.UncompressedSize += chunkInfo.Size;
        currentEntry.CompressedSize += chunkInfo.CompressedSize;
        currentEntry.Source = TEXT("IoStore");
        return true;
    });

    TArray<FToolFileEntry> result;
    for (const auto &KV : fileEntriesByFilename) {
        result.Add(KV.Value);
    }
    return result;
}

bool ListFilesInPak(const TArray<FString> &pakFiles, const FKeyChain &keyChain) {
    TArray<FToolFileEntry> files;

    for (const FString &it : pakFiles) {
        const FString pakFilename = GetPakPath(*it, false);

        const FString extension = FPaths::GetExtension(pakFilename);
        if (extension == TEXT("pak")) {
            auto result = ReadFileListFromPak(pakFilename, keyChain);
            if (!result) {
                return false;
            }
            files += result.GetValue();
        } else if (extension == TEXT("utoc")) {
            auto result = ReadFileListFromToc(pakFilename, keyChain);
            if (!result) {
                return false;
            }
            files += result.GetValue();
        } else {
            UE_LOG(LogPakFile, Error, TEXT("Expected .pak or .utoc file but got '%s'"), *pakFilename);
            return false;
        }
    }

    Algo::StableSort(files, [](const FToolFileEntry &a, const FToolFileEntry &b) { return a.CompressedSize > b.CompressedSize; });

    int64 totalSize = 0;
    for (const FToolFileEntry &file : files) {
        UE_LOG(LogPakFile, Display, TEXT("%s (%s) [Compression: %.02f%%]"), *file.Filename, *HumanSize(file.CompressedSize), 100.0 * file.CompressedSize / file.UncompressedSize);
        totalSize += file.CompressedSize;
    }

    UE_LOG(LogPakFile, Display, TEXT("Total compressed data: %s"), *HumanSize(totalSize));

    return true;
}
