#include "SkillBarWidget.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateLayoutTransform.h"
#include "Rendering/RenderingCommon.h"
#include "Rendering/SlateRenderer.h"
#include "Framework/Application/SlateApplication.h"
#include "Blaster/Character/BlasterCharacter.h"
#include "Blaster/Abilities/BlasterGameplayAbility.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Blaster/BlasterTypes/Agent.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "AbilitySystemComponent.h"
#include "GameplayAbilitySpec.h"
#include "GameFramework/PlayerController.h"

// 手上那个投掷物是不是这一槽的技能 —— 命中就高亮。
// 闪光（E）和火球（Q）共用一套"持投掷物"状态，只有**种类对得上**的那一槽才该亮：
// 两个都亮会让人以为是"两个技能都准备好了"，那是完全不同的意思。
static bool BlasterHoldMatchesSkill(EBlasterThrowableKind Hold, EBlasterSkillType Skill)
{
	switch (Hold)
	{
	case EBlasterThrowableKind::Flash:		return Skill == EBlasterSkillType::Flash;
	case EBlasterThrowableKind::Fireball:	return Skill == EBlasterSkillType::Fireball;
	case EBlasterThrowableKind::Wall:		return Skill == EBlasterSkillType::Wall;
	default:								return false;
	}
}

void USkillBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 纯自绘 widget：默认不会每帧重绘，必须手动强制（充能/冷却每帧都在变）
	Invalidate(EInvalidateWidgetReason::Paint);
}

void USkillBarWidget::CollectVisibleSkills(const UAbilitySystemComponent* ASC, TArray<UBlasterGameplayAbility*>& OutSkills) const
{
	if (!ASC) return;

	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		// 优先真实实例（首次激活后创建，含武装状态；配置属性也从 CDO 复制过来）；
		// 未激活前实例不存在 → 回退 CDO 读纯配置（bShowInSkillBar / SkillType / 槽位）。
		// 注意：绝不能只用 Spec.Ability，那是 CDO，IsArmed() 恒为 false，武装视觉会永远不显示。
		UBlasterGameplayAbility* Ability = Spec.GetPrimaryInstance()
			? Cast<UBlasterGameplayAbility>(Spec.GetPrimaryInstance())
			: Cast<UBlasterGameplayAbility>(Spec.Ability.Get());
		if (Ability && Ability->bShowInSkillBar)
		{
			OutSkills.Add(Ability);
		}
	}

	// 按槽位排序：SkillSlotIndex 升序，INDEX_NONE 排最后。
	// 注意 TArray<T*>::Sort 会解引用指针，谓词收到的是 const T&（UBlasterGameplayAbility&）。
	OutSkills.Sort([](const UBlasterGameplayAbility& A, const UBlasterGameplayAbility& B)
	{
		const int32 AIdx = A.SkillSlotIndex == INDEX_NONE ? MAX_int32 : A.SkillSlotIndex;
		const int32 BIdx = B.SkillSlotIndex == INDEX_NONE ? MAX_int32 : B.SkillSlotIndex;
		return AIdx < BIdx;
	});
}

FSlateBrush USkillBarWidget::MakeBrush(UTexture2D* Texture)
{
	FSlateBrush Brush;
	if (Texture)
	{
		Brush.SetResourceObject(Texture);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2f(Texture->GetSizeX(), Texture->GetSizeY());
	}
	return Brush;
}

void USkillBarWidget::DrawBox(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Position, const FVector2D& Size, const FLinearColor& Color) const
{
	const FSlateBrush Brush = MakeBrush(GEngine ? GEngine->DefaultTexture : nullptr);
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FVector2f(Size), FSlateLayoutTransform(FVector2f(Position)));
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geom, &Brush, ESlateDrawEffect::None, Color);
	++LayerId;
}

void USkillBarWidget::DrawLine(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& A, const FVector2D& B, const FLinearColor& Color, float Thickness) const
{
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	TArray<FVector2f> Points;
	Points.Add(FVector2f(A));
	Points.Add(FVector2f(B));
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geom, MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	++LayerId;
}

void USkillBarWidget::DrawCircleFilled(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, const FLinearColor& Color) const
{
	DrawCircleSector(OutDrawElements, LayerId, Geometry, Center, Radius, 0.f, 2.f * PI, Color);
}

