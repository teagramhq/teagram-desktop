/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "intro/intro_step.h"

namespace Ui {
class InputField;
class FlatLabel;
} // namespace Ui

namespace Intro::details {

class InviteWidget final : public Step {
public:
	InviteWidget(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);

	bool hasBack() const override {
		return true;
	}

	void activate() override;
	void cancelled() override;
	void submit() override;
	void setInnerFocus() override;
	void showAnimated(Animate animate) override;

	[[nodiscard]] int nextButtonTop() const override;
	[[nodiscard]] int errorTop() const override;
	[[nodiscard]] QWidget *firstTabWidget() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void showInviteError(const QString &text, bool select);
	void signInDone(const MTPauth_Authorization &result);
	void signInFail(const MTP::Error &error);

	object_ptr<Ui::InputField> _invite;
	object_ptr<Ui::FlatLabel> _note;
	mtpRequestId _sentRequest = 0;
};

} // namespace Intro::details
