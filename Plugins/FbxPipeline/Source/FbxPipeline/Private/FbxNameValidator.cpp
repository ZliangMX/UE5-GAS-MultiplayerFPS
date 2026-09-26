// Copyright PingLiangYe. All Rights Reserved.

#include "FbxNameValidator.h"

namespace FbxNaming
{
	namespace
	{
		/** 把首次出现的字符追加进 Out（保持出现顺序、不重复）。 */
		void AppendUnique(FString& Out, TCHAR Ch)
		{
			// FString::Find 在 UE5.4 没有单 TCHAR 重载，这里用 FCString::Strchr 查子串。
			if (FCString::Strchr(*Out, Ch) == nullptr)
			{
				Out.AppendChar(Ch);
			}
		}
	}

	FNamingConfig MakeDefaultConfig()
	{
		return FNamingConfig();
	}

	TArray<FNameIssue> ValidateName(const FString& RawName, const FNamingConfig& InConfig)
	{
		TArray<FNameIssue> Issues;

		if (RawName.IsEmpty())
		{
			Issues.Emplace(EIssueSeverity::Error, EIssueCode::Empty, TEXT("资产名为空"));
			return Issues; // 空名字没必要继续逐字符
		}

		// 单遍扫描分类：非 ASCII / 空白 / 不在允许字符集里的字符。
		bool bNonAscii = false;
		bool bWhitespace = false;
		FString BadChars;
		for (const TCHAR Ch : RawName)
		{
			if (Ch == TEXT(' ') || Ch == TEXT('\t'))
			{
				bWhitespace = true;
			}
			else if (Ch > 0x7F) // Windows 下 TCHAR 是 UTF-16，中文码位远大于 0x7F
			{
				bNonAscii = true;
			}
			else if (FCString::Strchr(*InConfig.AllowedChars, Ch) == nullptr)
			{
				AppendUnique(BadChars, Ch);
			}
		}

		if (bNonAscii)
		{
			Issues.Emplace(EIssueSeverity::Warning, EIssueCode::NonAscii,
				TEXT("含非 ASCII 字符（中文/全角等），跨 DCC/工具链常坏引用"));
		}
		if (bWhitespace)
		{
			Issues.Emplace(EIssueSeverity::Warning, EIssueCode::Whitespace,
				TEXT("含空格/Tab，建议用下划线分隔"));
		}
		if (!BadChars.IsEmpty())
		{
			Issues.Emplace(EIssueSeverity::Warning, EIssueCode::BadCharacter,
				FString::Printf(TEXT("含不允许的字符：%s（仅允许 %s）"), *BadChars, *InConfig.AllowedChars));
		}
		if (RawName.Len() > InConfig.MaxNameLen)
		{
			Issues.Emplace(EIssueSeverity::Error, EIssueCode::TooLong,
				FString::Printf(TEXT("名字过长（%d 字符 > %d）"), RawName.Len(), InConfig.MaxNameLen));
		}
		if (InConfig.bRequirePrefix && !InConfig.RequiredPrefixes.IsEmpty())
		{
			bool bMatched = false;
			for (const FString& Prefix : InConfig.RequiredPrefixes)
			{
				if (RawName.StartsWith(Prefix))
				{
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				Issues.Emplace(EIssueSeverity::Warning, EIssueCode::MissingPrefix,
					FString::Printf(TEXT("未命中任何已声明前缀（%s）"),
						*FString::Join(InConfig.RequiredPrefixes, TEXT(" / "))));
			}
		}

		return Issues;
	}

	TArray<TArray<FNameIssue>> ValidateNameBatch(
		const TArray<FString>& InNames,
		const TArray<FString>& InExistingNames,
		const FNamingConfig& InConfig)
	{
		const int32 N = InNames.Num();
		TArray<TArray<FNameIssue>> Out;
		Out.SetNum(N);

		TMap<FString, int32> Counts;
		for (const FString& Name : InNames)
		{
			Counts.FindOrAdd(Name)++;
		}
		TSet<FString> Existing(InExistingNames);

		for (int32 i = 0; i < N; ++i)
		{
			const FString& Name = InNames[i];
			const int32 Count = Counts.FindChecked(Name);
			if (Count > 1)
			{
				Out[i].Emplace(EIssueSeverity::Error, EIssueCode::Duplicate,
					FString::Printf(TEXT("同批 %d 个文件将产出同名资产 '%s'，会互相覆盖"),
						Count, *Name));
			}
			else if (Existing.Contains(Name))
			{
				Out[i].Emplace(EIssueSeverity::Warning, EIssueCode::Duplicate,
					FString::Printf(TEXT("目标目录已存在同名资产 '%s'，导入可能被自动改名 _001"),
						*Name));
			}
		}

		return Out;
	}

	EIssueSeverity WorstSeverity(const TArray<FNameIssue>& InIssues)
	{
		EIssueSeverity Worst = EIssueSeverity::Info;
		for (const FNameIssue& Issue : InIssues)
		{
			if (Issue.Severity == EIssueSeverity::Error)
			{
				return EIssueSeverity::Error;
			}
			if (Issue.Severity == EIssueSeverity::Warning)
			{
				Worst = EIssueSeverity::Warning;
			}
		}
		return Worst;
	}

	bool IsOK(const TArray<FNameIssue>& InIssues)
	{
		return WorstSeverity(InIssues) <= EIssueSeverity::Info;
	}

	FString SeverityToString(EIssueSeverity InSeverity)
	{
		switch (InSeverity)
		{
		case EIssueSeverity::Error:   return TEXT("Error");
		case EIssueSeverity::Warning: return TEXT("Warning");
		case EIssueSeverity::Info:    return TEXT("Info");
		default:                      return TEXT("Unknown");
		}
	}

	FString CodeToString(EIssueCode InCode)
	{
		switch (InCode)
		{
		case EIssueCode::None:          return TEXT("None");
		case EIssueCode::Empty:         return TEXT("Empty");
		case EIssueCode::NonAscii:      return TEXT("NonAscii");
		case EIssueCode::Whitespace:    return TEXT("Whitespace");
		case EIssueCode::BadCharacter:  return TEXT("BadCharacter");
		case EIssueCode::TooLong:       return TEXT("TooLong");
		case EIssueCode::MissingPrefix: return TEXT("MissingPrefix");
		case EIssueCode::Duplicate:     return TEXT("Duplicate");
		default:                        return TEXT("Unknown");
		}
	}
}
