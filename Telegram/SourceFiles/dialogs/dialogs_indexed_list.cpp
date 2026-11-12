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

IndexedList::IndexedList(SortMode sortMode, FilterId filterId,
						 PrefixSearch prefixSearch)
	: _sortMode(sortMode), _filterId(filterId),
	  _usePrefixSearch(prefixSearch == PrefixSearch::Enabled),
	  _list(sortMode, filterId), _empty(sortMode, filterId) {
	Expects(prefixSearch == PrefixSearch::Disabled
			|| sortMode == SortMode::Name);
}

void IndexedList::reserve(int size) {
	Expects(size >= this->size());
	_indexedLettersByRow.reserve(size);
}

void IndexedList::addNameRow(QChar ch, Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);

	auto i = _nameIndex.find(ch);
	if (i == _nameIndex.end()) {
		i = _nameIndex.emplace(ch, std::vector<not_null<Row *>>()).first;
	}
	auto &rows = i->second;
	if (rows.empty() || rows.back()->index() < row->index()) {
		rows.emplace_back(row);
	} else {
		const auto position
			= std::lower_bound(rows.begin(), rows.end(), row->index(),
							   [](const not_null<Row *> &entry, int index) {
								   return entry->index() < index;
							   });
		rows.emplace(position, row);
	}
}

void IndexedList::removeNameRow(QChar ch, Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);

	const auto i = _nameIndex.find(ch);
	Expects(i != _nameIndex.end());
	auto &rows = i->second;
	const auto position
		= std::lower_bound(rows.begin(), rows.end(), row->index(),
						   [](const not_null<Row *> &entry, int index) {
							   return entry->index() < index;
						   });
	Expects(position != rows.end() && position->get() == row);
	rows.erase(position);
	if (rows.empty()) {
		_nameIndex.erase(i);
	}
}

void IndexedList::addNameRows(Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);
	const auto [i, inserted]
		= _indexedLettersByRow.emplace(row, QVarLengthArray<QChar, 4>());
	Expects(inserted);
	auto &letters = i->second;
	for (const auto &ch : row->entry()->chatListFirstLetters()) {
		addNameRow(ch, row);
		letters.push_back(ch);
	}
}

bool IndexedList::removeNameRows(Row *row) {
	Expects(_sortMode == SortMode::Name);
	Expects(row != nullptr);
	const auto i = _indexedLettersByRow.find(row);
	if (i == _indexedLettersByRow.end()) {
		return false;
	}
	for (const auto &ch : i->second) {
		removeNameRow(ch, row);
	}
	_indexedLettersByRow.erase(i);
	return true;
}

bool IndexedList::searchIndexReady() const {
	return (_sortMode != SortMode::Name)
		   || (_indexedLettersByRow.size() == size_t(_list.size()));
}

bool IndexedList::indexSome(int limit) {
	if (searchIndexReady()) {
		return false;
	}
	const auto &rows = _list.rows();
	auto left = limit;
	while (left > 0 && _indexCursor < int(rows.size())) {
		const auto row = rows[_indexCursor++].get();
		if (_indexedLettersByRow.find(row) == _indexedLettersByRow.end()) {
			addNameRows(row);
			updatePrefixSearchIndex(row);
			--left;
		}
	}
	if (_indexCursor >= int(rows.size())) {
		// A row that was renamed after the cursor passed its place is found
		// by the next pass.
		_indexCursor = 0;
	}
	updateSearchIndexReady();
	return !searchIndexReady();
}

void IndexedList::updateSearchIndexReady() {
	const auto ready = searchIndexReady();
	if (!_searchIndexWasReady && ready) {
		_searchIndexReadyChanges.fire({});
	}
	_searchIndexWasReady = ready;
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
	if (_sortMode != SortMode::Name) {
		for (const auto &ch : key.entry()->chatListFirstLetters()) {
			auto j = _index.find(ch);
			if (j == _index.cend()) {
				j = _index.emplace(ch, _sortMode, _filterId).first;
				freezeNewIndexList(j->second);
			}
			result.letters.emplace(ch, j->second.addToEnd(key));
		}
	}
	updateSearchIndexReady();
	return result;
}

