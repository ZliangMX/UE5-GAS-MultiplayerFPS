// Copyright PingLiangYe. All Rights Reserved.

// FbxPipeline 命名校验核心的自动化测试。
// 跑法：编辑器 Tools(或 Window) > Developer Tools > Session Frontend > Automation 面板，
// 过滤 "FbxPipeline" 后 Run。或命令行：
//   UnrealEditor-Cmd Blaster.uproject -ExecCmds="Automation RunTests FbxPipeline; Quit" -unattended -nopause
// 只在带开发自动化测试的构建里编译（编辑器 Development 配置默认开启）。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "FbxNameValidator.h"

namespace
{
	using FbxNaming::EIssueCode;
	using FbxNaming::EIssueSeverity;
	using FbxNaming::FNameIssue;

	bool HasCode(const TArray<FNameIssue>& InIssues, EIssueCode InCode)
	{
		for (const FNameIssue& Issue : InIssues)
		{
			if (Issue.Code == InCode)
			{
				return true;
			}
		}
		return false;
	}
}

// 1) 合法名字：无任何问题
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_ValidName,
	"FbxPipeline.Naming.ValidName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_ValidName::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("SK_Mannequin_01"), Config);
	TestTrue(TEXT("合法名字不应产生任何问题"), Issues.Num() == 0 && FbxNaming::IsOK(Issues));
	return true;
}

// 2) 空名字：Error
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_EmptyName,
	"FbxPipeline.Naming.EmptyName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_EmptyName::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT(""), Config);
	TestTrue(TEXT("空名字应报 Empty"), HasCode(Issues, EIssueCode::Empty));
	TestTrue(TEXT("Empty 是 Error"), FbxNaming::WorstSeverity(Issues) == EIssueSeverity::Error);
	return true;
}

// 3) 中文：Warning NonAscii
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_NonAsciiName,
	"FbxPipeline.Naming.NonAsciiName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_NonAsciiName::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("角色_Mesh"), Config);
	TestTrue(TEXT("中文应报 NonAscii"), HasCode(Issues, EIssueCode::NonAscii));
	TestTrue(TEXT("NonAscii 是 Warning"), FbxNaming::WorstSeverity(Issues) == EIssueSeverity::Warning);
	return true;
}

// 4) 空格：Warning Whitespace
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_WhitespaceName,
	"FbxPipeline.Naming.WhitespaceName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_WhitespaceName::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("AK 47 Mesh"), Config);
	TestTrue(TEXT("空格应报 Whitespace"), HasCode(Issues, EIssueCode::Whitespace));
	return true;
}

// 5) 非法 ASCII 字符：Warning BadCharacter
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_BadCharacterName,
	"FbxPipeline.Naming.BadCharacterName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_BadCharacterName::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("AK@47_Mesh"), Config);
	TestTrue(TEXT("@ 应报 BadCharacter"), HasCode(Issues, EIssueCode::BadCharacter));
	return true;
}

// 6) 超长：Error TooLong
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_TooLongName,
	"FbxPipeline.Naming.TooLongName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_TooLongName::RunTest(const FString& Parameters)
{
	FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	Config.MaxNameLen = 8;
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("ABCDEFGHI"), Config); // 9 > 8
	TestTrue(TEXT("超长应报 TooLong"), HasCode(Issues, EIssueCode::TooLong));
	TestTrue(TEXT("TooLong 是 Error"), FbxNaming::WorstSeverity(Issues) == EIssueSeverity::Error);
	return true;
}

// 7) 前缀模式：未命中 → MissingPrefix；命中 → 通过
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_MissingPrefix,
	"FbxPipeline.Naming.MissingPrefix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_MissingPrefix::RunTest(const FString& Parameters)
{
	FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	Config.bRequirePrefix = true;
	Config.RequiredPrefixes = { TEXT("SK_"), TEXT("M_"), TEXT("A_") };

	const TArray<FNameIssue> NoPrefix = FbxNaming::ValidateName(TEXT("VP_Mesh"), Config);
	TestTrue(TEXT("未命中前缀应报 MissingPrefix"), HasCode(NoPrefix, EIssueCode::MissingPrefix));

	const TArray<FNameIssue> WithPrefix = FbxNaming::ValidateName(TEXT("SK_Mesh"), Config);
	TestTrue(TEXT("命中前缀应通过"), FbxNaming::IsOK(WithPrefix) && WithPrefix.Num() == 0);
	return true;
}

// 8) 默认配置是宽松的：没声明前缀就不强制前缀（避免默认误伤）
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_LenientByDefault,
	"FbxPipeline.Naming.LenientByDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_LenientByDefault::RunTest(const FString& Parameters)
{
	const FbxNaming::FNamingConfig Config = FbxNaming::MakeDefaultConfig();
	const TArray<FNameIssue> Issues = FbxNaming::ValidateName(TEXT("PlainName"), Config);
	TestTrue(TEXT("默认不应强制前缀"), FbxNaming::IsOK(Issues) && Issues.Num() == 0);
	return true;
}

// 9) 同批重名：Error Duplicate
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_BatchDuplicate,
	"FbxPipeline.Naming.BatchDuplicate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_BatchDuplicate::RunTest(const FString& Parameters)
{
	const TArray<FString> Names = { TEXT("Same"), TEXT("Other"), TEXT("Same") };
	const TArray<TArray<FNameIssue>> Results =
		FbxNaming::ValidateNameBatch(Names, {}, FbxNaming::MakeDefaultConfig());

	TestTrue(TEXT("重名文件 #0 应报 Duplicate"), Results[0].Num() > 0 &&
		Results[0][0].Code == EIssueCode::Duplicate &&
		Results[0][0].Severity == EIssueSeverity::Error);
	TestTrue(TEXT("不重名的 #1 应无批级问题"), Results[1].Num() == 0);
	TestTrue(TEXT("重名文件 #2 应报 Duplicate"), Results[2].Num() > 0 &&
		Results[2][0].Code == EIssueCode::Duplicate);
	return true;
}

// 10) 与既有资产撞名：Warning Duplicate（不是 Error——UE 会改 _001，导入不会失败）
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFbxNaming_ExistingCollision,
	"FbxPipeline.Naming.ExistingCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FFbxNaming_ExistingCollision::RunTest(const FString& Parameters)
{
	const TArray<FString> Names = { TEXT("Char") };
	const TArray<FString> Existing = { TEXT("Char") };
	const TArray<TArray<FNameIssue>> Results =
		FbxNaming::ValidateNameBatch(Names, Existing, FbxNaming::MakeDefaultConfig());

	TestTrue(TEXT("与既有资产重名应报 Duplicate"), Results[0].Num() > 0 &&
		Results[0][0].Code == EIssueCode::Duplicate);
	TestTrue(TEXT("既有撞名应是 Warning 而非 Error"),
		FbxNaming::WorstSeverity(Results[0]) == EIssueSeverity::Warning);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
