/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/wrap/slide_wrap.h"
#include "spellcheck/spellcheck_types.h"

#include <vector>

class History;
struct LanguageId;

namespace Ui {
class PlainShadow;
class PopupMenu;
class IconButton;
class FlatLabel;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {

class TranslateBar final {
public:
	TranslateBar(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<History*> history);
	~TranslateBar();
	void setHistory(not_null<History *> history);

	void show();
	void hide();
	void raise();
	void finishAnimating();

	void setShadowGeometryPostprocess(Fn<QRect(QRect)> postprocess);

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	[[nodiscard]] bool shouldBeShownForRegressionTest() const;
	[[nodiscard]] bool hiddenForRegressionTest() const {
		return _wrap.isHidden();
	}
	[[nodiscard]] bool controlsCreatedForRegressionTest() const;
	[[nodiscard]] int contentWidthForRegressionTest() const;
	[[nodiscard]] History *historyForRegressionTest() const { return _history; }
	[[nodiscard]] LanguageId overridenToForRegressionTest() const {
		return _overridenTo.current();
	}
	void setOverridenToForRegressionTest(LanguageId id) { _overridenTo = id; }
	[[nodiscard]] int translateToCallsForRegressionTest() const {
		return _translateToCallsForRegression;
	}
	void clickForRegressionTest();
#endif

	void move(int x, int y);
	void resizeToWidth(int width);
	[[nodiscard]] int height() const;
	[[nodiscard]] rpl::producer<int> heightValue() const;

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _wrap.lifetime();
	}

private:
	void setup();
	void refreshTo();
	void refreshPhrase();
	void setupControls();
	void translateTo(LanguageId id);
	void updateShadowGeometry(QRect wrapGeometry);
	[[nodiscard]] base::unique_qptr<Ui::PopupMenu> createMenu(
		not_null<Ui::IconButton*> button);
	void showMenu(base::unique_qptr<Ui::PopupMenu> menu);

	void showHiddenToast(not_null<PeerData*> peer);
	void showSettingsToast(not_null<PeerData*> peer, LanguageId ignored);
	void showToast(
		TextWithEntities text,
		const QString &buttonText,
		Fn<void()> buttonCallback);

	const not_null<Window::SessionController*> _controller;
	not_null<History *> _history;
	Ui::SlideWrap<> _wrap;
	std::unique_ptr<Ui::PlainShadow> _shadow;
	Fn<QRect(QRect)> _shadowGeometryPostprocess;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::variable<LanguageId> _overridenTo;
	rpl::variable<LanguageId> _to;
	rpl::variable<QString> _phrase;
	LanguageId _preferredTo;
	std::vector<LanguageId> _skipTranslationLanguages;
	Ui::FlatLabel *_label = nullptr;
	Fn<void()> _updateLabelGeometry;
	bool _shouldBeShown = false;
	bool _forceHidden = false;
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	int _translateToCallsForRegression = 0;
#endif
};

} // namespace HistoryView
