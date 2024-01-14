#include "IPlatformFilePak.h"
#include "IoDispatcher.h"
#include "KeyChainUtilities.h"
#include "PlatformFileManager.h"

namespace uetools {
TRefCountPtr<FPakFile> OpenPakFile(const FString &pakFilename, const FKeyChain &keyChain) {
    UE_LOG(LogPakFile, Display, TEXT("Reading from Pak '%s'"), *pakFilename);

    IPlatformFile *lowerLevelPlatformFile = &FPlatformFileManager::Get().GetPlatformFile();
    const TRefCountPtr pakFile = new FPakFile(lowerLevelPlatformFile, *pakFilename, false);

    UE_LOG(LogPakFile, Display, TEXT("Reading from Pak '%s'"), *pakFilename);
    if (!pakFile->IsValid()) {
        if (pakFile->GetInfo().Magic != 0 && pakFile->GetInfo().EncryptionKeyGuid.IsValid() && !keyChain.GetEncryptionKeys().Contains(pakFile->GetInfo().EncryptionKeyGuid)) {
            UE_LOG(LogPakFile, Fatal, TEXT("Missing encryption key %s for pak file \"%s\"."), *pakFile->GetInfo().EncryptionKeyGuid.ToString(), *pakFilename);
        } else {
            UE_LOG(LogPakFile, Fatal, TEXT("Unable to open pak file \"%s\"."), *pakFilename);
        }

        return nullptr;
    }

    return pakFile;
}

TUniquePtr<FIoStoreReader> CreateIoStoreReader(const FString &Path, const FKeyChain &KeyChain) {
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

FString GetFileWithoutInitialDots(const FString &filename) {
    FString result = filename;
    while (result.StartsWith(TEXT(".")) || result.StartsWith(TEXT("/")) || result.StartsWith(TEXT("\\"))) {
        result = result.RightChop(1);
    }
    return result;
}

} // namespace uetools
