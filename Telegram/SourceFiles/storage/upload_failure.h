/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"

#include <QtCore/QString>

#include <utility>

namespace Storage::details {

[[nodiscard]] inline bool IsFilePartTooBigError(const QString &type) {
	return (type == u"FILE_PART_TOO_BIG"_q);
}

template <typename Item, typename BeforeFailure>
void FailLocalMessageUpload(Item *item, bool markMessageFailed,
							BeforeFailure &&beforeFailure) {
	if (!item) {
		return;
	}
	if (const auto media = item->media()) {
		if (const auto photo = media->photo()) {
			photo->uploadingData = nullptr;
		}
		if (const auto document = media->document()) {
			document->uploadingData = nullptr;
		}
	}
	if (markMessageFailed && item->isEditingMedia()) {
		item->returnSavedMedia();
	}
	if (markMessageFailed && item->isSending()) {
		std::forward<BeforeFailure>(beforeFailure)(item);
		item->sendFailed();
	}
}

template <typename Item>
void FailLocalMessageUpload(Item *item, bool markMessageFailed = true) {
	FailLocalMessageUpload(item, markMessageFailed, [](Item *) {});
}

template <typename Failed, typename ShowLimit>
void HandleUploadFailure(bool showSizeLimit, int64 fileSize, Failed &&failed,
						 ShowLimit &&showLimit) {
	std::forward<Failed>(failed)();
	if (showSizeLimit) {
		std::forward<ShowLimit>(showLimit)(fileSize);
	}
}

template <typename ShowLimit, typename Failed>
void HandlePreflightUploadFailure(int64 fileSize, ShowLimit &&showLimit,
								  Failed &&failed) {
	std::forward<ShowLimit>(showLimit)(fileSize);
	std::forward<Failed>(failed)();
}

template <typename Failed, typename ShowLimit>
void HandleUploadPartFailure(const QString &errorType, int64 fileSize,
							 Failed &&failed, ShowLimit &&showLimit) {
	HandleUploadFailure(IsFilePartTooBigError(errorType), fileSize,
						std::forward<Failed>(failed),
						std::forward<ShowLimit>(showLimit));
}

template <typename Requests, typename Pending, typename Cancel>
void CancelUploadRequests(FullMsgId itemId, Requests &requests,
						  Pending &pending, Cancel &&cancel) {
	for (auto i = requests.begin(); i != requests.end();) {
		if (i->second.itemId == itemId) {
			cancel(i->first, i->second);
			i = requests.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = pending.begin(); i != pending.end();) {
		if (i->itemId == itemId) {
			i = pending.erase(i);
		} else {
			++i;
		}
	}
}

} // namespace Storage::details
