/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/widgets/fields/special_fields.h"

#include "styles/style_widgets.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>

#include <utility>

namespace Ui {
namespace {

constexpr auto kMaxUsernameLength = 32;

} // namespace

UsernameInput::UsernameInput(QWidget *parent, const style::InputField &st,
							 rpl::producer<QString> placeholder,
							 const QString &val, const QString &linkPlaceholder)
	: MaskedInputField(parent, st, std::move(placeholder), val) {
	setLinkPlaceholder(linkPlaceholder);
}

void UsernameInput::setLinkPlaceholder(const QString &placeholder) {
	_linkPlaceholder = placeholder;
	if (!_linkPlaceholder.isEmpty()) {
		setTextMargins(style::margins(
			_st.textMargins.left() + _st.style.font->width(_linkPlaceholder),
			_st.textMargins.top(), _st.textMargins.right(),
			_st.textMargins.bottom()));
		setPlaceholderHidden(true);
	}
}

void UsernameInput::setMaxLength(int maxLength) { _maxLength = maxLength; }

void UsernameInput::keyPressEvent(QKeyEvent *e) {
	if (isReadOnly() && e->matches(QKeySequence::DeleteStartOfWord)) {
		e->accept();
		return;
	}
	MaskedInputField::keyPressEvent(e);
}

void UsernameInput::paintAdditionalPlaceholder(QPainter &p) {
	if (!_linkPlaceholder.isEmpty()) {
		p.setFont(_st.style.font);
		p.setPen(_st.placeholderFg);
		p.drawText(
			QRect(_st.textMargins.left(), _st.textMargins.top(), width(),
				  height() - _st.textMargins.top() - _st.textMargins.bottom()),
			_linkPlaceholder, style::al_topleft);
	}
}

void UsernameInput::correctValue(const QString &was, int wasCursor,
								 QString &now, int &nowCursor) {
	auto newPos = nowCursor;
	auto from = 0, len = int(now.size());
	for (; from < len; ++from) {
		if (!now.at(from).isSpace()) {
			break;
		}
		if (newPos > 0) {
			--newPos;
		}
	}
	len -= from;
	const auto maxLength = (_maxLength > 0) ? _maxLength : kMaxUsernameLength;
	if (len > maxLength) {
		len = maxLength + (now.at(from) == '@' ? 1 : 0);
	}
	for (int32 to = from + len; to > from;) {
		--to;
		if (!now.at(to).isSpace()) {
			break;
		}
		--len;
	}
	setCorrectedText(now, nowCursor, now.mid(from, len), newPos);
}

} // namespace Ui
