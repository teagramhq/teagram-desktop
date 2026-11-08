/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_import_box.h"

#include "api/api_sticker_set_import.h"
#include "api/api_telegram_sticker_pack.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "boxes/sticker_import_feedback.h"
#include "boxes/sticker_set_box.h"
#include "core/file_utilities.h"
#include "core/sticker_import.h"
#include "core/telegram_sticker_import.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <optional>

namespace {

const auto kImportModeKey = u"sticker_import_mode"_q;
constexpr auto kShortNameCheckDelay = crl::time(400);

struct ImportFormState final : base::has_weak_ptr {
	explicit ImportFormState(not_null<Main::Session *> session)
		: api(&session->mtp()),
		  telegramBotToken(session->local().telegramStickerBotToken()) {}
	MTP::Sender api;
	Core::StickerImportSelection selection;
	Core::StickerImportScan scan;
	Core::StickerImportSuggestionGuard suggestionGuard;
	mtpRequestId nameCheckRequestId = 0;
	mtpRequestId suggestionRequestId = 0;
	mtpRequestId preflightRequestId = 0;
	std::unique_ptr<base::Timer> nameCheckTimer;
	std::unique_ptr<Api::StickerSetImport> importer;
	std::unique_ptr<Api::TelegramStickerPackDownloader> telegramDownloader;
	std::optional<Core::TelegramStickerPackLink> telegramPack;
	std::optional<Core::TelegramBotToken> telegramBotToken;
	rpl::lifetime rightsErrorLifetime;
	Fn<void()> showRightsError;
	bool scanPending = false;
	bool rightsError = false;
	QPointer<Ui::RoundButton> createButton;
	QPointer<Ui::RoundButton> suggestButton;
};

[[nodiscard]] bool StickerImportEnabled(const MTPhelp_AppConfig &config) {
	return config.match(
		[&](const MTPDhelp_appConfig &data) {
			const auto &value = data.vconfig();
			if (value.type() != mtpc_jsonObject) {
				return false;
			}
			for (const auto &element : value.c_jsonObject().vvalue().v) {
				const auto enabled = element.match(
					[&](const MTPDjsonObjectValue &item) {
						if (qs(item.vkey()) != kImportModeKey) {
							return false;
						}
						return item.vvalue().match(
							[&](const MTPDjsonString &text) {
								return qs(text.vvalue()) == u"on"_q;
							},
							[](const auto &) { return false; });
					},
					[](const auto &) { return false; });
				if (enabled) {
					return true;
				}
			}
			return false;
		},
		[](const MTPDhelp_appConfigNotModified &) { return false; });
}

[[nodiscard]] QString ScanProblemText(Core::StickerImportProblem problem) {
	switch (problem) {
	case Core::StickerImportProblem::None:
		return QString();
	case Core::StickerImportProblem::InvalidSource:
		return tr::lng_teagram_sticker_import_scan_invalid_source(tr::now);
	case Core::StickerImportProblem::InvalidArchive:
		return tr::lng_teagram_sticker_import_scan_invalid_archive(tr::now);
	case Core::StickerImportProblem::TooManyEntries:
		return tr::lng_teagram_sticker_import_scan_too_many_entries(
			tr::now, lt_count, 201);
	case Core::StickerImportProblem::TotalSizeLimit:
		return tr::lng_teagram_sticker_import_scan_total_size(tr::now);
	case Core::StickerImportProblem::NoFiles:
		return tr::lng_teagram_sticker_import_scan_no_files(tr::now);
	case Core::StickerImportProblem::FileTooLarge:
		return tr::lng_teagram_sticker_import_scan_file_too_large(tr::now);
	case Core::StickerImportProblem::UnsupportedFormat:
		return tr::lng_teagram_sticker_import_scan_unsupported_format(tr::now);
	case Core::StickerImportProblem::InvalidMedia:
		return tr::lng_teagram_sticker_import_scan_invalid_media(tr::now);
	case Core::StickerImportProblem::InvalidDimensions:
		return tr::lng_teagram_sticker_import_scan_invalid_dimensions(tr::now);
	case Core::StickerImportProblem::TooManyItems:
		return tr::lng_teagram_sticker_import_scan_too_many_items(tr::now);
	case Core::StickerImportProblem::ReadFailed:
		return tr::lng_teagram_sticker_import_scan_read_failed(tr::now);
	case Core::StickerImportProblem::SymbolicLink:
		return tr::lng_teagram_sticker_import_scan_symbolic_link(tr::now);
	}
	Unexpected("Sticker import scan problem.");
}

[[nodiscard]] QString TelegramDownloadFailureText(
	Ui::StickerImport::TelegramDownloadFailureMessage message,
	const QString &detail) {
	using Message = Ui::StickerImport::TelegramDownloadFailureMessage;
	switch (message) {
	case Message::InvalidToken:
		return tr::lng_teagram_sticker_import_telegram_token_rejected(tr::now);
	case Message::InvalidLink:
		return tr::lng_teagram_sticker_import_telegram_link_invalid(tr::now);
	case Message::UnsupportedPackType:
		return tr::lng_teagram_sticker_import_telegram_type_mismatch(tr::now);
	case Message::PackNotFound:
		return tr::lng_teagram_sticker_import_telegram_pack_missing(tr::now);
	case Message::ApiError:
		return tr::lng_teagram_sticker_import_telegram_api_error(
			tr::now, lt_error, detail);
	case Message::NetworkError:
		return tr::lng_teagram_sticker_import_telegram_network_error(tr::now);
	case Message::InvalidResponse:
		return tr::lng_teagram_sticker_import_telegram_response_invalid(
			tr::now);
	case Message::UnsafeFilePath:
		return tr::lng_teagram_sticker_import_telegram_file_path_invalid(
			tr::now);
	case Message::TooManyFiles:
		return tr::lng_teagram_sticker_import_telegram_too_many_files(tr::now);
	case Message::FileTooLarge:
		return tr::lng_teagram_sticker_import_telegram_file_too_large(tr::now);
	case Message::TotalTooLarge:
		return tr::lng_teagram_sticker_import_scan_total_size(tr::now);
	case Message::UnsupportedFormat:
		return tr::lng_teagram_sticker_import_telegram_format_unsupported(
			tr::now);
	}
	Unexpected("Telegram sticker download failure message.");
}

[[nodiscard]] QString ScanDescription(const Core::StickerImportScan &scan) {
	auto lines = QStringList();
	if (!scan.files.empty()) {
		lines.push_back(tr::lng_teagram_sticker_import_files_ready(
			tr::now, lt_count, int(scan.files.size())));
	}
	if (scan.problem != Core::StickerImportProblem::None) {
		lines.push_back(ScanProblemText(scan.problem));
	}
	for (const auto &issue : scan.issues) {
		lines.push_back(tr::lng_teagram_sticker_import_file_issue(
			tr::now, lt_name, issue.name, lt_reason,
			ScanProblemText(issue.problem)));
	}
	return lines.join('\n');
}

[[nodiscard]] QString ServerErrorText(const QString &error) {
	if (error == u"SHORT_NAME_INVALID"_q) {
		return tr::lng_teagram_sticker_import_short_name_invalid(tr::now);
	} else if (error == u"SHORT_NAME_OCCUPIED"_q) {
		return tr::lng_teagram_sticker_import_short_name_occupied(tr::now);
	} else if (error == u"TITLE_INVALID"_q) {
		return tr::lng_teagram_sticker_import_title_invalid(tr::now);
	} else if (error == u"STICKERS_TOO_MUCH"_q) {
		return tr::lng_teagram_sticker_import_server_too_many_items(tr::now);
	} else if (error == u"STICKERS_EMPTY"_q) {
		return tr::lng_teagram_sticker_import_server_no_files(tr::now);
	} else if (error == u"STICKER_FILE_SIZE"_q) {
		return tr::lng_teagram_sticker_import_server_file_too_large(tr::now);
	} else if (error == u"STICKER_PNG_DIMENSIONS"_q) {
		return tr::lng_teagram_sticker_import_server_invalid_dimensions(
			tr::now);
	} else if (error == u"STICKER_TGS_NOTGS"_q) {
		return tr::lng_teagram_sticker_import_server_invalid_tgs(tr::now);
	} else if (error == u"STICKER_VIDEO_NOWEBM"_q) {
		return tr::lng_teagram_sticker_import_server_invalid_webm(tr::now);
	} else if (error == u"STICKER_VIDEO_DURATION"_q) {
		return tr::lng_teagram_sticker_import_server_video_duration(tr::now);
	} else if (error == u"STICKER_FILE_INVALID"_q
			   || error == u"STICKER_DOCUMENT_INVALID"_q) {
		return tr::lng_teagram_sticker_import_server_invalid_file(tr::now);
	} else if (error.startsWith(u"FLOOD_WAIT_"_q)) {
		return tr::lng_teagram_sticker_import_server_flood_wait(tr::now);
	} else if (error == u"STICKERSET_INVALID"_q) {
		return tr::lng_teagram_sticker_import_server_set_invalid(tr::now);
	}
	return tr::lng_teagram_sticker_import_server_error(tr::now, lt_error,
													   error);
}

void ShowCreatedSetLink(not_null<Window::SessionController *> controller,
						const QString &url) {
	controller->show(
		Box<Ui::GenericBox>([=](not_null<Ui::GenericBox *> box) {
			box->setTitle(tr::lng_teagram_sticker_import_created());
			box->addRow(object_ptr<Ui::FlatLabel>(box, url, st::boxLabel));
			box->addButton(tr::lng_teagram_sticker_import_copy_link(), [=] {
				QGuiApplication::clipboard()->setText(url);
				controller->showToast(
					tr::lng_teagram_sticker_import_link_copied(tr::now));
				box->closeBox();
			});
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		}),
		Ui::LayerOption::KeepOther);
}

} // namespace

