/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"

#include "export/output/export_output_string.h"

TEST_CASE(JsonExportEscapesEveryControlByte) {
	auto input = QByteArray();
	for (auto value = 0; value != 32; ++value) {
		input.append(char(value));
	}
	const auto expected = QByteArrayLiteral(
		"\"\\x00\\x01\\x02\\x03\\x04\\x05\\x06\\x07\\x08\\t"
		"\\x0B\\x0C\\r\\x0E\\x0F\\x10\\x11\\x12\\x13\\x14\\x15"
		"\\x16\\x17\\x18\\x19\\x1A\\x1B\\x1C\\x1D\\x1E\\x1F\"");

	CHECK_EQ(Export::Output::details::Json::SerializeString(input), expected);
}

TEST_CASE(HtmlExportEscapesEveryControlByte) {
	auto input = QByteArray();
	for (auto value = 0; value != 32; ++value) {
		input.append(char(value));
	}
	const auto expected = QByteArrayLiteral(
		"&#x00;&#x01;&#x02;&#x03;&#x04;&#x05;&#x06;&#x07;&#x08;&#x09;<br>"
		"&#x0B;&#x0C;&#x0D;&#x0E;&#x0F;&#x10;&#x11;&#x12;&#x13;&#x14;&#x15;"
		"&#x16;&#x17;&#x18;&#x19;&#x1A;&#x1B;&#x1C;&#x1D;&#x1E;&#x1F;");

	CHECK_EQ(Export::Output::details::Html::SerializeString(input), expected);
}

TEST_CASE(ExportWritersPreserveHighBitUtf8AndSpecialSeparators) {
	const auto utf8 = QByteArray::fromHex("c3a9e282ac");
	CHECK_EQ(
		Export::Output::details::Json::SerializeString(utf8),
		QByteArrayLiteral("\"") + utf8 + '"');
	CHECK_EQ(Export::Output::details::Html::SerializeString(utf8), utf8);

	const auto separators = QByteArray::fromHex("e280a8e280a9");
	CHECK_EQ(
		Export::Output::details::Json::SerializeString(separators),
		QByteArrayLiteral("\"\\u2028\\u2029\""));
	CHECK_EQ(
		Export::Output::details::Html::SerializeString(separators),
		QByteArrayLiteral("<br><br>"));
}