void USkillBarWidget::DrawCircleSector(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, float StartRad, float SweepRad, const FLinearColor& Color) const
{
	if (!GEngine || !GEngine->DefaultTexture) return;
	if (SweepRad <= 0.f) return;

	const FSlateBrush WhiteBrush = MakeBrush(GEngine->DefaultTexture);
	const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(WhiteBrush);
	const FColor Fill = Color.ToFColor(true);
	const FSlateRenderTransform& RT = Geometry.GetAccumulatedRenderTransform();

	// 分段数按扫过角度自适应：整圆 24 段，小扇形最少 4 段
	const int32 N = FMath::Clamp((int32)(FMath::Abs(SweepRad) / (PI / 24.f)), 4, 48);
	TArray<FSlateVertex> Verts;
	TArray<SlateIndex> Indices;
	Verts.Reserve(N + 2);
	Indices.Reserve(N * 3);
	Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RT, FVector2f(Center), FVector2f(0.f, 0.f), Fill));
	for (int32 i = 0; i <= N; ++i)
	{
		const float A = StartRad + SweepRad * i / N;
		const FVector2D P = Center + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Radius;
		Verts.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(RT, FVector2f(P), FVector2f(0.f, 0.f), Fill));
	}
	for (int32 i = 0; i < N; ++i)
	{
		Indices.Add(0);
		Indices.Add(i + 1);
		Indices.Add(i + 2);
	}
	FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Verts, Indices, nullptr, 0, 0, ESlateDrawEffect::None);
	++LayerId;
}

void USkillBarWidget::DrawCircleRing(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Center, float Radius, const FLinearColor& Color, float Thickness) const
{
	const int32 N = 32;
	TArray<FVector2f> Points;
	Points.Reserve(N + 1);
	for (int32 i = 0; i <= N; ++i)
	{
		const float A = i * 2.f * PI / N;
		Points.Add(FVector2f(Center + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Radius));
	}
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(0.f, 0.f)));
	FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geom, MoveTemp(Points), ESlateDrawEffect::None, Color, true, Thickness);
	++LayerId;
}

void USkillBarWidget::DrawText(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& Position, const FString& Text, float FontSize, const FLinearColor& Color) const
{
	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(FName("Bold"), FontSize);
	const FPaintGeometry Geom = Geometry.ToPaintGeometry(FSlateLayoutTransform(FVector2f(Position)));
	FSlateDrawElement::MakeText(OutDrawElements, LayerId, Geom, Text, Font, ESlateDrawEffect::None, Color);
	++LayerId;
}

