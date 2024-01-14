#include "IPlatformFilePak.h"
#include "KeyChainUtilities.h"
#include "PakTools.h"

namespace uetools {

inline bool BufferedCopyFile(FArchive &Dest, FArchive &Source, const FPakEntry &Entry, void *Buffer, int64 BufferSize, const FKeyChain &InKeyChain) {
    // Align down
    BufferSize = BufferSize & ~(FAES::AESBlockSize - 1);
    int64 RemainingSizeToCopy = Entry.Size;
    while (RemainingSizeToCopy > 0) {
        const int64 SizeToCopy = FMath::Min(BufferSize, RemainingSizeToCopy);
        // If file is encrypted so we need to account for padding
        int64 SizeToRead = Entry.IsEncrypted() ? Align(SizeToCopy, FAES::AESBlockSize) : SizeToCopy;

        Source.Serialize(Buffer, SizeToRead);
        if (Entry.IsEncrypted()) {
            const FNamedAESKey *Key = InKeyChain.GetPrincipalEncryptionKey();
            check(Key);
            FAES::DecryptData((uint8 *)Buffer, SizeToRead, Key->Key);
        }
        Dest.Serialize(Buffer, SizeToCopy);
        RemainingSizeToCopy -= SizeToRead;
    }
    return true;
}

inline bool UncompressCopyFile(FArchive &Dest, FArchive &Source, const FPakEntry &Entry, uint8 *&PersistentBuffer, int64 &BufferSize, const FKeyChain &InKeyChain,
                               const FPakFile &PakFile) {
    if (Entry.UncompressedSize == 0) {
        return false;
    }

    // The compression block size depends on the bit window that the PAK file was originally created with. Since this isn't stored in the PAK file itself,
    // we can use FCompression::CompressMemoryBound as a guideline for the max expected size to avoid unncessary reallocations, but we need to make sure
    // that we check if the actual size is not actually greater (eg. UE-59278).
    FName EntryCompressionMethod = PakFile.GetInfo().GetCompressionMethod(Entry.CompressionMethodIndex);
    int32 MaxCompressionBlockSize = FCompression::CompressMemoryBound(EntryCompressionMethod, Entry.CompressionBlockSize);
    for (const FPakCompressedBlock &Block : Entry.CompressionBlocks) {
        MaxCompressionBlockSize = FMath::Max<int32>(MaxCompressionBlockSize, IntCastChecked<int32>(Block.CompressedEnd - Block.CompressedStart));
    }

    int64 WorkingSize = Entry.CompressionBlockSize + MaxCompressionBlockSize;
    if (BufferSize < WorkingSize) {
        PersistentBuffer = (uint8 *)FMemory::Realloc(PersistentBuffer, WorkingSize);
        BufferSize = WorkingSize;
    }

    uint8 *UncompressedBuffer = PersistentBuffer + MaxCompressionBlockSize;

    for (uint32 BlockIndex = 0, BlockIndexNum = Entry.CompressionBlocks.Num(); BlockIndex < BlockIndexNum; ++BlockIndex) {
        int64 CompressedBlockSize = Entry.CompressionBlocks[BlockIndex].CompressedEnd - Entry.CompressionBlocks[BlockIndex].CompressedStart;
        int64 UncompressedBlockSize = FMath::Min<int64>(Entry.UncompressedSize - Entry.CompressionBlockSize * BlockIndex, Entry.CompressionBlockSize);
        Source.Seek(Entry.CompressionBlocks[BlockIndex].CompressedStart + (PakFile.GetInfo().HasRelativeCompressedChunkOffsets() ? Entry.Offset : 0));
        int64 SizeToRead = Entry.IsEncrypted() ? Align(CompressedBlockSize, FAES::AESBlockSize) : CompressedBlockSize;
        Source.Serialize(PersistentBuffer, SizeToRead);

        if (Entry.IsEncrypted()) {
            const FNamedAESKey *Key = InKeyChain.GetEncryptionKeys().Find(PakFile.GetInfo().EncryptionKeyGuid);
            if (Key == nullptr) {
                Key = InKeyChain.GetPrincipalEncryptionKey();
            }
            check(Key);
            FAES::DecryptData(PersistentBuffer, SizeToRead, Key->Key);
        }

        if (!FCompression::UncompressMemory(EntryCompressionMethod, UncompressedBuffer, IntCastChecked<int32>(UncompressedBlockSize), PersistentBuffer,
                                            IntCastChecked<int32>(CompressedBlockSize))) {
            return false;
        }
        Dest.Serialize(UncompressedBuffer, UncompressedBlockSize);
    }

    return true;
}

bool ExtractFilesFromPak(const FKeyChain &keyChain, const FString &pakFile, const FString &outputDir) {
    const FString absolutePakFile = FPaths::ConvertRelativePathToFull(FGenericPlatformMisc::LaunchDir(), pakFile);
    const FString absoluteOutputDir = FPaths::ConvertRelativePathToFull(FGenericPlatformMisc::LaunchDir(), outputDir);

    UE_LOG(LogPakFile, Display, TEXT("Extracting files from %s"), *absolutePakFile);
    UE_LOG(LogPakFile, Display, TEXT("Output directory: %s"), *absoluteOutputDir);

    constexpr int64 bufferSize = 8 * 1024 * 1024; // 8MB buffer for extracting
    void *Buffer = FMemory::Malloc(bufferSize);

    int32 fileErrors = 0;

    uint8 *persistantCompressionBuffer = nullptr;
    int64 compressionBufferSize = 0;

    if (!FPaths::FileExists(absolutePakFile)) {
        UE_LOG(LogPakFile, Error, TEXT("Pak file '%s' does not exist."), *absolutePakFile);
        return false;
    }

    const FString extension = FPaths::GetExtension(absolutePakFile);
    if (extension == TEXT("pak")) {
        const auto pak = OpenPakFile(absolutePakFile, keyChain);
        if (!pak) {
            return false;
        }

        UE_LOG(LogPakFile, Display, TEXT("Mount Point: %s"), *pak->GetMountPoint());

        if (!pak->HasFilenames()) {
            UE_LOG(LogPakFile, Error, TEXT("PakFiles were loaded without filenames, cannot extract."));
            return false;
        }

        FSharedPakReader pakReader = pak->GetSharedReader(nullptr);

        for (FPakFile::FPakEntryIterator it(*pak, false); it; ++it) {
            const FString *filename = it.TryGetFilename();
            if (filename == nullptr) {
                UE_LOG(LogPakFile, Error, TEXT("Unable to get filename for pak file entry."));
                continue;
            }

            FString destFilename(absoluteOutputDir / *filename);

            UE_LOG(LogPakFile, Display, TEXT("Extracting '%s'"), *destFilename);

            pakReader->Seek(it.Info().Offset);

            FPakEntry EntryInfo;
            EntryInfo.Serialize(pakReader.GetArchive(), pak->GetInfo().Version);
            if (!EntryInfo.IndexDataEquals(it.Info())) {
                UE_LOG(LogPakFile, Error, TEXT("PakEntry mismatch for \"%s\"."), **filename);
                fileErrors++;
                continue;
            }

            TUniquePtr<FArchive> FileHandle(IFileManager::Get().CreateFileWriter(*destFilename));
            if (!FileHandle) {
                UE_LOG(LogPakFile, Error, TEXT("Unable to create file \"%s\"."), *destFilename);
                fileErrors++;
                continue;
            }

            if (it.Info().CompressionMethodIndex == 0) {
                if (!BufferedCopyFile(*FileHandle, pakReader.GetArchive(), it.Info(), Buffer, bufferSize, keyChain)) {
                    fileErrors++;
                }
            } else {
                if (!UncompressCopyFile(*FileHandle, pakReader.GetArchive(), it.Info(), persistantCompressionBuffer, compressionBufferSize, keyChain, *pak)) {
                    fileErrors++;
                }
            }
        }

    } else if (extension == TEXT("utoc")) {
        auto ioStoreReader = CreateIoStoreReader(absolutePakFile, keyChain);
        if (!ioStoreReader) {
            return false;
        }

        UE_LOG(LogPakFile, Display, TEXT("Reading from IoStore"));
        UE_LOG(LogPakFile, Display, TEXT("  Mount Point: %s"), *ioStoreReader->GetDirectoryIndexReader().GetMountPoint());

        auto visitor = [&](const FString &filename, uint32 TocEntryIndex) -> bool {
            TIoStatusOr<FIoStoreTocChunkInfo> chunkInfo = ioStoreReader->GetChunkInfo(TocEntryIndex);
            const auto actualFilename = GetFileWithoutInitialDots(filename);
            UE_LOG(LogPakFile, Display, TEXT("Extracting '%s'"), *actualFilename);

            if (!chunkInfo.IsOk()) {
                UE_LOG(LogPakFile, Error, TEXT("Unable to get chunk info for '%s' %s."), *actualFilename, *chunkInfo.Status().ToString());
                return true;
            }

            auto buffer = ioStoreReader->Read(chunkInfo.ValueOrDie().Id, {});
            if (!buffer.IsOk()) {
                UE_LOG(LogPakFile, Error, TEXT("Cannot read file \"%s\" %s."), *actualFilename, *buffer.Status().ToString());
                fileErrors++;
                return true;
            }

            const FString destFilename(absoluteOutputDir / *actualFilename);
            const TUniquePtr<FArchive> fileHandle(IFileManager::Get().CreateFileWriter(*destFilename));
            if (!fileHandle) {
                UE_LOG(LogPakFile, Error, TEXT("Unable to create file \"%s\"."), *destFilename);
                fileErrors++;
                return false;
            }

            const uint8 *data = buffer.ValueOrDie().GetData();
            fileHandle->Serialize(const_cast<uint8 *>(data), buffer.ValueOrDie().DataSize());
            fileHandle->Close();

            return true;
        };
        ioStoreReader->GetDirectoryIndexReader().IterateDirectoryIndex(FIoDirectoryIndexHandle::RootDirectory(), TEXT(""), visitor);
    } else {
        UE_LOG(LogPakFile, Error, TEXT("Expected .pak or .utoc file but got '%s'"), *absolutePakFile);
        return false;
    }

    if (fileErrors > 0) {
        UE_LOG(LogPakFile, Error, TEXT("Failed to extract %d files."), fileErrors);
    }

    return true;
}

} // namespace uetools
