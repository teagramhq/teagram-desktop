/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/sticker_import_box.h"

#include "api/api_sticker_set_import.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "boxes/sticker_set_box.h"
#include "core/file_utilities.h"
#include "core/sticker_import.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
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

namespace {

const auto kImportModeKey = u"sticker_import_mode"_q;
constexpr auto kShortNameCheckDelay = crl::time(400);

struct ImportFormState final : base::has_weak_ptr {
	explicit ImportFormState(not_null<Main::Session *> session)
		: api(&session->mtp()) {}

	MTP::Sender api;
	QString sourcePath;
	QString sourceType;
	Core::StickerImportScan scan;
	mtpRequestId nameCheckRequestId = 0;
	mtpRequestId suggestionRequestId = 0;
	mtpRequestId preflightRequestId = 0;
	std::unique_ptr<base::Timer> nameCheckTimer;
	std::unique_ptr<Api::StickerSetImport> importer;
	bool scanPending = false;
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
		return u"Choose a folder or ZIP archive."_q;
	case Core::StickerImportProblem::InvalidArchive:
		return u"The ZIP archive is invalid or uses unsupported compression."_q;
	case Core::StickerImportProblem::TooManyEntries:
		return u"The pack contains more than 201 entries."_q;
	case Core::StickerImportProblem::TotalSizeLimit:
		return u"The pack exceeds the 110 MB uncompressed size limit."_q;
	case Core::StickerImportProblem::NoFiles:
		return u"No supported sticker files were found."_q;
	case Core::StickerImportProblem::FileTooLarge:
		return u"The file is larger than the server limit for this format."_q;
	case Core::StickerImportProblem::UnsupportedFormat:
		return u"Use WEBP, TGS or WEBM files."_q;
	case Core::StickerImportProblem::InvalidMedia:
		return u"The file isn't valid sticker or emoji media."_q;
	case Core::StickerImportProblem::InvalidDimensions:
		return u"The image dimensions don't meet this pack type's limits."_q;
	case Core::StickerImportProblem::TooManyItems:
		return u"The set exceeds its item limit: 120 stickers or 200 custom emoji."_q;
	case Core::StickerImportProblem::ReadFailed:
		return u"The file couldn't be read."_q;
	case Core::StickerImportProblem::SymbolicLink:
		return u"Symbolic links are skipped for safety."_q;
	}
	Unexpected("Sticker import scan problem.");
}

[[nodiscard]] QString ScanDescription(const Core::StickerImportScan &scan) {
	auto lines = QStringList();
	if (!scan.files.empty()) {
		lines.push_back(QString(u"%1 supported file(s) ready to import."_q)
							.arg(int(scan.files.size())));
	}
	if (scan.problem != Core::StickerImportProblem::None) {
		lines.push_back(ScanProblemText(scan.problem));
	}
	for (const auto &issue : scan.issues) {
		lines.push_back(
			u"%1: %2"_q.arg(issue.name, ScanProblemText(issue.problem)));
	}
	return lines.join('\n');
}

[[nodiscard]] QString ServerErrorText(const QString &error) {
	if (error == u"SHORT_NAME_INVALID"_q) {
		return u"Use a letter first, followed by letters, numbers or underscores (up to 64 characters)."_q;
	} else if (error == u"SHORT_NAME_OCCUPIED"_q) {
		return u"This short name is already in use. Choose another one."_q;
	} else if (error == u"TITLE_INVALID"_q) {
		return u"The title must be 1 to 64 characters."_q;
	} else if (error == u"STICKERS_TOO_MUCH"_q) {
		return u"This set is at its sticker or emoji limit."_q;
	} else if (error == u"STICKERS_EMPTY"_q) {
		return u"Choose at least one supported file."_q;
	} else if (error == u"STICKER_FILE_SIZE"_q) {
		return u"A file is larger than the server limit for its format."_q;
	} else if (error == u"STICKER_PNG_DIMENSIONS"_q) {
		return u"A file's dimensions don't meet the server requirements."_q;
	} else if (error == u"STICKER_TGS_NOTGS"_q) {
		return u"A TGS file isn't a valid animated sticker."_q;
	} else if (error == u"STICKER_VIDEO_NOWEBM"_q) {
		return u"A WEBM file isn't a valid video sticker."_q;
	} else if (error == u"STICKER_VIDEO_DURATION"_q) {
		return u"A video sticker can't be longer than 3 seconds."_q;
	} else if (error == u"STICKER_FILE_INVALID"_q
			   || error == u"STICKER_DOCUMENT_INVALID"_q) {
		return u"The server couldn't use one of these files. Check the listed formats and limits."_q;
	} else if (error.startsWith(u"FLOOD_WAIT_"_q)) {
		return u"Please wait a little before creating another set."_q;
	} else if (error == u"STICKERSET_INVALID"_q) {
		return u"The server rejected this sticker set."_q;
	}
	return u"The server couldn't import this pack. Please try again."_q;
}

