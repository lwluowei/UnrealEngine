// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "CrashReportClientApp.h"

#if !CRASH_REPORT_UNATTENDED_ONLY

#include "SCrashReportClient.h"
#include "CrashReportClientStyle.h"
#include "SlateStyle.h"
#include "SThrobber.h"

#define LOCTEXT_NAMESPACE "CrashReportClient"

static void OnBrowserLinkClicked(const FSlateHyperlinkRun::FMetadata& Metadata, TSharedRef<SWidget> ParentWidget)
{
	const FString* UrlPtr = Metadata.Find(TEXT("href"));
	if(UrlPtr)
	{
		FPlatformProcess::LaunchURL(**UrlPtr, nullptr, nullptr);
	}
}

void SCrashReportClient::Construct(const FArguments& InArgs, TSharedRef<FCrashReportClient> Client)
{
	CrashReportClient = Client;

	auto CrashedAppName = CrashReportClient->GetCrashedAppName();

	// Set the text displaying the name of the crashed app, if available
	FText CrashedAppText = CrashedAppName.IsEmpty() ?
		LOCTEXT("CrashedAppNotFound", "An Unreal process has crashed") :
		LOCTEXT("CrashedApp", "The following process has crashed: ");

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		[
			SNew(SVerticalBox)

			// Stuff anchored to the top
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(3)
			[
				SNew(SHorizontalBox)
				+SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.TextStyle(FCrashReportClientStyle::Get(), "Title")
					.Text(CrashedAppText)
				]

				+SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.TextStyle(FCrashReportClientStyle::Get(), "Title")
					.Text(CrashedAppName)
				]
			]

			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(3)
			[
				SNew( SRichTextBlock )
				.Text( LOCTEXT("CrashDetailed", "We are very sorry that this crash occurred. Our goal is to prevent crashes like this from occurring in the future. Please help us track down and fix this crash by providing detailed information about what you were doing so that we may reproduce the crash and fix it quickly. You can also log a Bug Report with us at <a id=\"browser\" href=\"https://answers.unrealengine.com\" style=\"Hyperlink\">AnswerHub</> and work directly with support staff to report this issue.")
				)
				.AutoWrapText(true)
				.DecoratorStyleSet( &FCoreStyle::Get() )
				+ SRichTextBlock::HyperlinkDecorator( TEXT("browser"), FSlateHyperlinkRun::FOnClick::CreateStatic( &OnBrowserLinkClicked, AsShared() ) )
			]

			+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(3)
				[
					SNew( STextBlock )
					.Text( LOCTEXT("CrashThanks", "Thanks for your help in improving the Unreal Engine.")
					)
					.AutoWrapText(true)
				]

			+SVerticalBox::Slot()
				.AutoHeight()
				.Padding(3)
				[
					SNew( STextBlock )
					.Text( LOCTEXT("CrashPRovide", "Please provide detailed information about what you were doing when the crash occurred.")
					)
					.AutoWrapText(true)
				]

			+SVerticalBox::Slot()
			.FillHeight(0.3f)
			.Padding(3)
			[
				SNew(SMultiLineEditableTextBox)
				.OnTextCommitted(CrashReportClient.ToSharedRef(), &FCrashReportClient::UserCommentChanged)
				.Font( FSlateFontInfo( FPaths::EngineContentDir() / TEXT( "Slate/Fonts/Roboto-Regular.ttf" ), 9 ) )
				.AutoWrapText( true )
				.BackgroundColor(FSlateColor(FLinearColor::Black))
				.ForegroundColor(FSlateColor(FLinearColor::White))
				.HintText(LOCTEXT("CircumstancesOfCrash", "Circumstances of crash"))
			]
			
			+SVerticalBox::Slot()
			.FillHeight(0.7f)
			.Padding(3)
			[
				SNew(SOverlay)

				+SOverlay::Slot()
				[
					SNew(SBorder)
					.BorderImage(new FSlateBoxBrush(TEXT("Common/TextBlockHighlightShape"), FMargin(.5f)))
					.BorderBackgroundColor(FSlateColor(FLinearColor::Black))
					.IsEnabled(CrashReportClient.ToSharedRef(), &FCrashReportClient::AreCallstackWidgetsEnabled)
					[
						SNew(SScrollBox)
						+SScrollBox::Slot()
						[
							SNew(STextBlock)
							.TextStyle(FCrashReportClientStyle::Get(), "Code")
							.Text(Client, &FCrashReportClient::GetDiagnosticText)
						]
					]
				]

				+SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SThrobber)
					.Visibility(CrashReportClient.ToSharedRef(), &FCrashReportClient::IsThrobberVisible)
					.NumPieces(5)
				]
			]

			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding(3)
			[
				SNew(SHorizontalBox)

				+SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(ECheckBoxState::Checked)
					.OnCheckStateChanged(CrashReportClient.ToSharedRef(), &FCrashReportClient::SCrashReportClient_OnCheckStateChanged)
				]

				+SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("IAgree", "I agree to be contacted by Epic Games via email if additional information about this crash would help fix it."))
				]
			]

			// Stuff anchored to the bottom
			+SVerticalBox::Slot()
			.AutoHeight()
			.Padding( FMargin(3, 3+16, 3, 3) )
			[
				SNew(SHorizontalBox)

				+SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.HAlign(HAlign_Left)
				.VAlign(VAlign_Center)
				.Padding(0)
				[			
					SNew(SSpacer)
				]

				+SHorizontalBox::Slot()
				.AutoWidth()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding( FMargin(0,0,8,0) )
				[
					SNew(SButton)
					.ContentPadding( FMargin(8,2) )
					.Text(LOCTEXT("CopyCallstack", "Copy to Clipboard"))
					.OnClicked(CrashReportClient.ToSharedRef(), &FCrashReportClient::CopyCallstack)
					.IsEnabled(CrashReportClient.ToSharedRef(), &FCrashReportClient::AreCallstackWidgetsEnabled)
				]

				+SHorizontalBox::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.AutoWidth()
				.Padding( FMargin(0) )
				[
					SNew(SButton)
					.ContentPadding( FMargin(8,2) )
					.Text(LOCTEXT("Send", "Send"))
					.OnClicked(Client, &FCrashReportClient::Submit)
				]
			]
		]
	];

	FSlateApplication::Get().SetUnhandledKeyDownEventHandler(FOnKeyEvent::CreateSP(this, &SCrashReportClient::OnUnhandledKeyDown));
}

FReply SCrashReportClient::OnUnhandledKeyDown(const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Enter)
	{
		CrashReportClient->Submit();
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE

#endif // !CRASH_REPORT_UNATTENDED_ONLY
