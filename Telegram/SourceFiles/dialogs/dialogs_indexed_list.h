/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "dialogs/dialogs_list.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

#include <QVarLengthArray>

#include <rpl/event_stream.h>

class History;

namespace Dialogs {

struct RowsByLetter;
class Row;

class IndexedList {
  public:
	enum class PrefixSearch {
		Disabled,
		Enabled,
	};

	IndexedList(SortMode sortMode, FilterId filterId = 0,
				PrefixSearch prefixSearch = PrefixSearch::Disabled);

	RowsByLetter addToEnd(Key key);
	Row *addByName(Key key);
	void reserve(int size);
	void adjustByDate(const RowsByLetter &links);
	void moveToTop(Key key);
	void freeze();
	void unfreeze();
	bool updateHeight(Key key, float64 narrowRatio);
	bool updateHeights(float64 narrowRatio);

	// row must belong to this indexed list all().
	void movePinned(Row *row, int deltaSign);

	// For sortMode != SortMode::Date && != Complex
	void peerNameChanged(not_null<PeerData *> peer,
						 const base::flat_set<QChar> &oldChars);

	// For sortMode == SortMode::Date || == Complex
	void peerNameChanged(FilterId filterId, not_null<PeerData *> peer,
						 const base::flat_set<QChar> &oldChars);

	void remove(Key key, Row *replacedBy = nullptr);
	void clear();

	[[nodiscard]] const List &all() const { return _list; }
	[[nodiscard]] const std::vector<not_null<Row *>> *filtered(QChar ch) const {
		if (_sortMode == SortMode::Name) {
			const auto i = _nameIndex.find(ch);
			return (i != _nameIndex.end()) ? &i->second : nullptr;
		}
		const auto i = _index.find(ch);
		return (i != _index.end()) ? &i->second.rows() : nullptr;
	}
	[[nodiscard]] std::vector<not_null<Row*>> filtered(
		const QStringList &words) const;

	// The name index of a SortMode::Name list is built after the rows are
	// added, so that adding a long list does not wait for the names of every
	// row. Searches use the rows indexed so far and refresh when background
	// indexing completes.
	[[nodiscard]] bool searchIndexReady() const;
	[[nodiscard]] rpl::producer<> searchIndexReadyChanges() const {
		return _searchIndexReadyChanges.events();
	}
	// Returns true if some rows are still not indexed.
	bool indexSome(int limit);

	// Part of List interface is duplicated here for all() list.
	[[nodiscard]] int size() const { return all().size(); }
	[[nodiscard]] bool empty() const { return all().empty(); }
	[[nodiscard]] int height() const { return all().height(); }
	[[nodiscard]] bool contains(Key key) const {
		return all().contains(key);
	}
	[[nodiscard]] Row *getRow(Key key) const { return all().getRow(key); }
	[[nodiscard]] Row *rowAtY(int y) const { return all().rowAtY(y); }

	using iterator = List::iterator;
	using const_iterator = List::const_iterator;
	[[nodiscard]] const_iterator cbegin() const { return all().cbegin(); }
	[[nodiscard]] const_iterator cend() const { return all().cend(); }
	[[nodiscard]] const_iterator begin() const { return all().cbegin(); }
	[[nodiscard]] const_iterator end() const { return all().cend(); }
	[[nodiscard]] iterator begin() { return all().begin(); }
	[[nodiscard]] iterator end() { return all().end(); }
	[[nodiscard]] const_iterator cfind(Row *value) const {
		return all().cfind(value);
	}
	[[nodiscard]] const_iterator find(Row *value) const {
		return all().cfind(value);
	}
	[[nodiscard]] iterator find(Row *value) { return all().find(value); }
	[[nodiscard]] const_iterator findByY(int y) const {
		return all().findByY(y);
	}
	[[nodiscard]] iterator findByY(int y) { return all().findByY(y); }

  private:
	void adjustByName(Key key);
	void adjustNames(FilterId filterId, not_null<History *> history,
					 const base::flat_set<QChar> &oldChars);
	void addNameRow(QChar ch, Row *row);
	void removeNameRow(QChar ch, Row *row);
	void addNameRows(Row *row);
	// Returns false if the row was not indexed yet.
	bool removeNameRows(Row *row);
	void updateSearchIndexReady();
	void updatePrefixSearchIndex(Row *row);
	void removePrefixSearchIndex(Row *row);

	SortMode _sortMode = SortMode();
	FilterId _filterId = 0;
	bool _usePrefixSearch = false;
	List _list, _empty;
	base::flat_map<QChar, List> _index;
	base::flat_map<QChar, std::vector<not_null<Row *>>> _nameIndex;
	std::map<QString, std::unordered_set<Row *>> _prefixSearchIndex;
	std::unordered_map<Row *, std::vector<QString>> _searchKeysByRow;
	std::unordered_map<Row *, QVarLengthArray<QChar, 4>> _indexedLettersByRow;
	rpl::event_stream<> _searchIndexReadyChanges;
	bool _searchIndexWasReady = true;
	int _indexCursor = 0;
};

} // namespace Dialogs