void USkillBarWidget::DrawSkillIcon(FSlateWindowElementList& OutDrawElements, int32& LayerId, const FGeometry& Geometry, const FVector2D& C, float H, EBlasterSkillType SkillType, UTexture2D* Icon, bool bDimmed) const
{
	// —— 有贴图就画贴图，没有才走下面的矢量兜底 ——
	// 没充能时整张图压到 42% 透明度（= 素材里 _gray 那一版的透明度）。
	// 只调 alpha 不换图：白图 + alpha 就是灰效果，省一份资产，也不会出现两版图对不齐。
	const float IconAlpha = bDimmed ? 0.42f : 1.f;

	if (Icon)
	{
		const float TexW = (float)Icon->GetSizeX();
		const float TexH = (float)Icon->GetSizeY();
		if (TexW > 0.f && TexH > 0.f)
		{
			// 等比缩放到图标框内（框是 2H 见方）。素材的图标是 51x60 这种非正方，
			// 直接拉成正方形会把人拉胖，所以取小比例、居中。
			const float Scale = FMath::Min((H * 2.f) / TexW, (H * 2.f) / TexH);
			const FVector2D DrawSize(TexW * Scale, TexH * Scale);
			const FVector2D DrawPos = C - DrawSize * 0.5f;

			const FSlateBrush Brush = MakeBrush(Icon);
			const FPaintGeometry Geom = Geometry.ToPaintGeometry(
				FVector2f(DrawSize), FSlateLayoutTransform(FVector2f(DrawPos)));
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geom, &Brush, ESlateDrawEffect::None,
				FLinearColor(1.f, 1.f, 1.f, IconAlpha));
			++LayerId;
			return;
		}
		// 贴图尺寸读不出来（还没加载完）→ 掉到矢量兜底，别画成一个看不见的空槽
	}

	const FLinearColor W = FLinearColor(1.f, 1.f, 1.f, 0.95f * IconAlpha);

	switch (SkillType)
	{
	case EBlasterSkillType::Heal:
		// 十字
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.5f, 0.f), C + FVector2D(H * 0.5f, 0.f), W, 3.f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(0.f, -H * 0.5f), C + FVector2D(0.f, H * 0.5f), W, 3.f);
		break;

	case EBlasterSkillType::Wall:
		// 三条横线（墙体）
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.5f, -H * 0.4f), C + FVector2D(H * 0.5f, -H * 0.4f), W, 2.5f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.5f, 0.f), C + FVector2D(H * 0.5f, 0.f), W, 2.5f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.5f, H * 0.4f), C + FVector2D(H * 0.5f, H * 0.4f), W, 2.5f);
		break;

	case EBlasterSkillType::Recon:
		// 圆环 + 中心点（眼睛/侦察）
		DrawCircleRing(OutDrawElements, LayerId, Geometry, C, H * 0.45f, W, 2.5f);
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C, H * 0.14f, W);
		break;

	case EBlasterSkillType::Smoke:
		// 云（大圆 + 两个小圆叠出云感）
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C + FVector2D(0.f, H * 0.1f), H * 0.35f, FLinearColor(1.f, 1.f, 1.f, 0.85f));
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.25f, -H * 0.05f), H * 0.22f, FLinearColor(1.f, 1.f, 1.f, 0.85f));
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.25f, -H * 0.05f), H * 0.22f, FLinearColor(1.f, 1.f, 1.f, 0.85f));
		break;

	case EBlasterSkillType::Flash:
		// 星芒（闪光弹）：中心亮点 + 放射状短芒
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C, H * 0.16f, FLinearColor(1.f, 1.f, 1.f, 1.f));
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C, H * 0.38f, FLinearColor(1.f, 1.f, 1.f, 0.35f));
		{
			const int32 Spikes = 8;
			for (int32 i = 0; i < Spikes; ++i)
			{
				const float A = i * 2.f * PI / Spikes;
				const FVector2D Dir(FMath::Cos(A), FMath::Sin(A));
				DrawLine(OutDrawElements, LayerId, Geometry, C + Dir * H * 0.22f, C + Dir * H * 0.55f, W, 3.f);
			}
		}
		break;

	case EBlasterSkillType::Custom:
		// 小方块
		DrawBox(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.3f, -H * 0.3f), FVector2D(H * 0.6f, H * 0.6f), FLinearColor(1.f, 1.f, 1.f, 0.9f));
		break;

	case EBlasterSkillType::Updraft:
		// 腾空：向上的箭头（和 Dash 的右箭头区分开，方向就是它的语义）
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(0.f, H * 0.5f), C + FVector2D(0.f, -H * 0.45f), W, 3.f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.35f, -H * 0.15f), C + FVector2D(0.f, -H * 0.45f), W, 3.f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.35f, -H * 0.15f), C + FVector2D(0.f, -H * 0.45f), W, 3.f);
		break;

	case EBlasterSkillType::Cloudburst:
		// 逐风云：小云 + 一条往右的拖尾（区别于 Smoke 的静止云）
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.15f, 0.f), H * 0.32f, W);
		DrawCircleFilled(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.15f, H * 0.05f), H * 0.22f, W);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.35f, 0.f), C + FVector2D(H * 0.62f, 0.f), W, 2.f);
		break;

	case EBlasterSkillType::Dash:
	default:
		// 顺风冲刺：→ 箭头
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(-H * 0.5f, 0.f), C + FVector2D(H * 0.45f, 0.f), W, 3.f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.15f, -H * 0.35f), C + FVector2D(H * 0.45f, 0.f), W, 3.f);
		DrawLine(OutDrawElements, LayerId, Geometry, C + FVector2D(H * 0.15f, H * 0.35f), C + FVector2D(H * 0.45f, 0.f), W, 3.f);
		break;
	}
}

