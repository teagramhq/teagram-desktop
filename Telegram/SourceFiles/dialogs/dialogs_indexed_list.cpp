/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "dialogs/dialogs_indexed_list.h"

#include "main/main_session.h"
#include "data/data_session.h"
#include "history/history.h"

namespace Dialogs {
namespace {

constexpr auto kSearchPrefixLength = 8;

} // namespace

IndexedList::IndexedList(
		SortMode sortMode,
		FilterId filterId,
		bool usePrefixSearch)
: _sortMode(sortMode)
, _filterId(filterId)
, _usePrefixSearch(usePrefixSearch)
, _list(sortMode, filterId)
, _empty(sortMode, filterId) {
}

void IndexedList::addNameRow(QChar ch, Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);

	auto i = _nameIndex.find(ch);
	if (i == _nameIndex.end()) {
		i = _nameIndex.emplace(ch, std::vector<not_null<Row*>>()).first;
	}
	auto &rows = i->second;
	if (rows.empty() || rows.back()->index() < row->index()) {
		rows.emplace_back(row);
	} else {
		const auto position = std::lower_bound(
			rows.begin(),
			rows.end(),
			row->index(),
			[](const not_null<Row*> &entry, int index) {
				return entry->index() < index;
			});
		rows.emplace(position, row);
	}
}

void IndexedList::removeNameRow(QChar ch, Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);

	const auto i = _nameIndex.find(ch);
	if (i == _nameIndex.end()) {
		return;
	}
	auto &rows = i->second;
	const auto found = std::find_if(
		rows.begin(),
		rows.end(),
		[row](const not_null<Row*> &entry) { return entry.get() == row; });
	if (found != rows.end()) {
		rows.erase(found);
	}
}

void IndexedList::updatePrefixSearchIndex(Row *row) {
	if (!_usePrefixSearch || !row) {
		return;
	}
	removePrefixSearchIndex(row);
	auto keys = base::flat_set<QString>();
	for (const auto &word : row->entry()->chatListNameWords()) {
		if (word.size() >= kSearchPrefixLength) {
			keys.emplace(word.left(kSearchPrefixLength));
		}
	}
	if (keys.empty()) {
		return;
	}
	auto stored = std::vector<QString>();
	stored.reserve(keys.size());
	for (const auto &key : keys) {
		_prefixSearchIndex[key].insert(row);
		stored.push_back(key);
	}
	_searchKeysByRow.emplace(row, std::move(stored));
}

void IndexedList::removePrefixSearchIndex(Row *row) {
	if (!_usePrefixSearch || !row) {
		return;
	}
	const auto i = _searchKeysByRow.find(row);
	if (i == _searchKeysByRow.end()) {
		return;
	}
	for (const auto &key : i->second) {
		const auto j = _prefixSearchIndex.find(key);
		if (j == _prefixSearchIndex.end()) {
			continue;
		}
		j->second.erase(row);
		if (j->second.empty()) {
			_prefixSearchIndex.erase(j);
		}
	}
	_searchKeysByRow.erase(i);
}

RowsByLetter IndexedList::addToEnd(Key key) {
	if (const auto row = _list.getRow(key)) {
		return { row };
	}

	auto result = RowsByLetter{ _list.addToEnd(key) };
	for (const auto &ch : key.entry()->chatListFirstLetters()) {
		if (_sortMode == SortMode::Name) {
			addNameRow(ch, result.main.get());
			result.letters.emplace(ch, result.main);
		} else {
			auto j = _index.find(ch);
			if (j == _index.cend()) {
				j = _index.emplace(ch, _sortMode, _filterId).first;
			}
			result.letters.emplace(ch, j->second.addToEnd(key));
		}
	}
	updatePrefixSearchIndex(result.main.get());
	return result;
}