Row *IndexedList::addByName(Key key) {
	const auto [row, inserted] = _list.addByName(key);
	if (!inserted) {
		return row.get();
	}
	if (_sortMode != SortMode::Name) {
		for (const auto &ch : key.entry()->chatListFirstLetters()) {
			auto j = _index.find(ch);
			if (j == _index.cend()) {
				j = _index.emplace(ch, _sortMode, _filterId).first;
				freezeNewIndexList(j->second);
			}
			j->second.addByName(key);
		}
	}
	updateSearchIndexReady();
	return row.get();
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
	const auto row = _list.getRow(key);
	if (!row) {
		return;
	}
	const auto wasIndexed
		= (_sortMode == SortMode::Name) && removeNameRows(row);
	if (_list.moveToTop(key)) {
		if (_sortMode == SortMode::Name) {
			if (wasIndexed) {
				addNameRows(row);
			}
		} else {
			for (const auto &ch : key.entry()->chatListFirstLetters()) {
				if (auto it = _index.find(ch); it != _index.cend()) {
					it->second.moveToTop(key);
				}
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
			adjustByName(history);
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

void IndexedList::adjustByName(Key key) {
	Expects(_sortMode == SortMode::Name);

	const auto mainRow = _list.getRow(key);
	if (!mainRow) return;

	const auto wasIndexed = removeNameRows(mainRow);
	_list.adjustByName(key);
	if (wasIndexed) {
		addNameRows(mainRow);
		updatePrefixSearchIndex(mainRow);
	}
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
			freezeNewIndexList(j->second);
		}
		auto row = j->second.addToEnd(key);
		if (_sortMode == SortMode::Date) {
			history->addChatListEntryByLetter(filterId, ch, row);
		}
	}
}

void IndexedList::freezeNewIndexList(List &list) {
	for (auto i = 0; i != _freezeDepth; ++i) {
		list.freeze();
	}
}

void IndexedList::freeze() {
	++_freezeDepth;
	_list.freeze();
	for (auto &[ch, list] : _index) {
		list.freeze();
	}
}

void IndexedList::unfreeze() {
	if (!_freezeDepth) {
		return;
	}
	--_freezeDepth;
	const auto moved = _list.unfreeze();
	for (auto &[ch, list] : _index) {
		list.unfreeze();
	}
	if (_freezeDepth) {
		return;
	}
	_unfrozen.fire({});
	auto moveEvents = std::vector<Data::Session::ChatListEntryRefresh>();
	moveEvents.reserve(moved.size());
	for (const auto &[row, position] : moved) {
		moveEvents.push_back({
			.key = row->key(),
			.moved = position,
			.filterId = _filterId,
		});
	}
	if (!moveEvents.empty()) {
		moveEvents.front().key.entry()->owner().notifyChatListEntryMoves(
			std::move(moveEvents));
	}
}

void IndexedList::remove(Key key, Row *replacedBy) {
	if (_sortMode == SortMode::Name) {
		const auto row = _list.getRow(key);
		if (!row) {
			return;
		}
		removePrefixSearchIndex(row);
		removeNameRows(row);
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
	updateSearchIndexReady();
}

void IndexedList::clear() {
	_freezeDepth = 0;
	_list.clear();
	_index.clear();
	_nameIndex.clear();
	_prefixSearchIndex.clear();
	_searchKeysByRow.clear();
	_indexedLettersByRow.clear();
	_indexCursor = 0;
	updateSearchIndexReady();
}

std::vector<not_null<Row*>> IndexedList::filtered(
		const QStringList &words) const {
	const auto byLetter = (const std::vector<not_null<Row *>> *)nullptr;
	const auto byPrefix = (const std::unordered_set<Row *> *)nullptr;
	auto minimalByLetter = byLetter;
	auto minimalByPrefix = byPrefix;
	auto minimalSize = size_t(0);
	auto hasMinimal = false;
	for (const auto &word : words) {
		if (word.isEmpty()) {
			continue;
		}
		if (_usePrefixSearch && word.size() >= kSearchPrefixLength) {
			const auto i
				= _prefixSearchIndex.find(word.left(kSearchPrefixLength));
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
		ranges::sort(result,
					 [](const not_null<Row *> &a, const not_null<Row *> &b) {
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
