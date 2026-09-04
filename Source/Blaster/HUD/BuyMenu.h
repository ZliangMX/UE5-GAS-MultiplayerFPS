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

	UPROPERTY(meta = (BindWidgetOptional))
	UButton* CloseButton;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UFUNCTION()
	void OnRifleClicked();

	UFUNCTION()
	void OnPistolClicked();

	UFUNCTION()
	void OnSniperClicked();

	UFUNCTION()
	void OnShotgunClicked();

	UFUNCTION()
	void OnCloseClicked();

	void RequestBuy(TSubclassOf<AWeapon> WeaponClass);
	void RefreshCredits();
	void RefreshCostText(UTextBlock* Text, TSubclassOf<AWeapon> WeaponClass);
};
