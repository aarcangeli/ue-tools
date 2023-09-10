// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "KeyChainUtilities.h"

bool ExecutePakTools(const TCHAR *CmdLine);
bool ListFilesInPak(const TArray<FString> &pakFiles, const FKeyChain &keyChain);
