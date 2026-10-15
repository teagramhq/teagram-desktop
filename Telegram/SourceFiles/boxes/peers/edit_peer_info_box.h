/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/box_content.h"

namespace Settings {
struct IconDescriptor;
} // namespace Settings

namespace style {
struct SettingsCountButton;
} // namespace style

namespace Window {
class SessionNavigation;
} // namespace Window

namespace Ui {
class VerticalLayout;
class SettingsButton;
} // namespace Ui

class ChannelData;

class EditPeerInfoBox : public Ui::BoxContent {
public:
	EditPeerInfoBox(
		QWidget*,
		not_null<Window::SessionNavigation*> navigation,
		not_null<PeerData*> peer);

	void setInnerFocus() override {
		_focusRequests.fire({});
	}

	static bool Available(not_null<PeerData*> peer);

#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	struct ManageRowsForRegressionTest {
		bool directMessages = false;
		bool reactions = false;
		bool appearance = false;
	};

	[[nodiscard]] bool autoTranslateControlPresentForRegressionTest() const;
	[[nodiscard]] ManageRowsForRegressionTest
	manageRowsForRegressionTest() const;
	[[nodiscard]] QString titleForRegressionTest() const;
	[[nodiscard]] QString descriptionForRegressionTest() const;
	[[nodiscard]] bool descriptionErrorShownForRegressionTest() const;
	void saveRenameForRegressionTest(QString title);
	void
	saveManageChangesForRegressionTest(QString title, QString description,
									   not_null<ChannelData *> discussionLink,
									   int starsPerDirectMessage);
	void saveDescriptionForRegressionTest(QString description,
										  int starsPerDirectMessage);
	void saveDirectMessagesPriceForRegressionTest(int starsPerDirectMessage);
	void
	saveDirectMessagesPriceStageForRegressionTest(int starsPerDirectMessage);
	void saveAutoTranslateForRegressionTest(bool enabled);
#endif

	[[nodiscard]] static object_ptr<Ui::SettingsButton> CreateButton(
		not_null<QWidget*> parent,
		rpl::producer<QString> &&text,
		rpl::producer<QString> &&count,
		Fn<void()> callback,
		const style::SettingsCountButton &st,
		Settings::IconDescriptor &&descriptor);
	[[nodiscard]] static object_ptr<Ui::SettingsButton> CreateButton(
		not_null<QWidget*> parent,
		rpl::producer<QString> &&text,
		rpl::producer<TextWithEntities> &&labelText,
		Fn<void()> callback,
		const style::SettingsCountButton &st,
		Settings::IconDescriptor &&descriptor);

protected:
	void prepare() override;

private:
	rpl::event_stream<> _focusRequests;
	not_null<Window::SessionNavigation*> _navigation;
	not_null<PeerData*> _peer;
#ifdef TDESKTOP_LIFECYCLE_REGRESSION
	bool _autoTranslateControlPresentForRegressionTest = false;
	ManageRowsForRegressionTest _manageRowsForRegressionTest;
	Fn<QString()> _titleForRegressionTest;
	Fn<QString()> _descriptionForRegressionTest;
	Fn<bool()> _descriptionErrorShownForRegressionTest;
	Fn<void(QString)> _saveRenameForRegressionTest;
	Fn<void(QString, QString, not_null<ChannelData *>, int)>
		_saveManageChangesForRegressionTest;
	Fn<void(QString, int)> _saveDescriptionForRegressionTest;
	Fn<void(int)> _saveDirectMessagesPriceForRegressionTest;
	Fn<void(int)> _saveDirectMessagesPriceStageForRegressionTest;
	Fn<void(bool)> _saveAutoTranslateForRegressionTest;
#endif
};

void ShowEditChatPermissions(
	not_null<Window::SessionNavigation*> navigation,
	not_null<PeerData*> peer);
