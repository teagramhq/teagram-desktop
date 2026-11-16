/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tests/unit/unit_test.h"
#include "core/single_instance_lock.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QUuid>
#include <QtNetwork/QLocalServer>

#include <array>

namespace {

bool WaitForFile(const QString &path) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!QFile::exists(path) && timer.elapsed() < 10000) {
		QThread::msleep(5);
	}
	return QFile::exists(path);
}

bool Touch(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly);
}

void PauseLockCreation() {
	const auto child = qEnvironmentVariable("TDESKTOP_INCOMPLETE_LOCK_CHILD");
	CHECK(Touch(child + u"/created"_q));
	CHECK(WaitForFile(child + u"/resume"_q));
}

} // namespace

TEST_CASE(SingleInstanceRecoversIncompleteLockAfterCrash) {
	const auto child = qEnvironmentVariable("TDESKTOP_INCOMPLETE_LOCK_CHILD");
	if (!child.isEmpty()) {
		Core::SingleInstanceLock::SetAfterOpenForTests(&PauseLockCreation);
		auto lock = Core::SingleInstanceLock(child + u"/profile.lock"_q);
		const auto result = lock.tryAcquire();
		CHECK(result
			  == (qEnvironmentVariableIsSet("TDESKTOP_EXPECT_LOCK_BUSY")
					  ? Core::SingleInstanceLock::Result::Busy
					  : Core::SingleInstanceLock::Result::Acquired));
		return;
	}
	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	auto process = QProcess();
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(u"TDESKTOP_INCOMPLETE_LOCK_CHILD"_q, directory.path());
	process.setProcessEnvironment(environment);
	process.start(
		QCoreApplication::applicationFilePath(),
		{u"--case"_q, u"SingleInstanceRecoversIncompleteLockAfterCrash"_q});
	CHECK(process.waitForStarted(10000));
	CHECK(WaitForFile(directory.filePath(u"created"_q)));
	process.kill();
	CHECK(process.waitForFinished(10000));
	auto lock = Core::SingleInstanceLock(directory.filePath(u"profile.lock"_q));
	CHECK(lock.tryAcquire() == Core::SingleInstanceLock::Result::Acquired);
}

TEST_CASE(SingleInstancePausedCreationRespectsLiveOwner) {
	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	auto process = QProcess();
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(u"TDESKTOP_INCOMPLETE_LOCK_CHILD"_q, directory.path());
	environment.insert(u"TDESKTOP_EXPECT_LOCK_BUSY"_q, u"1"_q);
	process.setProcessEnvironment(environment);
	process.start(
		QCoreApplication::applicationFilePath(),
		{u"--case"_q, u"SingleInstanceRecoversIncompleteLockAfterCrash"_q});
	CHECK(process.waitForStarted(10000));
	CHECK(WaitForFile(directory.filePath(u"created"_q)));
	auto owner
		= Core::SingleInstanceLock(directory.filePath(u"profile.lock"_q));
	CHECK(owner.tryAcquire() == Core::SingleInstanceLock::Result::Acquired);
	CHECK(Touch(directory.filePath(u"resume"_q)));
	CHECK(process.waitForFinished(10000));
	CHECK_EQ(process.exitCode(), 0);
}

TEST_CASE(SingleInstanceDifferentProfilesDoNotBlock) {
	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	auto first = Core::SingleInstanceLock(directory.filePath(u"first.lock"_q));
	auto second
		= Core::SingleInstanceLock(directory.filePath(u"second.lock"_q));
	auto duplicate
		= Core::SingleInstanceLock(directory.filePath(u"first.lock"_q));
	auto invalid
		= Core::SingleInstanceLock(directory.filePath(u"missing/lock"_q));
	CHECK(first.tryAcquire() == Core::SingleInstanceLock::Result::Acquired);
	CHECK(second.tryAcquire() == Core::SingleInstanceLock::Result::Acquired);
	CHECK(duplicate.tryAcquire() == Core::SingleInstanceLock::Result::Busy);
	CHECK(invalid.tryAcquire() == Core::SingleInstanceLock::Result::Error);
	CHECK(!invalid.errorString().isEmpty());
	CHECK(invalid.errorString().contains(u"open"_q));
	CHECK(invalid.errorString().contains(u"missing/lock"_q));
	CHECK(QDir().mkpath(directory.filePath(u"missing"_q)));
	CHECK(invalid.tryAcquire() == Core::SingleInstanceLock::Result::Acquired);
	CHECK(invalid.errorString().isEmpty());
	auto directoryLock = Core::SingleInstanceLock(directory.path());
	CHECK(directoryLock.tryAcquire()
		  == Core::SingleInstanceLock::Result::Error);
	CHECK(!directoryLock.errorString().isEmpty());
}

