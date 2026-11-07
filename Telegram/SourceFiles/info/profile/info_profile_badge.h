/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"
#include "base/object_ptr.h"

namespace style {
struct InfoPeerBadge;
} // namespace style

namespace Data {
enum class CustomEmojiSizeTag : uchar;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
class AbstractButton;
} // namespace Ui

namespace Ui::Text {
class CustomEmoji;
} // namespace Ui::Text

namespace Info::Profile {

class EmojiStatusPanel;

enum class BadgeType : uchar {
	None = 0x00,
	Verified = 0x01,
	BotVerified = 0x02,
	Premium = 0x04,
	Scam = 0x08,
	Fake = 0x10,
	Direct = 0x20,
	EmojiStatus = 0x40,
};
inline constexpr bool is_flag_type(BadgeType) { return true; }

class Badge final {
public:
	struct Content {
		BadgeType badge = BadgeType::None;
		EmojiStatusId emojiStatusId;

		friend inline bool operator==(Content, Content) = default;
	};
	Badge(
		not_null<QWidget*> parent,
		const style::InfoPeerBadge &st,
		not_null<Main::Session*> session,
		rpl::producer<Content> content,
		EmojiStatusPanel *emojiStatusPanel,
		Fn<bool()> animationPaused,
		int customStatusLoopsLimit = 0,
		base::flags<BadgeType> allowed
			= base::flags<BadgeType>::from_raw(-1));

	~Badge();

	[[nodiscard]] Ui::RpWidget *widget() const;

	void setPremiumClickCallback(Fn<void()> callback);
	void setEmojiStatusClickCallback(Fn<void()> callback);
	void setOverrideStyle(const style::InfoPeerBadge *st);
	[[nodiscard]] rpl::producer<> updated() const;
	void move(int left, int top, int bottom);

	[[nodiscard]] Data::CustomEmojiSizeTag sizeTag() const;

private:
	void setContent(Content content);
	[[nodiscard]] const style::InfoPeerBadge &st() const;

	const not_null<QWidget*> _parent;
	const style::InfoPeerBadge &_st;
	const style::InfoPeerBadge *_overrideSt = nullptr;
	const not_null<Main::Session*> _session;
	EmojiStatusPanel *_emojiStatusPanel = nullptr;
	const int _customStatusLoopsLimit = 0;
	std::unique_ptr<Ui::Text::CustomEmoji> _emojiStatus;
	base::flags<BadgeType> _allowed;
	Content _content;
	Fn<void()> _premiumClickCallback;
	Fn<void()> _emojiStatusClickCallback;
	Fn<bool()> _animationPaused;
	object_ptr<Ui::AbstractButton> _view = { nullptr };
	rpl::event_stream<> _updated;
	rpl::lifetime _lifetime;

};

namespace details {

[[nodiscard]] inline Badge::Content
ResolveBadgeContentForPeer(BadgeType badge, EmojiStatusId emojiStatusId,
						   bool isUser, bool customServer) {
	if (customServer && emojiStatusId) {
		if (badge == BadgeType::Scam || badge == BadgeType::Fake
			|| badge == BadgeType::Direct) {
			return {badge};
		}
		return {BadgeType::EmojiStatus, emojiStatusId};
	}
	if (emojiStatusId.collectible && (badge == BadgeType::Verified)) {
		return {BadgeType::Premium, emojiStatusId};
	}
	if (badge == BadgeType::Verified) {
		badge = BadgeType::None;
	}
	if (isUser && badge != BadgeType::Premium) {
		emojiStatusId = EmojiStatusId();
	} else if (emojiStatusId && badge == BadgeType::None) {
		badge = BadgeType::Premium;
	}
	return {badge, emojiStatusId};
}

[[nodiscard]] inline Badge::Content
FilterBadgeContent(Badge::Content content, bool allowed,
				   bool premiumBadgesShown) {
	if (!allowed
		|| (!premiumBadgesShown && content.badge == BadgeType::Premium)) {
		content.badge = BadgeType::None;
	}
	return content;
}

} // namespace details

[[nodiscard]] rpl::producer<Badge::Content> BadgeContentForPeer(
	not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<Badge::Content> VerifiedContentForPeer(
	not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<Badge::Content> BotVerifyBadgeForPeer(
	not_null<PeerData*> peer);

} // namespace Info::Profile
