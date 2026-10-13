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
enum class MainQueueBarrierResult {
	Completed,
	TimedOut,
};

[[nodiscard]] MainQueueBarrierResult
WaitForMainQueueBarrierForRegressionTest(int timeoutMilliseconds = 5000,
										 bool suppressCompletion = false);

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

enum class TranslationRpcForRegressionTest {
	ChannelAutoTranslationToggle,
	Text,
	Rich,
};

struct TranslationRpcCountsForRegressionTest {
	int channelAutoTranslationToggle = 0;
	int text = 0;
	int rich = 0;
};

void RecordLifecycleWriteForRegressionTest(
	LifecycleWriteForRegressionTest operation);
void ResetLifecycleWriteCountsForRegressionTest();
[[nodiscard]] LifecycleWriteCountsForRegressionTest
GetLifecycleWriteCountsForRegressionTest();
void RecordTranslationRpcForRegressionTest(
	TranslationRpcForRegressionTest operation);
void ResetTranslationRpcCountsForRegressionTest();
[[nodiscard]] TranslationRpcCountsForRegressionTest
GetTranslationRpcCountsForRegressionTest();
#endif

void RunAccountLifecycleRegression(Fn<void(int)> done);

} // namespace Tests
