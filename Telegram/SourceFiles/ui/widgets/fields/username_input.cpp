/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/widgets/fields/special_fields.h"

#include "styles/style_widgets.h"

#include <QtGui/QAccessible>
#include <QtGui/QFontMetrics>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtWidgets/QAccessibleWidget>

#include <utility>

namespace Ui {
namespace {

class UsernameInputAccessible final : public QAccessibleWidget,
									  public QAccessibleTextInterface,
									  public QAccessibleEditableTextInterface {
  public:
	explicit UsernameInputAccessible(UsernameInput *input)
		: QAccessibleWidget(input, QAccessible::EditableText) {
		addControllingSignal(u"textChanged(const QString&)"_q);
		addControllingSignal(u"returnPressed()"_q);
	}

	void *interface_cast(QAccessible::InterfaceType type) override {
		if (type == QAccessible::TextInterface) {
			return static_cast<QAccessibleTextInterface *>(this);
		}
		if (type == QAccessible::EditableTextInterface
			&& !input()->isReadOnly()) {
			return static_cast<QAccessibleEditableTextInterface *>(this);
		}
		return QAccessibleWidget::interface_cast(type);
	}

	QAccessible::State state() const override {
		auto result = QAccessibleWidget::state();
		result.editable = !input()->isReadOnly();
		result.readOnly = input()->isReadOnly();
		result.selectableText = true;
		result.passwordEdit = input()->echoMode() != QLineEdit::Normal;
		return result;
	}

	QString text(QAccessible::Text type) const override {
		if (type == QAccessible::Value) {
			if (input()->echoMode() == QLineEdit::Normal) {
				return input()->text();
			} else if (input()->echoMode() != QLineEdit::NoEcho) {
				return QString(input()->text().size(), QChar::fromLatin1('*'));
			}
		}
		const auto result = QAccessibleWidget::text(type);
		return (result.isEmpty() && type == QAccessible::Description)
				   ? input()->placeholderText()
				   : result;
	}

	void setText(QAccessible::Text type, const QString &text) override {
		if (type == QAccessible::Value) {
			input()->setTextFromAccessibility(text, int(text.size()));
		} else {
			QAccessibleWidget::setText(type, text);
		}
	}

	void selection(int index, int *start, int *end) const override {
		*start = *end = 0;
		if (index) {
			return;
		}
		*start = input()->selectionStart();
		*end = *start + input()->selectedText().size();
	}

	int selectionCount() const override {
		return input()->hasSelectedText() ? 1 : 0;
	}

	void addSelection(int start, int end) override {
		setSelection(0, start, end);
	}

	void removeSelection(int index) override {
		if (!index) {
			input()->deselect();
		}
	}

	void setSelection(int index, int start, int end) override {
		if (!index) {
			input()->setSelection(start, end - start);
		}
	}

	int cursorPosition() const override { return input()->cursorPosition(); }

	void setCursorPosition(int position) override {
		input()->setCursorPosition(position);
	}

	QString text(int start, int end) const override {
		if (start > end || input()->echoMode() != QLineEdit::Normal) {
			return QString();
		}
		return input()->text().mid(start, end - start);
	}

	int characterCount() const override { return input()->text().size(); }

	QRect characterRect(int offset) const override {
		const auto character = text(offset, offset + 1);
		if (character.isEmpty()) {
			return QRect();
		}
		const auto metrics = QFontMetrics(input()->font());
		const auto margins = input()->textMargins();
		const auto availableWidth
			= input()->width() - margins.left() - margins.right();
		const auto value = input()->text();
		const auto textWidth = metrics.horizontalAdvance(value);
		auto left = margins.left();
		if (input()->alignment() & Qt::AlignRight) {
			left += availableWidth - textWidth;
		} else if (input()->alignment() & Qt::AlignHCenter) {
			left += (availableWidth - textWidth) / 2;
		}
		const auto x = left + metrics.horizontalAdvance(value.left(offset));
		const auto y = (input()->height() - metrics.height()) / 2;
		const auto width = metrics.horizontalAdvance(character);
		return QRect(input()->mapToGlobal(QPoint(x, y)),
					 QSize(width, metrics.height()));
	}

	int offsetAtPoint(const QPoint &point) const override {
		return input()->cursorPositionAt(input()->mapFromGlobal(point));
	}

	void scrollToSubstring(int start, int end) override {
		input()->setCursorPosition(end);
		input()->setCursorPosition(start);
	}

	QString attributes(int offset, int *start, int *end) const override {
		*start = *end = offset;
		return QString();
	}

	void deleteText(int start, int end) override {
		input()->setTextFromAccessibility(
			input()->text().remove(start, end - start), start);
	}

	void insertText(int offset, const QString &text) override {
		input()->setTextFromAccessibility(input()->text().insert(offset, text),
										  offset + text.size());
	}

	void replaceText(int start, int end, const QString &text) override {
		input()->setTextFromAccessibility(
			input()->text().replace(start, end - start, text),
			start + text.size());
	}

  private:
	UsernameInput *input() const {
		return static_cast<UsernameInput *>(object());
	}
};

QAccessibleInterface *UsernameAccessibilityFactory(const QString &,
												   QObject *object) {
	const auto input = dynamic_cast<UsernameInput *>(object);
	return input ? new UsernameInputAccessible(input) : nullptr;
}

} // namespace

void InitUsernameInputAccessibility() {
	[[maybe_unused]] static const auto Installed = [] {
		QAccessible::installFactory(UsernameAccessibilityFactory);
		return true;
	}();
}

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

void UsernameInput::setReadOnlyWithDescription(const QString &description) {
	setReadOnly(true);
	setAccessibleDescription(description);
}

void UsernameInput::setTextFromAccessibility(const QString &text,
											 int cursorAfterEdit) {
	if (isReadOnly()) {
		return;
	}
	const auto wasText = getLastText();
	const auto wasCursor = cursorPosition();
	auto now = text;
	auto nowCursor = cursorAfterEdit;
	correctValue(wasText, wasCursor, now, nowCursor);
	if (QLineEdit::text() != now) {
		setText(now);
	}
	setCursorPosition(nowCursor);
	if (wasText != now) {
		changed();
	}
}

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
	if (_maxLength > 0) {
		auto allowed = (len > 0 && now.at(from) == '@') ? 1 : 0;
		auto scalars = 0;
		while (allowed < len && scalars < _maxLength) {
			const auto &ch = now.at(from + allowed);
			if (ch.isHighSurrogate() && (allowed + 1) < len
				&& now.at(from + allowed + 1).isLowSurrogate()) {
				allowed += 2;
			} else {
				++allowed;
			}
			++scalars;
		}
		if (allowed < len) {
			len = allowed;
		}
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
