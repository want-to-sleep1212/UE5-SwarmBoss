#include "SwarmCrosshairHUD.h"

#include "Engine/Canvas.h"

void ASwarmCrosshairHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	const float CenterX = Canvas->ClipX * 0.5f;
	const float CenterY = Canvas->ClipY * 0.5f;
	constexpr float HalfLength = 6.0f;

	// 밝거나 어두운 배경에서도 보이도록 검은 외곽선 위에 흰색 십자를 겹친다.
	DrawLine(CenterX - HalfLength, CenterY, CenterX + HalfLength, CenterY, FLinearColor::Black, 4.0f);
	DrawLine(CenterX, CenterY - HalfLength, CenterX, CenterY + HalfLength, FLinearColor::Black, 4.0f);

	DrawLine(CenterX - HalfLength, CenterY, CenterX + HalfLength, CenterY, FLinearColor::White, 2.0f);
	DrawLine(CenterX, CenterY - HalfLength, CenterX, CenterY + HalfLength, FLinearColor::White, 2.0f);
}