Row *IndexedList::addByName(Key key) {
	if (const auto row = _list.getRow(key)) {
		return row;
	}

	const auto result = _list.addByName(key);
	for (const auto &ch : key.entry()->chatListFirstLetters()) {
		if (_sortMode == SortMode::Name) {
			addNameRow(ch, result.get());
		} else {
			auto j = _index.find(ch);
			if (j == _index.cend()) {
				j = _index.emplace(ch, _sortMode, _filterId).first;
			}
			j->second.addByName(key);
		}
	}
	updatePrefixSearchIndex(result.get());
	return result;
}

void IndexedList::adjustByDate(const RowsByLetter &links) {
	_list.adjustByDate(links.main);
	for (const auto &[ch, row] : links.letters) {
		if (auto it = _index.find(ch); it != _index.cend()) {
			it->second.adjustByDate(row);
		}
	}
}

bool IndexedList::updateHeights(float64 narrowRatio) {
	return _list.updateHeights(narrowRatio);
}

bool IndexedList::updateHeight(Key key, float64 narrowRatio) {
	return _list.updateHeight(key, narrowRatio);
}

void IndexedList::moveToTop(Key key) {
	if (_list.moveToTop(key)) {
		const auto row = _list.getRow(key);
		for (const auto &ch : key.entry()->chatListFirstLetters()) {
			if (_sortMode == SortMode::Name) {
				removeNameRow(ch, row);
				addNameRow(ch, row);
			} else if (auto it = _index.find(ch); it != _index.cend()) {
				it->second.moveToTop(key);
			}
		}
	}
}

void IndexedList::movePinned(Row *row, int deltaSign) {
	auto swapPinnedIndexWith = find(row);
	Assert(swapPinnedIndexWith != cend());
	if (deltaSign > 0) {
		++swapPinnedIndexWith;
	} else {
		Assert(swapPinnedIndexWith != cbegin());
		--swapPinnedIndexWith;
	}
	row->key().entry()->owner().reorderTwoPinnedChats(
		_filterId,
		row->key(),
		(*swapPinnedIndexWith)->key());
}

void IndexedList::peerNameChanged(
		not_null<PeerData*> peer,
		const base::flat_set<QChar> &oldLetters) {
	Expects(_sortMode != SortMode::Date);

	if (const auto history = peer->owner().historyLoaded(peer)) {
		if (_sortMode == SortMode::Name) {
			adjustByName(history, oldLetters);
		} else {
			adjustNames(FilterId(), history, oldLetters);
		}
	}
}

void IndexedList::peerNameChanged(
		FilterId filterId,
		not_null<PeerData*> peer,
		const base::flat_set<QChar> &oldLetters) {
	Expects(_sortMode == SortMode::Date);

	if (const auto history = peer->owner().historyLoaded(peer)) {
		adjustNames(filterId, history, oldLetters);
	}
}

void IndexedList::adjustByName(
		Key key,
		const base::flat_set<QChar> &oldLetters) {
	Expects(_sortMode == SortMode::Name);

	const auto mainRow = _list.adjustByName(key);
	if (!mainRow) return;

	auto toRemove = oldLetters;
	for (const auto &ch : key.entry()->chatListFirstLetters()) {
		auto j = toRemove.find(ch);
		if (j == toRemove.cend()) {
			addNameRow(ch, mainRow);
		} else {
			toRemove.erase(j);
			removeNameRow(ch, mainRow);
			addNameRow(ch, mainRow);
		}
	}
	for (auto ch : toRemove) {
		removeNameRow(ch, mainRow);
	}
	updatePrefixSearchIndex(mainRow);
}

void IndexedList::adjustNames(
		FilterId filterId,
		not_null<History*> history,
		const base::flat_set<QChar> &oldLetters) {
	const auto key = Dialogs::Key(history);
	auto mainRow = _list.getRow(key);
	if (!mainRow) return;

	auto toRemove = oldLetters;
	auto toAdd = base::flat_set<QChar>();
	for (const auto &ch : key.entry()->chatListFirstLetters()) {
		auto j = toRemove.find(ch);
		if (j == toRemove.cend()) {
			toAdd.insert(ch);
		} else {
			toRemove.erase(j);
		}
	}
	for (auto ch : toRemove) {
		if (_sortMode == SortMode::Date) {
			history->removeChatListEntryByLetter(filterId, ch);
		}
		if (auto it = _index.find(ch); it != _index.cend()) {
			it->second.remove(key, mainRow);
		}
	}
	for (auto ch : toAdd) {
		auto j = _index.find(ch);
		if (j == _index.cend()) {
			j = _index.emplace(ch, _sortMode, _filterId).first;
		}
		auto row = j->second.addToEnd(key);
		if (_sortMode == SortMode::Date) {
			history->addChatListEntryByLetter(filterId, ch, row);
		}
	}
}

