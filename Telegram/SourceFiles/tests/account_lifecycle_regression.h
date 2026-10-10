/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Tests {

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
enum class LifecycleWriteForRegressionTest {
	AuthorizationSnapshot,
	AuthorizationFailureMarker,
	CustomServerBlockMarker,
};

struct LifecycleWriteCountsForRegressionTest {
	int authorizationSnapshot = 0;
	int authorizationFailureMarker = 0;
	int customServerBlockMarker = 0;
};

enum class AiComposeApplyRegressionEvent {
	HistoryWidgetDraftRead,
	ComposeControlsDraftRead,
	ApplyDispatched,
};

struct AiComposeApplyRegressionCounts {
	int historyWidgetDraftReads = 0;
	int composeControlsDraftReads = 0;
	int applyDispatches = 0;
};

void RecordLifecycleWriteForRegressionTest(
	LifecycleWriteForRegressionTest operation);
void ResetLifecycleWriteCountsForRegressionTest();
[[nodiscard]] LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest();
void RecordAiComposeApplyRegressionEvent(
	AiComposeApplyRegressionEvent event);
void ResetAiComposeApplyRegressionCounts();
[[nodiscard]] AiComposeApplyRegressionCounts
GetAiComposeApplyRegressionCounts();
#endif

void RunAccountLifecycleRegression(Fn<void(int)> done);

} // namespace Tests
