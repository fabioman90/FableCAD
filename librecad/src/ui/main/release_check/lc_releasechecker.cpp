/*
 * ********************************************************************************
 * This file is part of the LibreCAD project, a 2D CAD program
 *
 * Copyright (C) 2025 LibreCAD.org
 * Copyright (C) 2025 sand1024
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * ********************************************************************************
 */

#include "lc_releasechecker.h"

#ifdef FABLECAD_SPARKLE
#include "lc_sparkleupdater.h"
#endif

#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QRegularExpression>
#include <algorithm>
#include <utility>

#include "rs_debug.h"
#include "rs_dialogfactory.h"
#include "rs_dialogfactoryinterface.h"
#include "rs_settings.h"

#ifdef Q_OS_WIN
#include <vector>
#include <qt_windows.h>
// <appmodel.h> is missing before MinGW-w64 v11; kernel32 exports this.
extern "C" WINBASEAPI LONG WINAPI GetCurrentPackageFullName(UINT32 *packageFullNameLength, PWSTR packageFullName);
#endif

namespace {
// A failed update check must never interrupt the user with a modal dialog.
// The check runs at startup and can be re-triggered from the menu, so log a
// warning to the console at most once per LibreCAD session, then stay silent.

// fixme - sand - well, this is actually disputable - nobody look at the console.
// but if the user set that noficiation is expected - it might be notified.
// probably instead of console it might be message in statusbar - yet
// actually I don't think modal is evil there.
// yet let it be silent - not too important thing.

void warnUpdateCheckFailedOnce(const QString &detail) {
    static bool warned = false;
    if (warned)
        return;
    warned = true;
    RS_Debug::Log(RS_Debug::D_WARNING)
        << "Checking for updates failed: " << detail;
}
} // namespace

LC_TagInfo::LC_TagInfo(int majorVer, int minorVer, int revisionNum, int bugfixVer, const QString &labelVer, const QString &tagNameVer):m_major(majorVer), m_minor(minorVer), m_revision(
                                                                                                                                          revisionNum), m_bugfix(bugfixVer), m_label(labelVer), m_tagName(tagNameVer) {
}

QString LC_TagInfo::getLabel() const {
    return m_label;
}

QString LC_TagInfo::getTagName() const {
    return m_tagName;
}

bool LC_TagInfo::isSameVersion(const LC_TagInfo& other) const {
    return m_major == other.m_major && m_minor == other.m_minor && m_revision == other.m_revision && m_bugfix == other.m_bugfix;
}

bool LC_TagInfo::isBefore(const LC_TagInfo& other) const {
    if (m_major < other.m_major) {
        return true;
    }
    if (m_major == other.m_major) {
        if (m_minor < other.m_minor) {
            return true;
        }
        if (m_minor == other.m_minor) {
            if (m_revision < other.m_revision) {
                return true;
            }
            if (m_revision == other.m_revision) {
                if (m_bugfix < other.m_bugfix) {
                    return true;
                }
            }
        }
    }
    return false;
}

LC_ReleaseChecker::LC_ReleaseChecker(const QString& ownTagName, const bool ownPreRelease) :
    m_emitSignalIfNoNewVersion{false}, m_ownReleaseInfo(getOwnReleaseInfo(ownTagName, ownPreRelease)) {
    connect(&m_webCtrl, &QNetworkAccessManager::finished, this, &LC_ReleaseChecker::infoReceived);
}

bool LC_ReleaseChecker::runsFromStorePackage() {
#ifdef Q_OS_WIN
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
        return false; // no package identity
    }
    std::vector<wchar_t> fullName(length);
    if (GetCurrentPackageFullName(&length, fullName.data()) != ERROR_SUCCESS) {
        return false;
    }
    // kernel32.dll does not export GetStagedPackageOrigin, and MinGW-w64
    // before v12 has no import library for it.
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    using OriginQuery = LONG (WINAPI *)(PCWSTR, int *);
    // void (*)() converts to any function type without -Wcast-function-type
    const auto query = kernelbase == nullptr ? nullptr
        : reinterpret_cast<OriginQuery>(reinterpret_cast<void (*)()>(GetProcAddress(kernelbase, "GetStagedPackageOrigin")));
    // 3 is PackageOrigin_Store: installed from the Microsoft Store.
    int origin = 0;
    return query != nullptr && query(fullName.data(), &origin) == ERROR_SUCCESS && origin == 3;
#else
    return false;
#endif
}

LC_ReleaseInfo LC_ReleaseChecker::getOwnReleaseInfo(const QString& tagName, const bool preRelease) const {
    constexpr bool draft = false;
    const LC_TagInfo ownTagInfo = parseTagInfo(tagName);
    auto result = LC_ReleaseInfo("", draft, preRelease, "", "");
    result.setTagInfo(ownTagInfo);
    return result;
}