void ShowCreatedSetLink(not_null<Window::SessionController *> controller,
						const QString &url) {
	controller->show(
		Box<Ui::GenericBox>([=](not_null<Ui::GenericBox *> box) {
			box->setTitle(u"Sticker pack created"_q);
			box->addRow(object_ptr<Ui::FlatLabel>(box, url, st::boxLabel));
			box->addButton(rpl::single(u"Copy link"_q), [=] {
				QGuiApplication::clipboard()->setText(url);
				controller->showToast(u"Link copied"_q);
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
			u"This server has disabled sticker pack imports."_q));
		return;
	}
	controller->show(Box<Ui::GenericBox>([=](not_null<Ui::GenericBox *> box) {
		box->setTitle(u"Import pack"_q);
		box->setWidth(st::boxWideWidth);
		const auto state = box->lifetime().make_state<ImportFormState>(session);

		const auto title = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			rpl::single(u"Title"_q)));
		title->setMaxLength(128);

		const auto shortName = box->addRow(object_ptr<Ui::InputField>(
			box, st::defaultInputField, Ui::InputField::Mode::SingleLine,
			rpl::single(u"Short name"_q)));
		shortName->setMaxLength(64);

		const auto availability
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto source
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto files
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));
		const auto error
			= box->addRow(object_ptr<Ui::FlatLabel>(box, st::boxLabel));

		const auto customEmoji = box->addRow(object_ptr<Ui::Checkbox>(
			box, u"Create a custom emoji set"_q, false, st::settingsCheckbox));
		const auto rights = box->addRow(object_ptr<Ui::Checkbox>(
			box, u"I have the right to share this pack."_q, false,
			st::settingsCheckbox));

		const auto folderButton = box->addRow(Settings::CreateButtonWithIcon(
			box, rpl::single(u"Choose folder"_q), st::settingsButton,
			{&st::menuIconStickers}));
		const auto zipButton = box->addRow(Settings::CreateButtonWithIcon(
			box, rpl::single(u"Choose ZIP file"_q), st::settingsButton,
			{&st::menuIconStickers}));
		const auto setPending = [=](bool pending) {
			title->setDisabled(pending);
			shortName->setDisabled(pending);
			customEmoji->setDisabled(pending);
			rights->setDisabled(pending);
			folderButton->setDisabled(pending);
			zipButton->setDisabled(pending);
			if (state->createButton) {
				state->createButton->setDisabled(pending);
			}
			if (state->suggestButton) {
				state->suggestButton->setDisabled(pending);
			}
		};
		const auto updateScan = [=] {
			if (state->sourcePath.isEmpty() || state->scanPending) {
				return;
			}
			const auto path = state->sourcePath;
			const auto fromFolder = state->sourceType == u"folder"_q;
			const auto type = customEmoji->checked()
								  ? Core::StickerImportType::Emoji
								  : Core::StickerImportType::Stickers;
			state->scanPending = true;
			setPending(true);
			files->setText(u"Scanning files…"_q);
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
						: u"Use a letter first, followed by letters, numbers or underscores (up to 64 characters)."_q);
				return;
			}
			availability->setText(u"Checking availability…"_q);
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
									  ? u"Available"_q
									  : u"This short name is already in use."_q);
						  }))
					  .fail(crl::guard(
						  box,
						  [=] {
							  state->nameCheckRequestId = 0;
							  availability->setText(
								  u"Availability couldn't be checked."_q);
						  }))
					  .send();
		};
		state->nameCheckTimer
			= std::make_unique<base::Timer>([=] { checkShortName(); });
		shortName->changes()
			| rpl::on_next(
				[=] {
					state->api.request(base::take(state->nameCheckRequestId))
						.cancel();
					availability->setText(QString());
					state->nameCheckTimer->callOnce(kShortNameCheckDelay);
				},
				shortName->lifetime());

		const auto chooseFolder = [=] {
			FileDialog::GetFolder(
				box.get(), u"Choose sticker pack folder"_q, QString(),
				crl::guard(box, [=](QString &&path) {
					if (path.isEmpty()) {
						return;
					}
					state->sourcePath = std::move(path);
					state->sourceType = u"folder"_q;
					source->setText(u"Folder: %1"_q.arg(
						QFileInfo(state->sourcePath).fileName()));
					updateScan();
				}));
		};
		folderButton->addClickHandler(chooseFolder);
		zipButton->addClickHandler([=] {
			FileDialog::GetOpenPath(
				box.get(), u"Choose sticker pack ZIP"_q,
				u"ZIP archives (*.zip)"_q,
				crl::guard(box, [=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					state->sourcePath = result.paths.front();
					state->sourceType = u"zip"_q;
					source->setText(u"ZIP file: %1"_q.arg(
						QFileInfo(state->sourcePath).fileName()));
					updateScan();
				}));
		});
		customEmoji->checkedChanges()
			| rpl::on_next([=](bool) { updateScan(); },
						   customEmoji->lifetime());

		box->boxClosing()
			| rpl::on_next(
				[=] {
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
					? u"The upload failed. Check your connection and try again."_q
					: ServerErrorText(message));
		};
		const auto startUpload = [=](const QString &name) {
			const auto type = customEmoji->checked()
								  ? Data::StickersType::Emoji
								  : Data::StickersType::Stickers;
			const auto titleText = title->getLastText().trimmed();
			const auto sourceType = state->sourceType;
			state->importer = std::make_unique<Api::StickerSetImport>(
				session, state->scan, titleText, name, type, sourceType);
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
					error->setText(QString(u"Uploading: %1%"_q).arg(percent));
				});
		};

		state
			->createButton = box->addButton(rpl::single(u"Import pack"_q), [=] {
			error->setText(QString());
			if (state->scanPending) {
				return;
			}
			if (!rights->checked()) {
				error->setText(
					u"Confirm that you have the right to share this pack."_q);
				return;
			}
			const auto titleText = title->getLastText().trimmed();
			if (!Core::IsValidStickerSetTitle(titleText)) {
				error->setText(u"The title must be 1 to 64 characters."_q);
				return;
			}
			const auto name = shortName->getLastText().trimmed();
			if (!Core::IsValidStickerSetShortName(name)) {
				error->setText(ServerErrorText(u"SHORT_NAME_INVALID"_q));
				return;
			}
			if (state->sourcePath.isEmpty()) {
				error->setText(u"Choose a folder or ZIP archive."_q);
				return;
			}
			if (state->scan.files.empty()
				|| state->scan.problem != Core::StickerImportProblem::None) {
				error->setText(state->scan.files.empty()
								   ? ScanDescription(state->scan)
								   : ScanProblemText(state->scan.problem));
				return;
			}
			setPending(true);
			availability->setText(u"Checking server import settings…"_q);
			state->preflightRequestId
				= state->api.request(MTPhelp_GetAppConfig(MTP_int(0)))
					  .done(crl::guard(
						  box,
						  [=](const MTPhelp_AppConfig &config) {
							  state->preflightRequestId = 0;
							  if (!StickerImportEnabled(config)) {
								  setPending(false);
								  availability->setText(
									  u"This server has disabled sticker pack imports."_q);
								  return;
							  }
							  availability->setText(
								  u"Checking availability…"_q);
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
														u"This short name is already in use. Choose another one."_q);
													return;
												}
												availability->setText(
													u"Available"_q);
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
								  u"Couldn't check whether this server allows imports. Try again."_q);
						  }))
					  .send();
		});
		state->suggestButton = box->addLeftButton(
			rpl::single(u"Suggest short name"_q), [=] {
				const auto titleText = title->getLastText().trimmed();
				if (titleText.isEmpty()) {
					error->setText(u"Enter a title first."_q);
					return;
				}
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
								  state->suggestionRequestId = 0;
								  result.match(
									  [&](const MTPDstickers_suggestedShortName
											  &data) {
										  shortName->setText(
											  qs(data.vshort_name()));
									  },
									  [&](const auto &) {
										  error->setText(
											  u"Couldn't suggest a short name."_q);
									  });
							  }))
						  .fail(crl::guard(
							  box,
							  [=](const MTP::Error &) {
								  state->suggestionRequestId = 0;
								  error->setText(
									  u"Couldn't suggest a short name."_q);
							  }))
						  .send();
			});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}
