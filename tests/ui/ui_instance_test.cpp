#include "ui/instance_guard.hpp"

#include <QCoreApplication>
#include <QUuid>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using tdm::ui::InstanceGuard;

TEST(UiInstanceGuard, SecondLaunchHandsItsUrlToThePrimary) {
    const QString key = QStringLiteral("TeslaDM-test-") + QUuid::createUuid().toString(QUuid::Id128);
    InstanceGuard primary(key);
    ASSERT_TRUE(primary.tryBecomePrimary(QString()));
    QStringList got;
    QObject::connect(&primary, &InstanceGuard::messageReceived, [&](const QString& m) { got << m; });

    // Each secondary blocks in tryBecomePrimary() while writing, so it runs on
    // its own thread; the primary keeps processing events here.
    auto launch = [&](const QString& msg) {
        bool secondary_result = true;
        std::thread t([&] {
            InstanceGuard second(key);
            secondary_result = second.tryBecomePrimary(msg);
        });
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const qsizetype before = got.size();
        while (got.size() == before && std::chrono::steady_clock::now() < end)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        t.join();
        return secondary_result;
    };
    EXPECT_FALSE(launch(QStringLiteral("https://example.com/a.zip")));
    EXPECT_FALSE(launch(QString()));  // plain second launch: just activate
    ASSERT_EQ(got.size(), 2);
    EXPECT_EQ(got[0], QStringLiteral("https://example.com/a.zip"));
    EXPECT_EQ(got[1], QString());
}

TEST(UiInstanceGuard, KeyIsPerUserAndStable) {
    EXPECT_EQ(InstanceGuard::defaultKey(), InstanceGuard::defaultKey());
    EXPECT_TRUE(InstanceGuard::defaultKey().startsWith(QStringLiteral("TeslaDM-")));
}
