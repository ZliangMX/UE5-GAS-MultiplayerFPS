// Copyright PingLiangYe. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** FBX 资产导入链路的命名校验核心。
 *
 *  设计目标（也是"讲得清"的三点）：
 *   1. 纯 C++，不依赖编辑器/资产/引擎模块 —— 逻辑可以在自动化测试里直接跑，
 *      也能被导入执行器在「导入前文件名」和「导入后资产名」两个环节复用。
 *   2. 所有规则数据化在 FNamingConfig 里 —— 换一套规则只改配置不改代码。
 *   3. 校验结果带 严重级别(Error/Warning/Info) + 稳定代号(EIssueCode)，
 *      UI 层用代号过滤/上色，测试用代号断言，都不依赖人类文案。
 *
 *  命名规则灵感来自实际 FBX 管线踩过的坑：
 *   中文/空格/非法符号跨 DCC 与引擎工具链经常坏引用；重名会让 UE 自动改 _001。
 */
namespace FbxNaming
{
	/** 严重级别。Error 应阻止导入；Warning 只提示。 */
	enum class EIssueSeverity : uint8
	{
		Info = 0,
		Warning = 1,
		Error = 2
	};

	/** 问题代号。测试与 UI 拿它做稳定断言/过滤，避免依赖可能改动的文案。 */
	enum class EIssueCode : uint8
	{
		None = 0,
		Empty,          // 名字为空
		NonAscii,       // 含中文等非 ASCII 字符
		Whitespace,     // 含空格 / Tab
		BadCharacter,   // 含允许字符集之外的字符
		TooLong,        // 超过 MaxNameLen
		MissingPrefix,  // 开启了前缀模式但没命中任何已声明前缀
		Duplicate       // 与同批其它名字 / 既有资产重名（UE 自动改 _001 的诱因）
	};

	/** 一条校验结果。 */
	struct FNameIssue
	{
		EIssueSeverity Severity = EIssueSeverity::Info;
		EIssueCode Code = EIssueCode::None;
		FString Message;

		FNameIssue() = default;
		FNameIssue(EIssueSeverity InSeverity, EIssueCode InCode, FString InMessage)
			: Severity(InSeverity), Code(InCode), Message(MoveTemp(InMessage)) {}
	};

	/** 命名规则配置。全字段可改，规则只从这里读。 */
	struct FNamingConfig
	{
		/** 单名字最大长度。 */
		int32 MaxNameLen = 63;

		/** 允许的字符集（逐字符校验）。 */
		FString AllowedChars =
			TEXT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_");

		/** 是否启用「必须命中已声明前缀」模式。 */
		bool bRequirePrefix = false;

		/** 前缀模式开启后，名字必须 StartsWith 其中之一。如 SK_ / M_ / A_ ... */
		TArray<FString> RequiredPrefixes;
	};

	FNamingConfig MakeDefaultConfig();

	/** 单个名字的字符级规则校验。可用于导入前文件名、导入后资产名两个环节。 */
	TArray<FNameIssue> ValidateName(const FString& RawName, const FNamingConfig& InConfig);

	/** 批级查重，返回与 InNames 一一对应的二维数组（每个名字至多一条批级问题，无问题为空）。
	 *  同批内重名 = Error（会互相覆盖）；与 InExistingNames 既有资产重名 = Warning（UE 会改 _001）。 */
	TArray<TArray<FNameIssue>> ValidateNameBatch(
		const TArray<FString>& InNames,
		const TArray<FString>& InExistingNames,
		const FNamingConfig& InConfig);

	/** 取一批问题里最严重的级别。空数组返回 Info（即视为通过）。 */
	EIssueSeverity WorstSeverity(const TArray<FNameIssue>& InIssues);

	/** 便捷判断：没有任何 Error/Warning 才算 OK。 */
	bool IsOK(const TArray<FNameIssue>& InIssues);

	FString SeverityToString(EIssueSeverity InSeverity);
	FString CodeToString(EIssueCode InCode);
}
