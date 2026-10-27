/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "data/data_msg_id.h"
#include "storage/upload_failure.h"
#include "storage/upload_limits.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct UploadFailurePhoto {
	std::unique_ptr<int> uploadingData = std::make_unique<int>(1);
};

struct UploadFailureDocument {
	std::unique_ptr<int> uploadingData = std::make_unique<int>(1);
};

struct UploadFailureMedia {
	UploadFailurePhoto *photo() const { return photoData; }
	UploadFailureDocument *document() const { return documentData; }

	UploadFailurePhoto *photoData = nullptr;
	UploadFailureDocument *documentData = nullptr;
};

struct UploadFailureMessage {
	bool isSending() const { return sending; }
	void sendFailed() {
		sending = false;
		failed = true;
	}
	UploadFailureMedia *media() const { return mediaData; }
	bool isEditingMedia() const { return editingMedia; }
	void returnSavedMedia() {
		if (editingMedia) {
			++returnSavedMediaCalls;
			const auto destroyReplacement = onReplacementDestroyed;
			onReplacementDestroyed = {};
			mediaData = savedMediaData;
			if (destroyReplacement) {
				destroyReplacement();
			}
			caption = savedCaption;
			editingMedia = false;
		}
	}

	FullMsgId itemId;
	UploadFailureMedia *mediaData = nullptr;
	UploadFailureMedia *savedMediaData = nullptr;
	std::string caption;
	std::string savedCaption;
	std::function<void()> onReplacementDestroyed;
	bool sending = true;
	bool failed = false;
	bool editingMedia = false;
	int returnSavedMediaCalls = 0;
};

struct UploadRequest {
	FullMsgId itemId;
};

struct UploadFailureAlbumItem {
	FullMsgId itemId;
	int64 size = 0;
};

} // namespace

TEST_CASE(ServerUploadLimitRejectsLargerFiles) {
	CHECK(Storage::details::ExceedsUploadMaxBytes(500'001, 500'000));
}

TEST_CASE(ServerUploadLimitAllowsSmallerFiles) {
	CHECK(!Storage::details::ExceedsUploadMaxBytes(499'999, 500'000));
}

TEST_CASE(ServerUploadLimitAllowsFilesWhenConfigIsAbsent) {
	CHECK(!Storage::details::ExceedsUploadMaxBytes(900'000'000, std::nullopt));
}

TEST_CASE(FilePartTooBigErrorIsRecognized) {
	CHECK(Storage::details::IsFilePartTooBigError(u"FILE_PART_TOO_BIG"_q));
}

