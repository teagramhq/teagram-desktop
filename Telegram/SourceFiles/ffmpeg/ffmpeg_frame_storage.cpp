/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ffmpeg/ffmpeg_frame_storage.h"

#include <QImage>

#include <algorithm>
#include <cstdint>
#include <new>

namespace FFmpeg {
namespace {

constexpr auto kImageFormat = QImage::Format_ARGB32_Premultiplied;
constexpr auto kMaxFrameStorageBytes = 64 * 1024 * 1024;

struct AlignedFrameStorageLayout {
	int width = 0;
	int height = 0;
	int bytesPerLine = 0;
	int totalBytes = 0;
};

void AlignedImageBufferCleanupHandler(void *data) {
	const auto buffer = static_cast<uchar *>(data);
	delete[] buffer;
}

[[nodiscard]] bool
ComputeAlignedFrameStorageLayout(QSize size, int minBytesPerLine,
								 AlignedFrameStorageLayout *out) {
	const auto width = size.width();
	const auto height = size.height();
	if (width <= 0 || height <= 0 || minBytesPerLine < 0) {
		return false;
	}
	const auto requested
		= std::max(int64_t(width) * kPixelBytesSize, int64_t(minBytesPerLine));
	const auto remainder = requested % kAlignImageBy;
	const auto bytesPerLine
		= remainder ? (requested + kAlignImageBy - remainder) : requested;
	if (bytesPerLine > kMaxFrameStorageBytes) {
		return false;
	}
	const auto totalBytes = bytesPerLine * height + kAlignImageBy;
	if (totalBytes > kMaxFrameStorageBytes) {
		return false;
	}
	out->width = width;
	out->height = height;
	out->bytesPerLine = int(bytesPerLine);
	out->totalBytes = int(totalBytes);
	return true;
}

[[nodiscard]] bool IsAlignedImage(const QImage &image) {
	return !(reinterpret_cast<uintptr_t>(image.bits()) % kAlignImageBy)
		   && !(image.bytesPerLine() % kAlignImageBy);
}

} // namespace

bool GoodStorageForFrame(const QImage &storage, QSize size,
						 int minBytesPerLine) {
	if (storage.isNull() || (storage.format() != kImageFormat)
		|| (storage.size() != size) || !storage.isDetached()
		|| !IsAlignedImage(storage)) {
		return false;
	} else if (minBytesPerLine <= 0) {
		return true;
	}
	const auto bytesPerLine = storage.bytesPerLine();
	if (bytesPerLine < minBytesPerLine
		|| bytesPerLine > kMaxFrameStorageBytes) {
		return false;
	}
	auto layout = AlignedFrameStorageLayout();
	return ComputeAlignedFrameStorageLayout(size, int(bytesPerLine), &layout);
}

// Create a QImage of desired size where all the data is properly aligned.
QImage CreateFrameStorage(QSize size, int minBytesPerLine) {
	auto layout = AlignedFrameStorageLayout();
	if (!ComputeAlignedFrameStorageLayout(size, minBytesPerLine, &layout)) {
		return {};
	}
	const auto buffer = new (std::nothrow) uchar[layout.totalBytes];
	if (!buffer) {
		return {};
	}
	const auto address = reinterpret_cast<uintptr_t>(buffer);
	const auto alignedBuffer
		= buffer
		  + ((address % kAlignImageBy)
				 ? (kAlignImageBy - (address % kAlignImageBy))
				 : 0);
	return QImage(alignedBuffer, layout.width, layout.height,
				  layout.bytesPerLine, kImageFormat,
				  AlignedImageBufferCleanupHandler, buffer);
}

} // namespace FFmpeg
