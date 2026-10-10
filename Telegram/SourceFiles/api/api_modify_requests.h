/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"

#include <QtCore/QString>

namespace Api {

// A setting is saved by a request registered under a key of its own. A newer
// request for the same key takes its place: `cancel` is called with the id of
// the request being replaced, and that request's owner is told through the
// callback it registered. The notification is what makes the replacement
// usable: a cancelled request never reports anything by itself, so an owner
// waiting for its answer would wait forever, and a save that waits for such an
// answer leaves its UI busy with no error to show.
class ModifyRequestRegistry final {
public:
	void registerRequest(
			const QString &key,
			int requestId,
			Fn<void()> onSuperseded,
			const Fn<void(int)> &cancel) {
		const auto i = _entries.find(key);
		if (i != _entries.end()) {
			cancel(i->second.requestId);
			// Taken out of the entry before it is overwritten, so that the
			// notification may register a request under this very key. The new
			// request is in the map before the notification runs.
			const auto onOldSuperseded = std::move(i->second.onSuperseded);
			i->second = Entry{
				.requestId = requestId,
				.onSuperseded = std::move(onSuperseded),
			};
			if (onOldSuperseded) {
				onOldSuperseded();
			}
		} else {
			_entries.emplace(key, Entry{
				.requestId = requestId,
				.onSuperseded = std::move(onSuperseded),
			});
		}
	}

	// The request answered, so its owner no longer waits. Dropping the entry
	// here is what keeps a later request for the same key from being reported
	// as a supersession of an answer that already arrived.
	void clear(const QString &key) {
		_entries.remove(key);
	}

private:
	struct Entry {
		int requestId = 0;
		Fn<void()> onSuperseded;
	};

	base::flat_map<QString, Entry> _entries;

};

} // namespace Api
