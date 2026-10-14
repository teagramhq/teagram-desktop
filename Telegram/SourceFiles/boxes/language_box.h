/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/box_content.h"
#include "base/binary_guard.h"

struct LanguageId;

namespace Ui {
class MultiSelect;
struct ScrollToRequest;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

class LanguageBox : public Ui::BoxContent {
public:
	LanguageBox(
		QWidget*,
		Window::SessionController *controller,
		const QString &highlightId = QString());

	void setInnerFocus() override;

	[[nodiscard]] static base::binary_guard
	Show(Window::SessionController *controller,
		 const QString &highlightId = QString());

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	[[nodiscard]] bool translateChatsRowPresentForRegressionTest() const;
	[[nodiscard]] bool translateButtonRowPresentForRegressionTest() const;
	[[nodiscard]] bool doNotTranslateRowPresentForRegressionTest() const;
	[[nodiscard]] bool platformTranslationRowPresentForRegressionTest() const;
	[[nodiscard]] bool translationAboutPresentForRegressionTest() const;
	[[nodiscard]] bool platformPackNotePresentForRegressionTest() const;
#endif

  protected:
	void prepare() override;
	void showFinished() override;

	void keyPressEvent(QKeyEvent *e) override;

  private:
	void setupTop(not_null<Ui::VerticalLayout*> container);
	[[nodiscard]] int rowsInPage() const;

	Window::SessionController *_controller = nullptr;
	QString _highlightId;
	QPointer<Ui::RpWidget> _showButtonToggle;
	QPointer<Ui::RpWidget> _translateChatsToggle;
	QPointer<Ui::RpWidget> _doNotTranslateButton;
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	QPointer<Ui::RpWidget> _platformTranslationToggle;
	bool _translationAboutPresent = false;
	bool _platformPackNotePresent = false;
#endif
	rpl::event_stream<bool> _translateChatTurnOff;
	Fn<void()> _setInnerFocus;
	Fn<Ui::ScrollToRequest(int rows)> _jump;

};