void LC_ReleaseChecker::checkForNewVersion(const bool forceCheck) {
#ifdef FABLECAD_SPARKLE
    if (LC_SparkleUpdater::check(forceCheck, LC_GET_ONE_BOOL("Startup", "CheckForNewVersions", true)))
        return;
#ifdef FABLECAD_ANNUAL_EDITION
    // The global release list also contains releases from other editions.
    return;
#endif
#endif
    if (runsFromStorePackage()) {
        if (forceCheck)
            QDesktopServices::openUrl(QUrl(QStringLiteral("ms-windows-store://downloadsandupdates")));
        return;
    }
    m_emitSignalIfNoNewVersion |= forceCheck;
    for (const auto* reply : m_webCtrl.findChildren<QNetworkReply*>()) {
        if (reply->isRunning())
            return;
    }

    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/fabioman90/FableCAD/releases")));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(15000);
    m_webCtrl.get(request);
}

void LC_ReleaseChecker::infoReceived(QNetworkReply* reply) {
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray responseContent = reply->readAll();
        // auto resultStr = QString(responseContent);
        processReleasesJSON(responseContent);
    }
    else {
        m_emitSignalIfNoNewVersion = false;
        warnUpdateCheckFailedOnce(reply->errorString());
    }
    reply->deleteLater();
}

LC_TagInfo LC_ReleaseChecker::parseTagInfo(const QString& tagName) const {
    static const QRegularExpression version(
        QStringLiteral(R"(\Av?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?(?:[-_]([A-Za-z0-9][A-Za-z0-9._-]*))?\z)"));
    const auto match = version.match(tagName);
    if (!match.hasMatch())
        return {};

    int parts[4]{};
    for (int i = 0; i < 4; ++i) {
        const auto component = match.captured(i + 1);
        if (component.isEmpty())
            continue;
        bool valid = false;
        parts[i] = component.toInt(&valid);
        if (!valid)
            return {};
    }
    return {parts[0], parts[1], parts[2], parts[3], match.captured(5), tagName};
}

void LC_ReleaseChecker::processReleasesJSON(const QByteArray& responseContent) {
    const bool manualCheck = std::exchange(m_emitSignalIfNoNewVersion, false);
    m_latestRelease = {};
    m_latestPreRelease = {};

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(responseContent, &error);
    if (!document.isArray()) {
        warnUpdateCheckFailedOnce(error.error == QJsonParseError::NoError
            ? QStringLiteral("unexpected release feed format") : error.errorString());
        return;
    }

    LC_GROUP_GUARD("Startup");
    const bool ignorePreReleases = !m_ownReleaseInfo.m_isPrerelease &&
        LC_GET_BOOL("IgnorePreReleaseVersions", true);
    const auto ignoredRelease = parseTagInfo(LC_GET_STR("IgnoredRelease", "0.0.0.0"));
    const auto ignoredPreRelease = parseTagInfo(LC_GET_STR("IgnoredPreRelease", "0.0.0.0"));
    const auto ownVersion = m_ownReleaseInfo.getTagInfo();
    QVector<LC_ReleaseInfo> stable, previews;
    for (const auto& value : document.array()) {
        const auto release = value.toObject();
        if (!release["draft"].isBool() || !release["prerelease"].isBool() ||
            release["draft"].toBool())
            continue;

        const auto tag = parseTagInfo(release["tag_name"].toString());
        if (ownVersion.getTagName().isEmpty() || tag.getTagName().isEmpty() || !ownVersion.isBefore(tag))
            continue;

        const bool prerelease = release["prerelease"].toBool() || !tag.getLabel().isEmpty();
        if (prerelease && ignorePreReleases)
            continue;
        const auto& ignored = prerelease ? ignoredPreRelease : ignoredRelease;
        if (!ignored.isBefore(tag))
            continue;

        const auto url = QStringLiteral("https://github.com/fabioman90/FableCAD/releases/tag/") + tag.getTagName();
        LC_ReleaseInfo candidate(release["published_at"].toString(), false, prerelease, url, {});
        candidate.setTagInfo(tag);
        (prerelease ? previews : stable).append(candidate);
    }
    sortReleasesInfo(stable);
    sortReleasesInfo(previews);
    if (!stable.isEmpty())
        m_latestRelease = stable.last();
    if (!previews.isEmpty())
        m_latestPreRelease = previews.last();

    if (m_latestRelease.isValid() || m_latestPreRelease.isValid() || manualCheck)
        emit updatesAvailable();
}

void LC_ReleaseChecker::sortReleasesInfo(QVector<LC_ReleaseInfo>& releases) const {
    std::sort(releases.begin(), releases.end(), [](const auto& left, const auto& right) {
        const auto& a = left.getTagInfo();
        const auto& b = right.getTagInfo();
        return a.isSameVersion(b) ? right.isAfter(left) : a.isBefore(b);
    });
}

const LC_ReleaseInfo& LC_ReleaseChecker::getLatestRelease() const {
    return m_latestRelease;
}

const LC_ReleaseInfo& LC_ReleaseChecker::getLatestPreRelease() const {
    return m_latestPreRelease;
}
