#include "IPlatformFilePak.h"
#include "PakTools.h"
#include "PlatformFileManager.h"
#include "Private/IoDispatcherFileBackend.h"

// Defined in PakFileUtilities.cpp
FString GetPakPath(const TCHAR *SpecifiedPath, bool bIsForCreation);

// Defined in IoDispatcherFileBackend.cpp
TSharedRef<FFileIoStore> CreateIoDispatcherFileBackend();

namespace uetools {
struct FToolFileEntry {
    FString Filename;
    int64 UncompressedSize = 0;
    int64 CompressedSize = 0;
    const TCHAR *Source; // "Pak" or "IoStore"
};

FString HumanSize(int64 size) {
    const FString units[] = {TEXT("B"), TEXT("KB"), TEXT("MB"), TEXT("GB"), TEXT("TB"), TEXT("PB")};
    int32 unitIndex = 0;
    double sizeIt = size;

    while (sizeIt >= 1024.0 && unitIndex < 5) {
        sizeIt /= 1024.0;
        unitIndex++;
    }

    return FString::Printf(TEXT("%.2f %s"), sizeIt, *units[unitIndex]);
}

using FFullFileVisitor = TFunctionRef<bool(FString, const FIoDirectoryIndexHandle &)>;

TOptional<TArray<FToolFileEntry>> ReadFileListFromPak(const FString &pakFilename, const FKeyChain &keyChain) {
    const auto pakFile = OpenPakFile(pakFilename, keyChain);

    if (!pakFile->IsValid()) {
        return NullOpt;
    }

    TArray<FToolFileEntry> result;

    // Iterate over all files in the pak file
    for (FPakFile::FPakEntryIterator iterator(*pakFile, true); iterator; ++iterator) {
        const FString *filename = iterator.TryGetFilename();
        if (filename == nullptr) {
            UE_LOG(LogPakFile, Error, TEXT("Unable to get filename for pak file entry."));
            continue;
        }
        FString fullPath = pakFile->GetMountPoint() / *filename;
        result.Add({fullPath, iterator.Info().UncompressedSize, iterator.Info().Size, TEXT("Pak")});
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
} // namespace uetools
