/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_signup_invite.h"

#include "intro/intro_signup_invite_validation.h"
#include "intro/intro_signup_name.h"
#include "intro/intro_signup_controls.h"
#include "intro/intro_widget.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "mtproto/mtproto_response.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "styles/style_intro.h"

namespace Intro::details {

InviteWidget::InviteWidget(QWidget *parent, not_null<Main::Account *> account,
						   not_null<Data *> data)
	: Step(parent, account, data),
	  _invite(this, st::introName, tr::lng_teagram_invite_field()),
	  _note(this, tr::lng_teagram_invite_field_empty(), st::introDescription) {
	_note->resizeToWidth(st::introDescription.minWidth);
	setTitleText(tr::lng_teagram_invite_step_title());
	setDescriptionText(tr::lng_teagram_invite_step_desc(
		tr::now, lt_server, getData()->serverEndpoint));
	setErrorCentered(false);

	_invite->setAccessibleName(tr::lng_teagram_invite_field(tr::now));
	_invite->setAccessibleDescription(
		tr::lng_teagram_invite_field_empty(tr::now));
	_invite->changes()
		| rpl::on_next(
			[=] {
				hideError();
				_invite->setAccessibleDescription(
					tr::lng_teagram_invite_field_empty(tr::now));
				setAccessibleDescription(QString());
			},
			_invite->lifetime());
	_invite->submits()
		| rpl::on_next([=](Qt::KeyboardModifiers) { submit(); },
					   _invite->lifetime());
}

int InviteWidget::nextButtonTop() const {
	return contentTop() + st::introServerNextTop;
}

int InviteWidget::errorTop() const {
	return contentTop() + st::introSignUpNameErrorTop;
}

QWidget *InviteWidget::firstTabWidget() const { return _invite.data(); }

void InviteWidget::activate() {
	Step::activate();
	_invite->show();
	_note->show();
	_invite->setFocusFast();
	if (getData()->signupInviteError) {
		getData()->signupInviteError = false;
		showInviteError(tr::lng_intro_signup_invite_unusable(tr::now), true);
	}
}

void InviteWidget::setInnerFocus() { _invite->setFocusFast(); }

void InviteWidget::showAnimated(Animate animate) {
	Step::showAnimated(animate);
	ShowSignupControls(this);
}

void InviteWidget::cancelled() {
	api().request(base::take(_sentRequest)).cancel();
}

void InviteWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	_invite->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	_note->moveToLeft(contentLeft() + st::buttonRadius,
					  contentTop() + st::introSignUpNoteTop);
}

void InviteWidget::showInviteError(const QString &text, bool select) {
	_invite->showError();
	_invite->setAccessibleDescription(text);
	setAccessibleDescription(text);
	showError(rpl::single(text));
	_invite->setFocusFast();
	if (select) {
		_invite->selectAll();
	}
}

void InviteWidget::submit() {
	if (_sentRequest) {
		return;
	}
	const auto input = _invite->getLastText();
	if (input.trimmed().isEmpty()) {
		showInviteError(tr::lng_teagram_invite_field_empty(tr::now), false);
		return;
	}
	const auto secret = NormalizeInviteSecret(input);
	if (secret.isEmpty()) {
		showInviteError(tr::lng_teagram_invite_field_bad(tr::now), false);
		return;
	}
	_invite->setText(secret);
	hideError();
	setAccessibleDescription(QString());
	_sentRequest = api()
					   .request(MTPauth_SignIn(
						   MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
						   MTP_string(getData()->phone),
						   MTP_bytes(getData()->phoneHash), MTP_string(secret),
						   MTP_emailVerificationCode(MTP_string(QString()))))
					   .done([=](const MTPauth_Authorization &result) {
						   _sentRequest = 0;
						   signInDone(result);
					   })
					   .fail([=](const MTP::Error &error) {
						   _sentRequest = 0;
						   signInFail(error);
					   })
					   .handleAllErrors()
					   .send();
}

void InviteWidget::signInDone(const MTPauth_Authorization &result) {
	result.match(
		[&](const MTPDauth_authorization &) { finish(result); },
		[&](const MTPDauth_authorizationSignUpRequired &data) {
			if (const auto terms = data.vterms_of_service()) {
				terms->match([&](const MTPDhelp_termsOfService &termsData) {
					getData()->termsLock
						= Window::TermsLock::FromSignupMTP(termsData);
				});
			} else {
				getData()->termsLock = Window::TermsLock();
			}
			goNextOrBack<SignUpNameWidget>();
		});
}

void InviteWidget::signInFail(const MTP::Error &error) {
	if (error.type() == u"INVITE_HASH_INVALID"_q) {
		showInviteError(tr::lng_intro_signup_invite_unusable(tr::now), true);
	} else if (error.type() == u"INPUT_REQUEST_INVALID"_q) {
		showInviteError(tr::lng_intro_signup_closed(tr::now), false);
	} else if (error.type() == u"PHONE_CODE_INVALID"_q
			   || error.type() == u"PHONE_CODE_EXPIRED"_q) {
		getData()->usernameCode.drop();
		goBack();
	} else {
		showInviteError(tr::lng_intro_server_error(tr::now), false);
	}
}

} // namespace Intro::details