TEST_CASE(FilePartTooBigFailureCancelsPartsAndFailsLocalMessage) {
	const auto itemId = FullMsgId(PeerId(1), 1);
	const auto otherId = FullMsgId(PeerId(1), 2);
	auto photo = UploadFailurePhoto();
	auto media = UploadFailureMedia{.photoData = &photo};
	auto message = UploadFailureMessage{.mediaData = &media};
	auto requests = std::map<int, UploadRequest>{
		{1, {itemId}},
		{2, {otherId}},
		{3, {itemId}},
	};
	auto pending = std::vector<UploadRequest>{
		{itemId},
		{otherId},
	};
	auto cancelled = std::vector<int>();
	auto sizeLimitShown = false;
	auto shownSize = int64(0);

	Storage::details::HandleUploadPartFailure(
		u"FILE_PART_TOO_BIG"_q, 500'001,
		[&] {
			Storage::details::CancelUploadRequests(
				itemId, requests, pending,
				[&](int requestId, const UploadRequest &) {
					cancelled.push_back(requestId);
				});
			Storage::details::FailLocalMessageUpload(&message);
		},
		[&](int64 size) {
			sizeLimitShown = true;
			shownSize = size;
		});

	CHECK(requests.size() == 1);
	CHECK_EQ(requests.begin()->first, 2);
	CHECK(pending.size() == 1);
	CHECK(pending.front().itemId == otherId);
	CHECK(cancelled.size() == 2);
	CHECK_EQ(cancelled[0], 1);
	CHECK_EQ(cancelled[1], 3);
	CHECK(!message.sending);
	CHECK(message.failed);
	CHECK(!photo.uploadingData);
	CHECK(sizeLimitShown);
	CHECK_EQ(shownSize, int64(500'001));
}

TEST_CASE(OversizedAlbumMemberIsRemovedBeforeFailureMark) {
	const auto rejectedId = FullMsgId(PeerId(1), 1);
	const auto validId = FullMsgId(PeerId(1), 2);
	auto message = UploadFailureMessage{.itemId = rejectedId};
	auto album = std::vector<UploadFailureAlbumItem>{
		{rejectedId, 500'001},
		{validId, 250'000},
	};
	auto cancellationBeforeFailure = false;

	Storage::details::HandlePreflightUploadFailure(
		500'001, [](int64) {},
		[&] {
			Storage::details::FailLocalMessageUpload(
				&message, true, [&](UploadFailureMessage *item) {
					cancellationBeforeFailure = item->sending;
					const auto rejected = std::find_if(
						album.begin(), album.end(), [&](const auto &entry) {
							return (entry.itemId == item->itemId);
						});
					if (rejected != album.end()) {
						album.erase(rejected);
					}
				});
		});

	CHECK(cancellationBeforeFailure);
	CHECK(!message.sending);
	CHECK(message.failed);
	CHECK(album.size() == 1);
	CHECK(album.front().itemId == validId);
	CHECK_EQ(album.front().size, int64(250'000));
}

TEST_CASE(PreflightUploadLimitFailsLocalDocumentMessage) {
	auto document = UploadFailureDocument();
	auto media = UploadFailureMedia{.documentData = &document};
	auto message = UploadFailureMessage{.mediaData = &media};
	auto sizeLimitShown = false;
	auto failureAfterLimitShown = false;

	Storage::details::HandlePreflightUploadFailure(
		500'001, [&](int64 size) { sizeLimitShown = (size == 500'001); },
		[&] {
			failureAfterLimitShown = sizeLimitShown;
			Storage::details::FailLocalMessageUpload(&message);
		});

	CHECK(!message.sending);
	CHECK(message.failed);
	CHECK(!document.uploadingData);
	CHECK(sizeLimitShown);
	CHECK(failureAfterLimitShown);
}

TEST_CASE(RejectedPhotoReplacementRestoresSavedMediaAndCaption) {
	auto savedPhoto = UploadFailurePhoto();
	auto savedMedia = UploadFailureMedia{.photoData = &savedPhoto};
	auto replacementPhoto = UploadFailurePhoto();
	auto replacementMedia = UploadFailureMedia{.photoData = &replacementPhoto};
	auto message = UploadFailureMessage{
		.mediaData = &replacementMedia,
		.savedMediaData = &savedMedia,
		.caption = "replacement caption",
		.savedCaption = "original caption",
		.sending = false,
		.editingMedia = true,
	};

	Storage::details::HandlePreflightUploadFailure(
		500'001, [](int64) {},
		[&] { Storage::details::FailLocalMessageUpload(&message); });

	CHECK(!replacementPhoto.uploadingData);
	CHECK(message.mediaData == &savedMedia);
	CHECK_EQ(message.caption, std::string("original caption"));
	CHECK(!message.editingMedia);
}

TEST_CASE(RejectedDocumentReplacementRestoresSavedMediaAndCaption) {
	auto savedDocument = UploadFailureDocument();
	auto savedMedia = UploadFailureMedia{.documentData = &savedDocument};
	auto replacementDocument = UploadFailureDocument();
	auto replacementMedia = UploadFailureMedia{
		.documentData = &replacementDocument,
	};
	auto message = UploadFailureMessage{
		.mediaData = &replacementMedia,
		.savedMediaData = &savedMedia,
		.caption = "replacement caption",
		.savedCaption = "original caption",
		.sending = false,
		.editingMedia = true,
	};

	Storage::details::HandleUploadPartFailure(
		u"FILE_PART_TOO_BIG"_q, 500'001,
		[&] { Storage::details::FailLocalMessageUpload(&message); },
		[](int64) {});

	CHECK(!replacementDocument.uploadingData);
	CHECK(message.mediaData == &savedMedia);
	CHECK_EQ(message.caption, std::string("original caption"));
	CHECK(!message.editingMedia);
}

TEST_CASE(CancellingReplacementDuringRestoreDoesNotReenterSavedMedia) {
	auto savedPhoto = UploadFailurePhoto();
	savedPhoto.uploadingData.reset();
	auto savedMedia = UploadFailureMedia{.photoData = &savedPhoto};
	auto replacementPhoto = UploadFailurePhoto();
	auto replacementMedia = UploadFailureMedia{.photoData = &replacementPhoto};
	auto message = UploadFailureMessage{
		.mediaData = &replacementMedia,
		.savedMediaData = &savedMedia,
		.caption = "replacement caption",
		.savedCaption = "original caption",
		.sending = false,
		.editingMedia = true,
	};
	message.onReplacementDestroyed = [&] {
		replacementPhoto.uploadingData.reset();
		Storage::details::FailLocalMessageUpload(&message, false);
	};

	message.returnSavedMedia();

	CHECK_EQ(message.returnSavedMediaCalls, 1);
	CHECK(message.mediaData == &savedMedia);
	CHECK_EQ(message.caption, std::string("original caption"));
	CHECK(!message.editingMedia);
	CHECK(!replacementPhoto.uploadingData);
	CHECK(!message.failed);
}

TEST_CASE(CancelledLocalUploadClearsDataWithoutFailingMessage) {
	auto document = UploadFailureDocument();
	auto media = UploadFailureMedia{.documentData = &document};
	auto message = UploadFailureMessage{.mediaData = &media};

	Storage::details::FailLocalMessageUpload(&message, false);

	CHECK(message.sending);
	CHECK(!message.failed);
	CHECK(!document.uploadingData);
}