bool StickerImportEnabled(not_null<Main::Session *> session) {
	return session->appConfig().get<QString>(kImportModeKey, u"off"_q)
		   == u"on"_q;
}

void ShowStickerImportBox(not_null<Window::SessionController *> controller) {
	const auto session = &controller->session();
	if (!StickerImportEnabled(session)) {
		controller->show(Ui::MakeInformBox(
			tr::lng_teagram_sticker_import_disabled(tr::now)));
		return;
	}
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox *> box) {
		box->setTitle(tr::lng_teagram_sticker_import_title());
		box->setWidth(st::boxWideWidth);
		const auto state = box->lifetime().make_state<ImportFormState>(session);

		const auto title = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			tr::lng_teagram_sticker_import_title_placeholder()));
		title->setMaxLength(128);

		const auto shortName = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			tr::lng_teagram_sticker_import_short_name_placeholder()));
		shortName->setMaxLength(64);

		const auto availability
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto source
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto files
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto customEmoji = box->addRow(object_ptr<Ui::Checkbox>(
			box, tr::lng_teagram_sticker_import_custom_emoji(tr::now), false,
			st::settingsCheckbox));

		const auto folderButton = box->addRow(Settings::CreateButtonWithIcon(
			box, tr::lng_teagram_sticker_import_choose_folder(),
			st::settingsButton, {&st::menuIconStickers}));
		const auto zipButton = box->addRow(Settings::CreateButtonWithIcon(
			box, tr::lng_teagram_sticker_import_choose_zip(),
			st::settingsButton, {&st::menuIconStickers}));
		const auto telegramLink = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			tr::lng_teagram_sticker_import_telegram_link_placeholder()));
		telegramLink->setMaxLength(256);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_teagram_sticker_import_telegram_token_info(tr::now),
			st::boxLabel));
		auto telegramTokenContainer = object_ptr<Ui::RpWidget>(box);
		telegramTokenContainer->resize(telegramTokenContainer->width(),
									   st::defaultInputField.heightMin);
		const auto telegramTokenInput = Ui::CreateChild<Ui::PasswordInput>(
			telegramTokenContainer.data(), st::defaultInputField,
			tr::lng_teagram_sticker_import_telegram_token_placeholder());
		telegramTokenContainer->geometryValue()
			| rpl::on_next(
				[=](const QRect &r) {
					telegramTokenInput->resize(r.width(),
											   telegramTokenInput->height());
					telegramTokenInput->move(0, 0);
				},
				telegramTokenContainer->lifetime());
		const auto telegramTokenRow
			= box->addRow(std::move(telegramTokenContainer));
		telegramTokenInput->setMaxLength(256);
		telegramTokenInput->setEchoMode(QLineEdit::Password);
		telegramTokenInput->setCompleter(nullptr);
		telegramTokenInput->setInputMethodHints(Qt::ImhNoPredictiveText
												| Qt::ImhNoAutoUppercase);
		const auto telegramTokenSaved = box->addRow(object_ptr<Ui::FlatLabel>(
			box, tr::lng_teagram_sticker_import_telegram_token_saved(tr::now),
			st::boxLabel));
		const auto saveTelegramTokenButton
			= box->addRow(Settings::CreateButtonWithIcon(
				box, tr::lng_teagram_sticker_import_telegram_token_save(),
				st::settingsButton, {&st::menuIconStickers}));
		const auto removeTelegramTokenButton
			= box->addRow(Settings::CreateButtonWithIcon(
				box, tr::lng_teagram_sticker_import_telegram_token_remove(),
				st::settingsButton, {&st::menuIconStickers}));
		const auto telegramDownloadButton
			= box->addRow(Settings::CreateButtonWithIcon(
				box, tr::lng_teagram_sticker_import_telegram_download(),
				st::settingsButton, {&st::menuIconStickers}));
		const auto rights
			= box->addRow(object_ptr<Ui::Checkbox>(
							  box, tr::lng_teagram_sticker_import_rights(),
							  false, st::defaultBoxCheckbox),
						  {
							  st::boxRowPadding.left(),
							  st::boxRowPadding.left(),
							  st::boxRowPadding.right(),
							  st::defaultBoxCheckbox.margin.bottom(),
						  });
		rights->setAllowTextLines(5);
		const auto check
			= static_cast<Ui::CheckView *>(rights->checkView().get());
		state->showRightsError = Ui::CheckView::PrepareNonToggledError(
			check, state->rightsErrorLifetime);
		const auto error
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto rightsErrorText
			= tr::lng_teagram_sticker_import_rights_error(tr::now);
		rights->checkedChanges()
			| rpl::on_next(
				[=](bool checked) {
					state->selection.setRightsConfirmed(checked);
					if (checked && state->rightsError) {
						state->rightsError = false;
						rights->setAccessibleDescription(QString());
						error->setText(QString());
						error->setTextColorOverride(std::nullopt);
					}
				},
				rights->lifetime());
		const auto clearRightsError = [=] {
			if (!state->rightsError) {
				return;
			}
			state->rightsError = false;
			rights->setAccessibleDescription(QString());
			error->setText(QString());
			error->setTextColorOverride(std::nullopt);
			state->rightsErrorLifetime = rpl::lifetime();
			check->setUntoggledOverride(std::nullopt);
			state->showRightsError = Ui::CheckView::PrepareNonToggledError(
				check, state->rightsErrorLifetime);
		};
		const auto setPending = [=](bool pending) {
			title->setDisabled(pending);
			shortName->setDisabled(pending);
			customEmoji->setDisabled(
				pending
				|| state->selection.type()
					   == Core::StickerImportSource::Telegram);
			rights->setDisabled(pending);
			folderButton->setDisabled(pending);
			zipButton->setDisabled(pending);
			telegramLink->setDisabled(pending);
			telegramTokenInput->setDisabled(pending);
			saveTelegramTokenButton->setDisabled(pending);
			removeTelegramTokenButton->setDisabled(pending);
			telegramDownloadButton->setDisabled(pending);
			if (state->createButton) {
				state->createButton->setDisabled(pending);
			}
			if (state->suggestButton) {
				state->suggestButton->setDisabled(pending);
			}
		};
		const auto updateTelegramTokenControls = [=] {
			const auto saved = state->telegramBotToken.has_value();
			telegramTokenRow->setVisible(!saved);
			saveTelegramTokenButton->setVisible(!saved);
			telegramTokenSaved->setVisible(saved);
			removeTelegramTokenButton->setVisible(saved);
		};
		updateTelegramTokenControls();
		const auto updateScan = [=] {
			if (state->selection.location().isEmpty() || state->scanPending
				|| state->selection.type()
					   == Core::StickerImportSource::Telegram) {
				return;
			}
			const auto path = state->selection.location();
			const auto fromFolder
				= state->selection.type() == Core::StickerImportSource::Folder;
			const auto type = customEmoji->checked()
								  ? Core::StickerImportType::Emoji
								  : Core::StickerImportType::Stickers;
			state->scanPending = true;
			setPending(true);
			files->setText(tr::lng_teagram_sticker_import_scanning(tr::now));
			const auto weak = QPointer<Ui::GenericBox>(box.get());
			const auto weakState = base::make_weak(state);
			crl::async([=] {
				auto scan = fromFolder
								? Core::ScanStickerImportFolder(path, type)
								: Core::ScanStickerImportZipFile(path, type);
				crl::on_main([=, scan = std::move(scan)]() mutable {
					const auto current = weakState.get();
					if (!weak || !current) {
						return;
					}
					current->scan = std::move(scan);
					current->scanPending = false;
					files->setText(ScanDescription(current->scan));
					setPending(false);
				});
			});
		};
		const auto checkShortName = [=] {
			const auto name = shortName->getLastText().trimmed();
			state->api.request(base::take(state->nameCheckRequestId)).cancel();
			if (!Core::IsValidStickerSetShortName(name)) {
				availability->setText(
					name.isEmpty()
						? QString()
						: tr::lng_teagram_sticker_import_short_name_invalid(
							  tr::now));
				return;
			}
			availability->setText(
				tr::lng_teagram_sticker_import_availability_checking(tr::now));
			state->nameCheckRequestId
				= state->api
					  .request(MTPstickers_CheckShortName(MTP_string(name)))
					  .done(crl::guard(
						  box,
						  [=](const MTPBool &result) {
							  state->nameCheckRequestId = 0;
							  if (name != shortName->getLastText().trimmed()) {
								  return;
							  }
							  availability->setText(
								  mtpIsTrue(result)
									  ? tr::lng_teagram_sticker_import_availability_available(
											tr::now)
									  : tr::lng_teagram_sticker_import_short_name_occupied(
											tr::now));
						  }))
					  .fail(crl::guard(
						  box,
						  [=] {
							  state->nameCheckRequestId = 0;
							  availability->setText(
								  tr::lng_teagram_sticker_import_availability_failed(
									  tr::now));
						  }))
					  .send();
		};
		state->nameCheckTimer
			= std::make_unique<base::Timer>([=] { checkShortName(); });
		const auto invalidateSuggestion = [=] {
			state->suggestionGuard.invalidate();
			state->api.request(base::take(state->suggestionRequestId)).cancel();
		};
		title->changes()
			| rpl::on_next([=] { invalidateSuggestion(); }, title->lifetime());
		shortName->changes()
			| rpl::on_next(
				[=] {
					invalidateSuggestion();
					state->api.request(base::take(state->nameCheckRequestId))
						.cancel();
					availability->setText(QString());
					state->nameCheckTimer->callOnce(kShortNameCheckDelay);
				},
				shortName->lifetime());

		const auto selectSource
			= [=](Core::StickerImportSource type, QString path) {
				  state->telegramDownloader.reset();
				  state->telegramPack.reset();
				  state->scan = {};
				  state->scanPending = false;
				  state->selection.selectSource(type, std::move(path));
				  clearRightsError();
				  rights->setChecked(false);
			  };
		const auto saveTelegramToken = [=] {
			auto text = telegramTokenInput->text().trimmed();
			auto token = Core::TelegramBotToken::Create(text);
			text.fill(QChar(0));
			if (!token) {
				telegramTokenInput->showError();
				error->setTextColorOverride(std::nullopt);
				error->setText(
					tr::lng_teagram_sticker_import_telegram_token_invalid(
						tr::now));
				return false;
			}
			if (!session->local().writeTelegramStickerBotToken(*token)) {
				error->setTextColorOverride(std::nullopt);
				error->setText(
					tr::lng_teagram_sticker_import_telegram_token_save_failed(
						tr::now));
				return false;
			}
			state->telegramBotToken = std::move(token);
			telegramTokenInput->clear();
			telegramTokenInput->hideError();
			updateTelegramTokenControls();
			error->setText(QString());
			return true;
		};
		saveTelegramTokenButton->addClickHandler([=] { saveTelegramToken(); });
		removeTelegramTokenButton->addClickHandler([=] {
			state->telegramBotToken.reset();
			const auto removed
				= session->local().clearTelegramStickerBotToken();
			updateTelegramTokenControls();
			error->setTextColorOverride(std::nullopt);
			error->setText(
				removed
					? QString()
					: tr::lng_teagram_sticker_import_telegram_token_remove_failed(
						  tr::now));
		});
		telegramDownloadButton->addClickHandler([=] {
			error->setText(QString());
			error->setTextColorOverride(std::nullopt);
			files->setTextColorOverride(std::nullopt);
			const auto parsed = Core::ParseTelegramStickerPackLink(
				telegramLink->getLastText().trimmed());
			if (!parsed) {
				error->setText(
					tr::lng_teagram_sticker_import_telegram_link_invalid(
						tr::now));
				return;
			}
			if (!state->telegramBotToken && !saveTelegramToken()) {
				return;
			}
			state->telegramDownloader.reset();
			state->telegramPack = *parsed;
			state->selection.selectSource(Core::StickerImportSource::Telegram,
										  parsed->canonicalUrl());
			state->selection.setRightsConfirmed(false);
			clearRightsError();
			rights->setChecked(false);
			customEmoji->setChecked(parsed->type
									== Core::StickerImportType::Emoji);
			state->scan = {};
			state->scanPending = true;
			title->setText(QString());
			source->setText(tr::lng_teagram_sticker_import_telegram_source(
				tr::now, lt_link, parsed->canonicalUrl()));
			files->setText(tr::lng_teagram_sticker_import_telegram_downloading(
				tr::now, lt_current, QString::number(0), lt_total,
				QString::number(0)));
			setPending(true);
			const auto weakBox = QPointer<Ui::GenericBox>(box.get());
			const auto weakState = base::make_weak(state);
			state->telegramDownloader
				= std::make_unique<Api::TelegramStickerPackDownloader>(
					*parsed, state->telegramBotToken->clone());
			state->telegramDownloader->start(
				[=](QString packTitle, Core::StickerImportScan scan) mutable {
					const auto current = weakState.get();
					if (!weakBox || !current) {
						return;
					}
					current->scan = std::move(scan);
					current->scanPending = false;
					title->setText(packTitle);
					files->setTextColorOverride(std::nullopt);
					files->setText(ScanDescription(current->scan));
					setPending(false);
				},
				[=](Api::TelegramStickerPackDownloadError downloadError,
					QString detail) {
					const auto current = weakState.get();
					if (!weakBox || !current) {
						return;
					}
					const auto feedback
						= Ui::StickerImport::TelegramDownloadFailureFeedback(
							downloadError, detail);
					current->scan = {};
					current->scanPending = feedback.pending;
					setPending(feedback.pending);
					if (feedback.isError) {
						files->setTextColorOverride(st::boxTextFgError->c);
					} else {
						files->setTextColorOverride(std::nullopt);
					}
					files->setText(
						TelegramDownloadFailureText(feedback.message, detail));
				},
				[=](int count, int total) {
					if (!weakBox || !weakState.get()) {
						return;
					}
					files->setText(
						tr::lng_teagram_sticker_import_telegram_downloading(
							tr::now, lt_current, QString::number(count),
							lt_total, QString::number(total)));
				});
		});
		const auto chooseFolder = [=] {
			FileDialog::GetFolder(
				box.get(),
				tr::lng_teagram_sticker_import_choose_folder_dialog(tr::now),
				QString(), crl::guard(box, [=](QString &&path) {
					if (path.isEmpty()) {
						return;
					}
					selectSource(Core::StickerImportSource::Folder,
								 std::move(path));
					source->setText(
						tr::lng_teagram_sticker_import_source_folder(
							tr::now, lt_name,
							QFileInfo(state->selection.location()).fileName()));
					updateScan();
				}));
		};
		folderButton->addClickHandler(chooseFolder);
		zipButton->addClickHandler([=] {
			FileDialog::GetOpenPath(
				box.get(),
				tr::lng_teagram_sticker_import_choose_zip_dialog(tr::now),
				tr::lng_teagram_sticker_import_zip_filter(tr::now),
				crl::guard(box, [=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					selectSource(Core::StickerImportSource::Zip,
								 result.paths.front());
					source->setText(tr::lng_teagram_sticker_import_source_zip(
						tr::now, lt_name,
						QFileInfo(state->selection.location()).fileName()));
					updateScan();
				}));
		});
		customEmoji->checkedChanges()
			| rpl::on_next([=](bool) { updateScan(); },
						   customEmoji->lifetime());

		box->boxClosing()
			| rpl::on_next(
				[=] {
					if (state->telegramDownloader) {
						state->telegramDownloader->cancel();
					}
					state->api.request(base::take(state->nameCheckRequestId))
						.cancel();
					state->api.request(base::take(state->suggestionRequestId))
						.cancel();
					state->api.request(base::take(state->preflightRequestId))
						.cancel();
					if (state->importer) {
						state->importer->cancel();
					}
				},
				box->lifetime());

		const auto failImport = [=](QString message) {
			if (state->importer) {
				state->importer->cancel();
				state->importer.reset();
			}
			setPending(false);
			error->setText(
				message.isEmpty()
					? tr::lng_teagram_sticker_import_upload_failed(tr::now)
					: ServerErrorText(message));
		};
		const auto startUpload = [=](const QString &name) {
			const auto type = customEmoji->checked()
								  ? Data::StickersType::Emoji
								  : Data::StickersType::Stickers;
			const auto titleText = title->getLastText().trimmed();
			const auto sourceType = state->selection.type();
			const auto telegramPackName
				= state->telegramPack ? state->telegramPack->name : QString();
			state->importer = std::make_unique<Api::StickerSetImport>(
				session, state->scan, titleText, name, type, sourceType,
				telegramPackName);
			state->importer->start(
				[=](MTPmessages_StickerSet) {
					const auto importType
						= (type == Data::StickersType::Emoji)
							  ? Core::StickerImportType::Emoji
							  : Core::StickerImportType::Stickers;
					const auto link = session->createInternalLinkFull(
						Core::StickerImportShareLinkPath(importType, name));
					box->closeBox();
					controller->show(Box<StickerSetBox>(
						controller->uiShow(),
						StickerSetIdentifier{.shortName = name}, type));
					ShowCreatedSetLink(controller, link);
				},
				failImport,
				[=](int percent) {
					error->setText(tr::lng_teagram_sticker_import_uploading(
						tr::now, lt_percent, QString::number(percent)));
				});
		};

		state
			->createButton = box->addButton(tr::lng_teagram_sticker_import_title(), [=] {
			error->setText(QString());
			error->setTextColorOverride(std::nullopt);
			state->rightsError = false;
			rights->setAccessibleDescription(QString());
			const auto titleText = title->getLastText().trimmed();
			const auto name = shortName->getLastText().trimmed();
			switch (Core::ValidateStickerImportStart(
				state->scanPending, state->selection.rightsConfirmed(),
				titleText, name, !state->selection.location().isEmpty(),
				state->scan)) {
			case Core::StickerImportValidation::ScanPending:
				return;
			case Core::StickerImportValidation::RightsNotConfirmed:
				state->rightsError = true;
				state->showRightsError();
				error->setTextColorOverride(st::boxTextFgError->c);
				error->setText(rightsErrorText);
				rights->setAccessibleDescription(rightsErrorText);
				return;
			case Core::StickerImportValidation::InvalidTitle:
				error->setText(
					tr::lng_teagram_sticker_import_title_invalid(tr::now));
				return;
			case Core::StickerImportValidation::InvalidShortName:
				error->setText(ServerErrorText(u"SHORT_NAME_INVALID"_q));
				return;
			case Core::StickerImportValidation::InvalidSource:
				error->setText(
					tr::lng_teagram_sticker_import_scan_invalid_source(
						tr::now));
				return;
			case Core::StickerImportValidation::InvalidScan:
				error->setText(state->scan.files.empty()
								   ? ScanDescription(state->scan)
								   : ScanProblemText(state->scan.problem));
				return;
			case Core::StickerImportValidation::Ready:
				break;
			}
			setPending(true);
			availability->setText(
				tr::lng_teagram_sticker_import_preflight_checking(tr::now));
			state->preflightRequestId
				= state->api.request(MTPhelp_GetAppConfig(MTP_int(0)))
					  .done(crl::guard(
						  box,
						  [=](const MTPhelp_AppConfig &config) {
							  state->preflightRequestId = 0;
							  if (!StickerImportEnabled(config)) {
								  setPending(false);
								  availability->setText(
									  tr::lng_teagram_sticker_import_disabled(
										  tr::now));
								  return;
							  }
							  availability->setText(
								  tr::lng_teagram_sticker_import_availability_checking(
									  tr::now));
							  state->preflightRequestId
								  = state->api
										.request(MTPstickers_CheckShortName(
											MTP_string(name)))
										.done(crl::guard(
											box,
											[=](const MTPBool &available) {
												state->preflightRequestId = 0;
												if (!mtpIsTrue(available)) {
													setPending(false);
													availability->setText(
														tr::lng_teagram_sticker_import_short_name_occupied(
															tr::now));
													return;
												}
												availability->setText(
													tr::lng_teagram_sticker_import_availability_available(
														tr::now));
												startUpload(name);
											}))
										.fail(crl::guard(
											box,
											[=](const MTP::Error
													&requestError) {
												state->preflightRequestId = 0;
												setPending(false);
												availability->setText(
													ServerErrorText(
														requestError.type()));
											}))
										.send();
						  }))
					  .fail(crl::guard(
						  box,
						  [=](const MTP::Error &) {
							  state->preflightRequestId = 0;
							  setPending(false);
							  availability->setText(
								  tr::lng_teagram_sticker_import_preflight_failed(
									  tr::now));
						  }))
					  .send();
		});
		state->suggestButton = box->addLeftButton(
			tr::lng_teagram_sticker_import_suggest_short_name(), [=] {
				const auto titleText = title->getLastText().trimmed();
				if (titleText.isEmpty()) {
					error->setText(
						tr::lng_teagram_sticker_import_enter_title(tr::now));
					return;
				}
				const auto request = state->suggestionGuard.begin(
					titleText, shortName->getLastText().trimmed());
				state->api.request(base::take(state->suggestionRequestId))
					.cancel();
				state->suggestionRequestId
					= state->api
						  .request(MTPstickers_SuggestShortName(
							  MTP_string(titleText)))
						  .done(crl::guard(
							  box,
							  [=](const MTPstickers_SuggestedShortName
									  &result) {
								  if (!state->suggestionGuard.isCurrent(
										  request,
										  title->getLastText().trimmed(),
										  shortName->getLastText().trimmed())) {
									  return;
								  }
								  state->suggestionRequestId = 0;
								  result.match(
									  [&](const MTPDstickers_suggestedShortName
											  &data) {
										  shortName->setText(
											  qs(data.vshort_name()));
									  },
									  [&](const auto &) {
										  error->setText(
											  tr::lng_teagram_sticker_import_suggestion_failed(
												  tr::now));
									  });
							  }))
						  .fail(crl::guard(
							  box,
							  [=](const MTP::Error &) {
								  if (!state->suggestionGuard.isCurrent(
										  request,
										  title->getLastText().trimmed(),
										  shortName->getLastText().trimmed())) {
									  return;
								  }
								  state->suggestionRequestId = 0;
								  error->setText(
									  tr::lng_teagram_sticker_import_suggestion_failed(
										  tr::now));
							  }))
						  .send();
			});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}
