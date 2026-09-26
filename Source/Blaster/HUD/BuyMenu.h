#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuyMenu.generated.h"

class UButton;
class UTextBlock;
class AWeapon;

// 购买菜单（经济系统）：BuyPhase 时按 B 打开，点按钮购买武器
UCLASS()
class BLASTER_API UBuyMenu : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* CreditsText;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* RifleButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* RifleCostText;

	UPROPERTY(EditAnywhere, Category = "Buy")
	TSubclassOf<AWeapon> RifleClass;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* PistolButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* PistolCostText;

	UPROPERTY(EditAnywhere, Category = "Buy")
	TSubclassOf<AWeapon> PistolClass;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* SniperButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* SniperCostText;

	UPROPERTY(EditAnywhere, Category = "Buy")
	TSubclassOf<AWeapon> SniperClass;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* ShotgunButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* ShotgunCostText;

	UPROPERTY(EditAnywhere, Category = "Buy")
	TSubclassOf<AWeapon> ShotgunClass;

	// --- 护甲两档（可选控件：WBP_BuyMenu 里没放就不显示，不影响武器那几个按钮）---
	// 加两个 UButton（LightArmorButton / HeavyArmorButton）和两个 UTextBlock
	// （LightArmorCostText / HeavyArmorCostText）即可，名字要和这里完全一致。
	UPROPERTY(meta = (BindWidgetOptional))
	UButton* LightArmorButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* LightArmorCostText;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* HeavyArmorButton;

	UPROPERTY(meta = (BindWidgetOptional))
	UTextBlock* HeavyArmorCostText;

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* CloseButton;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UFUNCTION()
	void OnRifleClicked();

	UFUNCTION()
	void OnLightArmorClicked();

	UFUNCTION()
	void OnHeavyArmorClicked();

	UFUNCTION()
	void OnPistolClicked();

	UFUNCTION()
	void OnSniperClicked();

	UFUNCTION()
	void OnShotgunClicked();

	UFUNCTION()
	void OnCloseClicked();

	void RequestBuy(TSubclassOf<AWeapon> WeaponClass);
	// 护甲走另一条（不是武器，没有 AWeapon 类可传）：只报「买哪一档」+ 显示用价格
	void RequestBuyArmor(int32 ArmorAmount);
	void RefreshArmorCostText(UTextBlock* Text, int32 Cost);
	void RefreshCredits();
	void RefreshCostText(UTextBlock* Text, TSubclassOf<AWeapon> WeaponClass);
};