TEST_CASE(SingleInstanceCompetingTakeovers) {
	const auto child = qEnvironmentVariable("TDESKTOP_TAKEOVER_TEST_CHILD");
	const auto folder = qEnvironmentVariable("TDESKTOP_TAKEOVER_TEST_DIR");
	const auto serverName
		= qEnvironmentVariable("TDESKTOP_TAKEOVER_TEST_SERVER");
	if (!child.isEmpty()) {
		CHECK(Touch(folder + u"/ready"_q + child));
		CHECK(WaitForFile(folder + u"/start"_q));
		auto lock = Core::SingleInstanceLock(folder + u"/profile.lock"_q);
		const auto acquired = lock.tryAcquire();
		auto server = QLocalServer();
		if (acquired == Core::SingleInstanceLock::Result::Acquired) {
			CHECK(server.listen(serverName));
			CHECK(Touch(folder + u"/winner"_q + child));
			CHECK(WaitForFile(folder + u"/release"_q));
		} else {
			CHECK(acquired == Core::SingleInstanceLock::Result::Busy);
			CHECK(Touch(folder + u"/loser"_q + child));
			CHECK(WaitForFile(folder + u"/release"_q));
			auto timer = QElapsedTimer();
			timer.start();
			auto retry = lock.tryAcquire();
			while (retry == Core::SingleInstanceLock::Result::Busy
				   && timer.elapsed() < 10000) {
				QThread::msleep(5);
				retry = lock.tryAcquire();
			}
			CHECK(retry == Core::SingleInstanceLock::Result::Acquired);
			CHECK(server.listen(serverName));
			CHECK(Touch(folder + u"/retried"_q + child));
		}
		return;
	}

	auto directory = QTemporaryDir();
	CHECK(directory.isValid());
	const auto prefix = directory.path();
#ifdef Q_OS_WIN
	const auto name
		= u"teagram-takeover-test-"_q + QUuid::createUuid().toString();
#else  // Q_OS_WIN
	const auto name = prefix + u"/server"_q;
#endif // Q_OS_WIN
	auto processes = std::array<QProcess, 2>();
	for (auto i = 0; i != 2; ++i) {
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert(u"TDESKTOP_TAKEOVER_TEST_CHILD"_q,
						   QString::number(i + 1));
		environment.insert(u"TDESKTOP_TAKEOVER_TEST_DIR"_q, prefix);
		environment.insert(u"TDESKTOP_TAKEOVER_TEST_SERVER"_q, name);
		processes[i].setProcessEnvironment(environment);
		processes[i].start(
			QCoreApplication::applicationFilePath(),
			{u"--case"_q, u"SingleInstanceCompetingTakeovers"_q});
		CHECK(processes[i].waitForStarted(10000));
	}
	CHECK(WaitForFile(prefix + u"/ready1"_q));
	CHECK(WaitForFile(prefix + u"/ready2"_q));
	CHECK(Touch(prefix + u"/start"_q));
	auto timer = QElapsedTimer();
	timer.start();
	while (timer.elapsed() < 10000
		   && !(QFile::exists(prefix + u"/winner1"_q)
				&& QFile::exists(prefix + u"/loser2"_q))
		   && !(QFile::exists(prefix + u"/winner2"_q)
				&& QFile::exists(prefix + u"/loser1"_q))) {
		QThread::msleep(5);
	}
	const auto firstWins = QFile::exists(prefix + u"/winner1"_q)
						   && QFile::exists(prefix + u"/loser2"_q);
	const auto secondWins = QFile::exists(prefix + u"/winner2"_q)
							&& QFile::exists(prefix + u"/loser1"_q);
	CHECK(firstWins || secondWins);
	CHECK(Touch(prefix + u"/release"_q));
	for (auto &process : processes) {
		CHECK(process.waitForFinished(10000));
		CHECK_EQ(process.exitCode(), 0);
	}
	CHECK(
		QFile::exists(prefix + (firstWins ? u"/retried2"_q : u"/retried1"_q)));
}