void IndexedList::freeze() {
	_list.freeze();
	for (auto &[ch, list] : _index) {
		list.freeze();
	}
}

void IndexedList::unfreeze() {
	_list.unfreeze();
	for (auto &[ch, list] : _index) {
		list.unfreeze();
	}
}

void IndexedList::remove(Key key, Row *replacedBy) {
	if (_sortMode == SortMode::Name) {
		const auto row = _list.getRow(key);
		if (!row) {
			return;
		}
		removePrefixSearchIndex(row);
		for (const auto &ch : key.entry()->chatListFirstLetters()) {
			removeNameRow(ch, row);
		}
		_list.remove(key, replacedBy);
	} else {
		if (_list.remove(key, replacedBy)) {
			for (const auto &ch : key.entry()->chatListFirstLetters()) {
				if (const auto it = _index.find(ch); it != _index.cend()) {
					it->second.remove(key, replacedBy);
				}
			}
		}
	}
}

void IndexedList::clear() {
	_list.clear();
	_index.clear();
	_nameIndex.clear();
	_prefixSearchIndex.clear();
	_searchKeysByRow.clear();
}

std::vector<not_null<Row*>> IndexedList::filtered(
		const QStringList &words) const {
	const auto byLetter
		= (const std::vector<not_null<Row*>>*)nullptr;
	const auto byPrefix = (const std::unordered_set<Row*>*)nullptr;
	auto minimalByLetter = byLetter;
	auto minimalByPrefix = byPrefix;
	auto minimalSize = size_t(0);
	auto hasMinimal = false;
	for (const auto &word : words) {
		if (word.isEmpty()) {
			continue;
		}
		if (_usePrefixSearch && word.size() >= kSearchPrefixLength) {
			const auto i = _prefixSearchIndex.find(
				word.left(kSearchPrefixLength));
			if (i == _prefixSearchIndex.end() || i->second.empty()) {
				return {};
			}
			if (!hasMinimal || (i->second.size() < minimalSize)) {
				minimalByLetter = nullptr;
				minimalByPrefix = &i->second;
				minimalSize = i->second.size();
				hasMinimal = true;
			}
		} else {
			const auto found = filtered(word[0]);
			if (!found || found->empty()) {
				return {};
			}
			if (!hasMinimal || (found->size() < minimalSize)) {
				minimalByLetter = found;
				minimalByPrefix = nullptr;
				minimalSize = found->size();
				hasMinimal = true;
			}
		}
	}
	auto result = std::vector<not_null<Row*>>();
	if (!hasMinimal) {
		return result;
	}
	result.reserve(minimalSize);
	const auto appendIfFound = [&](Row *row) {
		const auto &nameWords = row->entry()->chatListNameWords();
		const auto found = [&](const QString &word) {
			for (const auto &name : nameWords) {
				if (name.startsWith(word)) {
					return true;
				}
			}
			return false;
		};
		const auto allFound = [&] {
			for (const auto &word : words) {
				if (!found(word)) {
					return false;
				}
			}
			return true;
		}();
		if (allFound) {
			result.push_back(row);
		}
	};
	if (minimalByPrefix) {
		for (const auto row : *minimalByPrefix) {
			appendIfFound(row);
		}
		ranges::sort(result, [](const not_null<Row*> &a, const not_null<Row*> &b) {
			return a->index() < b->index();
		});
	} else {
		for (const auto &row : *minimalByLetter) {
			appendIfFound(row.get());
		}
	}
	return result;
}

} // namespace Dialogs
