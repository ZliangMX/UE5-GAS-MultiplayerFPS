#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "WeaponKillIconSet.generated.h"

class UTexture2D;
class USoundBase;

/**
 * 一把武器的「击杀确认反馈」—— 本回合第 1~6 个击杀各自画哪张图、响哪一声。
 *
 * 图标和音效放在**同一个资产**里，是因为它俩本来就是一套东西：在瓦里两者都跟着武器皮肤走
 *（换了 RGX 皮肤，击杀图标和击杀音效一起换），拆成两份资产只会让人填一半漏一半。
 * 类名里的 Icon 是历史叫法，别被它骗了 —— 下面这 12 个槽是一起的。
 *
 * ── 怎么用 ──────────────────────────────────────────────────────────────
 * 1. 在 Content 里右键 → Miscellaneous → Data Asset → 选 UWeaponKillIconSet，
 *    命名建议 DA_KillIcons_<武器名>；
 * 2. 打开，把 6 张贴图拖进 KillIcon1 ~ KillIcon6、6 段音效拖进 KillSound1 ~ KillSound6；
 * 3. 打开对应的**武器蓝图**，Details 里 "Kill Icons" 分类下把这份资产填进 KillIconSet。
 *
 * ── 留空的语义（重要）─────────────────────────────────────────────────
 * · 单个槽留空 → **只有那一档**回落到默认表现，别档照常走你填的。所以完全可以只填第 3、4、5 档。
 *   ——图标那档回落成 C++ 里的矢量造型（X / 菱形 / 五星…）；
 *   ——音效那档回落成 PC 蓝图里配的 KillConfirmSound / HeadshotKillSound（爆头另有一条），
 *     再没有就用项目自带的拾取提示音。也就是说**没填音效的枪，听起来和现在完全一样**。
 * · 武器蓝图里 KillIconSet 整个是 None → 图标全走矢量造型、音效全走老那套，
 *   也就是**加这套东西之前的表现，一点都不会变**。
 *
 * ── 图标怎么画 ─────────────────────────────────────────────────────────
 * ABlasterHUD::DrawKillMarker 优先用贴图：按 HUD 上的 KillIconSize 当成**正方形**居中缩放着画
 * （不是按贴图原始像素尺寸 —— 想让图标大一点/小一点调 KillIconSize 就行，不用重新导图），
 * 那一档没填才回落到 DrawKillMarkerShape 画矢量造型。
 * 贴图建议做成**正方形、带 alpha**：画的时候整体乘一个透明度做渐隐，
 * 颜色由贴图自己决定（HUD 不染色）。
 *
 * ── 音效怎么放 ─────────────────────────────────────────────────────────
 * ABlasterPlayerController::PlayKillSound 里先问这份资产要第几杀的那一段，
 * 有就播它、没有才走老那套。音效是 **2D 播放**（PlaySound2D），不受距离/朝向影响。
 *
 * ⚠ 和 AWeapon 上的 CrosshairXxx / AimTexture 一样，这是**每把枪一份**的资产。
 *   几把枪想共用同一套（比如两种手枪），让它们的 KillIconSet 指向同一个资产即可，不用复制。
 */
UCLASS(BlueprintType)
class BLASTER_API UWeaponKillIconSet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// ——— 图标：本回合第 1~6 杀各用哪张。留空 = 这一档回落到矢量造型 ———
	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon1;

	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon2;

	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon3;

	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon4;

	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon5;

	// 第 6 杀。瓦里到 5 杀就封顶，留到 6 是给「多杀集锦 / 自定义模式」的余量；
	// 6 杀以上一律沿用这张。
	UPROPERTY(EditAnywhere, Category = "击杀图标")
	TObjectPtr<UTexture2D> KillIcon6;

	// ——— 音效：本回合第 1~6 杀各响哪一声。留空 = 这一档回落到 PC 上的击杀确认音 ———
	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound1;

	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound2;

	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound3;

	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound4;

	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound5;

	// 同 KillIcon6：6 杀以上沿用这一段。
	UPROPERTY(EditAnywhere, Category = "击杀音效")
	TObjectPtr<USoundBase> KillSound6;

	/**
	 * 取第 KillCount 杀的那张图。KillCount 是 **1 起**的（本回合第几个击杀）。
	 * 返回 nullptr = 这一档没填，调用方回落到矢量造型。
	 * KillCount > 6 一律给第 6 张；KillCount <= 0 视为没填。
	 */
	UFUNCTION(BlueprintPure, Category = "击杀图标")
	UTexture2D* GetKillIcon(int32 KillCount) const;

	/**
	 * 取第 KillCount 杀的那段音效，档位语义和 GetKillIcon 完全一样。
	 * 返回 nullptr = 这一档没填，调用方回落到 PC 上的 KillConfirmSound / HeadshotKillSound。
	 */
	UFUNCTION(BlueprintPure, Category = "击杀音效")
	USoundBase* GetKillSound(int32 KillCount) const;
};