int32 USkillBarWidget::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	APlayerController* PC = GetOwningPlayer();
	if (!PC || !PC->IsLocalController()) return LayerId;

	ABlasterPlayerController* BPC = Cast<ABlasterPlayerController>(PC);

	// Lobby（选人）地图：技能 UI 不显示（BlasterHUD 另会把整块 Collapsed，这里双保险）
	if (BPC && BPC->IsInLobby()) return LayerId;

	// 要显示技能的角色：
	//   观战（阵亡看队友）→ 被观察的存活队友（读服务器复制的技能快照）；
	//   存活自己         → 直接读本地 ASC（预测即时，维持原行为）；
	//   阵亡但还没进观战 / 全队阵亡无目标 → 不画（隐藏自己的尸块技能条）。
	const bool bSpectating = BPC && BPC->IsSpectating();
	const ABlasterCharacter* DisplayChar = bSpectating
		? BPC->GetSpectateTargetCharacter()
		: (PC->GetPawn() ? Cast<ABlasterCharacter>(PC->GetPawn()) : nullptr);
	if (!DisplayChar || DisplayChar->IsElimmed()) return LayerId;

	// 单槽位的显示数据：本地 ASC 直读 / 观战快照两个来源都先解析成它，绘图共用一份逻辑。
	struct FSkillDrawData
	{
		EBlasterSkillType SkillType = EBlasterSkillType::Custom;
		// 图标贴图（能力 CDO 上配的）。空 = 退回按 SkillType 画的矢量形状。
		// 两个来源都给：本地直读 Ability->SkillIcon，观战读快照里的 Entry.SkillIcon。
		UTexture2D* Icon = nullptr;
		int32 Charges = 0;
		int32 MaxCharges = 0;
		float TimeUntilNextCharge = 0.f;
		float CooldownDuration = 0.f;
		bool bCooldownValid = false;
		bool bArmed = false;
		float ArmedTimeRemaining = 0.f;
		float ArmedWindowDuration = 0.f;
		bool bSageSelecting = false;
		// 手上拿着这一槽对应的投掷物（Phoenix 持闪光 / 持火球）→ 槽框脉冲高亮
		bool bHoldingThrowable = false;
	};

	TArray<FSkillDrawData> Slots;

	if (bSpectating)
	{
		// —— 观战：被观察队友的服务器快照（Charges/冷却/武装 全服务器权威）——
		const float ServerNow = BPC->GetServerTime();
		const bool bRemoteSage = DisplayChar->GetReplicatedSageSelecting();
		const EBlasterThrowableKind RemoteHold = DisplayChar->GetReplicatedThrowableKind();
		for (const FBlasterReplicatedSkill& E : DisplayChar->GetReplicatedSkills())
		{
			if (!E.bValid) continue;
			FSkillDrawData D;
			D.SkillType = E.SkillType;
			D.Icon = E.SkillIcon;
			D.MaxCharges = E.MaxCharges;
			D.Charges = E.Charges;
			D.CooldownDuration = E.CooldownDuration;
			D.bCooldownValid = E.bCooldownValid != 0;
			// 充能恢复剩余：绝对结束服务器时间 - 同步后的服务器时间 → 平滑倒数
			D.TimeUntilNextCharge = (E.NextChargeServerEndTime > 0.f)
				? FMath::Max(0.f, E.NextChargeServerEndTime - ServerNow)
				: 0.f;
			D.bArmed = E.bArmed != 0;
			D.ArmedWindowDuration = E.ArmedWindowDuration;
			D.ArmedTimeRemaining = (E.bArmed && E.ArmedServerEndTime > 0.f)
				? FMath::Max(0.f, E.ArmedServerEndTime - ServerNow)
				: 0.f;
			// Sage 选中态高亮 Heal / Phoenix 持球高亮 Flash（服务器镜像状态）
			D.bSageSelecting = bRemoteSage && E.SkillType == EBlasterSkillType::Heal;
			D.bHoldingThrowable = BlasterHoldMatchesSkill(RemoteHold, E.SkillType);
			Slots.Add(D);
		}
	}
	else
	{
		// —— 存活自己：本地 ASC 直读（预测即时）——
		UAbilitySystemComponent* ASC = DisplayChar->GetAbilitySystemComponent();
		if (!ASC) return LayerId;
		TArray<UBlasterGameplayAbility*> Skills;
		CollectVisibleSkills(ASC, Skills);
		for (UBlasterGameplayAbility* Ability : Skills)
		{
			const FBlasterAbilityCooldownInfo Info = Ability->GetCooldownInfo(ASC);
			FSkillDrawData D;
			D.SkillType = Ability->SkillType;
			D.Icon = Ability->SkillIcon;
			D.MaxCharges = Info.MaxCharges;
			D.Charges = Info.Charges;
			D.TimeUntilNextCharge = Info.TimeUntilNextCharge;
			D.CooldownDuration = Info.CooldownDuration;
			D.bCooldownValid = Info.bValid;
			D.bArmed = Ability->IsArmed();
			D.ArmedTimeRemaining = Ability->GetArmedTimeRemaining();
			D.ArmedWindowDuration = Ability->ArmedWindowDuration;
			D.bSageSelecting = DisplayChar->IsSageHealSelecting() && Ability->SkillType == EBlasterSkillType::Heal;
			D.bHoldingThrowable = BlasterHoldMatchesSkill(DisplayChar->GetHeldThrowableKind(), Ability->SkillType);
			Slots.Add(D);
		}
	}
	// 大招点：从 PlayerState 读。**不走技能快照那条路** —— UltPoints 本来就复制给所有端，
	// 观战看队友时和看自己用的是同一个来源。
	// （大招的**生效状态**也不走技能快照：它挂在角色上复制，见 ABlasterCharacter::ActiveUltimate）
	const ABlasterPlayerState* DisplayPS = DisplayChar->GetPlayerState<ABlasterPlayerState>();
	if (!DisplayPS)
	{
		// 观战看的是远端代理（SimulatedProxy 没有 Controller）→ 它自己的 GetPlayerState() 是空的，
		// 得反查 PlayerArray 里"谁的 Pawn 是这个角色"（和 ScoreboardWidget 同一招）。
		if (const ABlasterGameState* GS = GetWorld() ? GetWorld()->GetGameState<ABlasterGameState>() : nullptr)
		{
			for (APlayerState* Entry : GS->PlayerArray)
			{
				ABlasterPlayerState* BPS = Cast<ABlasterPlayerState>(Entry);
				if (BPS && BPS->GetPawn() == DisplayChar)
				{
					DisplayPS = BPS;
					break;
				}
			}
		}
	}

	const int32 UltPoints = DisplayPS ? DisplayPS->GetUltPoints() : 0;
	const int32 UltRequired = DisplayPS ? DisplayPS->GetUltPointsRequired() : 0;
	const bool bShowUlt = DisplayPS != nullptr && UltRequired > 0;

	// 大招正在生效：这段时间点数**还没扣或刚扣掉**（Phoenix 的扣点在回到标记点那一刻，
	// Jett 的按下即扣），只看 UltPoints 会一直显示"就绪 / X"，等于在骗玩家"你还有一发"。
	// 按角色状态盖掉，改画倒计时。
	// 这里读的是通用状态（ActiveUltimate / UltimateEndTime）—— 两个大招都走同一个槽，
	// 以后新加的大招也自动有倒计时，HUD 不用改。
	const bool bUltActive = DisplayChar->IsUltimateActive();
	float UltRemain = 0.f;
	if (bUltActive && BPC && DisplayChar->UltimateEndTime > 0.f)
	{
		// 绝对服务器结束时间 - 同步后的服务器时间 → 平滑倒数（和上面充能、武装同一个算法）
		UltRemain = FMath::Max(0.f, DisplayChar->UltimateEndTime - BPC->GetServerTime());
	}

	// 一个技能都没有 + 没大招槽 = 没什么可画（观战看技能条为空的队友会走到这）
	if (Slots.Num() == 0 && !bShowUlt) return LayerId;

	const FVector2D Size = AllottedGeometry.GetLocalSize();

	// 布局：底部居中横排，每个技能占 SlotWidth，图标 42x42 + 下方充能点。
	// 大招槽挂在最左、和技能槽之间留 UltGap —— 视觉上和普通技能分开，一眼认出是大招。
	const float SlotWidth = 64.f;
	const float IconSize = 42.f;
	const float IconHalf = IconSize * 0.5f;
	const float UltRadius = IconHalf * 1.3f;
	const float UltGap = 22.f;

	const float SkillsWidth = SlotWidth * Slots.Num();
	const float UltBlockWidth = bShowUlt ? (UltRadius * 2.f + (Slots.Num() > 0 ? UltGap : 0.f)) : 0.f;
	const float StartX = Size.X * 0.5f - (SkillsWidth + UltBlockWidth) * 0.5f;
	const float AnchorY = Size.Y - 150.f;

	// —— 大招槽（Valorant 的 X 位）：外环 + 内圈按攒点进度填充 + 中心显示当前点数 ——
	if (bShowUlt)
	{
		const FVector2D UltCenter(StartX + UltRadius, AnchorY + IconHalf);
		const float Progress = FMath::Clamp((float)UltPoints / (float)UltRequired, 0.f, 1.f);
		// 生效中不算"就绪"：那一发的点数还没扣（扣点在回到标记点），但已经花出去了，不能再显示成可用。
		const bool bReady = UltPoints >= UltRequired && !bUltActive;
		// "点亮" = 用得上的状态（生效中 或 已就绪）→ 环加粗、扇形画满亮度；其中只有真就绪才脉冲
		const bool bLit = bReady || bUltActive;

		// 英雄强调色：攒满后整槽点亮成该英雄的色，"我的大招好了"一眼可见
		const FLinearColor Accent = DisplayPS->HasSelectedAgent()
			? BlasterAgent::GetAccentColor(DisplayPS->GetAgent())
			: FLinearColor(0.55f, 0.55f, 0.55f, 1.f);

		// 底盘
		DrawCircleFilled(OutDrawElements, LayerId, AllottedGeometry, UltCenter, UltRadius,
			FLinearColor(0.02f, 0.03f, 0.06f, 0.92f));

		// 进度：从 12 点方向顺时针扫过的扇形（-PI/2 起）。半径内缩 4px，让外环和填充之间
		// 留一圈暗底 —— 看起来才像"环形进度"，而不是一块实心饼。
		if (Progress > 0.f)
		{
			DrawCircleSector(OutDrawElements, LayerId, AllottedGeometry, UltCenter, UltRadius - 4.f,
				-PI * 0.5f, Progress * 2.f * PI, FLinearColor(Accent.R, Accent.G, Accent.B, bLit ? 0.95f : 0.55f));
		}

		// 外环：三态 —— 生效中（亮色，不脉冲）/ 就绪（亮色 + 脉冲，和技能槽武装态同一套节奏）/ 攒点中（暗环）
		const float Pulse = bReady ? 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 4.f) : 0.f;
		const FLinearColor RingColor = bReady
			? FLinearColor(Accent.R + (1.f - Accent.R) * Pulse, Accent.G + (1.f - Accent.G) * Pulse,
				Accent.B + (1.f - Accent.B) * Pulse, 1.f)
			: (bUltActive
				? FLinearColor(Accent.R, Accent.G, Accent.B, 1.f)
				: FLinearColor(0.85f, 0.95f, 1.f, 0.6f));
		DrawCircleRing(OutDrawElements, LayerId, AllottedGeometry, UltCenter, UltRadius, RingColor,
			bLit ? 3.f : 1.5f);

		// 大招图标：从 DefaultAbilities 里 bIsUltimate 那个能力的 CDO 上读（角色类配置，
		// **不是** ASC 实例数据）→ 观战看队友时也读得到，和技能槽那条快照路径无关。
		UTexture2D* UltIcon = nullptr;
		if (const UBlasterGameplayAbility* UltCDO = DisplayChar->GetUltimateAbilityCDO())
		{
			UltIcon = UltCDO->SkillIcon;
		}

		// 中心文字：生效中显示剩余秒数；攒满显示 X（大招键位）；否则显示当前点数（Valorant 就是这么显示的）。
		// 生效中但没有倒计时（Duration<=0 的大招，比如"刀留到回合结束"）→ 显示 X 而不是 0，
		// 0 会被读成"一点大招点都没有"，X 才表示"这一发已经花出去了、正在生效"。
		const bool bUltCounting = bUltActive && UltRemain > 0.f;
		// 只要中心位置换了图标，就把原来那句 X 省掉 —— 不过攒点数字和倒计时**不省**：
		//「还差几点」和「这发还剩几秒」的信息量都比一张图大，不能被图标顶掉。
		// 所以图标只占住 bLit && !bUltCounting 这个位置（也就是原本画"X"的那个状态）。
		const bool bShowUltIcon = (UltIcon != nullptr) && bLit && !bUltCounting;
		if (bShowUltIcon)
		{
			// 半径压到刚好塞进外环内圈（内圈半径 UltRadius-4），别压到环上
			DrawSkillIcon(OutDrawElements, LayerId, AllottedGeometry, UltCenter,
				(UltRadius - 6.f) * 0.5f, EBlasterSkillType::Custom, UltIcon, /*bDimmed=*/false);
		}
		else
		{
			const FString UltTxt = bUltCounting
				? FString::Printf(TEXT("%d"), FMath::CeilToInt(UltRemain))
				: ((bUltActive || bReady) ? FString(TEXT("X")) : FString::Printf(TEXT("%d"), UltPoints));
			const float UltFontSize = bLit ? 20.f : 18.f;
			// DrawText 收的是左上角，这里按字号粗估字宽做居中（Slate 侧拿不到文本框尺寸，够用就行）
			const float EstWidth = UltTxt.Len() * UltFontSize * 0.58f;
			DrawText(OutDrawElements, LayerId, AllottedGeometry,
				UltCenter + FVector2D(-EstWidth * 0.5f, -UltFontSize * 0.62f), UltTxt, UltFontSize,
				FLinearColor(1.f, 1.f, 1.f, 0.98f));
		}
	}

	// 技能槽从大招块右边开始排（没大招槽时 SkillsStartX == StartX，布局和加这功能之前完全一致）
	const float SkillsStartX = StartX + UltBlockWidth;

	for (int32 i = 0; i < Slots.Num(); ++i)
	{
		const FSkillDrawData& S = Slots[i];
		const FVector2D SlotCenter(SkillsStartX + SlotWidth * (i + 0.5f), AnchorY + IconHalf);
		const FVector2D IconPos(SlotCenter.X - IconHalf, SlotCenter.Y - IconHalf);

		// 武装（逐风）/ Sage 选中 Heal / 手持闪光或火球 任一亮 → 槽框青色脉冲高亮
		const bool bHighlight = S.bArmed || S.bSageSelecting || S.bHoldingThrowable;

		// 1) 图标槽（深底 + 亮边框；武装/选中中边框青色脉冲高亮）
		DrawBox(OutDrawElements, LayerId, AllottedGeometry, IconPos, FVector2D(IconSize, IconSize), FLinearColor(0.02f, 0.03f, 0.06f, 0.88f));
		const float Pulse = bHighlight ? 0.5f + 0.5f * FMath::Sin(GetWorld()->GetTimeSeconds() * 4.f) : 0.f;
		const FLinearColor Border = bHighlight
			? FLinearColor(0.15f + 0.55f * Pulse, 0.85f, 1.f, 0.95f)
			: FLinearColor(0.85f, 0.95f, 1.f, 0.75f);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, IconPos, IconPos + FVector2D(IconSize, 0.f), Border, 1.5f);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, IconPos, IconPos + FVector2D(0.f, IconSize), Border, 1.5f);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, IconPos + FVector2D(IconSize, 0.f), IconPos + FVector2D(IconSize, IconSize), Border, 1.5f);
		DrawLine(OutDrawElements, LayerId, AllottedGeometry, IconPos + FVector2D(0.f, IconSize), IconPos + FVector2D(IconSize, IconSize), Border, 1.5f);

		// 武装窗口进度条：图标槽上方横跨整槽的一条，随窗口剩余时间从右往左缩短（12→0）。
		// 独立于图标槽、配边框，醒目指示「窗口还剩多久」（不画数字，避免和充能倒计时混淆）。
		if (S.bArmed)
		{
			const float WindowFrac = S.ArmedWindowDuration > KINDA_SMALL_NUMBER
				? FMath::Clamp(S.ArmedTimeRemaining / S.ArmedWindowDuration, 0.f, 1.f)
				: 0.f;
			const float BarX = SlotCenter.X - SlotWidth * 0.5f;
			const float BarY = IconPos.Y - 9.f;
			const float BarW = SlotWidth;
			const float BarH = 6.f;
			DrawBox(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX, BarY), FVector2D(BarW, BarH), FLinearColor(0.f, 0.f, 0.f, 0.75f));
			const FLinearColor BarEdge(1.f, 1.f, 1.f, 0.55f);
			DrawLine(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX, BarY), FVector2D(BarX + BarW, BarY), BarEdge, 1.f);
			DrawLine(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX, BarY), FVector2D(BarX, BarY + BarH), BarEdge, 1.f);
			DrawLine(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX + BarW, BarY), FVector2D(BarX + BarW, BarY + BarH), BarEdge, 1.f);
			DrawLine(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX, BarY + BarH), FVector2D(BarX + BarW, BarY + BarH), BarEdge, 1.f);
			if (WindowFrac > 0.01f)
			{
				DrawBox(OutDrawElements, LayerId, AllottedGeometry, FVector2D(BarX, BarY), FVector2D(BarW * WindowFrac, BarH), FLinearColor(0.15f, 0.85f, 1.f, 0.95f));
			}
		}

		// 图标：配了贴图画贴图，没配退回按类型画的矢量形状。
		// 没充能（Charges==0）整张图压暗到 42% —— 和 Valorant 一样"用掉的技能是灰的"，
		// 比只靠下面那几个灰球更容易一眼看出"这个技能现在放不了"。
		DrawSkillIcon(OutDrawElements, LayerId, AllottedGeometry, SlotCenter, IconHalf, S.SkillType, S.Icon, S.Charges <= 0);

		// 2) 充能点（图标下方横排居中）：充能好的青球永远靠最右，消耗从最左边开始。
		const int32 MaxC = FMath::Max(1, S.MaxCharges);
		const float DotRadius = 6.f;
		const float DotSpacing = 18.f;
		const float DotsW = DotSpacing * (MaxC - 1);
		const FLinearColor AvailableColor = FLinearColor(0.30f, 0.85f, 1.f, 1.f);      // 充能好的青球
		const FLinearColor RechargingBaseColor = FLinearColor(0.42f, 0.47f, 0.53f, 1.f); // 正在充能的灰底
		for (int32 c = 0; c < MaxC; ++c)
		{
			const FVector2D DotC(SlotCenter.X - DotsW * 0.5f + DotSpacing * c, SlotCenter.Y + IconHalf + 16.f);
			// 布局（右靠，穷举确认）：
			//   充能好的青球 = 最右连续 c >= MaxC - Charges；
			//   紧挨青堆左边的 = 唯一在冷却的层（c == MaxC - Charges - 1，灰底 + 青色扇形进度）；
			//   更左边 = 已耗待恢复的灰层。
			// 消耗从最左边开始；恢复后回补到右侧 → 好的永远在冷却的右边。
			const bool bAvailable = c >= MaxC - S.Charges;
			const bool bRefilling = c == MaxC - S.Charges - 1 && S.bCooldownValid;

			// 底槽（暗圆 + 细描边）
			DrawCircleFilled(OutDrawElements, LayerId, AllottedGeometry, DotC, DotRadius + 1.5f, FLinearColor(0.06f, 0.08f, 0.12f, 0.9f));
			DrawCircleRing(OutDrawElements, LayerId, AllottedGeometry, DotC, DotRadius + 1.5f, FLinearColor(1.f, 1.f, 1.f, 0.35f), 1.2f);

			if (bAvailable)
			{
				// 充能好的层 = 青色实心球
				DrawCircleFilled(OutDrawElements, LayerId, AllottedGeometry, DotC, DotRadius, AvailableColor);
			}
			else
			{
				// 非可用层统一灰底；正在充能的那层再叠青色扇形（其余灰球等待恢复）
				DrawCircleFilled(OutDrawElements, LayerId, AllottedGeometry, DotC, DotRadius, RechargingBaseColor);
				if (bRefilling)
				{
					const float Progress = S.CooldownDuration > KINDA_SMALL_NUMBER
						? FMath::Clamp(1.f - S.TimeUntilNextCharge / S.CooldownDuration, 0.f, 1.f)
						: 0.f;
					if (Progress > 0.01f)
					{
						DrawCircleSector(OutDrawElements, LayerId, AllottedGeometry, DotC, DotRadius, -PI * 0.5f, Progress * 2.f * PI, AvailableColor);
					}
				}
			}
		}

		// 3) 图标上方倒计时：武装中 → 窗口由上方青条指示（不画数字）；否则 → 白色充能恢复剩余。
		//    两者不同时显示，避免和青条/图标重叠。
		if (!S.bArmed && S.bCooldownValid && S.TimeUntilNextCharge > 0.05f)
		{
			const FString Txt = FString::Printf(TEXT("%.1f"), S.TimeUntilNextCharge);
			DrawText(OutDrawElements, LayerId, AllottedGeometry, SlotCenter + FVector2D(-20.f, -IconHalf - 20.f), Txt, 14.f, FLinearColor(1.f, 1.f, 1.f, 0.95f));
		}
	}

	return LayerId;
}
